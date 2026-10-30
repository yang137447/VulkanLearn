# 地形全局 layer catalog 与多 biome sourceSet 设计

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 类型 | 架构设计 / 未来工作（**非契约**） |
| 状态 | 未实现。当前行为以 `documents/contracts/terrain.md` 与源码为准 |
| 基线 | `codex/terrain-openworld` @ `bf07d42`（2026-10-30） |
| 目标 | 16km 级世界、256+ 全局层、多 biome、每 tile 32 候选、多 `sourceSet`、显存预算 |
| 非目标 | 编辑器绘制工具、GPU-driven 几何、最终美术资产、跨进程层注册服务 |
| 关联 | [层纹理流式设计](terrain-layer-texture-streaming-design.md)、[大世界与上百层计划](terrain-large-world-layers-plan.md)、[terrain 契约](../../contracts/terrain.md) |

> 本文只描述“未来怎么做”，不表示任何能力已经实现，也不替代
> [`contracts/terrain.md`](../../contracts/terrain.md)。文中出现的字段名、位宽、
> 预算和阶段都可以在落地前调整；一旦落地，必须把真实行为写回契约，而不是继续引用本文。

---

## 0. 结论摘要

1. **推荐方案 A（全局 catalog + 全局 layer ID）+ 统一 atlas**：
   TR 里的每一层在 World 构建期映射到一个稳定的**全局 layer ID**，ID/权重图直接存
   全局 ID，shader 的一次 catalog 查询就能拿到该层的页参数与 `sourceSet`。
2. **`sourceSet` 只在 CPU/上传侧生效，不进入 shader 采样路径**：
   所有 biome 的层页共用同一组 4096² `sampler2DArray` atlas（按 slice 分池），
   不同 `sourceSet` 只是“从哪一组源数组 copy 到同一张 atlas”。因此多 biome
   **不增加材质采样器数量**，也不需要 descriptor indexing / bindless。
3. **当前 15 个采样器已经接近上限**：
   `M_terrain.json` 现在声明 15 张纹理；Vulkan 保证的
   `maxPerStageDescriptorSamplers` 下限是 16。任何“每个 sourceSet 一套
   catalog/页表/atlas”的方案都会直接越过这个下限。
4. **256 层以内保持 8-bit ID**；257..4096 层用两张 RGBA8 ID 图（hi/lo）扩展，
   代价是 +1 个采样器（正好到 16）与 feedback 的高位编码。
5. **catalog 的 256 宽度和 shader 的 `stride = texelCount / 256` 是一处隐式耦合**；
   扩到 512+ 层必须先把 stride 改成显式 uniform（或 stride 表），否则页号会串层。
6. **16km / 512m tile = 1024 个 cover page，正好等于当前 32×32 页表容量**，
   而物理槽位只有 1023 个可用；16km 单地形必须扩页表或放大 tile 才能全驻留。

---

## 1. 现状与瓶颈

### 1.1 当前层纹理的数据通路

```text
TR_*.json.layers
  -> TerrainLayerDesc[]                        (terrainAssetTypes.h)
  -> TerrainLayerTable::BuildImage             (256×4 RGBA32F catalog)
  -> TerrainWeightMapBuilder::BuildPage        (每 texel top-4 ID/权重, uint8)
  -> TerrainPageManager                        (覆盖页: 32×32 页表 + 4096² ID/权重 atlas)
  -> TerrainLayerVisibilityResolver            (覆盖页 top-4 -> 可见层 -> (layer,mip,page))
  -> TerrainLayerPageManager                   (层页: 每 mip 一张 RGBA32F 页表 + 物理页池)
  -> layerAlbedo/Normal/Pbr atlas              (4096² × slice sampler2DArray)
  -> mf_terrainLayerVT.glsl                    (catalog -> 页表 -> physical page -> atlas)
```

关键符号：

- `TerrainLayerDesc`（`source/terrain/terrainAssetTypes.h`）：`name`、`heightRange`、
  `slopeRange`、`uvScale`、`uvRotation`、`sourceResolution`、`avgColor`、
  `sourceSet`、`flags`。
- `TerrainLayerTable::MaxLayers = 256`、`Rows = 4`；
  `BuildEntries` / `BuildImage` / `ReadLayer`（`source/terrain/terrainLayerTable.*`）。
- `TerrainWeightMapBuilder::BuildPage`
  （`source/terrain/terrainWeightMap.cpp`）：每 texel 写 4 个 `uint8_t` ID 和 4 个
  8-bit 权重；ID 就是 `plan.desc.layers` 的**下标**。
- `TerrainPageManager`：`PageTableSize = 32`、`PageResolution = 128`、
  `AtlasResolution = 4096`、`PageSlotCount = 1024`、
  `UsablePhysicalSlotCount = 1023`（`source/terrain/terrainPageManager.h`）。
- `TerrainLayerPageManager`：`TerrainLayerPageKey { layerId, mip, pageX, pageY }`、
  `VirtualPageIndex = layerId * mipStride(mip) + pageY * side + pageX`、
  每 mip 一张 `RGBA32F` 页表、物理页到 `(slice, tileX, tileY)` 的映射
  （`source/terrain/terrainLayerPageManager.*`）。
- `TerrainLayerVisibilityResolver`：`ExtractCoverPageLayers` 取 top-4、
  `MaxMipForLayer`、`ExpandCoarserFallbackCandidates`
  （`source/terrain/terrainLayerVisibility.*`）。
- `mf_terrainLayerVT.glsl` / `terrainLayerVTMath.glsl`：
  `TerrainLayerCatalogRow(layer, row)`、`TerrainLayerMipForUv`、
  `TerrainLayerAddressAtMip`、`TerrainLayerPageTableFetch`、
  `perLayerStride = textureSize(pageTable).x * y / TERRAIN_VT_MAX_LAYERS`。
