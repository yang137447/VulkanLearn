# 上百层地表纹理虚拟化设计

> **状态：未来工作，非契约。**
> 本文只描述“16km、256 全局层、每 tile 32 候选层、top-4 混合”目标下的纹理虚拟化方案，
> 不表示任何能力已经实现，也不替代 [`contracts/terrain.md`](../../contracts/terrain.md)。
> 当前运行时行为以契约和源码为准；本文的格式、预算、页大小和阶段都可以在实现前调整。

| 项目 | 内容 |
| --- | --- |
| 类型 | 架构设计 / 实施计划 |
| 状态 | 未实现。当前是单张 `sampler2DArray` + 全量常驻，没有层纹理页表 |
| 目标 | 256 层全局 catalog、每 tile 32 候选、top-4、16km 流式走查 |
| 预算档位 | 512 MiB / 1 GiB（地形纹理常驻上限） |
| 非目标 | 地形 LOD/skirt、碰撞、编辑工具、GPU-driven 几何、最终美术资产 |
| 关联 | [大世界与上百层计划](terrain-large-world-layers-plan.md)、[VT 流式闭环](terrain-vt-streaming-design.md)、[terrain 契约](../../contracts/terrain.md)、[texture 资产契约](../../contracts/texture-asset-json-v1.md) |

---

## 0. 结论摘要

1. **“256 层 × 1K × 三张图”不能用一个 `sampler2DArray` 无条件全量常驻。**
   RGBA8 + mip 链约 4 GiB；BC7/BC5 + mip 链约 1 GiB。前者超出合理预算，
   后者也只是“刚好塞进 1 GiB”，没有任何按需淘汰能力。
2. **当前引擎没有 bindless、没有 BC 格式、没有整数采样器、没有 SSBO。**
   `MaterialDescriptorSchema::Build` 只接受 `sampler2D` / `sampler2DArray`，
   每个材质纹理 `descriptorCount = 1`；设备只启用了 swapchain 扩展和
   `depthClamp/samplerAnisotropy/dualSrcBlend`。因此“每层一个 texture + descriptor
   indexing”不是低风险增量。
3. **推荐目标架构：方案 C（第二级 VT）+ 可选的热层 `sampler2DArray` 快速路径。**
   全局层 catalog 只保存每层元数据与虚拟页基址；`top-4 ID → catalog → 层页表 →
   atlas`。albedo/normal/pbr 的页三元组共分配，页表条目一次查三张图。
4. **近期可先做方案 A 的“小层集快速路径”**：把当前 `sampler2DArray` 保留给
   ≤32/≤64 个热层或小场景；完整 256 层走 VT。两条路径用材质宏切换，便于 A/B 验收。
5. **BC 压缩是预算档位的前提。** 没有 BC 时同样预算只能容纳约 1/4 的层页；
   当前 `TextureIO` 虽能识别 `.ktx2` / `.dds` 扩展名，但实际解码仍走 stb_image，
   没有块压缩容器实现。
6. **L4 的 feedback 驱动驻留依赖 P3。** 当前 `terrainFeedback` 只做只读统计，
   `RequestPageAtEpoch` 已就绪但生产路径没有调用；层纹理流式必须排在 P3 之后。

---

## 1. 现状与瓶颈

### 1.1 当前层纹理的数据通路

当前 `M_terrain` 的层纹理是**三张同分辨率、同层数的 `sampler2DArray`**：

- `shader/glsl/M_terrain.json` 声明
  `layerAlbedo`（sRGB）、`layerNormal`（linear）、`layerPbr`
  （R=roughness、G=metallic、B=AO、A=height），三者都是 `sampler2DArray`。
- `mf_terrainLayerBlend.glsl` 的 `EvaluateMFTerrainInputs` 先读
  `layerIds`/`layerWeights`（RGBA8 atlas，top-4），再对每个 slot：
  1. `TerrainLayerExtra(layer)` 从 `layerTable` 取 UV 参数；
  2. `texture(layerAlbedo, vec3(uv, float(layer)))`
     直接用**全局 layerId 作为数组 slice**；
  3. 同法采样 normal/pbr。
- `TerrainLayerTable::BuildImage` 生成 256×4 `RGBA32F` LUT（L1 起；
  row2/row3 目前只有 CPU 侧消费）；
  `MaxLayers = 256`（`terrainLayerTable.h:19`）。
- `TerrainWeightMapBuilder::Build/BuildPage` 每 texel 选 top-4，
  ID 为 8-bit 全局层号（`SelectTop4` / `BakeTile`）。
- `RendererTerrainLoader::LoadTerrainObject` 在绑定前检查
  `layerAlbedo->GetArrayLayers() >= plan.desc.layers.size()`
  （`rendererTerrainLoader.cpp:499-511`），即数组层数必须覆盖 TR 的层数。
- 三张数组由 `DeviceTextureFactory::CreateResourceFromHostImage` 逐层上传，
  并按层生成 mip 链；`Texture` 在 `CreateDesc::textureArray` 时创建
  `2D_ARRAY` view（`texture.cpp`）。

当前冒烟资产（资源根 `Generated/Validation/TerrainB32/Textures/T_terrain_b32_*.json`）
是 32 层、每层 32×32 的小图，只用于验证 slice 索引和混合，不代表目标分辨率。

### 1.2 当前单 `sampler2DArray` 的好处与上限

**好处**

- 一个 combined image sampler、一个 binding，`descriptorCount = 1`，
  与当前 `MaterialDescriptorSchema` / 反射 / ABI 完全兼容。
- 硬件直接做 array slice 选择，shader 只需 `vec3(uv, layer)`，
  没有页表间接寻址，采样开销最低。
- 每层 mip 链由设备纹理统一管理，过滤/各向异性行为一致。
- 不需要新的 RenderGraph 资源；层数组就是普通材质纹理。

**上限**

- 所有 slice 必须**同分辨率、同格式、同 mip 数**；
  不能给岩石 2K、草地 1K，也不能只给近景层更高分辨率。
- 不能“部分常驻”：要么整层存在，要么整层不存在；
  无法按页淘汰，无法只保留近景 mip。
- 256 层正好是 Vulkan 对 `maxImageArrayLayers` 的**最低保证值**
  （规范下限 256）；512 层必须先查询设备能力。
- 256 层同时上传/生成 mip 的加载时间与峰值内存都很高；
  World 切换无法增量发布。
- 全部层共享一个格式，albedo 需要 sRGB，normal/pbr 需要 linear，
  因此必须拆成三张数组；三张数组的 slice 数、分辨率又被绑死。

### 1.3 显存估算（含计算过程）

记号：

```text
单图 mip0      = R × R × bpp
单图 mip 链    ≈ mip0 × (1 + 1/4 + 1/16 + …) = mip0 × 4/3
单层三图       = 3 × 单图 mip 链
256 层         = 256 × 单层三图
```

- `RGBA8`：`bpp = 4 B/texel`。
- `BC7` / `BC5`：16 B / 4×4 block = **1 B/texel**（BC7 用于 albedo/pbr，
  BC5 用于 normal 的 XY）。
- 1 MiB = 1024² B。

**每层三图占用**

| 分辨率 | RGBA8 mip0 | RGBA8 含 mip | BC mip0 | BC 含 mip |
| --- | --- | --- | --- | --- |
| 512² | 3 MiB | 4 MiB | 768 KiB | 1 MiB |
| 1024² | 12 MiB | 16 MiB | 3 MiB | 4 MiB |
| 2048² | 48 MiB | 64 MiB | 12 MiB | 16 MiB |

**256 全局层同时常驻**

| 分辨率 | RGBA8 含 mip | BC 含 mip |
| --- | --- | --- |
| 512² | 1 GiB | 256 MiB |
| 1024² | 4 GiB | 1 GiB |
| 2048² | 16 GiB | 4 GiB |

**每 tile 32 候选层**（只算被 tile 引用的层，不含页级淘汰）：

| 分辨率 | RGBA8 含 mip | BC 含 mip |
| --- | --- | --- |
| 512² | 128 MiB | 32 MiB |
| 1024² | 512 MiB | 128 MiB |
| 2048² | 2 GiB | 512 MiB |

计算示例（1K RGBA8）：

```text
1024² × 4 B          = 4 MiB        （单图 mip0）
4 MiB × 4/3          = 5.33 MiB     （单图 mip 链）
5.33 × 3             = 16 MiB       （单层三图）
16 MiB × 256         = 4096 MiB     （= 4 GiB，256 层）
16 MiB × 32          = 512 MiB      （32 候选层）
```

计算示例（1K BC）：

```text
1024² × 1 B          = 1 MiB
1 MiB × 4/3          = 1.33 MiB
1.33 × 3             = 4 MiB
4 MiB × 256          = 1024 MiB     （= 1 GiB）
4 MiB × 32           = 128 MiB
```

结论：