- `RenderSystem` 的层页请求/上传/统计接线（`source/renderSystem.cpp`），
  World 级共享资源由 `RendererResourceLoadCoordinator` 建立
  （`source/render/resource/rendererResourceLoadCoordinator.cpp`）。

### 1.2 硬约束（每条都能指到源码）

| # | 约束 | 证据 | 后果 |
| --- | --- | --- | --- |
| C1 | `layerId` 就是 `TR.layers` 的下标 | `terrainWeightMap.cpp` 的 `topIndices` -> `layerIds[index]`；`LayerWeight(plan.desc.layers[layerId], ...)` | 同一层在不同 TR 里是不同的 ID，无法跨 TR 去重 |
| C2 | ID/权重图是 RGBA8，layer ID 只有 8 bit | `TerrainWeightMap::ids` 为 `std::vector<uint8_t>`；`layerIds` 纹理格式 RGBA8 | 全局层数上限 255/256 |
| C3 | catalog 固定 256 宽、4 行 | `TerrainLayerTable::MaxLayers = 256`、`Rows = 4`；`BuildImage` 中 `width = MaxLayers` | 与 C2 共同锁死 256 层 |
| C4 | shader 用 `/256` 还原页表 stride | `mf_terrainLayerVT.glsl` 的 `TERRAIN_VT_MAX_LAYERS` 与 `perLayerStride` 计算 | 改 catalog 宽度而不改 stride 还原会串层 |
| C5 | 同一 World 的所有分层地形必须层定义完全相同 | `TerrainLayerTable::AreLayersIdentical` + `rendererResourceLoadCoordinator.cpp` 的 throw | 多 biome 直接加载失败 |
| C6 | 同一 World 只允许一组 albedo/normal/pbr 源数组 | `rendererTerrainLoader.cpp` 比较 `streamingState.layerSources.albedo != layerAlbedo` 后 throw | 第二个 biome 的源纹理无法接入 |
| C7 | `sourceSet` 被校验、写进 catalog row2.z，但没有消费者 | `terrainAssetValidator.h` 只校验 0..255；`TerrainLayerTable::BuildImage` 写 `row2+2`；shader 只读 `pageRow.x` | 字段存在但没有语义，无法选择源纹理组 |
| C8 | `u_uvScale` 必须全 World 一致 | `rendererTerrainLoader.cpp` 在 `vtRequested` 时比较并 throw | 不同 biome 不能有不同平铺密度 |
| C9 | 层页 feedback 的 layerId/pageIndex 各 8 bit | `terrainLayerFeedback.h`：`R = layerId`、`G = pageIndex`、`MaxPageIndex = 255` | 层数、单层页数都被 8 bit 限制 |
| C10 | `M_terrain` 已声明 15 个采样器 | `shader/glsl/M_terrain.json` 的 `textures` 段 | 再加 2 个就超过保证下限 16 |
| C11 | 覆盖页容量 = 32×32 = 1024，物理可用 1023 | `TerrainPageManager::PageTableSize`、`UsablePhysicalSlotCount` | 16km/512m = 1024 tile 时最后一页永远缺 |
| C12 | 每 tile 候选层只有 CPU 结构，没有 GPU 消费者 | `TerrainTileCandidateList` 只出现在 `terrainWeightMap.*` 与测试；shader 只读每 texel top-4 | “每 tile 32 候选”目前只是烘焙中间量 |

### 1.3 多 biome 为什么卡住

把“森林”和“沙漠”两个 TR 放进同一个 World，会依次撞上：

1. **层定义不一致 → 加载失败**：两个 TR 的 `layers` 数组长度/参数不同，
   `AreLayersIdentical` 返回 false，coordinator 直接抛
   `All layer-table terrains in a World must share identical layer definitions`。
2. **源数组不一致 → 加载失败**：即使层定义相同，两个 MI 绑定不同的
   `layerAlbedo/layerNormal/layerPbr`，`rendererTerrainLoader.cpp` 会抛
   `All layer-table terrains in a World must share the same ... layer texture arrays`。
3. **改 `sourceSet` 没有用**：字段能写进 catalog，但没有任何代码按它选择源纹理；
   写 0 和写 7 的行为完全一样。
4. **合并成一个大 TR 也不行**：`TR.layers` 最多 256 项，且 ID 是 8 bit；
   256+ 层的目标无法表达。
5. **不同 biome 不能有不同的 `u_uvScale`**：层 UV 直接用材质的全局
   `u_uvScale`，feedback 也只有一个 `u_uvScale`。

结论：多 biome 不是“多写几个 layer JSON”的问题，而是**身份、位宽、源纹理组**三件事
同时缺失。

### 1.4 现状数字基线

| 项目 | 当前值 | 位置 |
| --- | --- | --- |
| 全局层数上限 | 256 | `TerrainLayerTable::MaxLayers` |
| 每 texel 混合层数 | top-4 | `terrainWeightMap.cpp` 的 `TopLayerCount = 4` |
| 覆盖页分辨率 | 128² | `TerrainPageResolution` |
| 覆盖页表 | 32×32 = 1024 | `TerrainPageTableSize` |
| 覆盖 atlas | 4096² slice | `TerrainPageManager::AtlasResolution` |
| ID/权重图 | 两张 4096² RGBA8 = 128 MiB | `rendererTerrainLoader.cpp` 的 atlas 创建 |
| 层页 atlas | 3 × 4096² × slice | `CreateTerrainLayerAtlasGpuResources` |
| 层页物理页池 | 默认 1024（1 slice，fallback 占 1） | `DefaultPhysicalPageCount` |
| 层页预算 | 512 驻留页 | `DefaultResidentCapacity` |
| 每帧层页请求/上传 | 16 / 8 | `TerrainLayerVisibilityConfig` |

16km 的算术：

```text
16km = 1,600,000 cm；tile = 512m = 51,200 cm
1,600,000 / 51,200 = 31.25 -> 每边 32 tile -> 1024 个虚拟页
页表容量 1024，物理槽位 1023（fallback 占 1）
-> 16km 单地形刚好把页表占满，且必然有 1 页无法驻留
```

这不是“实现 bug”，而是当前 32×32 页表 + 512m tile 的容量边界；
16km 落地前必须扩页表（64×64）或放大 tile。

---

## 2. 目标与边界

### 2.1 目标

| 项目 | 目标值 | 说明 |
| --- | --- | --- |
| 世界尺寸 | 16km × 16km | 与 `terrain-large-world-layers-plan.md` 一致 |
| 全局层数 | 256 起，可扩到 1024 | 8-bit ID 为主，扩展位宽可选 |
| 每 tile 候选层 | 32 | 用于烘焙/裁剪，不进入每像素循环 |
| 每像素混合 | top-4 | 保持现有 12 次层采样（4×3） |
| biome / sourceSet | 至少 4 组 | 每组一套 albedo/normal/pbr 源数组 |
| 层源分辨率 | 512 / 1024 / 2048 | 已有约束，保持不变 |
| 压缩 | albedo BC7 sRGB、normal BC5、pbr BC7 linear | L0 已实现容器解码 |
| 地形纹理常驻预算 | 256 MiB（低） / 512 MiB（高） | 含覆盖 ID/权重与层 atlas |

### 2.2 非目标

- 不改变几何 LOD/skirt、碰撞、chunk streaming 的现有语义；
- 不引入 SSBO、storage image、bindless、descriptor indexing 作为**前置条件**；
- 不做跨进程的层注册服务、不做多人编辑冲突合并；
- 不保证 >256 层的 feedback 精度（那是 S3 的独立目标）。

---

## 3. 方案对比

### 3.1 评估轴

1. ID/权重图兼容性（是否要重烘、位宽是否够）；
2. 页表/atlas 是否可跨 terrain 共享；
3. shader 分支与采样成本（每像素几次 fetch、是否有动态 sampler 索引）；
4. 资产迁移成本（旧 TR/旧场景是否要改）；
5. 显存（atlas 池是否按 sourceSet 重复）；
6. 编辑器/资产工具可用性；
7. 是否影响 LOD/碰撞（应当完全不影响）。

### 3.2 方案 A：全局 catalog + 全局 layer ID

```text
World build:
  for each terrain placement:
    for each TR.layers[i]:
      key = layerKey(TR, layer)
      globalId = registry.Resolve(key, params)   // 去重 + 分配
      localToGlobal[placement][i] = globalId
  bake ID/weight: 写 globalId（uint8 / 扩展位宽）

GPU:
  catalog[globalId] -> pagesPerSide/sourceSet/uv/avgColor
  ID/weight atlas  -> globalId
  page table       -> (globalId, mip, pageXY) -> physical page
  atlas            -> physical page
```

- **ID/权重图**：直接存全局 ID；单 TR 时全局 ID = 局部下标，旧数据/旧截图可保持逐字节一致。
- **页表/atlas 共享**：所有地形共用同一页表与 atlas 池；同一层在不同 placement
  自动共享页，天然解决“同一 TR 多摆放”。
- **shader**：没有额外映射层；仍是 1 次 catalog 行 + 1 次页表 + 3 次 atlas。
- **资产迁移**：TR 的 `layers` 仍是作者数据，但要新增/推导 `layerKey`；
  ID/权重是加载期烘焙，不需要持久化迁移。
- **显存**：最优；页按全局层共享。
- **编辑器**：需要一张 World 级“全局层列表”，显示 ID、key、sourceSet、引用计数。
- **LOD/碰撞**：完全无影响（只消费高度图/TRS）。

风险：全局 ID 是 World 内分配，跨 World 不稳定；跨 World 复用 atlas 缓存需要
按 `layerKey` 而不是 ID 做 cache key。

### 3.3 方案 B：每 terrain 子 catalog + 局部 ID，再映射到全局

```text
ID 图存局部 ID -> shader 先查“该地形的子 catalog 基址” -> 得到 globalId
             -> 再查全局 catalog -> 页表 -> atlas
```

- **ID/权重图**：完全不用改，旧烘焙数据可直接用（局部 ID 不变）。
- **页表/atlas**：仍可全局共享（映射后按 globalId 查）。
- **shader**：多一次间接 fetch，且需要“当前地形属于哪个子 catalog”的
  **逐 draw 数据**。当前材质 UBO 是 per-material，不能按 terrain placement 变化；
  要么把 base offset 塞进已有的 per-object UBO（需要新增字段并让
  `MaterialFunctionContext` 暴露它），要么把子 catalog 做成按 UV 偏移的大图。
- **资产迁移**：最低。
- **显存**：多一份子 catalog（很小），但多一份映射逻辑/调试面。
- **编辑器**：局部 ID 对作者友好，但“同一层两个局部 ID”会让层列表去重变复杂。
- **LOD/碰撞**：无影响。

风险：**引擎目前没有 per-draw 纹理绑定**；B 必须新增 per-object 数据通道，
相当于先补一套“逐 draw 材质参数”，成本高于 A。

### 3.4 方案 C：每 sourceSet 独立 catalog / 页表 / atlas

```text
sourceSet 0: catalog0 + pageTable0..4 + 3 atlas
sourceSet 1: catalog1 + pageTable0..4 + 3 atlas
shader: 按 layer 的 sourceSet 做固定 N 路 if/else
```