- 1K RGBA8 的 4 GiB 超出“地形 1 GiB”预算；2K 更不可能。
- 1K BC 的 1 GiB 只是“全部层都常驻”的静态值，没有给阴影、后处理、
  staging、编辑器留余量，也没有任何按需能力。
- 32 候选层在 1K BC 下是 128 MiB/tile；如果每个可见 tile 都独立常驻
  32 层，多个 tile 会迅速叠加。**层纹理必须跨 tile 共享**，不能按 tile 复制。

### 1.4 当前页表/ID 图的硬约束

这些约束会直接影响层纹理 VT 的格式设计：

- `TerrainPageManager::PageTableSize = 32`、`PageResolution = 128`、
  `AtlasResolution = 4096`、`PageSlotCount = 1024`
  （`terrainPageManager.h:25-28`）。
- 页表是 **RGBA8**：`PageTableUpload::entry[4]` 直接对应 texel；
  物理页坐标按 `0..31` 存进 8-bit 通道。
  一旦物理页数超过 256，就不能再复用这个编码。
- `layerIds` 是 **RGBA8**，每 texel 4 个 8-bit 全局层号。
  256 层正好用满；257 层就必须升级为 16-bit ID（atlas 翻倍到 128 MiB）。
- `terrainFeedback` 格式 v1 是 128×128 RGBA8，
  页坐标只留 **5+5 bit**（`terrainFeedback.h`），
  装不下“层号 + 层页 + mip”。
- 覆盖 atlas 目前**没有 mip**：`CreateTerrainPageAtlasGpuResources`
  用 `generateMipmaps = false`（`rendererTerrainLoader.cpp:218-230`）。
  层纹理 VT 若也照搬“无 mip”，远处会出现严重闪烁；若加 mip，
  又不能简单给 atlas 生成整链（页会互相渗色），必须按 mip 分页。

### 1.5 引擎能力缺口（决定方案可行性）

| 能力 | 当前状态 | 依据 |
| --- | --- | --- |
| Vulkan API | 1.3 | `vulkanManager.cpp:81` `VK_API_VERSION_1_3` |
| 设备扩展 | 只有 swapchain | `VulkanManager.h:114-115` |
| 设备特性 | 只有 depthClamp / samplerAnisotropy / dualSrcBlend | `vulkanManager.cpp:203-208` |
| descriptor indexing | 未启用 | 同上，无 `VkPhysicalDeviceDescriptorIndexingFeatures` |
| 稀疏绑定 | 未启用 | 无 sparse 扩展/特性 |
| 材质纹理类型 | 仅 `sampler2D` / `sampler2DArray` | `materialDescriptorSchema.cpp:183-195`、`materialAssetValidator.cpp:369` |
| 材质 descriptor | 每纹理 `descriptorCount = 1`，`eCombinedImageSampler` | `materialDescriptorSchema.cpp:191-201` |
| SSBO / samplerBuffer | 未支持 | 材质 schema 只有 UBO + 纹理 |
| 整数采样器 | 未支持（`glslType` 白名单） | 同上 |
| BC 压缩 | 未支持 | `DeviceTextureFactory::ToVkFormat` 只有 RGBA8/16F/32F 等 |
| KTX2/DDS | 仅识别扩展名，解码仍走 stb_image | `textureIO.cpp:51-63`、`Load` 分支 |

因此：

- 方案 B（per-layer bindless）需要新增设备特性查询/启用、可更新/可变计数描述符、
  着色器非 uniform 索引、材质 schema 与 ABI 变更，**不是一个小改动**。
- 方案 C 可以只用现有 `sampler2D` + `texelFetch` + 普通材质纹理实现，
  与当前 descriptor/renderGraph 合同最兼容。
- 任何“页表用 `R16_UINT`”的设计都会撞上“材质只允许 sampler2D/sampler2DArray”；
  在扩展 schema 之前，页表只能存成浮点纹理（如 RGBA32F）。

---

## 2. 目标与预算

### 2.1 目标数字

| 项目 | 目标值 | 说明 |
| --- | --- | --- |
| 世界尺寸 | 16 km × 16 km（1,600,000 cm） | 与既有大世界计划一致 |
| 全局层数 | 256 | 8-bit layer ID 上限；257+ 需格式升级 |
| 每 tile 候选层 | 32 | 只用于驻留/筛选，不进入逐像素循环 |
| 每像素有效层 | top-4 | 现有 ID/weight 图语义 |
| tile 尺寸 | 500 m（50,000 cm） | 32×32 tile；512 m 则覆盖 16.384 km |
| 覆盖页 | 128×128 / tile | 当前 `PageResolution`；500 m 对应约 3.9 m/texel |
| 层页 | 128×128 | 默认；256² 作为大层/2K 层的可选项 |
| 层源分辨率 | 1K 默认，2K 仅 hero 层 | 由 catalog 按层声明 |
| 压缩 | albedo BC7 sRGB、normal BC5、pbr BC7 linear | 见 §4.6 |
| 预算 | 512 MiB 或 1 GiB | 见 §2.2 |

### 2.2 两档预算

以 128² 层页、BC 压缩、albedo/normal/pbr 页三元组共分配为前提
（每页 16 KiB，三元组 48 KiB）：

**档位 S：512 MiB**

| 项目 | 计算 | 占用 |
| --- | --- | --- |
| 覆盖 ID atlas | 4096² × 4 B | 64 MiB |
| 覆盖 weight atlas | 4096² × 4 B | 64 MiB |
| 覆盖 page table | 32² × 4 B | 4 KiB |
| 层 atlas mip0 | 4 slices × 3 maps × 16 MiB | 192 MiB |
| 层 atlas mip1.. | mip0 × 1/3（4/3 链减去 mip0） | 64 MiB |
| 层页表 + catalog | 约 22k 项 × 16 B | < 1 MiB |
| feedback / upload ring | 2 帧 | 约 10 MiB |
| **合计** | | **约 395 MiB** |

容量：`4 slices × 1024 pages/slice = 4096` 个物理页三元组；
1K 层每层 85 页（8² + 4² + 2² + 1²），
可完整容纳约 **48 个 1K 层**。超过部分靠淘汰/低 mip。

**档位 M：1 GiB**

| 项目 | 计算 | 占用 |
| --- | --- | --- |
| 覆盖 ID/weight | 同 S | 128 MiB |
| 层 atlas mip0 | 8 slices × 3 maps × 16 MiB | 384 MiB |
| 层 atlas mip1.. | mip0 × 1/3 | 128 MiB |
| 页表/catalog/ring | | 约 11 MiB |
| **合计** | | **约 651 MiB** |

容量：`8 × 1024 = 8192` 个物理页三元组，约 **96 个 1K 层** 完整常驻；
剩余约 350 MiB 可用于 2K hero 层或更远的预取。

> 若最终不加 BC：同预算只能容纳约 1/4 的层页；档位 S 的层 atlas 会变成
> 768 MiB（RGBA8），已经超过 512 MiB 上限。因此 **BC 是这两档预算的前提**。

### 2.3 非目标

- 不改变 `TerrainMeshBuilder` 的几何/LOD；层纹理虚拟化只改材质采样与页驻留。
- 不把覆盖 ID/weight 页从 128² 扩到 256²；那是独立的精度问题。
- 不做 GPU sparse residency、meshlet、GPU-driven 层选择。
- 不做地形编辑/笔刷；层页仍由 CPU 烘焙或离线工具生成。
- 不把“方案 B 的 bindless”写进当前实现；它需要单独的设备能力评审。

---

## 3. 方案对比

### 3.1 评估轴

1. 与当前材质 schema / descriptor / RenderGraph 的兼容性；
2. 显存可控性（能否按页/按层淘汰）；
3. 采样开销与 shader 复杂度；
4. 改动面（设备特性、ABI、编辑器、测试）；
5. 256 层 / 2K / 未来 512 层的扩展性。

### 3.2 方案 A：单一全局 texture array（现状扩展）

```text
layerAlbedo/Normal/Pbr = sampler2DArray[256]
layerId 直接作为 slice
```

**做法**：把当前三张 `sampler2DArray` 扩到 256 slice，统一 1K（或 512）+
BC7/BC5 + mip；显存按 §1.3 固定。

**兼容性**：最高。`M_terrain.json` 已经声明三张 `sampler2DArray`；
`MaterialDescriptorSchema`、反射、RenderGraph、terrain loader 的层数校验都无需改。

**改动面**：

- 新增 BC 上传路径（`HostImage`/`TextureIO`/`DeviceTextureFactory`）；
- 资源加载从 32 张小图扩到 256 层；
- `TerrainLayerTable` 已经支持 256 层；
- 若要部分驻留，需要“整层换出/换入”，即拷贝整个 slice（4 MiB/层），
  不能按页做。

**优点**：实现最短、采样最快、没有页表 indirect、没有 atlas 渗色。

**缺点**：