- **ID/权重图**：ID 需要能表达 “sourceSet + 局部层号”，否则 shader 不知道该选哪套。
- **页表/atlas**：完全隔离；同层不能跨 set 共享，物理页池按 set 重复分配。
- **shader**：每个 set 至少 +1 catalog +5 页表 +3 atlas；`M_terrain` 现在已有 15 个
  采样器，加一个 set 就 ≥24，超过 16 的保证下限；要落地必须 descriptor indexing
  或把页表/atlas 合并成数组。分支还会带来 divergence。
- **资产迁移**：每层必须显式声明 sourceSet，且同一 terrain 的层最好同属一个 set。
- **显存**：最差；每个 set 都要独立 atlas slice。
- **编辑器**：set 概念清晰，但页/预算要按 set 分别呈现。
- **LOD/碰撞**：无影响。

风险：**采样器上限**是硬约束。除非未来统一引入 descriptor indexing，否则 C 不可行。

### 3.5 方案 A′：全局 catalog + 统一 atlas（推荐）

A 的细化：**不按 sourceSet 拆 atlas**，而是让所有 sourceSet 的层页写入
同一组 `layerAlbedoAtlas / layerNormalAtlas / layerPbrAtlas`。

- 前提：每张 atlas 的所有内容格式一致（albedo 全 BC7 sRGB、normal 全 BC5、
  pbr 全 BC7 linear）。这与现有 `CreateTerrainLayerAtlasGpuResources`
  “一张 atlas 一个格式”的约束一致。
- `sourceSet` 只决定**上传时从哪一组源数组 copy**，以及编辑器/校验分组；
  shader 只看到 `physicalPage -> atlas`，不需要知道 sourceSet。
- 采样器数量不变（15），不需要 bindless。
- 需要扩的是**物理页池**（按 1024 页/slice 增加 slice）与**CPU 源纹理注册表**。

### 3.6 对比矩阵

| 维度 | A（全局 ID） | B（局部→全局映射） | C（每 set 独立） | A′（统一 atlas） |
| --- | --- | --- | --- | --- |
| ID 图兼容 | 需重烘（加载期） | 不改 | 需扩展编码 | 需重烘（加载期） |
| 页表/atlas 共享 | 全局 | 全局 | 按 set 隔离 | 全局 |
| shader 额外成本 | 0 | +1 次映射 fetch | +N 路分支/+N 套采样器 | 0 |
| 采样器数量 | 15 | 15（需 per-draw 数据） | ≥24 | 15 |
| 显存 | 最优 | 优 | 最差 | 最优 |
| 迁移成本 | 中 | 低 | 高 | 中 |
| 是否需要 bindless | 否 | 否（但要 per-draw 通道） | **是** | 否 |
| 推荐 | 备选 | 否 | 否 | **是** |

---

## 4. 推荐架构（A′）的数据格式

### 4.1 总览

```text
                 ┌──────────────────────────────┐
TR_*.json ──────►│ WorldLayerRegistry           │
 (layers)        │  key -> globalId             │
                 │  globalId -> LayerRecord     │
                 └──────────┬───────────────────┘
                            │
        ┌───────────────────┼────────────────────┐
        ▼                   ▼                    ▼
 catalog (globalId)   ID/weight bake       sourceSet table
 256/512/1024 × 6     globalId per texel   set -> 3 source arrays
        │                   │                    │
        │                   ▼                    ▼
        │            cover atlas          GPU→GPU page copy
        │            (4096² RGBA8)               │
        ▼                                        ▼
  per-mip page table ────────────────► unified layer atlas (3 × 4096² × slice)
```

### 4.2 全局 layer ID 的稳定规则

**身份不是参数哈希，而是显式的 `layerKey`。** 参数哈希只用于一致性检查。

```text
layerKey 的取值顺序：
1. TR layer 显式字段 "key": "<string>"（推荐，跨 TR 共享层必须写）
2. 缺省："<TR assetPath>#<layer.name>"（保证同一 TR 的多次摆放自动共享）
3. sourceSet 不参与 key；它只是该层的上传分组
```

规则：

- **同一个 key 必须参数完全一致**（`name`、height/slope、`uvScale/uvRotation`、
  `sourceResolution`、`avgColor`、`sourceSet`、`flags`）。不一致 → 加载失败，
  错误里同时打印 TR 路径、层名、key。不要“后者覆盖前者”。
- **同一个 TR 内 `name` 必须唯一**（新增校验）；否则缺省 key 会撞车。
- **参数哈希**（例如 BLAKE3 前 8 字节）只写进诊断/缓存指纹，
  **不作为 ID**：内容一改 ID 就漂移，ID/权重图和 feedback 全部失效。
- **ID 分配顺序 = 场景对象顺序 + 每个 TR 的 `layers` 顺序首次出现**。
  这样单 TR World 的 globalId 恰好等于局部下标，旧场景 catalog/ID 图逐字节不变。
- 冲突处理：
  - 同 key 同参数 → 复用同一个 globalId（跨 TR 去重）；
  - 同 key 不同参数 → **加载失败**；
  - 不同 key 但内容相同 → 不去重，允许共存（作者要共享就写同一个 key）。
- **World 内 ID 稳定，跨 World 不保证**。跨 World 的 atlas 缓存必须以
  `layerKey` 为 key，而不是 globalId；持久化注册表（`layerRegistry.json`）
  留到 S4。

### 4.3 TR.layers 如何引用/去重，sourceSet 如何映射

TR 层字段（在现有基础上新增 `key`）：

```json
{
  "name": "grass",
  "key": "biome/temperate/grass",
  "heightRange": [0.0, 0.6],
  "slopeRange": [0.0, 35.0],
  "uvScale": 1.0,
  "sourceResolution": 1024,
  "avgColor": [0.18, 0.32, 0.12],
  "sourceSet": 1,
  "flags": 0
}
```