- 1K BC 全量 1 GiB；512² 才降到 256 MiB；
- 2K 不可行（4 GiB）；
- 无法按页/按 mip 淘汰，16km 走查时要么全驻留，要么整层替换；
- 256 slice 已经是 `maxImageArrayLayers` 的最低保证值；
- 加载 256×3 张图 + mip 的峰值时间/内存高。

**适用**：小场景、层数 ≤32/≤64 的热层快速路径，或“预算充足且不要求流式”的
过渡实现。

### 3.3 方案 B：每层独立纹理 + descriptor indexing / bindless

```text
layout(set=1, binding=N) uniform sampler2D layerTextures[];  // 非 uniform 索引
layerId -> descriptor index
```

**做法**：每层 albedo/normal/pbr 各一张独立纹理；用一个 descriptor array
（或 `VK_EXT_descriptor_indexing` 的 bindless）按 layerId 索引；
每层可独立分辨率、独立 mip、独立淘汰。

**兼容性**：低，且当前不可用。

- 实例只启用 Vulkan 1.3、设备只启用 swapchain 扩展；
  `descriptorIndexing`、`shaderSampledImageArrayNonUniformIndexing`、
  `descriptorBindingPartiallyBound`、`runtimeDescriptorArray` 等都没有查询/启用。
- `MaterialDescriptorSchema` 每个纹理固定 `descriptorCount = 1`；
  要做 bindless 必须支持“一个 binding 多个 descriptor + 可变计数”，
  并改动 descriptor set 分配、更新、反射/ABI、shader 生成。
- `MaterialAssetValidator` 只允许 `sampler2D` / `sampler2DArray`，
  没有 `sampler2D textures[]` 的语法与校验。
- 每层三张纹理意味着 768 个 descriptor；需要 `updateAfterBind`、
  `descriptorBindingVariableDescriptorCount`、池容量重算。

**优点**：流式粒度最好（整层/整 mip 即可），不需要页表间接；
每层分辨率自由；未来 512+ 层也自然。

**缺点**：设备特性、驱动差异、材质 schema、ABI、编辑器、测试全线改动；
当前项目没有 bindless 基础设施，风险最高。

**结论**：**不作为本期目标**。只有未来引擎决定全面引入 descriptor indexing
时才重新评估；届时它可以替代方案 C 的页表间接层。

> 注：任务描述里提到“Vulkan 1.4”，但当前代码请求的是 **Vulkan 1.3**
> （`VK_API_VERSION_1_3`）。即使升到 1.4，descriptor indexing 仍需要
> 显式查询/启用特性和扩展，不会自动可用。

### 3.4 方案 C：第二级 VT（albedo/normal/pbr 各一张页表 + atlas）

```text
layerId -> global layer catalog（元数据 + 虚拟页基址）
        -> layer page table（虚拟页 -> 物理页）
        -> atlas（物理页）
```

**做法**：

- 三张 atlas（albedo BC7 sRGB、normal BC5、pbr BC7 linear），
  每张由若干 4096² slice 组成，页 128²。
- 一个 `layerPageTable`（RGBA32F，见 §4.4）把
  `(layerId, mip, pageX, pageY)` 映射到物理页号；
- 因为 albedo/normal/pbr 的页**共分配**（同一物理页号对应三张图的同一页），
  一次页表查询就能服务三张图。
- 层页按 `layerId + mip + page` 全局共享，与 terrain `placementId` 无关；
  同一 TR 多处摆放不会重复占页。

**兼容性**：较高。

- atlas 就是 `sampler2D`（或 `sampler2DArray` 的多 slice 页池），
  页表是 RGBA32F `sampler2D`，catalog 是 RGBA32F `sampler2D`；
  全部落在现有材质纹理白名单内。
- 不需要新的 RenderGraph pass（层页表通过 MI 纹理绑定）；
  只有可选的层 feedback 需要新 RT。
- 不需要 descriptor indexing / sparse / BC 之外的设备特性。

**改动面**：

- 新增层页管理器（CPU residency、物理页分配、mip、淘汰）；
- 新增 atlas/catalog/页表 GPU 资源与上传批次；
- shader 增加一次页表 fetch + `textureGrad`；
- `M_terrain.json` 增加 5 个纹理绑定（catalog、页表、三张 atlas），
  旧的三张数组保留为快速路径；
- 编辑器 MI 预览/缩略图需要认识新纹理（当前缩略图不渲染地形）。

**优点**：

- 显存按页/按 mip 控制，可实现 512 MiB / 1 GiB 档位；
- 层数不受 `maxImageArrayLayers` 限制（只受 8-bit ID 限制，256 层内）；
- 页全局共享，多个 terrain placement 不重复；
- 能在不引入 bindless 的前提下实现 16km 流式。

**缺点**：

- 每层每张图多一次页表 fetch；top-4 × 3 图 = 12 次额外 fetch；
- atlas 需要页内 inset、禁止 atlas mip、限制各向异性，否则页边渗色；
- mip 必须按“每 mip 一张页表 + 每 mip 物理页”管理，不能直接给 atlas 生成整链；
- 页表格式受“只允许浮点 sampler”限制（除非先扩展 schema）。

### 3.5 方案 D：数组页（每 8/16 层一个 array page）或 clipmap / 稀疏数组

**做法**：

- 把 256 层切成 16/32 个 `sampler2DArray`，每个数组 8/16 层；
  以“数组页”为单位驻留/淘汰。
- 或使用 clipmap：近景高分辨率环、远景低分辨率环，
  每环固定若干层数组。
- 或使用 `VK_EXT_sparse_residency` 的稀疏数组，按 tile 提交物理页。

**兼容性**：中。

- 多数组仍可用现有 `sampler2DArray`，但材质需要固定数量的 binding
  （例如 16 个数组），并且 shader 要按 layerId 选择数组；
  这会把“数组选择”变成 shader 分支/索引，且每个 binding 仍是 `descriptorCount = 1`。
- 稀疏数组需要 `sparseBinding` / `sparseResidency*` 特性，
  当前设备创建未启用；实现和验证成本高。

**优点**：

- 比方案 A 的整层粒度更细（8/16 层一组）；
- clipmap 对远景带宽友好，适合“远处只要低 mip”的场景。

**缺点**：

- 组粒度仍是 8/16 层 × 全分辨率，最坏 64 MiB/组（1K BC + mip）；
- 数组数量多，材质 binding 固定且难以按资产变化；
- 稀疏数组的平台支持与调试成本高；
- 仍然不能表达“每层不同分辨率/不同页”。

**结论**：作为方案 C 的**页池实现细节**（atlas 由多个 array slice 组成）
可以采用；作为整体架构不如 C 灵活。

### 3.6 对比与推荐

| 维度 | A 单一数组 | B bindless | C 二级 VT | D 数组页/稀疏 |
| --- | --- | --- | --- | --- |
| 当前 schema 兼容 | 最高 | 低（需大改） | 高 | 中 |
| 显存可控 | 差（整层） | 最好（每层） | 好（每页/每 mip） | 中（每组） |
| 采样开销 | 最低 | 低 | 中（+页表 fetch） | 低-中 |
| 256 层扩展 | 受 array 上限 | 好 | 好 | 中 |
| 2K/异构分辨率 | 不支持 | 支持 | 支持 | 部分 |
| 改动面 | 小 | 很大 | 中 | 中大 |
| 16km 流式 | 不适合 | 适合 | 适合 | 适合（组粒度粗） |

**推荐：方案 C 作为目标架构；方案 A 保留为“热层/小场景”快速路径。**

理由：

- C 是唯一在“不引入 bindless、不改 descriptor 模型”的前提下，
  能同时满足 256 层、1 GiB 预算和按页流式的方案；
- A 的采样路径最短，适合 ≤32/≤64 层的近景热层或小场景，
  也可以作为 C 的性能基线和回退路径；
- B 的收益（去掉页表间接）在当前引擎里换不来相应的改动成本；
- D 的数组页可以作为 C 的 atlas 组织方式，但不作为独立架构。

---

## 4. 推荐架构的数据格式

### 4.1 总览

```text
TR.layers（每层作者数据）
  -> 全局 layer catalog（256 项，稳定 layerId）
       - height/slope/noise/uvScale/uvRotation（沿用现有字段）
       - pagesPerSide、虚拟页基址、平均色、flags
  -> tile 候选层表（每 tile ≤32 个 layerId，CPU 侧）
  -> 每 texel ID/权重（现有 RGBA8 atlas，top-4）
  -> shader：
       top-4 ID
         -> catalog[layerId]
         -> layer page table[virtualPage(layerId, mip, page)]
         -> atlas[physicalPage]
```

层源纹理与 terrain tile **解耦**：层纹理是“每层一份、可平铺”的资产，
tile 只决定“哪些层可能出现在这里”。因此：

- 物理页按 `(layerId, mip, page)` 全局共享；
- 同一 TR 多处摆放不会重复烘焙/占页；
- 每 tile 32 候选只影响驻留优先级，不影响页表结构。

### 4.2 全局 layer catalog

建议把现有 `TerrainLayerTable` 从 256×2 扩成 **256×4 RGBA32F**
（仍是 `sampler2D`，不改 schema）：

| row | 通道 | 含义 |
| --- | --- | --- |
| 0 | R/G/B/A | heightMin / heightMax / slopeMin / slopeMax |
| 1 | R/G/B/A | noise / blendEdge / uvScale / uvRotation(rad) |
| 2 | R/G/B/A | pagesPerSide / pageTableBase / sourceSet / flags |
| 3 | R/G/B/A | avgColor.r / avgColor.g / avgColor.b / reserved |

- `layerId` 仍等于 TR.layers 的下标（0..255），稳定不变；
  权重图、候选表、编辑器都引用 ID，不引用名字。
- `pagesPerSide`：该层源分辨率 / 页分辨率（1K/128 → 8；2K/128 → 16）。
- `pageTableBase`：该层在虚拟页表里的起始项；
  `virtualPage = pageTableBase + mipBase + pageY * pagesPerSide(mip) + pageX`。
  也可以按 mip 分表，避免 `mipBase` 计算；见 §4.4。
- `avgColor`：缺页/低 mip 回退色，避免直接跳到中性灰。
- `flags`：sRGB/linear、是否有 alpha、是否允许 2K、biome 过滤位等。

> 为什么仍然用 `RGBA32F` 纹理而不是 SSBO：当前材质 schema 没有 storage buffer，
> 也没有整数采样器；`RGBA32F` 是唯一既能表达 22k 量级页号/基址、
> 又不需要改 `MaterialDescriptorSchema` 的载体。float32 的 24-bit 尾数
> 足以精确表示 0..87,296 的整数。

### 4.3 tile 候选层表（32 层）如何映射到纹理页

候选表本身**不需要 GPU 常驻**：它只用于 CPU 侧的驻留/预取决策。

```text
struct TerrainTileLayerCandidates
{
    uint32 tileX, tileZ;
    uint32 count;              // <= 32
    uint8  layerIds[32];
    uint8  priority[32];       // 可选：按面积/坡度/相机距离
};
```

- 来源：`TerrainWeightMapBuilder::BuildTileCandidates`
  已经按 16×16 高度/坡度采样筛出候选层；扩到 32 层时沿用同一规则。
- 用途：
  1. 决定 tile 的层驻留集合（32 → 实际可见 top-4 可能更少）；
  2. 预取下一圈 tile 的候选层；
  3. 给 `TerrainLayerPageManager` 设 priority。
- 如果将来需要 GPU 侧直接按候选表裁剪，才需要把候选表做成 buffer；
  当前 shader 仍然只读 top-4 ID/权重，不遍历候选表。

### 4.4 层页表与物理页

**虚拟页编号**

每个 `(layerId, mip, pageX, pageY)` 对应一个虚拟页。按 mip 分表时：

```text
virtualPage(mip) = layerId * pagesPerMip(mip) + pageY * pagesPerSide(mip) + pageX
```

128² 页、1K 层的页数：

| mip | 层尺寸 | pagesPerSide | pages |
| --- | --- | --- | --- |
| 0 | 1024² | 8 | 64 |
| 1 | 512² | 4 | 16 |
| 2 | 256² | 2 | 4 |
| 3 | 128² | 1 | 1 |
| 4.. | 64² 及以下 | — | 打包进 1 个 tail 页（可选） |

不含 tail 时每层 85 页；256 层 = 21,760 项。2K 层为 341 页/层，
256 层 = 87,296 项。按 16 B/项（RGBA32F）约 340 KiB / 1.33 MiB，
仍然很小。

**页表 texel**

推荐**每个 mip 一张** `layerPageTable[mip]`（RGBA32F，`nearest`、`ClampToEdge`、
`generateMipmaps = false`）；mip0..3 各一张，mip4+ 可选 tail 表：

| 通道 | 含义 |
| --- | --- |
| R | 物理页号（0..N-1，float 精确表示） |
| G | 该物理页的有效 mip（0..3；tail 用 4） |
| B | valid / fallback / pinned 标志 |
| A | 预留：priority 或版本 |

物理页号三元组共分配：

```text
physicalPage k:
  albedo atlas page k
  normal atlas page k
  pbr atlas page k
```

因此一次页表 fetch 同时得到三张图的物理页，不需要三张独立页表。

**atlas 组织**

```text
layerAlbedoAtlas = sampler2DArray
  slice 0..S-1，每 slice 4096² BC7 sRGB
  physicalPage -> (slice, tileX, tileY)
layerNormalAtlas = sampler2DArray（BC5 linear，同布局）
layerPbrAtlas    = sampler2DArray（BC7 linear，同布局）
```

- 用 `sampler2DArray` 而不是单张 8192²/16384²，是为了把
  `maxImageDimension2D` 保持在规范保证的 4096；
  容量靠 slice 数扩展（`maxImageArrayLayers` 保证 256，远大于所需的 4/8）。
- `physicalPage = slice * pagesPerSlice + tileY * tilesPerSide + tileX`。
- 每张 atlas **不生成 mip**；每个 mip 是独立物理页，避免页间渗色。

### 4.5 shader 采样路径（伪代码）

保留现有 top-4 ID/权重读取，替换“直接用 layerId 采样数组”这一段：

```glsl
vec4 page = texelFetch(pageTable, coverageTile, 0);   // 现有覆盖页表
...
vec4 topWeights = texture(layerWeights, atlasUv);
ivec4 topIds = ivec4(round(texture(layerIds, atlasUv).rgba * 255.0));

for (int slot = 0; slot < 4; ++slot)
{
    int layer = topIds[slot];
    vec4 meta = FetchLayerCatalog(layer);             // 256x4 RGBA32F
    float pagesPerSide = meta.z;                      // row2.r

    // 1. 层 UV（沿用现有 uvScale/uvRotation）
    vec2 layerUv = Rotate(meta.w) * (context.texCoord * u_uvScale * meta.y);
    vec2 duv = dFdx(layerUv), dvv = dFdy(layerUv);

    // 2. 选 mip（按层自身纹素密度，不用 atlas UV）
    float lod = ComputeLod(layerUv, duv, dvv, layerResolution);
    int mip = clamp(int(round(lod)), 0, MaxLayerMip);

    // 3. 虚拟页 + 页表（每个 mip 一张表时）
    vec2 pageUv = fract(layerUv);
    ivec2 pageXY = ivec2(floor(pageUv * pagesPerMip[mip]));
    int virtualPage = layer * pagesPerMip[mip] +
                      pageXY.y * pagesPerSide[mip] + pageXY.x;
    vec4 entry = texelFetch(layerPageTable[mip], PageTableCoord(virtualPage), 0);

    // 4. 物理页 -> atlas 局部 UV
    if (entry.b < 0.5) { /* fallback: 低 mip 或 avgColor */ }
    int physical = int(entry.r + 0.5);
    int slice = physical / PagesPerSlice;
    ivec2 tile = ivec2(physical % PagesPerSlice % TilesPerSide,
                       physical % PagesPerSlice / TilesPerSide);
    vec2 local = clamp(pageUv, 0.5 / u_layerPageResolution,
                       1.0 - 0.5 / u_layerPageResolution);
    vec2 atlasUv = (vec2(tile) * u_layerPageResolution +
                    local * u_layerPageResolution) / 4096.0;

    // 5. textureGrad：导数是层 UV 的 1/pagesPerSide，atlas 只有 mip0
    vec3 uv3 = vec3(atlasUv, float(slice));
    albedo += textureGrad(layerAlbedoAtlas, uv3, duv / pagesPerSide, dvv / pagesPerSide)
              * topWeights[slot];
    ...
}
```

关键点：

- **mip 选择用层 UV 的导数**，不是 atlas UV 的导数；
  atlas 自身不生成 mip，`textureGrad` 只在页内做双线性过滤。
- `pageUv = fract(layerUv)`：层纹理是平铺的，虚拟页在层 UV 空间循环；
  页边用半 texel inset，避免跨页采样。
- 各向异性要限制（或关闭），否则 `textureGrad` 的高各向异性 tap
  会越过页边界；若需要各向异性，应在页内做多次 tap 或给页加 1 texel border。
- `pageXY` 必须 clamp 到 `[0, pagesPerSide-1]`，
  处理 `fract(1.0) == 0` 的边界（与现有覆盖页表的 `step` 规则一致）。

### 4.6 页分辨率、mip、压缩与 sRGB 规则

**页分辨率**

- 默认 128²。BC7/BC5 下每页 16 KiB，三元组 48 KiB；
  4096² slice 正好 32×32 = 1024 页。
- 256² 页适合：
  - 2K hero 层（2048/256 = 8，每层 64+16+4+1 = 85 页，与 1K/128 相同）；
  - 大面积非平铺 macro 层（减少页表项和 upload 次数）。