`sourceSet` 的解析（v1，不新增 TR 顶层字段）：

- 每个 terrain 的 MI 仍然绑定自己的 `layerAlbedo/layerNormal/layerPbr`；
- loader 用 `(MI, sourceSet)` 建立 `TerrainSourceSetRecord`：
  `{ sourceSet, albedo, normal, pbr, arrayLayers, mipLevels, extent, formatGroup }`；
- 同一 `sourceSet` 在同一个 World 内必须指向同一组 shared_ptr；
  不同 `sourceSet` 可以指向不同纹理；
- 层页上传时按 catalog row2.z 找到 `TerrainSourceSetRecord`，从该组源图 copy。

v2（推荐，等编辑器/离线烘焙需要时再加）：TR 顶层增加

```json
"sourceSets": [
  { "id": 0, "albedo": "T_GrassAlbedo.json", "normal": "...", "pbr": "..." },
  { "id": 1, "albedo": "T_SandAlbedo.json", "normal": "...", "pbr": "..." }
]
```

这样不依赖 MI 绑定也能校验、离线烘焙和跨工具复用；加载器把 TR 的
`sourceSets` 与 MI 实际绑定做一致性校验。

**格式分组**：`formatGroup` 由三张源图的实际 `vk::Format` 推导
（例如 `{BC7_SRGB, BC5, BC7_UNORM}` = group 0）。同一 `formatGroup` 的
sourceSet 共享 atlas；不同 group 需要第二组 atlas（见 4.6 的 `B` 通道方案）。

### 4.4 ID/权重图的位宽

| 全局层数 | ID 存储 | 采样器增量 | feedback 编码 |
| --- | --- | --- | --- |
| ≤256（当前目标） | 1 × RGBA8，`R/G/B/A = 4 个 uint8` | 0 | 现状：`R = layerId` |
| 257..4096 | 2 × RGBA8：`idLo` + `idHi`（每个 ID 16 bit，取低 12 bit） | +1（15→16） | `R = idLo`，`B` 的 bit4..7 存 `idHi` 低 4 bit |
| >4096 | 候选槽位方案：ID 图存 5-bit candidate slot，另加 tile candidate 表（32×16-bit globalId） | +1（candidate 表） | feedback 报 slot + tile，由 CPU 还原 globalId |

说明：

- 16-bit ID 图**不需要整数采样器**：`MaterialDescriptorSchema` 只支持
  `sampler2D/sampler2DArray`，所以用两张 UNORM8 图、shader 里 `round(v*255)` 拼 16 bit。
- `idHi` 的采样必须与 `idLo` 使用同一 UV/同一 texel 中心；两者都是 nearest，
  ID 不能被线性插值（现状 `layerIds` 用 `texture()` 线性过滤，见
  `mf_terrainLayerBlend.glsl`；多 ID 图时应改为 `texelFetch` 或 nearest）。
- `TerrainLayerFeedbackMaxPageIndex = 255` 意味着**单层单 mip 最多 256 页**：
  2K 源 mip0 = 16×16 = 256 页，刚好；4K 源会溢出。支持 4K 层时必须同时改
  feedback 的 `G` 通道（拆成 2 个 8-bit 或扩到 16-bit）。
- 257..4096 的 feedback 高位：`B` 现在只用 bit0..2（mip）和 bit7（valid），
  bit3..6 共 4 bit 空闲，足够存 12-bit ID 的高 4 bit。

### 4.5 layer catalog 布局（行 0..5）与 source set table

catalog 从 `256×4` 扩成 `LayerCapacity×6`，行 0..3 保持现有含义：

| 行 | 内容 | 现状 |
| --- | --- | --- |
| 0 | heightMin / heightMax / slopeMin / slopeMax | 已实现 |
| 1 | noise / blendEdge / uvScale / uvRotation(rad) | 已实现，shader 消费 |
| 2 | pagesPerSide(mip0) / pageTableBase / sourceSet / flags | 已实现 |
| 3 | avgColor.rgb / reserved | 已实现，shader 消费 |
| 4（新） | sourceResolution / formatGroup / layerKeyHashLo / reserved | 新增 |
| 5（新） | layerKeyHashHi / biomeMask / sortPriority / reserved | 新增 |

- `sourceResolution` 在 row4 冗余一份，方便 shader/工具在不解析
  `pagesPerSide*128` 的情况下做校验；shader 仍以 row2 为准。
- `formatGroup` 让 shader 在需要时选择 atlas 组；A′ 单组时恒为 0。
- `layerKeyHash` 只是编辑器/诊断用的弱指纹，不参与采样。
- `biomeMask` 供未来“按 biome 过滤调试视图/流式预取”使用，当前不消费。
- `LayerCapacity` 建议按 `256 / 512 / 1024` 三档配置；纹理宽度就是容量，
  高度 6。1024×6 RGBA32F = 96 KiB，可忽略。

**stride 必须显式化**：当前 shader 用 `textureSize(pageTable)/256` 还原
`mipStride`，隐含“层容量 = 256”。扩容量时改为：

```glsl
uniform float u_layerMipStride[5];   // 来自 TerrainLayerPageManager::VirtualPagesPerLayer
uniform vec2  u_layerPageTableSize[5];
```

或一张 1×5 的 `strideTable` 纹理（`RG32F`）。否则 512 层时 shader 会把
页号算成一半，直接串层。这是 S3 的前置条件。

**source set table**（CPU/GPU 双形态）：

```cpp
struct TerrainSourceSetRecord
{
    uint32_t sourceSet;
    uint32_t formatGroup;      // 0 = BC7_SRGB/BC5/BC7_UNORM
    uint32_t sourceResolution; // 512/1024/2048（同 set 内必须一致）
    uint32_t atlasSliceBase;   // 该 set 可用的物理页起点（A′ 不用，保留）
    std::shared_ptr<Texture> albedo, normal, pbr;
};
```

- GPU 侧可以先不做表；A′ 的 shader 不需要。若未来按 `formatGroup` 拆 atlas，
  再加一张 `sourceSetTable`（每行 1 个 texel，`RGBA32F`）给 shader 查
  `formatGroup -> atlas`，仍不需要动态 sampler 索引。

### 4.6 page table 与 atlas 分组

- **页表**：保持每 mip 一张 `RGBA32F` 页表，条目含义不变：
  `R = physicalPage`、`G = mip`、`B = valid | (formatGroup << 1)`、
  `A = priority`。
  - 现在 `B` 只有 0/1；bit1..7 空闲，正好放 `formatGroup`。
  - `physicalPage` 在**统一 atlas 池**内全局编号：`slice = page / 1024`、
    `tile = page % 1024`，与 `TerrainLayerPageManager::DecodePhysicalPage` 一致。
- **atlas**：
  - 每个 `formatGroup` 一组三张 `4096² × slice` 的 `sampler2DArray`；
  - slice 数 = `ceil(该 group 物理页数 / 1024)`；
  - A′ 只有一个 group，因此仍是 3 张 atlas；
  - 同一 group 内不同 `sourceSet` 共用物理页池 —— 页内容来自哪个 set
    由 CPU 的 `TerrainSourceSetRecord` 决定，GPU 不需要知道。
- **fallback**：保留每张 atlas 的末页作为 fallback/中性页；
  每增加一个 group，就多一个保留页，预算按 group 计。
- **页表容量**：`TerrainLayerPageManager::PageTableLayoutForEntries` 已按
  4096 宽展开，层数扩到 1024/2K 时总项数约 5.3 MiB，仍可接受；
  但 `VirtualPageTableSize(mip)` 变大后 shader 的 `%`/`/` 仍是整数，
  不会引入精度问题（float32 精确到 24 bit）。

### 4.7 shader 采样路径与采样器预算

A′ 的路径（与现状同形，只多了可选的 ID hi 与 group 选择）：

```glsl
ids  = decodeIds(atlasUv);             // 1 或 2 张 RGBA8
w    = texture(layerWeights, atlasUv); // 归一化
for (slot = 0; slot < 4; ++slot) {
    layer   = ids[slot];                  // globalId
    row1    = catalog(layer, 1);          // uvScale/rotation
    row2    = catalog(layer, 2);          // pagesPerSide/sourceSet
    row3    = catalog(layer, 3);          // avgColor
    row4    = catalog(layer, 4);          // formatGroup
    mip     = clamp(round(log2(texelsPerPixel)), 0, maxMip);
    page    = addressAtMip(uv, pagesPerSide, mip);
    vpage   = layer * u_layerMipStride[mip] + page.pageIndex;
    entry   = texelFetch(pageTable[mip], texel(vpage), 0);
    // 缺页 -> 更粗 mip；全部无效 -> row3.avgColor
    group   = int(entry.b * 255.0) >> 1;
    atlas   = selectAtlas(group);         // 固定 2/3 路 if/else，不用动态 sampler
    color  += textureLod(atlas.albedo, uv, 0) * w[slot];
    ...
}
```

采样器预算：

| 配置 | 采样器数 | 是否 ≤16 |
| --- | --- | --- |
| 现状（mode 0 + mode 1 同 shader） | 15 | 是 |
| A′ 256 层（catalog 变宽，不加纹理） | 15 | 是 |
| A′ 257..4096 层（+`layerIdHi`） | 16 | 是（正好） |
| A′ + 第二 `formatGroup`（+3 atlas） | 19 | **否**，需 descriptor indexing 或合并 group |
| C（每 set 独立） | ≥24 | **否** |

结论：**A′ 在 256 层与 4096 层两档都不需要 bindless；只有“多 atlas 格式组”
才会撞上限。** 如果确实需要第二个 group，优先把两张 atlas 合并成
“一个 `sampler2DArray` + slice 分区”（要求格式相同），而不是加采样器。

### 4.8 显存预算

以 128² 页、4096² slice 计：

| 项目 | 公式 | 256 层 / 1K / BC | 1024 层 / 2K / BC |
| --- | --- | --- | --- |
| catalog | `W × 6 × 16 B` | 24 KiB | 96 KiB |
| 页表（全部 mip，按 4096 宽量化） | 见下 | ≈ 448 KiB | ≈ 6 MiB |
| 层 atlas（1 slice，3 图） | `3 × 16 MiB` | 48 MiB | 48 MiB |
| 层 atlas（3 slice，3 图） | `3 × 3 × 16 MiB` | 144 MiB | 144 MiB |
| 覆盖 ID/权重 atlas | `2 × 64 MiB` | 128 MiB | 128 MiB |
| 覆盖页表 | `1024 × 4 B` | 4 KiB | 4 KiB |
| **合计（1 slice）** | | **≈ 177 MiB** | **≈ 182 MiB** |
| **合计（3 slice）** | | **≈ 273 MiB** | **≈ 278 MiB** |

页表量化说明（`ceil(entries/4096)` 行，每行 4096×16 B = 64 KiB）：