- 64² 页适合远景低 mip 或内存极紧的档位，但不建议作为主路径
  （页数 ×4，页表项 ×4）。

**mip**

- 按 mip 分页/分表：mip0..3 各一张页表，mip4+ 可选 tail 页。
- 远处降到 mip2/3 或 tail；页数不足时优先保留低 mip。
- 层页的 mip 链不与覆盖 atlas 的 mip 混用；覆盖 atlas 仍可继续无 mip。

**压缩**

| 图 | 格式 | 颜色空间 | 说明 |
| --- | --- | --- | --- |
| albedo | BC7 | sRGB | `VK_FORMAT_BC7_SRGB_BLOCK` |
| normal | BC5 | linear | 2 通道 XY，shader 重建 Z |
| pbr | BC7 | linear | R=roughness、G=metallic、B=AO、A=height |

规则：

- 同一 atlas 只能有一种格式/颜色空间，所以三张 atlas 必须分开；
  不能把 sRGB albedo 和 linear pbr 塞进同一个数组。
- 页尺寸必须是 4 的倍数（BC block 对齐）；128/256 都满足。
- 压缩页的上传按 block 行跨度处理；`bufferRowLength` 对 block-compressed
  格式按块行解释，不能沿用 RGBA8 的 texel 行跨度。
- 若 BC 不可用，退化为 RGBA8：每页 64 KiB、三元组 192 KiB，
  同样预算只能容纳 1/4 的页（见 §2.2 注）。

### 4.7 是否保留 `sampler2DArray` 作为快速路径

**保留，但作为“小层集/热层”路径，不作为唯一路径。**

```text
if (u_terrainLayerMode == ARRAY_FAST && layer < u_hotLayerCount)
    sample layerAlbedo/Normal/Pbr (sampler2DArray)
else
    sample layerAlbedoAtlas/... via page table
```

- 小场景/验证场景（≤32/≤64 层）继续走数组，保持当前画面与性能；
- 256 层大世界走 VT；
- 两条路径共用同一份 ID/权重、catalog 字段和 top-4 混合代码，
  只替换“层纹理采样”这一段。
- 材质用宏或参数切换，便于 A/B 截图与性能对比；
  不要让两套路径在同一个 draw 里混合（会增加分支与寄存器压力）。

---

## 5. 流式与驻留

### 5.1 与现有 `TerrainPageManager` 的关系

现有 `TerrainPageManager` 管的是**覆盖页**（ID/weight），键是
`placementId + tile`；层纹理页的键是 `layerId + mip + page`，
与 placement 无关。两者不能共用同一张页表，但应共用同一套
**物理页分配 / 退休 / quarantine** 机制。

推荐结构：

```text
TerrainStreamingState
  ├─ TerrainPageManager        coverage domain（现有）
  │    page table 32x32, atlas 4096² ID/weight
  ├─ TerrainLayerPageManager   layer domain（新增）
  │    page tables per mip, atlas slices 4096² x N
  └─ TerrainVirtualPageAllocator（新增，或抽出的共享基类）
       物理槽位分配、free list、lastSeenEpoch、pin 计数、
       quarantine、统计
```

理由：

- 两个 domain 的页大小、格式、atlas 数量、键空间都不同，
  硬塞进一个 `TerrainPageManager` 会让现有覆盖页逻辑变复杂；
- 但物理槽位/epoch/quarantine 的语义完全一样，
  抽成共享 allocator 可以避免两套 LRU 行为和两套统计。
- `TerrainStreamingState` 已经是 World-local 并随
  `WorldLocalResourcePackage` 退休；层页管理器应挂在同一 state 上，
  不要新建第二个生命周期。

### 5.2 驻留策略

**初始阶段（L4a）：层级驻留 + 预取**

- 用 CPU 相机/视锥 + tile 候选表决定“哪些层需要驻留”；
- 对每个候选层，按距离请求 mip 0..3 中合适的一级（不是整链）；
- 近景 tile 的 32 候选优先，远景只保留 mip2/3；
- 不依赖逐像素层 feedback，先验证页表/atlas 正确性与预算。

> 实施时的取舍（2026-10-30）：实际 L4a 没有新增 CPU 视锥/相机距离输入，而是直接复用
> P2/P3 已经落地的 128×128 覆盖页 feedback（页坐标 + priority），再由覆盖页的
> top-4 ID/权重推导可见层。这样不新增 pass/RT，也不会出现“同一地形被两套可见性
> 判断分别驱动”的第二份真值；代价是 mip 只能按覆盖页 priority 近似，精确到
> 层号 + 层页 + mip 的 feedback 仍是 L4b。

**进阶（L4b）：feedback 驱动的页级驻留**

- 当前 `terrainFeedback` 只能容纳覆盖页坐标（5+5 bit），
  装不下层号 + 层页 + mip；
- 需要格式 v2 或第二个 RT：
  - 方案 1：`terrainLayerFeedback` 256×256 RGBA8，
    每个屏幕像素对应 2×2 子 texel，每个子 texel 表示一个 top-4 slot：
    `R=layerId`、`G=pageIndex(0..63)`、`B=mip`、`A=valid|tag`；
  - 方案 2：128×128 RGBA16UI ×2，容纳 16-bit 层号/页号；
  - 无论哪种，都要与现有 `terrainFeedback` 的 frameTag/ring 规则一致。
- CPU 汇总后对 `TerrainLayerPageManager` 调
  `RequestPageAtEpoch(...)` / `MarkPageUsedAtEpoch(...)`；
  淘汰沿用 `lastSeenEpoch + EvictionHysteresisFrames <= completedEpoch`
  安全窗（现有 `EvictionHysteresisFrames = 2`）。
- 层页的 priority 可由“屏幕覆盖面积 × 候选权重 × 距离”估算；
  近景/大面积层优先。

### 5.3 每帧上传预算

沿用 P1 的 `BuildUploadBatch` / `CommitUploadBatch` / `AbortUploadBatch`
模式，但层页 batch 要同时覆盖页表与三张 atlas：

```text
每帧预算示例（128² BC，16 KiB/页/图）：
  32 个三元组 = 32 × 48 KiB = 1.5 MiB/帧
  60 fps → 90 MiB/s
```

- 页表项很小（16 B/项），可以每帧全量更新 dirty 部分；
- atlas 页按物理槽位升序打包，保证“页表不会先指向尚未上传的页”；
- 单帧超预算时截断，剩余 dirty 留到下一帧（P1 已实现该截断语义）；
- staging 用帧 ring（2 帧，`MAX_FRAMES_IN_FLIGHT = 2`），
  不要每帧创建/销毁 buffer；
- 首帧零初始化 atlas 建议用 `vkCmdClearColorImage`，
  不要像现在这样在 CPU 构造 4096² 零图再上传（当前覆盖 atlas 仍是后者）。

### 5.4 fallback 与淡入

缺页时不能直接采样 layer 0（现有 `mf_terrainLayerBlend.glsl`
在覆盖页 miss 时已经改成中性表面，层页 miss 也要同样处理）：

1. **优先低 mip**：如果 mip0 页缺失但 mip2 存在，采样 mip2；
2. **其次平均色**：catalog 的 `avgColor` 或 1×1 fallback 页；
3. **最后中性表面**：baseColor 0.18、roughness 0.9、几何法线；
4. **淡入**：页到达后 2–4 帧内按页 id 抖动/线性混合，
   避免整块“啪”地出现；可以用 `frameTag` 或页版本做 dither。

### 5.5 物理页复用与 in-flight 安全

这是接 streaming 前必须补的 GPU 侧约束：

- CPU 安全窗（`lastSeenEpoch + 2 <= completedEpoch`）只减少抖动，
  **不能替代 GPU barrier**；
- 物理页被淘汰后，不能立刻分给新虚拟页：
  旧帧可能仍在采样该 atlas 页；
- 推荐 **quarantine（隔离队列）**：
  1. 淘汰时把物理页放进 `pendingRelease`，记录淘汰 epoch；
  2. 只有 `completedEpoch >= evictEpoch + framesInFlight + 1`
     才回到 free list；
  3. 重新分配给新页前，在帧首插入
     `TransferDst -> ShaderReadOnly` 或对应的 availability barrier；
  4. 清零/覆盖与旧帧读取之间必须有执行依赖
     （event/barrier 或 queue 级等待），不能只靠 CPU 认为“已经过了两帧”。
- 与 P3 的关系：P3 处理覆盖页的同一问题；
  层页 quarantine 应复用 P3 的 allocator/epoch 实现，不要另起一套。

---

## 6. 分步实施

每一步都可以独立分派；括号里是依赖关系。所有模块测试走现有 root
`vulkanlearn_add_gtest(...)`，不新增 runtime test flag；
冒烟截图是手工/脚本采集，不是 runtime test。

### L0：BC7/BC5 与 KTX2/DDS 压缩纹理管线（无依赖）

**目标**：让压缩层纹理能进 GPU，且预算档位成立。

**触碰文件**

- `source/resource/image/hostImage.h/.cpp`
- `source/resource/image/textureIO.h/.cpp`
- `source/resource/device/deviceTextureFactory.h/.cpp`
- `source/texture.h/.cpp`
- `source/textureAssetLoader.h/.cpp`
- `tool/texture-asset-tests/*`
- 实现落地后更新 `documents/contracts/texture-asset-json-v1.md`

**内容**

- `HostImage::PixelFormat` 增加 BC7/BC5（或增加 block-compressed 描述，
  保留 `rowStrideBytes` 的 block 行语义）；
- `DeviceTextureFactory::ToVkFormat` 增加
  `eBc7SrgbBlock` / `eBc7UnormBlock` / `eBc5UnormBlock`；
- KTX2/DDS 解码：至少支持单层 + 数组层 + 预生成 mip；
  不做 CPU mip 生成（BC 的 mip 必须离线编码）；
- 上传按 block 行跨度拆层/拆 mip，数组层连续；
- 纹理 JSON 增加 `compression` / `format` 字段（或从容器自动识别）。

**GoogleTest**

- BC7/BC5 fixture 解码：尺寸、block 数、行跨度、层数；
- sRGB vs linear 选择正确的 `vk::Format`；
- 压缩数组逐层上传后回读（headless device）匹配源块；
- 拒绝非 4 倍数尺寸、拒绝把 BC 当 RGBA8 采样。

**冒烟**

- 用一张 BC7 层数组替换现有 32×32 BMP 数组，
  在 `SC_terrain_b32.json` 截图中颜色/法线无可见回归；
- 记录单层显存占用，确认与 §1.3 的 BC 列一致。

**依赖**：无。L1 可以先用 RGBA8 并行开始。

**状态（2026-10-30，codex/terrain-openworld）**

- 已实现：`HostImage` 增加 `BC7_UNORM` / `BC7_SRGB` / `BC5_UNORM`
  （4x4、16 字节/块、layer-major 且层内 mip-major，`GetMipRowStride` /
  `GetMipRowCount` / `GetMipOffset` 明确块语义）；`TextureIO` 从 DDS
  （legacy `ATI2`/`BC5U`、DX10 BC7/BC5）与 KTX2（BC5/BC7、
  `supercompressionScheme = 0`）解码数组层与 mip 链，并把 KTX2 的
  level/layer 顺序重排成 CPU 布局；cubemap、volume、未知格式、
  BasisLZ/Zstd/ZLIB、截断数据与不一致 DFD 全部显式拒绝；
  `DeviceTextureFactory` 映射 `eBc7SrgbBlock` / `eBc7UnormBlock` /
  `eBc5UnormBlock`，按 (mip, layer) 录制 region，BC 禁止设备端 mip；
  纹理 JSON 不需要新字段，`colorSpace` 仍决定 BC7 的 sRGB/UNORM。
- 未实现（有意）：BC 编码器、Basis/Zstd 转码、运行时 BC mip 生成、
  BC 运行时行翻转（源必须离线按引擎 V 方向编码）。
- 证据：`texture_asset_tests` 58/58；headless BC7 逐 mip/逐层字节回读；
  `Generated/Validation/TerrainB32/SC_terrain_b32_bc7.json` 把 32 层 albedo
  换成 BC7 DDS 后与 BMP 基线相比，排除运行时 UI 文字区域后最大通道差 2、
  平均 0.14。冒烟资产在资源根 `Generated/Validation/TerrainB32/Bc7Smoke/`，
  不在 git 仓库内。

### L1：Layer catalog v2 + CPU 层页管理器（依赖 L0 取得目标格式，可先用 RGBA8）

**目标**：纯 CPU 侧把“层 → 页 → 物理槽位”的驻留/淘汰语义做对。

**触碰文件**

- `source/terrain/terrainLayerTable.h/.cpp`
- `source/terrain/terrainAssetTypes.h`（层可选字段：源分辨率/压缩/flags）
- 新增 `source/terrain/terrainLayerPageManager.h/.cpp`
- `tool/terrain-tests/terrainTests.cpp`
- 实现落地后更新 `documents/contracts/terrain.md`（不在本步）

**内容**

- catalog 从 256×2 扩到 256×4；写入 `pagesPerSide`/`pageTableBase`/
  `avgColor`/`flags`；
- `TerrainLayerPageManager`：
  - `PageKey { layerId, mip, pageX, pageY }`；
  - 虚拟页 → 物理页分配；1K/2K 层自动计算 `pagesPerSide`；
  - resident set、LRU/priority、refcount、pin 计数；
  - `lastSeenEpoch` + `EvictionHysteresisFrames` 安全窗；
  - `BuildUploadBatch/Commit/Abort`，与 P1 语义一致；
  - 统计：request/hit/miss/eviction/deferred。
- 层页内容来源：离线烘焙的 KTX2/DDS 页，或从源图 CPU 切页（先做后者）。

**GoogleTest**

- catalog 往返：256 层、未使用层填零、float 页号精确；
- 512/1K/2K 的 `pagesPerSide`/页数/虚拟页编号；
- 物理页分配/释放/复用；`fallback` 不占普通页；
- 安全窗内拒绝淘汰、窗外成功淘汰；
- 重叠 batch 的 pin 计数、Commit 只清未变 revision；
- 256 层 ID 越界/257 层拒绝。

**冒烟**

- 不接 GPU：用 CPU 断言验证“整层 256 层可按预算驻留约 48/96 层”；
- 打印每层页数、总项数、catalog 字节数。

**依赖**：L0（目标格式）；RGBA8 可以先跑通语义。

**状态（2026-10-30，codex/terrain-openworld）**

- 已实现：`TerrainLayerDesc` 增加 `sourceResolution`（512/1024/2048，默认 1024）、
  `avgColor`、`sourceSet`、`flags`，TR validator/loader 接受并校验，旧 TR 默认值不变；
  `TerrainLayerTable::BuildImage` 改为 256×4 RGBA32F，
  row2=`pagesPerSide/pageTableBase/sourceSet/flags`、
  row3=`avgColor.rgb/reserved`，未使用层全零，并有 `ReadLayer` CPU accessor；
  shader 只把 `TerrainLayerExtra` 的 row1 采样 v 从 0.75 改为 0.375，
  其余采样路径未动。
- 已实现：`source/terrain/terrainLayerPageManager.h/.cpp` 提供
  `(layerId,mip,pageX,pageY)` → 物理页的 CPU 驻留/淘汰/LRU/priority/refcount/
  pin 计数/epoch 安全窗/quarantine/上传批次；物理页池默认 1024、
  fallback 固定最后一个槽位；mip 页数 512=21、1K=85、2K=341（含 mip4 尾页）；
  虚拟页号按设计 §4.4 的 per-mip 公式，并把 stride 取为该 mip 下所有层的最大
  页数，混合 512/1K/2K 时不别名；另有 catalog `pageTableBase` 对应的扁平编号。
- 未实现（有意）：GPU 层 atlas/页表、从源图切页/上传、shader 层 VT 采样、
  L4 的 feedback 驱动层页驻留；`TerrainLayerPageManager` 目前没有生产调用者。
- 证据：`terrain_tests` 104/104；`ctest -R "^Terrain"` 111/111；
  `SC_terrain_b32.json` 与 `SC_terrain_m3.json` 截图与改动前逐字节一致
  （SHA-256 `1D30D8…3F54` / `0E5D5F…BAAE`），stderr 为空且无 Vulkan validation error。

### L2：GPU atlas / 页表 / catalog 资源与上传（依赖 L1、P1）

**目标**：把层页真正送进 GPU，但还不改变 shader 的层采样路径。

**触碰文件**

- `source/render/resource/rendererTerrainLoader.h/.cpp`
- `source/render/resource/rendererResourceLoadCoordinator.cpp`
- `source/render/resource/rendererResourceCache.h/.cpp`
- `source/commonFunction.h`（`vkCmdClearColorImage` 零初始化、批量 region）
- `source/render/rhi/vulkan/*`、`source/render/backend/*`（如需新 pass-through）
- `tool/renderer-backend-tests/*`、`tool/terrain-tests/terrainTests.cpp`

**内容**

- `TerrainStreamingState` 增加
  `TerrainLayerPageManager layerPageManager;`
  `TerrainLayerGpuResources layerPages;`
  `layerLayerTable`/`layerPageTable` 纹理；
- 创建 atlas：`S` 个 4096² slice × 3 图，零初始化用 clear；
- 创建 catalog（256×4 RGBA32F）与页表（每 mip 一张 RGBA32F）；
- `BuildLayerUploadBatch` + 单 command buffer 批量 copy；
  失败 Abort 保留 dirty；
- 初始内容版本复用 `pendingUploadRevision`/`uploadedRevision`；
- MI 预览/扩展候选继续共享 active state（不要 reset）。

**GoogleTest**