```text
1K 层（256 层）：mip0 16384 项 = 4 行 = 256 KiB
                 mip1  4096 项 = 1 行 =  64 KiB
                 mip2  1024 项 = 1 行 =  64 KiB
                 mip3   256 项 = 1 行 =  64 KiB
                 ---------------------------------
                 合计 448 KiB
2K 层（256 层）：mip0 65536 = 16 行 = 1 MiB
                 mip1 16384 =  4 行 = 256 KiB
                 mip2  4096 =  1 行 =  64 KiB
                 mip3  1024 =  1 行 =  64 KiB
                 mip4   256 =  1 行 =  64 KiB
                 ---------------------------------
                 合计 1.44 MiB；1024 层再 ×4 ≈ 5.8 MiB
```

工作集与 slice 的关系（关键）：

```text
1K 层 mip0 = 8×8 = 64 页/层
32 个候选层 × 64 = 2048 页 + fallback -> 3 个 slice（3072 页，144 MiB BC）
20 个可见层 × 64 = 1280 页 -> 2 个 slice（96 MiB BC）
512² 层 mip0 = 4×4 = 16 页/层 -> 32 层 = 512 页（1 个 slice）
```

因此：

- **16km 近景 32 层不能全用 1K mip0**：需要 3 个 slice（144 MiB）或把
  远景层降到 512²。
- 现状默认 1024 物理页（1 slice，48 MiB BC）只够 “20 层 × 1K mip1（16 页）”
  或 “32 层 × 512² mip0”。
- RGBA8 时同页数成本 ×4：1 slice = 192 MiB，3 slice = 576 MiB，
  已经超过“地形 512 MiB”档位；**BC 是预算成立的前提**。

---

## 5. 迁移与兼容

### 5.1 旧单组 TR / 场景

- 单 TR World：globalId = 局部下标（首次出现顺序），catalog 行内容不变，
  ID/权重图字节不变，mode 0 / mode 1 截图应逐字节一致。
- 多 TR 但层定义完全相同：旧行为是“必须完全相同才能加载”；新实现去重后
  仍得到相同的 globalId 顺序，输出不变。
- 旧 TR 没有 `key`：缺省 `"<assetPath>#<name>"`，不同 TR 的同名层不会误合并。

### 5.2 同一 TR 多摆放

- 缺省 key 相同 → 同一个 globalId → 同一组层页，天然去重；
- chunk key 仍含 `placementId`（`TerrainChunkResourceKey`），几何不共享，
  只有层纹理页共享；
- 页表/atlas 仍按虚拟页（cover tile）分配，两个 placement 的 cover page
  不重叠（由 `pageOrigin` 决定）。

### 5.3 不同 TR 的同名层

| 情况 | 结果 |
| --- | --- |
| 同 `key`、参数一致 | 合并为一个 globalId，共享页 |
| 同 `key`、参数不一致 | 加载失败（错误带两个 TR 路径） |
| 不同 `key`、名字相同 | 视为两层，各自分配 ID；允许（≤容量） |
| 不同 `key`、内容相同 | 不去重；作者应统一 `key` |

### 5.4 编辑器 / 资产工具

- World 级“全局层列表”：`globalId / key / name / sourceSet / 引用地形数 / 页数`；
- 冲突在编辑期就报，不要等到 Play；
- debug view 2 显示 globalId；同时提供“显示 layerKey”的文本通道（可选）；
- `terrainlayerstats` 增加 `globalLayers / sourceSets / dedupedLayers / capacity`；
- mode 0 必须保留：它是“层数少、不开 VT”的快速路径与回归基线。

### 5.5 明确的兼容边界

- `sourceSet` 目前只在 CPU 侧生效；把同一层从 set 0 改到 set 1 会改变
  上传来源，但不改变 shader 路径；
- 同一 `sourceSet` 内 `sourceResolution` 必须一致（页表 stride 以层为单位，
  但上传 extent 校验按 set 做更简单）；跨 set 可以不同；
- `u_uvScale` 的 World 级一致性约束**仍然保留**；如果要多 biome 不同密度，
  把 `u_uvScale` 乘进每层 `uvScale`，或另开“逐 terrain UV scale”议题，
  不要在本方案里偷偷放开。

---

## 6. 分步实施

### S1：World 级全局 catalog + 全局 ID（无 GPU 格式变化）

**目标**：多个 TR 可以有不同的层定义并同时出图；同层去重；单 TR 输出逐字节不变。

**触碰文件**

- `source/terrain/terrainAssetTypes.h`：`TerrainLayerDesc::key`；
- `source/terrain/terrainLayerTable.*`：按全局层列表构建 catalog，
  新增 `LayerCapacity`（先固定 256）；
- 新增 `source/terrain/terrainLayerRegistry.*`：`layerKey` 解析、去重、
  冲突检测、`local -> global` 映射；
- `source/terrain/terrainWeightMap.*`：`BuildPage` 接收映射表，写 globalId；
- `source/render/resource/rendererResourceLoadCoordinator.cpp`：
  用 registry 替换 `AreLayersIdentical` 的直接抛错；
- `source/render/resource/rendererTerrainLoader.*`：把 catalog/映射注入 state；
- `tool/terrain-tests/`：registry 单测 + 多 TR 场景测试；
- 文档：`contracts/terrain.md`（落地后）、`testing.md`、`routing.md`。

**测试**：registry 去重/冲突、local→global 映射、单 TR 恒等映射、
多 TR 不同层定义可加载、ID 越界拒绝。

**冒烟**：新增 `SC_terrain_biome_two.json`（森林 + 沙漠，两个 TR、两套层）；
旧 B32/M3/BC7 截图哈希不变。

**依赖**：无。**不需要 descriptor 变更。**

### S2：多 sourceSet 的源纹理注册 + 统一 atlas 上传

**目标**：两个以上 biome 各自绑定不同源数组，但共享同一组层 atlas。

**触碰文件**

- `source/render/resource/rendererTerrainLoader.*`：
  `TerrainLayerSourceTextures` 改成 `map<sourceSet, TerrainSourceSetRecord>`；
  移除“全 World 单组 shared_ptr”的 throw；