- clear 后 atlas 为 0；单批 copy 覆盖页表 + 三张 atlas；
- `maxAtlasPages` 截断后页表不指向未上传页；
- state revision 幂等、失败保留 dirty；
- `TerrainStreamingState` 拷入/拷出 package 后层页表内容不变。

**冒烟**

- `SC_terrain_b32.json` 仍走旧数组路径，截图与基线一致；
- 控制台打印层页统计（resident/physical/free），确认预算；
- 无 Vulkan validation error。

**依赖**：L1、P1（批量上传/record-only 已落地）。

**状态（2026-10-30，codex/terrain-openworld）**

- 已实现：`TerrainStreamingState` 持有 `TerrainLayerPageManager layerPageManager`、
  `TerrainLayerSourceTextures` 与扩展后的 `TerrainLayerGpuResources`
  （catalog + mip0..4 的 RGBA32F 页表 + 三张 4096²/slice、无 mip 的
  `sampler2DArray` atlas）；页表宽度上限 4096，1K mip0 为 4096×4。
- 已实现：`Texture::CreateEmptyArray` 与 backend `RecordClearColorImage` /
  `RecordCopyImageRegions` / `CopyImageRegions`、CommonFunction 的
  `ShaderReadOnly ↔ TransferSrc/TransferDst` 往返。非压缩 atlas 用
  `vkCmdClearColorImage`；BC atlas 用一个 16 KiB 零页 staging 反复 copy
  （`vkCmdClearColorImage` 不允许压缩格式），staging 活到 submit 之后。
- 已实现：`BuildUploadBatch` 增加 `pageTableUpdates`（有效映射 + 淘汰失效项），
  另有 `pageTableRevision`/`pageTableUploadedRevision`；`maxPages` 截断时页表项
  与 atlas 页一起延后，Commit 只清 revision 未变的项，Abort 保留 dirty。
- 已实现：层源纹理从 MI 的 `layerAlbedo/Normal/Pbr` 捕获，要求同一 World
  共享同一组 shared_ptr；切页用 GPU→GPU `vkCmdCopyImage`（源 mip/layer →
  atlas slice/tile），按源 mip 真实尺寸裁剪 extent（现有验证资产只有 32×32）。
- 已实现加载期测试集合：每层尾页 `(0,0)`，请求前设置 epoch 上下文；
  B32 上传 32 页，BC7 场景 report `Bc7SrgbBlock`。控制台为
  `terrainlayerstats`，不是 runtime flag。
- L2 当时未实现（后续 L3 已补 shader 采样与 sourceResolution 校验）：
  shader 采样层 atlas、层页 feedback/预取（L4）、逐帧上传 ring、
  不同 `sourceSet` 的多组源纹理、同一 TR 多摆放的页内容去重。
- 证据：`terrain_tests` 112/112、`vl_renderer_backend_tests` 61/61、
  `ctest -R "^Terrain"` 120/120；`SC_terrain_b32.json` /
  `SC_terrain_m3.json` / `SC_terrain_b32_bc7.json` 截图与 L1 基线逐字节一致，
  stderr 为空且无 Vulkan validation error。

### L3：shader VT 路径 + 材质 schema（依赖 L2）

**目标**：`M_terrain` 能按 `top-4 ID → catalog → 页表 → atlas` 采样，
并保留旧数组快速路径。

**触碰文件**

- `shader/glsl/M_terrain.json`
- `shader/glsl/M_terrain.surface.glsl` / `M_terrain.vertex.glsl`
- `shader/glsl/materialFunction/mf_terrainLayerBlend.glsl`
- 新增 `shader/glsl/materialFunction/mf_terrainLayerVT.glsl`
- `source/material/materialDescriptorSchema.cpp`
- `source/material/validation/materialAssetValidator.cpp`
- `tool/material-schema-tests/*`、`tool/terrain-tests/terrainTests.cpp`

**内容**

- `M_terrain.json` 增加：
  `layerCatalog`、`layerPageTable0..3`（或一张按 mip 索引的页表）、
  `layerAlbedoAtlas`、`layerNormalAtlas`、`layerPbrAtlas`；
  旧 `layerAlbedo/Normal/Pbr` 保留。
- 宏/参数 `VL_TERRAIN_LAYER_VT` 或 `u_terrainLayerMode` 选择路径；
- VT shader：
  - 层 UV 导数 → mip；
  - 虚拟页 → 页表 → 物理页；
  - `textureGrad` + 页内 inset；
  - 缺页 fallback/平均色；
  - debug view：层号/页号/mip/权重。
- 若页表必须用整数格式，先扩展 `MaterialDescriptorSchema` 的白名单
  与生成的 GLSL 类型；否则坚持 RGBA32F 浮点页表。

**GoogleTest**

- 材质 schema：新纹理 binding 顺序、`default` 校验、反射匹配；
- MI 缺纹理时按现有规则失败；
- CPU 参考实现：给定 catalog/页表/UV，验证虚拟页与 atlas 坐标；
- 边界：`fract` 在 `u==1.0`、页边 inset、mip 选择。

**冒烟**

- 同一场景分别用数组路径/VT 路径截图，允许极小差异（过滤/压缩），
  记录差异像素比例；
- debug view 1/2/3 在 VT 路径仍正确；
- 无 validation error、无页边黑线。

**依赖**：L2。

**状态（2026-10-30，codex/terrain-openworld）**

- 已实现：`M_terrain.json` 保留旧 `layerAlbedo/Normal/Pbr`，新增
  `layerPageTable0..4` 与三张 `layer*Atlas`；`u_terrainLayerMode` 默认 0，
  新增 `u_layerPageResolution/u_layerAtlasResolution/u_layerPagesPerAtlasSlice/
  u_layerTilesPerAtlasSide/u_layerMaxMip`。`MaterialDescriptorSchema` 无需扩展：
  现有 `sampler2D`/`sampler2DArray` + float 参数足够。
- 已实现：`materialFunction/mf_terrainLayerVT.glsl` 按层 UV 导数选 mip，
  `fract(layerUv)` 后按 `pagesPerSide(mip)` 切页，页表总量/256 还原 per-layer
  stride，虚拟页 → 页表 texel → 物理页 → slice/tile/半 texel atlas UV；
  固定 `textureLod(...,0)`，三张 atlas 共用物理页。无效页表项使用 catalog
  row3 `avgColor` + roughness 0.9 / AO 1，不回退 layer 0。
- 已实现：旧数组路径放在 `u_terrainLayerMode<=0.5` 分支；debug 0..3 语义不变，
  新增 4=VT 物理页、5=VT mip（mode 0 下为 0）。
- 已实现：`RendererTerrainLoader` 在首个 layer-table MI 载入后创建 atlas 并绑定
  页表/atlas 与尺寸参数，不覆盖作者的 `u_terrainLayerMode`。mode 1 请求但
  资源不可用/源尺寸不匹配时加载期失败。
- 已实现：`sourceResolution` 与真实源图边长一致性校验。mode 0 不一致只 warning；
  mode 1 直接报错。新增 512²、32 层验证资产与 mode 0/1 场景。
- 已实现：mode 1 加载期全量层页预取（`RequestAllTerrainLayerPages`），
  32 层 512² 为 672 页；这是 L3 验证路径，不是 L4 streaming 策略。
- 证据：`terrain_tests` 119/119、`material_schema_tests` 11/11、
  `vl_renderer_backend_tests` 61/61；mode 0 三张截图与 L2 基线逐字节一致；
  mode 1 672/672 页上传、atlas 201,326,592 B、与 mode 0 平均通道差 2.40/255，
  stderr 为空且无 Vulkan validation error。
- 未实现（有意）：L4 的层页 feedback/预取驱动驻留、逐帧上传 ring、
  低分辨率 fallback/淡入、同一 TR 多摆放的页内容去重、不同 `sourceSet` 的多组源纹理。

### L4：feedback/预取驱动驻留、预算与 quarantine（依赖 L3、P3）

**目标**：真正按帧流式，16km 走查不超预算、不黑洞。

**触碰文件**

- `source/renderSystem.cpp`
- `source/terrain/terrainPageManager.h/.cpp`（抽共享 allocator/quarantine）
- `source/terrain/terrainFeedback.h/.cpp`
- `shader/glsl/pass/terrainFeedback.*`、`M_terrainFeedback.json`（若加层 feedback）
- `config/renderGraphConfig.json`（若新增第二个 feedback RT）
- `source/render/resource/rendererTerrainLoader.*`
- `tool/terrain-tests/terrainTests.cpp`、`tool/renderer-backend-tests/*`

**内容**

- L4a：CPU 视锥 + tile 候选 + 距离 → 请求层页/mip；
- L4b：层 feedback（见 §5.2）驱动 `RequestPageAtEpoch`/`MarkPageUsedAtEpoch`；
- 每帧上传预算（§5.3）、priority、LRU；
- 物理页 quarantine + barrier（§5.5）；
- fallback 低 mip/平均色 + 淡入；
- 控制台 `terrainlayerstats`（不是 runtime test flag）输出
  resident/physical/free/request/hit/miss/evict/uploadBytes。