- `source/render/resource/rendererResourceLoadCoordinator.cpp`：
  按 set 创建/校验 source records；
- 层页上传：按 catalog row2.z 选源组，`RecordTerrainLayerPageUploads` 的
  region 来源改为 `record.albedo/normal/pbr`；
- `source/terrain/terrainLayerPageManager.*`：页键不变，
  但物理页分配可按 `formatGroup` 分段（先只有 group 0）；
- 测试：两个 set、不同源分辨率、同 set 不同指针必须失败。

**冒烟**：双 biome 场景，`terrainlayerstats` 显示 `sourceSets=2`，
两套源图都能出图；BC 与 RGBA8 各跑一次。

**依赖**：S1。**不需要 descriptor 变更。**

### S3：>256 层的位宽与 stride 显式化（可选，按目标决定）

**目标**：512/1024 全局层可用。

**触碰文件**

- `shader/glsl/M_terrain.json`：新增 `layerIdHi`（sampler2D）；
- `mf_terrainLayerBlend.glsl`：ID 解码拼 16 bit；
- `mf_terrainLayerVT.glsl`：`u_layerMipStride[5]` 或 stride 表；
- `terrainLayerFeedback.h/.cpp` + `pass/terrainFeedback.frag`：
  `R=idLo`、`B` 高位存 idHi；
- `TerrainLayerTable`：`MaxLayers` 参数化；
- `TerrainLayerPageManager`：stride 由容量推导，不再写死 256；
- 测试：512 层 catalog/页表/feedback 往返、跨 mip 页号不串层。

**依赖**：S1/S2。**采样器从 15 增到 16，正好是保证下限**；
若设备实测 `maxPerStageDescriptorSamplers` 只有 16，`M_terrain` 不能再加任何纹理。

### S4：跨 World 注册表、页去重与编辑器

**目标**：持久化 `layerRegistry.json`；atlas 缓存按 `layerKey` 跨 World 复用；
同一 TR 多摆放的层页只烘焙一次；编辑器全局层列表。

**触碰文件**：registry 持久化、`RendererResourceCache` 的跨 World 缓存键、
编辑器层列表 UI、`terrainlayerstats`。

**依赖**：S1–S3。**不需要 descriptor 变更。**

### S5（条件项）：多 atlas 格式组

只有出现“albedo 需要 alpha / 非 BC7”的层时才做：

- 页表 `B` 的 bit1..7 存 `formatGroup`；
- shader 固定 2 路 atlas if/else；
- 采样器 15+3=18 > 16 → **必须**引入 descriptor indexing，或把组压成
  相同格式的 slice 分区。

---

## 7. 风险

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 采样器上限 16 | 任何“每 set 一套纹理”的方案都不可行 | A′ 统一 atlas；先测设备 `maxPerStageDescriptorSamplers` |
| shader 写死 `/256` | 扩容后页号串层、颜色错乱 | S3 先显式化 `u_layerMipStride`，再加容量 |
| feedback 8-bit pageIndex | 4K 层源、单层 >256 页时 feedback 失真 | 限制 2K；或扩 `G` 通道 |
| feedback 8-bit layerId | >256 层时命中错层 | S3 用 `B` 高位存 idHi |
| 16km/512m = 1024 tile | 页表刚好占满、必然缺 1 页 | 64×64 页表或 1km tile |
| 跨 World ID 漂移 | atlas 缓存误命中 | cache key 用 `layerKey`，不用 globalId |
| 同名层误合并 | 两个 biome 的“草”被当成同一层 | 缺省 key 带 TR 路径；共享必须显式 `key` |
| 不同格式源图 | 统一 atlas 无法容纳 | 按 `formatGroup` 拆 atlas（S5，需 bindless） |
| `u_uvScale` 全局一致 | 多 biome 密度无法独立 | 用每层 `uvScale` 表达，或另立议题 |
| mode 0 回归 | 旧场景截图变化 | S1 保证单 TR 恒等映射；每次改动用哈希对比 |
| CPU 页烘焙同步 | 新层首次可见时卡顿 | 本设计不改变；由既有 L4 预算/后续异步烘焙解决 |

---

## 8. 验收口径

### 8.1 功能

- 一个 World 内至少 2 个 biome、2 套 `sourceSet`、层定义不同，能同时出图；
- 同一层在 2 个 TR 中显式同 `key` 时只烘焙一份页，`terrainlayerstats`
  的 `globalLayers` 小于 `sum(TR.layers)`；
- 同 `key` 不同参数必须加载失败，错误信息包含两个 TR 路径；
- 单 TR 旧场景（B32/M3/BC7）截图 SHA-256 逐字节不变；
- mode 0 仍可用，且不依赖 VT atlas。

### 8.2 预算

- 256 层 / 1K / BC / 1 slice：地形纹理常驻 ≤ 180 MiB（实测 `terrainlayerstats`
  的 `atlasBytesTotal + 覆盖 ID/权重`）；
- 32 层 × 1K mip0 场景：要么 3 slice（≤ 280 MiB），要么自动降 512²；
  不允许“静默缺页成片”；
- 任意 `terrainlayer capacity N` 下，可见集之外的页必须能回收；
  可见集本身超预算时统计要明确报告，不伪装成 0 miss。

### 8.3 回归

- `terrain_tests`、`vl_renderer_backend_tests`、`material_schema_tests`、
  `texture_asset_tests` 全绿；
- 双 biome 场景 stderr 为空、无 Vulkan validation error；
- `validate-docs.py` 通过；
- 文档落地时把“已实现”的字段/位宽/预算写回 `contracts/terrain.md`，
  本文只保留“为什么”和“还没做”。