**GoogleTest**

- feedback 编解码（layerId/page/mip/tag）与过期丢弃；
- request/hit/miss/evict 计数；
- 安全窗 + pin + quarantine 的槽位复用；
- 单帧预算截断后下一帧收敛；
- 256 层、2K 层的页表索引边界。

**冒烟**

- `SC_terrain_b32_two.json` + 16km 场景，三个相机位置各跑一次：
  近景/中景/远景；
- `resident` 在预算内波动、`evictionCount > 0`、无整块缺失；
- 快速移动相机时无黑洞/明显闪烁；
- stderr 0 字节，Vulkan validation 无 layout/hazard 错误；
- 记录上传带宽与帧时间，确认无同步卡顿。

**依赖**：L3、P3（覆盖页的 feedback 驱动驻留）。
在 P3 完成前，L4a 可以用 CPU 预取先行验证，但不能宣称“流式闭环完成”。

**L4a 状态（2026-10-30，已完成）**

- 已实现：`source/terrain/terrainLayerVisibility.*` 把 P2/P3 的覆盖页 feedback
  推导成“可见层 → mip → 该 mip 全部页”，不新增 pass/RT，也不新增 runtime flag。
  覆盖页 top-4 来自 CPU atlas 镜像或 `TerrainWeightMapBuilder::BuildPage` 重建
  （每帧限流并缓存）；priority 阶梯 `>=192/>=128/>=64` 映射 mip0/1/2/3，
  再按层 mip 链与源数组 mip 数 clamp。
- 已实现：跨帧 `pendingLayerPageRequests`，按 priority 降序 + deferred 轮转，
  每帧最多 16 个 `RequestPageAtEpoch`；可见层的所有已驻留 mip 每帧续命，
  离开视野后先过 GPU 安全窗，再按 idle（180 epoch）与 budget 淘汰。
- 已实现：`TerrainLayerUploadGpuRing`（槽数 = frame fence 数、常驻映射、只打包
  16 B 页表项）与 `TerrainLayerUploadTransaction`；每帧 scene pass 之前
  `BuildUploadBatch(8)` + `RecordTerrainLayerPageUploads`（页表 copy + 三张 atlas
  的 GPU→GPU copy）录制进主帧，`SubmitFrame` 成功后才 Commit。
- 已实现：mode 1 加载期只请求每层尾页；`terrainlayer stats|capacity N|preload`
  提供观测、预算与全量对照。`terrainlayerstats` 增加 visibleLayers / 本帧请求/上传 /
  pending / mip 分布。
- 已实现：shader 在选中 mip 无效时回退到更粗 mip，避免收敛期间整页 avgColor。
- 证据：`terrain_tests` 125/125、`vl_renderer_backend_tests` 61/61、
  `material_schema_tests` 11/11、`texture_asset_tests` 58/58；
  mode 1 稳定态 `resident≈248–268/512`、`pending=0`、`evictions` 仅启动期少量；
  `terrainlayer preload` 后 `resident=672/672` 且截图与 L3 基线最大差 1；
  mode 0 三张截图仍逐字节一致，stderr 为空且无 Vulkan validation error。
- 未实现（留给 L4b）：层号+mip+层页的专用 feedback 格式（当前只按覆盖页推导，
  层页 mip 是“覆盖页 priority → mip”的近似）；硬预算淘汰（可见 working set 大于
  预算时允许暂时超预算）；低分辨率 fallback/淡入；相机/视锥预取；同一 TR 多摆放的
  页内容去重；不同 `sourceSet` 的多组源纹理。

---

## 7. 风险与备选

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| bindless 不可用 | 方案 B 无法落地 | 目标锁定方案 C；只有未来统一引入 descriptor indexing 才重评 B |
| BC 压缩质量/编码链 | normal 条纹、albedo 色带 | BC5 normal、BC7 albedo；离线编码 + 回读对比；保留 RGBA8 回退格式 |
| KTX2/DDS 解码成本 | 新容器解析/数组/mip | L0 单独做；先支持单层 + 数组 + 预生成 mip，不做 CPU BC 编码 |
| 页表层数爆炸 | 页表项/内存/上传变大 | 页三元组共分配、按 mip 分表、16 B/项；1K 仅 348 KiB，2K 1.33 MiB |
| 材质 schema 扩展 | 新纹理 binding/反射/编辑器 | 沿用 sampler2D + float 页表；避免整数采样器与 SSBO；L3 单步验证 |
| 编辑器预览 | MI 预览/缩略图不认识新纹理 | 共享 `TerrainStreamingState`（已有约束）；缩略图暂不渲染地形 |
| World 级共享 | 多 terrain/多 biome 层定义冲突 | 层 catalog World 级；`AreLayersIdentical` 仍要求一致；页按 layerId 全局共享 |
| 覆盖页精度不足 | 128²/500m ≈ 3.9m/texel，近景层边界糊 | 独立问题：提高覆盖页到 256²或加近景 detail page；不在本设计内混做 |
| `maxImageDimension2D` | 8192/16384 atlas 不保证 | 用 4096² slice + 多 slice；先查询设备上限 |
| `maxImageArrayLayers` | 256 层数组正好是保证下限 | VT 不受此限；方案 A 仅用于 ≤64 热层 |
| atlas 页边渗色 | 黑线/错误过滤 | 页内半 texel inset、禁用 atlas mip、限制各向异性或加 border |
| mip 与 UV 平铺 | `fract` 导数在页边界跳变 | 用未 wrap 的层 UV 算导数；`textureGrad`；边界 clamp |
| 缺页跳变 | 走查时块状闪烁 | 低 mip → 平均色 → 中性表面；2–4 帧淡入 |
| 物理页复用 | 旧帧仍采样被覆盖页 | quarantine + epoch + barrier；复用 P3 实现 |
| 256→512 层 | 8-bit ID 用满 | 257+ 需 16-bit ID：ID atlas 翻倍、feedback/页表格式 v2 |
| 显存预算被固定 atlas 占满 | 无法按需释放 | atlas 固定但页可淘汰；预算档位与 slice 数可配置 |

**备选路线**

- 如果 BC 暂时做不了：先用 512² RGBA8（256 层含 mip 1 GiB）或 64² 页 RGBA8，
  接受更低的层分辨率/更高的页开销；
- 如果最终确认目标机器都支持 descriptor indexing：可把方案 C 的页表间接
  替换为方案 B 的 bindless 数组，采样路径更短，但要重做材质/描述符/ABI；
- 如果只需要“256 层、不要求流式”：方案 A + 512² BC（256 MiB）是最省事的过渡；
- 如果页表间接成为瓶颈：保留少量热层走数组快速路径（§4.7），
  或把 top-4 中权重最高的 1–2 层固定走数组。

---

## 8. 验收口径

层纹理虚拟化完成的判定不是“远处看起来有草”，而是：

1. **预算**：16km 场景走查时，层纹理常驻字节数稳定在 512 MiB / 1 GiB 档位内，
   `resident` 随相机变化，`evictionCount > 0`。
2. **正确性**：同一场景的数组路径与 VT 路径截图差异在约定阈值内；
   debug view（层号/权重/页号/mip）一致；无页边黑线。
3. **同步**：Vulkan validation 无 layout/hazard 错误；
   快速移动/World 重载无黑洞、无悬空页、无重复释放。
4. **流式**：单帧上传字节数有上限；走查无同步卡顿；
   miss 页在 2–4 帧内被低 mip/平均色兜底。
5. **模块测试**：`terrain_tests`、`vl_renderer_backend_tests`、
   `material_schema_tests`、`texture_asset_tests` 全绿；
   若改材质 schema，必须覆盖反射/绑定顺序。
6. **文档门禁**：实现落地后更新
   `documents/contracts/terrain.md`、`texture-asset-json-v1.md`、
   `documents/contracts/testing.md`、`documents/routing.md`；
   运行 `py -3 tool\validate-docs.py`。

---

## 9. 与现有文档/契约的关系

- 当前行为只看 `documents/contracts/terrain.md`：覆盖页、ID/weight、top-4、
  32×32 页表、4096² atlas、feedback 统计都是**已实现**的现状。
- 本文的 256 层层纹理页表、BC 压缩、atlas slice、层 feedback、
  quarantine 都是**未来工作**，实现前不得写进 contracts。
- `terrain-large-world-layers-plan.md` 给出 256/16km/32 候选的总目标；
  本文把其中“层纹理 VT”这一块展开为可实施的格式与步骤。
- `terrain-vt-streaming-design.md` 的 P0–P4 针对**覆盖页**（ID/weight）与
  feedback；本文的层页流式（L4）明确依赖它的 P3。
- 方案 B 的评估只用于说明“为什么当前不做”；如果未来设备/材质系统升级，
  应新开一份 bindless 评审，而不是把本文改成“已支持”。

