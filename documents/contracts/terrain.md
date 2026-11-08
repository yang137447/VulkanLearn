# Terrain Runtime

> 状态：**当前契约**。本文件只描述 `source/` 与场景/TR 资产现在的行为；地形
> 专用低分辨率兜底页/时序淡入、运行时异步 GPU 上传与 Play 中的实时碰撞重建
> 不在当前能力内。运行时几何 LOD、异步 CPU chunk 构建 + GT 上传/卸载、
> 层页 force-mip/avgColor 混合与相机前向预取已接入；
> 编辑器侧已有 TR 资产页、Landscape Sculpt/Paint 会话、高度图写回与 Paint 快照保存，
> 边界见“编辑器地形编辑”。

## 数据流

```text
SC_*.json (type: "terrain")
  -> SceneAssetValidator::BuildLoadPlan
  -> TerrainAssetLoader::Prepare(terrainPath)
       -> TR_*.json 解析与校验
       -> 高度图解码为 HostImage
  -> RendererResourceLoadCoordinator
  -> RendererTerrainLoader
       -> TerrainMeshBuilder::BuildChunks(每级)
       -> RenderableObject / MeshObjectBuildPlan
  -> WorldBuilder -> World mesh objects -> geometry pass
  -> WorldSnapshotBuilder (按 chunk AABB 距离选一级，滞回)
  -> terrainFeedback pass (可见页反馈，驱动驻留) -> frame-fenced readback ring
```

地形没有独立的 World 类型。正常出图仍和普通 mesh 一样走 scene object identity、
mesh/material resource key、World mesh object 和 geometry pass；P2 另有一个只读
`terrainFeedback` pass，它只写独立的 128×128 统计 RT，不参与出图。区别只在 CPU
顶点/索引由 `TerrainMeshBuilder` 生成，而不是从 SM/FBX/OBJ 导入。
`M_terrain` 的像素采样有两条路径：默认 `u_terrainLayerMode=0` 直接采样三张
`sampler2DArray`；作者显式设为 1 时走 L3 的层页表/atlas VT 路径。

## TR JSON

```json
{
    "name": "TR_terrain_smoke",
    "type": "terrain",
    "description": "optional authoring note",
    "heightmapPath": "Generated/Validation/TerrainSmoke/Source/heightmap.bmp",
    "size": [10000.0, 10000.0],
    "heightScale": 2000.0,
    "heightOffset": 0.0,
    "gridResolution": 64,
    "chunkQuads": 32,
    "materialInstancePath": "Generated/Validation/TerrainSmoke/Materials/MI_terrain_smoke.json"
}
```

- `size` 是 X/Z 全宽，单位 cm；`heightScale`、`heightOffset` 也是 cm。
- `heightScale` 允许为 0 或负值：0 表示平地，权重图的归一化高度固定取 0.5；
  负值表示几何高度翻转。归一化高度 `(height-heightOffset)/heightScale` 会钳制到
  `[0,1]`，因此 float 高度图样本超出 `[0,1]` 时不会让所有层窗口都不命中而静默
  退化为 layer 0。几何高度仍是 `sample*heightScale+heightOffset`，不受该钳制影响。
- `gridResolution` 是总 quad 数，`chunkQuads` 是每块 quad 数；两者必须为正整数，
  `chunkQuads <= gridResolution` 且 `gridResolution % chunkQuads == 0`。
- `heightmapMargin`（可选，0..16，默认 0）是高度图四周的 overscan 采样数。
  大于 0 时高度图必须恰好为
  `gridResolution + 2*heightmapMargin + 1` 方形样本；mesh 的位置仍覆盖
  `size`，但高度/法线、层权重/VLP1 与 Jolt 高度场统一把 mesh UV 映射到
  内圈，边界差分因此能读取 tile 外的一圈邻接样本。margin=0 保持历史
  `[0,1]` 全图采样。
  `heightmapMargin>0` 的资产当前面向离线 PCG tile；编辑器 Sculpt/Paint
  仍按完整高度图 UV 计算笔刷，尚未抵消 overscan。
- `pageOrigin`（可选，默认 `[0,0]`）和 `pageTilesPerSide`（可选，默认 8）决定该地形
  在全局 32×32 page table 中占用的 tile 区域；`pageOrigin + pageTilesPerSide <= 32`。
- `lodLevels`（可选，1..4，默认 1）与 `lodDistances`（可选，cm、严格升序、
  长度 `lodLevels-1`）是 CPU 几何 LOD 的生成/选择参数；缺省距离按地形最长边
  按级数均分（2 级 = 0.5×，4 级 = 0.25/0.5/0.75×）。`chunkQuads` 必须能被
  最粗步长 `2^(lodLevels-1)` 整除。renderer 按加载期驻留窗口生成
  (chunk, LOD)，运行时由 `WorldSnapshotBuilder` 选择。
- `chunkResidencyDistances`（可选，cm、严格升序、长度 `lodLevels-1`）是几何
  chunk 的加载期驻留窗口，见“几何 chunk 驻留（第一版）”；不写时取
  `lodDistances × 1.1`。只有 `lodLevels=1` 时允许空数组。
- 场景里的 `terrain` 对象可以再次覆盖 `pageOrigin` / `pageTilesPerSide`（见“场景对象”）；
  覆盖只改该次摆放的虚拟页区域，不修改 TR 资产本身。
- `heightmapPath`、`materialInstancePath` 都是资源根相对路径；材质在预检阶段至少必须可解析。
- `paintPath`（可选）是资源根相对的 `.vlpaint` paint 快照路径，且要求至少一层
  `layers`。文件不存在是合法状态：此时它只是 Save Paint 的写入目标；文件存在时
  `TerrainAssetLoader` 会按当前层数恢复覆盖层，存在但格式/层数/权重非法则加载失败。
- `layers` 可选：1..256 层。每层必有 `name`、`heightRange`、`slopeRange`；可选
  `noise`、`uvScale`、`uvRotation` 与 L1 新增的 `sourceResolution`（512/1024/2048，
  默认 1024）、`avgColor`（3 个 0..1 线性色分量，默认中性 0.18）、`sourceSet`（0..255，
  默认 0）、`flags`（0..65535，默认 0）。未知字段、越界值与非有限数都会被拒绝；
  旧 TR 不写新字段时行为与默认值不变。
- 每层还可选 `key`（1..256 字符）：跨 TR 共享层的显式身份。缺省为
  `"<assetPath>#<name>"`；同一个 TR 内重复的 `key`（含同名导致的缺省 key
  重复）在资产校验期失败。
- `M_terrain` 使用这些层生成 256×4 `RGBA32F` layer table（catalog），未使用的层
  四行全零；布局与 CPU accessor 见“层 catalog v2”。
- 未知字段、缺失字段、非有限数值、非法范围都会在场景预检阶段失败。

## 编辑器地形编辑

### TR 资产页

`TR_*.json` 现在同时是标准资产文档，命令前缀为 `terrain.*`：
`terrain.get_document` / `get_capabilities` / `modify` / `validate` /
`save` / `revert` / `reload`。它复用 `AssetDocumentService` 的 baseline/draft/
revision、源摘要冲突检查和原子保存，不是第二套文件编辑器。

- 顶层可编辑字段：`name`、`description`、`heightmapPath`、`paintPath`、
  `materialInstancePath`、`size`、`heightScale`、`heightOffset`、
  `gridResolution`、`chunkQuads`、`lodLevels`、`lodDistances`、
  `chunkResidencyDistances`、`pageOrigin`、`pageTilesPerSide`。
- `layers` 既可按整表提交（增删/重排），也可用 `layerIndex` +
  `expectedLayerName` 定位单层后编辑 `name`/`key`/`heightRange`/`slopeRange`/
  `noise`/`uvScale`/`uvRotation`/`sourceResolution`/`avgColor`/`sourceSet`/`flags`。
  层身份在写入前复核；草稿重排后旧索引不会静默写到另一层。
- `validate`/`save` 复用正式 `TerrainAssetValidator`，并解析
  `heightmapPath` 与 `materialInstancePath`；后者必须是 `type:"materialInstance"`。
  这里不导入高度图像素、不准备 Vulkan 资源；World 加载仍走正式事务。
- 资源编辑器里的 Terrain 标签页显示这些字段、层表和 heightmap 预览（PNG/JPG/
  BMP/TGA 直接由 RmlUi 解码；EXR 只显示“preview unavailable”），保存走
  `terrain.save`，重读走 `terrain.reload`。TR 资产页仍不显示层纹理预览；Landscape
  层库的 source-set 预览见下一节。关闭标签不会
  提交草稿，重新打开同一路径仍会看到服务里的未保存草稿。

### Landscape 层库 source-set 预览

Landscape 的 Layers/Paint 层卡使用 CPU 预览，不读取 GPU 纹理，也不把运行时
texture array 当作编辑器资源。解析链固定为：

```text
TerrainAssetDesc.materialInstancePath
  -> MI textures.layerAlbedo / layerNormal / layerPbr
  -> 三张 T_*.json (type: "textureArray")
  -> sources[slice]
  -> layerAlbedo 组可选的 displacementSources[slice]（16-bit grayscale）
```

- `slice` 是**同 sourceSet 中早于当前层的层数**，不是 sourceSet 编号。缺失的
  `sources` 项保留空位，避免后续层滑到下一张图。三张数组必须使用同一个
  `slice`；`M_pbr` 材质、缺少 albedo/normal/pbr 任一阵列、不是 `textureArray`
  或任一源文件缺失/损坏都视为“无预览”，退回 `avgColor`。
- 每个层 source-set 中由 `layerAlbedo` 指向的 `textureArray` 可携带
  `displacementSources`：它与 `sources` 同序、同长度，预览按同一个 `slice`
  取 16-bit 灰度高度图。该字段只服务编辑器层预览，运行时不会上传或替换
  `layerPbr`；`textureAssetValidation` 接受它，但验证时要求与 `sources` 同长。
- Grid/List 卡优先显示 `96×96` RGBA 球体缩略图：圆形 alpha mask，CPU PBR
  （Lambert diffuse + GGX specular + directional key light + ambient，ACES
  近似 tone map）。albedo 提供 base color；normal map 在球面 tangent basis
  上扰动法线；pbr 通道固定为 R=roughness、G=metallic、B=AO。
  球体以 albedo/normal/pbr 三源共同着色，不再生成纯 albedo 高光贴图。
- displacement 主路径是 `displacementSources` 的 16-bit grayscale：用
  `stbi_load_16` 单通道解码并除以 `65535`，保留 `0x8000` 与 `0xFFFF` 的差别，
  用于法线梯度与轻微轮廓半径扰动。只有旧组**没有** `displacementSources`
  时才回退到 `pbr.a`（有效 alpha）或 albedo 亮度近似；声明了该字段但当前
  slice 缺失/无法解码时直接拒绝预览，不静默降到 8-bit 或 `pbr.a`。
  位移仅影响预览，不改写地形高度、TR 字段或运行时纹理数组；运行时
  `layerPbr` 的 A=height 合同保持不变。
- 悬停层显示 `160×112` 无边框平面，3×3 repeat tile 加一层细网格；RmlUi 的三竖线/
  两横线仍叠加在上层。平面同样使用 normal + pbr + 16-bit displacement 着色；
  没有平面路径时保留 `avgColor` 背景并隐藏图片。
- albedo 由 `TextureIO::Load` 以 RGBA、`ForceOff`、sRGB 读取；normal/pbr 以
  linear RGBA 读取，全部不导入 Vulkan；displacement 由独立的
  `LoadTerrainLayerDisplacement` 走 16-bit 解码，也不导入 Vulkan。预览缓存 key
  包含 albedo/normal/pbr/displacement 四个源绝对路径、各自文件大小与
  `last_write_time`、球体/平面尺寸、着色版本与位移参数以及 sphere/plane 种类，输出到
  `resourcePath/Generated/TerrainPreview/PreviewCache/<key>.png`。目标 PNG 只按
  完整 key 复用，重复请求不重写文件（mtime 保持不变）；首次生成仍先写同目录临时
  文件再原子替换。内存缓存按源文件身份复用三张 RGBA8 解码结果与 16-bit 高度场。
- 只对当前可见的层卡请求球体/平面缓存，不遍历 256 层。单个源失败只输出一次
  诊断；预览不修改 `M_terrain`、MI、纹理数组或任何 TR 字段。

### Landscape Sculpt

Landscape 工具只在 authoring（`ScriptPlayState::Editing`）、非 offline、
非 `SC_model_preview.json` 的实时关卡中可用。Play 会销毁编辑会话；编辑期间不允许
Play 修改同一地形。

Sculpt 同时支持带 `layers` 的地形。带层地形的覆盖页权重由
`TerrainPageManager` 按高度/坡度烘焙，因此每次雕塑提交都会在同一
render-thread-idle 边界替换该 placement 的高度图快照，并失效已驻留的覆盖页；
正常情况下下一轮 feedback 会用新高度重新请求、重新烘焙。若页管理更新失败，
dirty 会保留供下一次笔刷提交重试，但失败到重试之间几何已经更新、权重仍使用旧
快照；Diagnostics/Editor 会保留这条失败信息。

- 会话以 `TerrainCollisionInstance.plan` 创建 `TerrainHeightmapEditor`，复制
  高度样本到 CPU；支持 Raise/Lower/Smooth、Circle/Square 笔刷形状、UE 风格
  smooth falloff、归一化 UV 半径、strength 与 falloff。Circle 使用归一化 UV
  的欧氏距离，Square 使用同一归一化 UV 的 Chebyshev 距离
  `max(|du|, |dv|)`；两者共用同一 falloff/strength 计算。笔刷半径在 UI 中以
  cm 输入，按地形 X 世界跨度换算成 UV，因此非正方形地形上的世界足迹仍是椭圆。
  Sculpt 没有 noise；Paint 的 Spray 已按 texel 接入噪声，但旋转与 Stamp 仍未进入
  Sculpt/Paint，属于后续 brush library。
- 拖动左键一次发一个 `BeginStroke`，移动发 `ApplyBrush`，释放发 `EndStroke`；
  Esc/焦点丢失/选择或 World 换代发 `CancelStroke`。一次 stroke 只形成一个
  会话 undo；历史最多 8 条，撤销/重做后全量标记 dirty。
- 每个 dirty chunk 会按全部 LOD 重建 CPU 网格；运行时只替换 World 中实际存在的
  (chunk, LOD) `RenderableObject`（资源 key 与加载期一致），同时更新
  `World::meshObjects` 的 chunk 包围盒和多层 LOD0 组包围盒。被加载期驻留窗口
  裁掉的细 LOD 不会凭空创建。
- 每次提交同时更新该 placement 的 World 高度图快照与页管理器内部的高度图
  副本；带层地形会失效已驻留覆盖页，下一轮 feedback 用新高度重新烘焙权重。
  下一次 Play 的 `ScriptPhysicsWorld::SetTerrainCollisions` 才消费新高度图
  重建碰撞，Play 中不做实时 Jolt body 重建。
- GPU 重建失败时不会清 dirty；下一次笔刷/结束/撤销会重试，并在 Editor/Diagnostics
  中保留失败信息。
- Sculpt 没有 GPU 侧整张高度图上传，也没有与 Scene 文档合并的 Undo；Sculpt 的
  undo/redo 是会话局部的，不进入 Edit 菜单的 Scene 历史。

### Landscape Paint

Paint 面向带 `layers` 的地形，使用独立 `TerrainPaintSession`：

- Landscape 面板的 Paint 组提供 Radius、Hardness、Opacity、Flow 与 Paint/Erase；
  笔刷形状与 Sculpt 共用 Circle/Square 语义（Square 使用归一化 UV 的 Chebyshev
  距离）。`TerrainPaintInput` 携带 `strength`/`falloff`/`opacity`/`flow`/
  `noise`/`noiseSeed`/`shape`：UI 将 `falloff` 映射为
  `clamp(1 - hardness, 0..1)`，并把兼容用的 `strength` 固定为 1；Opacity、
  Flow 与 Spray 各自独立生效，不再合成为单一 strength。
- Flow 是每个 stamp 的推进速度，Opacity 是整个 stroke 的覆盖上限。对足迹内
  每个 texel，`step = falloff * clamp(strength) * flow`，存在 spray 时再乘
  `1 - noise + noise * StableNoise01(x, y, noiseSeed)`。Paint 的
  `targetOpacity = max(currentOpacity, opacity)`，再以
  `current + (target - current) * step` 同时推进 opacity 与 layer weight；
  因此重复 stamp 不会突破当前画笔的 opacity 上限，也不会把已有更高覆盖擦低。
  Erase 保持 weight 不变，只按 `currentOpacity * (1 - step)` 降低覆盖率。
- Spray 噪声按 texel 与 `noiseSeed` 确定性生成，同一 seed 的重绘结果一致；
  其 32-bit 混合公式与 `editor/brush/brushSampler.cpp` 的 `StableNoise01`
  保持一致。选择 Paint 预设会复制 `profile.noise`/`profile.noiseSeed`；手动修改
  Hardness/Opacity/Flow 会保留当前噪声参数，只有重选预设才覆盖它们。
  Sculpt 没有 Opacity/Flow/Spray 语义，只保留自己的 Strength/Falloff；
  两套状态互不影响。旋转与 Stamp 仍未接入运行时 Paint。
  一次左键拖动发一收
  `BeginStroke/ApplyBrush/EndStroke`，Esc/选择变化/Play 发 `CancelStroke`。
- 覆盖层是 `TerrainLayerPaintData`（layer-major 的 weight/opacity；无持久化快照时
  按层数新分配 256²/128²/64² 的空白层）。`TerrainWeightMapBuilder` 在烘焙覆盖率页时
  按 bilinear 混合覆盖值，tile 候选层也会补入被 Paint 命中的层。
- TR 的 `paintPath` 指向资源根相对的 `.vlpaint` 文件。文件存在时，加载期快照成为
  Paint 会话的起始状态（必须与 TR 层数一致、正方形、2..4096 边长），首次笔画不会
  把已保存权重清空；文件不存在时 `plan.layerPaint` 为空，会话按上述分辨率分配空白层。
- 每次提交都会把覆盖层快照写回 World 的 placement（`UpdateTerrainPlacementLayerPaint`），
  因此 Play/Stop 或切换 Actor 销毁会话后，下一次笔画从最新覆盖层继续。只有重新
  加载 World 才会回到 `paintPath` 的文件内容，未 Save Paint 的改动在重载后丢失。
- 提交时在 render thread idle 边界更新 `TerrainPageManager` 的 plan 副本并失效该
  placement 的已驻留覆盖页；feedback 会用新权重重新请求/上传页。当前不在 Play 中
  修改页表。

### Save Heightmap

Landscape 工具栏的 `Save Heightmap` 把当前 Sculpt 快照写回 TR 的 `heightmapPath`：

- `.png` 默认写 grayscale，位深按目标文件现有 PNG 决定：目标已是 16-bit PNG 时
  保留 16-bit，新文件或 8-bit 目标写 8-bit。`TerrainHeightmapWriter::WriteOptions::PngBitDepth::Sixteen`
  可显式写出真正的 16-bit grayscale PNG（IHDR bit depth 16、color type 0、
  大端样本）；`Eight` 保持旧的 8-bit 行为。8-bit 写入路径的字节语义不变。
  `.bmp` 写 24-bit grayscale；`.exr` 先把 R32_FLOAT 高度扩成 RGBA16_FLOAT
  （R/G/B 同值、A=1）再走 `TextureIO` 的半精度 EXR 输出，且不做 [0,1] clamp，
  保留编辑器已有的越界/HDR 高度；其他扩展名明确失败。
- 使用同目录临时文件 + 原子替换；不会改写 TR JSON，也不会自动重新加载 World。
  Paint 覆盖与高度图不是同一份数据，Save Heightmap 不会保存 Paint；Paint 走
  下面的 Save Paint。

### Save Paint

Paint 模式下工具栏的同一按钮显示为 `Save Paint`，把当前会话快照写到 TR 的
`paintPath`：

- 有活动 stroke、会话没有可保存数据或 TR 没有 `paintPath` 时拒绝；缺失路径的反馈是
  `TR has no paintPath; add one to persist terrain paint.`。
- 路径按 `resourcePath / paintPath` 解析，写 `.vlpaint` v1；成功后反馈
  `Terrain paint saved: <path>`。保存不改写 TR JSON、不失效页、不重新加载 World。
- 加载与保存共用 `ResolveTerrainPaintPath`：解析结果按 weak canonical 归一化后
  必须仍位于资源根内，绝对路径、`..` 逃逸以及 junction/symlink 指向根外都会失败。
- 文件格式固定为小端：

| 偏移 | 类型 | 内容 |
| --- | --- | --- |
| 0 | `char[4]` | `VLP1` |
| 4 | `uint32` | `width` |
| 8 | `uint32` | `height` |
| 12 | `uint32` | `layerCount` |
| 16 | `uint32` | `flags`，v1 为 0 |
| 20 | `uint32` | `reserved`，v1 为 0 |
| 24 | `float32[width*height*layerCount]` | layer-major 权重 |
| `24 + 4*N` | `uint8[width*height*layerCount]` | layer-major opacity |

- 读取器拒绝错误 magic、非正方形、边长不在 `[2,4096]`、层数不在 `[1,256]`、
  与 TR 层数不一致、文件长度与头部声明不符、非有限或超出 `[0,1]` 的权重、
  非零 `flags`/`reserved`、尺寸乘法溢出，以及编码后超过 512 MiB 的负载；
  错误信息包含文件路径。`paintPath` 存在但不是常规文件（例如目录）时按损坏
  资产处理，不会静默当作“未绘制”。
- 写入使用同目录临时文件 + 原子替换，失败时清理临时文件。

## 高度图

- `TerrainAssetLoader::Prepare` 通过专用的 `TerrainHeightmapIO::Load` 读取高度图，
  不改写 `TextureIO` 的通用贴图语义。16-bit grayscale PNG 用 `stbi_load_16`
  单通道解码，每个样本除以 `65535` 后写入 `R32_FLOAT`，完整保留 16 bit；
  PNG 16-bit 的大端样本序由解码器处理。
- 16-bit PNG 必须是单通道/grayscale；RGB/RGBA 16-bit PNG 会带路径诊断被拒绝。
  损坏、截断或尺寸非正的 16-bit PNG 同样拒绝，不静默降为 8-bit。当前不支持
  无头部的 raw 16-bit little-endian：TR 没有可信任的宽高来源，不能凭空推断。
- 其他格式完全走 `TextureIO::Load` 原有路径：8-bit PNG/BMP/TGA 按 0..1
  归一化；EXR/HDR 走 float 路径。当前 EXR 沿用 `TextureIO` 的 `RgbaInputFile`，
  因此需要 RGBA EXR；单通道 EXR 尚未支持。
- 高度图至少 2×2；采样使用双线性插值。
- 地形 UV `u` 对应 +X，`v` 对应 +Z；顶点 `x = (u-0.5)*size.x`、
  `z = (v-0.5)*size.y`、`y = sample*heightScale + heightOffset`。
- 高度图不套用材质纹理的固定 V 翻转；翻转会同时改变高度场方向。
- 当前支持 16-bit PNG 解码出的 `R32_FLOAT`，以及 `TextureIO` 实际产出的
  `RGBA32_FLOAT`、`R32_FLOAT`、`RGBA8_UNORM/SRGB`、`R8_*`、`RG8_*` 标量读取；
  其他格式在加载阶段报错。

## 网格

- 每块有 `(chunkQuads+1)²` 个顶点、`chunkQuads²×2` 个三角形、uint32 索引。
- 顶点 UV 是整块地形的 `[0,1]` 全局坐标，留给材质乘 `uvScale`；切线为 `(1,0,0,1)`。
- 法线用全局高度图中心差分：`n = normalize(-dh/dx, 1, -dh/dz)`。相邻 chunk 共享的
  边界采样同一个全局 UV，因此不会出现 chunk 内法线接缝。粗 LOD 的边界环固定使用
  `step=1` 法线；距边界 3 个粗格以内的粗顶点把 `step=2^lod` 法线按 smoothstep
  混向 `step=1` 法线，避免缝合带把法线变化压在一个格子里形成明暗线。
- 每个 chunk 计算局部 AABB，交给 World/渲染快照做视锥与阴影裁剪。
- 索引绕序与引擎的 Clockwise front face 对齐；材质可以使用 Back culling。
- `lodLevels=1` 的旧资产仍只生成/绑定 LOD0，且 LOD0 无额外边界几何、顶点/索引与历史
  实现逐字节一致。`lodLevels>1` 时 renderer 为每个 chunk 生成 0..lodLevels-1 级，
  `lod > 0` 的级别带一圈**边界缝合带**；运行时每帧按相机到 chunk 世界 AABB
  的距离只输出一级。运行时 `TerrainChunkStreamer` 会按相机窗口补齐/卸载
  (chunk, LOD) 并退休其 RenderableObject/逐对象 GPU 资源。
  地表 ID/权重 atlas 的页内容已按需流式驻留，top-4 混合已实现；几何 chunk
  也由加载期窗口 + 运行时 worker 构建共同驻留。

### CPU 几何 LOD 与边界缝合

- 第 `lod` 级的合并步长是 `2^lod`：一个 chunk 变成
  `quadsPerChunk = chunkQuads / 2^lod` 个格子，基础顶点
  `(quadsPerChunk+1)²`、三角形 `quadsPerChunk²×2`。
- 基础顶点的世界位置、UV、`texCoord1` 与 LOD0 同名网格点一致；采样点落在每
  `2^lod` 个 LOD0 网格点上。
- 内部粗顶点的法线用 `step=2^lod` 中心差分；距 chunk 四条边不超过 3 个粗格
  （更粗的 LOD 若整块只有 1~2 个粗格则整块）的顶点，把粗法线按距离 smoothstep
  混向同一位置的 `step=1` 法线。边界环顶点始终是 `step=1` 法线，因此相邻 chunk
  无论 LOD 相同还是不同，共享边界法线都逐位一致。
- `lodLevels=1` 时 LOD0 的顶点/索引与历史实现逐字节一致（无 skirt、索引顺序不变）。
- 没有 skirt。`lod > 0` 的 chunk 在四条边上追加一圈**按 LOD0 间距采样**的边界环
  （`4 × chunkQuads` 个顶点），再用一圈过渡三角形把它连到第一条粗网格线。
  因为任何级别的 chunk 边界都落在同一组 LOD0 采样点上，相邻 chunk 无论差几级
  LOD 都逐点重合，天然水密；构建期不需要知道邻居级别，着色器也不需要 morph。
- 过渡带每侧 `quadsPerChunk × (2^lod + 1)` 个三角形：每个粗区间把 `2^lod` 段细边
  扇形连到粗顶点，再用一个三角形收口。粗 LOD 最外一圈常规四边形因此不再生成，
  改由过渡带覆盖；内部保留 `(quadsPerChunk-2)²` 个格子。
- 基础 `(quadsPerChunk+1)²` 顶点保持原顺序与位置（测试与 LOD 对齐语义不变），
  边界环排在它们之后；粗网格最外一圈顶点因此不被任何索引引用，只占少量内存。
- 过渡带顶点与地表同高，AABB 不变；绕序与内部网格一致（几何法线朝下），
  不会被 Back culling 剔除。
- chunk 资源 key 含 `lod` 与合并步长；`lodDistances` 只影响运行时选择，不进 key。
- `TerrainChunkVertexCount` / `TerrainChunkIndexCount` 的 `stitched` 参数
  （默认 true）按上述公式给出精确值；驻留预算与测试共用同一函数。
- `TerrainLodDistancesCm(desc)`：显式距离表，或按地形最长边按级数均分
  （2 级 = 0.5×；4 级 = 0.25/0.5/0.75×）；`TerrainSelectLodLevel(distanceCm, distances)`
  在达到第 i 个阈值时返回 `i+1`，超过最后一个阈值用最粗一级，负距离按 0 处理。

### 几何 chunk 驻留与运行时 streaming

加载期仍按窗口预生成一批 (chunk, LOD) 作为首帧基线；之后的相机移动由
`TerrainChunkStreamer` 继续补齐/卸载：

- **加载期驻留**：`RendererTerrainLoader::LoadTerrainObject` 在 World 加载时
  决定初始创建哪些 (chunk, LOD) 网格；窗口外的细 LOD 连 CPU 顶点/法线都不生成，
  也不会 `BindRenderableObject` 到 `RendererResourceCache`。
- **运行时流式补齐**：`EngineLoop` 在每个 World snapshot 发布前读取
  `World::GetRenderCamera()`，用 `ComputeTerrainChunkStreamingSet` 计算期望集合。
  最粗一级恒在集合内；更细一级用 residency 窗口 × 1.15 的滞回。CPU
  `TerrainMeshBuilder::BuildChunks` 在独立 worker 线程构建；GT 在渲染线程空闲边界
  创建 `RenderableObject`/挂 `WorldMeshObject`。每帧最多 4 个新请求、2 个 GPU
  上传、16 个在途构建；离开期望集合的 chunk 连续缺席 2 帧后卸载，每帧最多 4 个。
- **卸载与退休**：卸载先 `World::RemoveMeshObject`，再用
  `RendererResourceCache::TakeRenderableObject` / `TakeObjectResource` 取出 GPU
  资源，交给 `ResourceRetireQueue` 按已提交 epoch 释放。
- **选择不变**：`WorldSnapshotBuilder` 仍是唯一 LOD 选择点；它只读 World 里实际
  存在的 `lodLevel` 生成 resident 位图，目标级未驻留时回退到更粗一级，最坏停在
  coarsest。streaming 不另写一套选择规则。
- **范围**：只处理 `lodLevels>1`、存在 terrain collision instance、且至少有一个
  模板 chunk 的 placement；单级地形、无相机/离线路径不进入 streaming。
- **最粗一级永远驻留**：`lodLevels-1` 不受窗口约束，因此任何组都至少有一份
  可绘制网格，不会出现空洞。

窗口与公式：

- TR 可选 `chunkResidencyDistances`（cm，长度必须是 `lodLevels-1`，严格升序正数），
  第 l 级（`0..lodLevels-2`）在“相机到 chunk AABB 最近距离 `<= window[l]`”时创建。
  未写时由 `TerrainChunkResidencyDistancesCm(desc)` 取
  `lodDistances[l] × (1 + 10%)`，与运行时“变粗”阈值同源，避免加载期漏掉刚处在
  滞回带内的目标级。作者显式给出更小的窗口时，初始视角就可能走兜底。
- 距离口径与运行时选择完全一致：chunk 的 LOD0 局部 AABB 变换到世界空间，
  多级资产直接使用 LOD0 AABB（缝合带与地表同高，没有 skirt 下移），
  然后取到相机位置的最近点距离
  （`TerrainChunkWorldBounds` / `TerrainChunkLevelIsResident`）。
- 判定只需要高度范围，不生成法线：`TerrainMeshBuilder::ComputeChunkBounds`
  直接从高度图采样 LOD0 顶点位置范围；`TerrainChunkBuildFilter::chunkMask`
  再让 `BuildChunks` 只生成窗口内的 chunk。mask 只影响“生成哪些 chunk”，
  不影响任何 chunk 的顶点/索引内容，因此不进入资源 key。
- 加载期相机来自场景 JSON 的 camera 对象（`ReadInitialTerrainChunkResidency`），
  只决定首帧基线；之后执行的 `camera pose` 由 streamer 在每个 snapshot 前消费。
  没有 camera / 离线 / 单级地形时，加载期驻留判定关闭，行为回到“全部创建”。
- `WorldSnapshotBuilder` 仍是唯一选择点：先按距离 + 滞回选目标级；目标级不在
  World 里（加载期没驻留）就沿更粗方向找最近的 resident 级，最坏停在 coarsest。
  组内 resident 级位图直接由“World 里存在哪些 `lodLevel`”得到，不另存一份状态。
  阴影、拾取、MaterialInstance 上下文继续消费同一份快照。

控制台 `terrainchunk stats` 输出加载期基线与当前帧绘制：

- `placements / groups / chunks / skippedChunks`，以及实测的
  `vertexBytes / indexBytes / skippedVertexBytes / skippedIndexBytes /
  totalIfAllBytes`（`totalIfAllBytes` = 实际 + 被跳过，等价于不做驻留的全量值；
  被跳过的顶点/索引数是按 `TerrainChunkVertexCount/IndexCount` 的确定性公式估算，
  与 `BuildChunks` 实际输出逐级对齐）。
- 每个 placement 一行：`chunksPerSide / levels / residency / windowsCm`。
- 每级一行：`chunks=实际/总数`、`vertices / indices / bytes / skippedBytes`。
- `draw` 行：`groups`、`drawnChunks`（本帧 LOD 组 draw 数）、`meshDraws`
  （全部 mesh draw，含单级地形）、`drawnByLevel`、`objectsByLevel`。
  单级地形没有 LOD 组，`drawnChunks=0` 而 `meshDraws` 非零是正常的。

2026-10-31 实测（Debug，`--frame-limit 60`，`terrainchunk stats`）：

| 场景 | 组 | 创建/全量 chunk | 实际几何 | 全量几何 | 帧内 draw |
| --- | --- | --- | --- | --- | --- |
| `SC_terrain_lod_near`（20km，3 级，4×4） | 16 | 32/48 | 1 462 272 B | 3 618 048 B | 0:4, 1:8, 2:4 |
| `SC_terrain_lod_mid` | 16 | 20/48 | 428 224 B | 3 618 048 B | 1:4, 2:12 |
| `SC_terrain_lod_far` | 16 | 16/48 | 239 360 B | 3 618 048 B | 2:16 |
| `SC_terrain_chunk16k`（16km，4 级，8×8，512 网格） | 64 | 128/256 | 7 616 512 B | 53 276 672 B | 0:4, 2:32, 3:28 |
| `SC_terrain_b32_32x32`（`lodLevels=1`） | 4 | 4/4 | 586 176 B | 586 176 B | `residency=off` |

16km 场景把 `chunkResidencyDistances` 全部调到 1e9 后得到 `256/256`、
53 276 672 B 的全量基线；两张截图（排除左侧场景名 UI）最大通道差 8、平均
5e-5、无 >8 的采样，说明驻留只改内存不改画面。

### 运行时选择

- `RendererTerrainLoader` 按加载期驻留窗口生成 (chunk, LOD)，`MeshObjectBuildPlan` /
  `WorldMeshObject` 携带 `lodGroupId`、`lodLevel`、`lodLevelCount`、
  `lodDistancesCm` 和 LOD0 组 AABB。组 id 是
  `placementId|chunkX|chunkZ`；非地形/单级地形为空组，行为不变。
- 唯一选择点是 `WorldSnapshotBuilder::Build`。它每帧用相机位置到组世界 AABB 的
  最近距离调用 `TerrainSelectLodLevelHysteresis`，同一组只向 `meshDraws` 输出
  一个 draw。阴影、拾取、MaterialInstance 上下文都消费这一份快照。
- 滞回比例是 10%：从 LOD `n` 变粗要 `distance >= lodDistances[n] * 1.1`，
  变细要 `distance < lodDistances[n-1] * 0.9`。首次选择没有历史时与
  `TerrainSelectLodLevel` 相同。每组的当前级、切换次数和 `previousObjectModels`
  一样只缓存在 builder，不写回 World。
- 组内各级共享同一个 `RuntimeId`（由 `lodGroupId` 派生），级切换不会让编辑器
  选择/描边失配；非地形对象仍由 `objectName` 派生。若某组缺少当前级的可见网格
  （加载期驻留窗口外），builder 回退到 resident 中最接近的更粗一级；只剩
  coarsest 时就画 coarsest，不会留空洞，也不会偷偷把级号报成目标级。
- 控制台 `terrainlod stats` 打印每个组的目标级/总级数、相机距离和累计切换次数；
  `terrainchunk stats` 额外给出 resident 位图、实际绘制级和几何字节基线。
  两者都只在命令执行时输出，不每帧刷屏。

## 场景对象

```json
{
    "name": "Terrain",
    "type": "terrain",
    "terrainPath": "Maps/SC_x/Terrains/TR_x.json",
    "position": [0.0, 0.0, 0.0],
    "rotation": [0.0, 0.0, 0.0],
    "scale": [1.0, 1.0, 1.0]
}
```

`terrainPath` 必填。场景对象身份是 `terrain|name`，与 mesh/light 的重复身份校验一致。
该身份同时作为 `TerrainBuildPlan::placementId` 写入地形计划；同一 TR 因此可以在一个
World 中摆放多次，只要对象名不同。
场景对象可选字段：

- `pageOrigin`：2 个非负整数，每个必须在 `0..31`。
- `pageTilesPerSide`：正整数，必须在 `1..32`。
- 两者同时给出时 `pageOrigin + pageTilesPerSide <= 32`；只给 `pageTilesPerSide` 时，
  与 TR 自带 `pageOrigin` 的组合由 `TerrainPageManager::RegisterTerrain` 按最终区域拒绝。

覆盖值在 `SceneAssetValidator::BuildLoadPlan` 完成 `TerrainAssetLoader::Prepare` 之后写入
`TerrainBuildPlan::desc`；未提供时保持 TR 原值。`placementId` 为空（旧测试/旧资产）时
注册和资源 key 回退到 `desc.assetPath`。
`position/rotation/scale` 通过模型矩阵应用到所有 chunk；chunk 的 `sceneObjectIdentity`
仍是地形对象名，编辑器 actor 树因此把多个 chunk 聚合成一个独立 `Terrain` actor；
该类型只属于编辑器身份/展示，World 中仍是 chunk mesh。

## 材质

- 整块地形使用一个 `materialInstancePath`；当前不按 chunk 或层拆分材质。
- `M_terrain`（`shader/glsl/M_terrain.json`）是 DefaultLit 材质。旧数组快速路径
  绑定三张 `sampler2DArray`：`layerAlbedo`（sRGB）、`layerNormal`（linear）、
  `layerPbr`（linear，R=roughness、G=metallic、B=AO、A=height）；L3 VT 路径另有
  256×4 `layerTable` catalog、`layerPageTable0..4`（RGBA32F 页表）和三张
  `layerAlbedoAtlas/layerNormalAtlas/layerPbrAtlas`（4096²/slice、无 mip）。
  `u_terrainLayerMode` 默认 0，保持旧数组路径逐字节不变；1 才读取层页表/atlas。
- 当前实现是 Phase C2c + P3/P3.1 流式：`RendererResourceLoadCoordinator` 先收集本 World 的全部 terrain，
  通过 `TerrainPageManager` 注册到同一张 **32×32 全局 page table** 和
  **4096×4096 全局 atlas**；page 大小 128×128。地形顶点第二套 UV（`texCoord1`）携带
  `pageOrigin + localUv * pageTilesPerSide` 的全局页表坐标，shader 不再需要 per-terrain
  tile 偏移 uniform。最后一列/最后一行的顶点在生成时按 `TerrainPageResolution=128`
  内缩半个页 texel，保证 `pageOrigin + pageTilesPerSide` 恰好落在边界时不会采到邻居页；
  feedback 读取同一份 `texCoord1`。不同 `pageOrigin` 的地形可以共享这张页表/atlas；
  `pageTable` 是 RGBA8，记录**虚拟页**（tile 在全局页表中的位置）到**物理 atlas 槽位**的映射；
  shader 用 `texelFetch` 查页表，再按 page 坐标采样 `layerIds`（RGBA8，nearest）
  与 `layerWeights`（RGBA8，linear）。每 texel 仍是 top-4 全局层号与权重。
- 物理槽位由 `TerrainPageManager` 从最低可分配槽位给出，淘汰后进入 CPU quarantine，
  到期后可被任意虚拟页复用；
  页表位置（`virtualPageX/Y`）注册后不变，atlas 位置（`physicalPageX/Y`）只在 resident
  期间有效。**全部 32×32 物理槽位（1024）都可分配**：不再为缺页保留任何保底物理页。
  缺页由 shader 的 `invalidPage` 分支直接输出中性表面，不依赖保底页内容；虚拟 `(31,31)`
  是普通页，`pageTilesPerSide=32` 的单地形因此可以注册。
- `TerrainPageManager` 的注册身份是 `placementId`（场景对象身份），不是 `assetPath`。
  同一 `assetPath` 可以注册多次；同一 `placementId` 重复注册、或虚拟区域重叠都会在
  `RegisterTerrain` 抛错，错误信息同时带 `assetPath` 与 `placementId`。所有公开页 API
  的第一个字符串参数都是 `placementId`。
- `RegisterTerrain(preload=true)` 仍是测试/小场景兼容入口：逐页请求常驻，容量不足或候选页
  被 pin/引用时 `RequestPage` 返回 false，注册继续并把次数记入
  `Stats::preloadDeferredCount`，不会死循环。生产 renderer 在 P3 改为
  `RegisterTerrain(preload=false)`：注册只建立虚拟页与物理槽位身份，不烘焙页面。
  每个 placement 只预取中心 2×2 页（clamp 到地形范围）作为初始可见集合，其余页由
  feedback 逐帧请求。resident budget 的**下限**是
  `TerrainPageManager::DefaultResidentCapacity` = 512（实测 2km/32×32 宽视野
  working set 约 415 页）。World 注册完成后，loader 会把容量抬到
  `min(max(512, 已注册虚拟页数), UsablePhysicalSlotCount)`——4×4 tile × 8×8 页
  = **1024 个虚拟页**，`UsablePhysicalSlotCount` 也是 1024（全部 32×32 槽位可分配；
  缺页由 shader 直接输出中性表面，不再保留任何保底物理页），因此容量取 1024。
  **这条必须成立**：容量若停在 512，LRU 会在世界内部切出
  一条直线，线外的层 ID/权重读成 0、地形退化为默认层（实测近半张图无材质）。
  预算不足时安全窗内的页仍不会被淘汰，未驻留可见页保持中性表面；
  `terrain capacity N` 可手动覆盖。
- renderer 先用零数据创建 page table/atlas。加载期初始上传走
  `TerrainPageManager::BuildUploadBatch` 收集 dirty 并把页表项、ID atlas 页和权重
  atlas 页打包进一个 staging buffer；`RecordTerrainPageUploads` 在**同一个**
  command buffer 里完成三张 image 的 `ShaderReadOnly -> TransferDst -> ShaderReadOnly`
  与全部 region copy，随后一次 submit + fence wait，再 `CommitUploadBatch`。
  因此三张 image 是 1 次提交，而不是每张 image 各提交一次。`BuildUploadBatch` 只
  收集并 pin 涉及页，不直接清 dirty；`CommitUploadBatch` 只清 revision 未变的槽位，
  `AbortUploadBatch` 保留全部 dirty、只解除本批新加的 pin。`maxAtlasPages` 非 0 时按
  物理槽位升序截断，并把指向被截断槽位的有效页表项一起留下，保证 GPU 页表不会先
  指向尚未上传/已被清零的 atlas 页；剩余 dirty 在下一批继续。
- 每帧上传使用 `TerrainStreamingState::uploadRing`（`TerrainUploadGpuRing`）：
  槽数等于后端 frame fence 数，host-visible/coherent、常驻映射、预分配，不每帧创建/
  销毁 buffer。`RenderSystem` 在 `BeginFrame`（已等待本 frameIndex fence）之后、
  scene pass 之前执行 `BuildUploadBatch(maxUploadPagesPerFrame)`，把 batch 打进当前槽位，
  再用 `RecordTerrainPageUploads` 录制进**主帧 command buffer**。`SubmitFrame` 成功后才
  `CommitUploadBatch`；录制/提交抛异常时 `TerrainUploadTransaction` 析构 `Abort`，保留
  dirty 并只释放本批 pin。每帧最多上传
  `TerrainPageManager::MaxUploadPagesPerFrame` = 8 个 atlas 页，剩余 dirty 下一帧继续。
  默认路径没有 `device.waitIdle()`，也没有逐页 submit。
- P3 的 `ApplyFeedback` 在 feedback 回读完成后执行：已注册命中页调用
  `MarkPageUsedAtEpoch(copiedEpoch, priority)`，同时推进 `lastSeenEpoch`（安全窗）
  与 `lastTouchedEpoch`（epoch-aware LRU recency）；已注册 miss 页进入跨帧 `pendingPageRequests`
  队列，按 priority 降序、同优先级按 deferred 次数轮转，每帧最多
  `MaxPageRequestsPerFrame` = 8 个请求。请求调用 `RequestPageAtEpoch(completedEpoch)`；
  失败项保留到下一帧并记录 `deferredRequests`。未注册虚拟页（例如 M_pbr 地形）只计数、
  不请求、不抛错。pending 队列每帧按当前 feedback 可见页清理：相机移走后不可见的旧
  miss 不再占用烘焙预算，重新可见时会再次入队。新请求页在同帧 `BuildUploadBatch`
  中上传。只有同时具备 `TR.layers` 且材质绑定 `layerTable` 的地形才设置
  `terrainFeedbackEligible`；layerless 的 M_pbr/M_unlit 地形不进入 feedback，
  也不在 `TerrainPageManager` 注册，因此不会误推进其他地形的页。
- 物理槽位淘汰不再清零/上传 atlas：`EvictPageAt` 只让页表失效并清理该槽位未发布的
  atlas dirty；atlas 旧内容保持到 quarantine 结束。quarantine 规则是
  `releaseEpoch = max(lastSeenEpoch, gpuLastSubmittedEpoch) + framesInFlight + 1`，
  `AllocatePhysicalSlot` 只复用 `releaseEpoch <= completedEpoch` 的槽位；没有安全槽位时
  `RequestPageAtEpoch` 返回 false，不强制覆盖。`ReclaimExpiredQuarantines(completedEpoch)`
  会把已到期的隔离槽位真正转成空槽位并同步 `free/quarantine` 计数；`AllocatePhysicalSlot`
  开头和每帧 stats 发布前都会调用它，因此不需要等下一次分配才回收。
  `ImmediatelyAllocatablePhysicalSlotCount()` 表示当前可立即分配的空槽位；仍被
  resident 占用或仍在 quarantine 的槽位都不计入。没有永久保留的槽位。
- P0 已把 `TerrainPageManager`、page table/atlas 的 GPU texture 和 World 共享层表收进
  `TerrainStreamingState`，随 `RendererResourceCache::WorldLocalResourcePackage` 一起发布；
  coordinator 不再使用函数局部 page manager。完整 World 候选创建新 state，MI 预览与
  Blueprint 扩展候选都只读共享 active state：预览只替换材质/MI，若清空 state 会让
  feedback ring、页表和驻留状态静默失效。`TerrainStreamingState` 另有
  `pendingUploadRevision`（CPU 内容版本）与 `uploadedRevision`（成功写 GPU 的版本），
  上传成功后只前进不回退。
- page manager 具备 streaming 状态字段与 API：每页 `lastSeenEpoch`/`lastTouchedEpoch`/
  `contentRevision`/`uploadedRevision`/`uploadPinCount`，`MarkPageUsedAtEpoch`、
  `CanEvictAt`、`PinPageForUpload`/`UnpinPage`、`SetGpuEpochContext`、
  `RequestPageAtEpoch`、`SetResidentCapacityAtEpoch`，以及带安全窗的
  `EvictLeastRecentlyUsed(completedEpoch)`。有效安全窗是
  `EvictionWindowFrames() = max(2, framesInFlight + 1)`，同时覆盖 feedback 回读的
  2 帧延迟与提交队列中的 in-flight 帧；判断式是
  `lastSeenEpoch + EvictionWindowFrames() <= completedEpoch`。
  epoch-aware 淘汰先比 priority（低者先淘汰），再比 `lastTouchedEpoch`（旧者先淘汰），
  再比 `lastSeenEpoch`/旧 `lastUsedFrame`；feedback 命中会真正改变淘汰顺序。
  被引用或 pin 的页不能被淘汰。`uploadPinCount` 是计数而不是 bool：重叠上传批次
  各自持有一份 pin，任意一批 Commit/Abort 只释放自己那一次。
- `RequestPageAtEpoch` 是 streaming 入口：容量/物理槽位不足时只淘汰已过安全窗的页，
  没有安全候选就返回 false；旧 `RequestPage` 保留给 preload/加载期，仍走无窗口 LRU。
  新驻留页的 `lastSeenEpoch` 取本帧已提交 epoch（`currentFrame+1`，与 feedback
  frameTag 同源）；加载期还没有提交过时退回已消费 feedback epoch。
- `RenderSystem` 在每帧 `BeginFrame` 后**先**把 `feedbackSubmittedEpoch`、
  `TerrainStreamingState::lastConsumedFeedbackEpoch` 和后端 fence 数写入
  `SetGpuEpochContext`，再消费 feedback 并调用 `ApplyFeedback`；这样本帧新驻留页
  拿到的始终是本帧提交 epoch，而不是上一帧已消费值。
  `terrain capacity N` 在帧首通过 `SetResidentCapacityAtEpoch` 消费：降预算只淘汰
  已过安全窗的页；窗口内没有安全候选时保持超预算，不会为了回到预算淘汰仍在
  in-flight 帧里被采样的页。
  旧 `SetResidentCapacity` 仍保留无窗口 LRU 语义，只供加载期/测试使用。
  本帧请求页在 `BuildUploadBatch` 中被 pin，不会同帧被淘汰；淘汰出的物理槽位
  始终走 quarantine，不会让 in-flight 帧采样到被覆盖的 atlas。
- page table 的无效项是 `A=0`。shader 缺页时把页坐标 remap 到物理 `(31,31)` 只是为了让
  采样坐标保持在 atlas 界内；该槽位现在是普通可分配槽位，其内容随后被缺页掩码替换，
  不参与最终输出。缺页输出是中性表面：baseColor 0.18、roughness 0.9、metallic 0、AO 1、
  几何法线/切线。有效页的 `invalidPage=0`，输出与原采样路径逐位一致。
- `layerTable` 是 256×4 `RGBA32F` LUT：row0 height/slope、row1 noise/blendEdge/
  `uvScale`/`uvRotation`、row2 `pagesPerSide`/`pageTableBase`/`sourceSet`/`flags`、
  row3 `avgColor`/reserved。mode 0 只消费 row1（v 坐标 `0.375`）的每层 UV 参数；
  L3 VT 路径还消费 row2 的 `pagesPerSide` 与 row3 的 `avgColor`（缺页 fallback），
  层权重来自 ID/权重图。
- page table/ID/权重 atlas 和 layer table 都由 World 级共享资源提供；同一 World 内带 layer
  的地形必须使用完全相同的 layer 定义（`AreLayersIdentical`），否则加载失败。这样共享 MI
  不会被不同层定义静默覆盖；多 biome 需要后续 global layer catalog/ID 方案。
- terrain loader 会校验 `TR.layers.size()` 不超过 `layerAlbedo/layerNormal/layerPbr` 三张
  数组的 `arrayLayers`，越界在 GPU 绘制前失败；`u_pageTableSize`/`u_pageResolution` 会按
  page manager 的实际值强制写入，MI 不能覆盖成不匹配的尺寸。
- `u_uvScale` 控制层 UV 的全局平铺；`u_pageTableSize` 默认 32，`u_pageResolution`
  默认 128；`u_debugMode`：
  `0` 正常着色，`1` top-4 权重，`2` top-4 层号，`3` 混合 albedo，
  `4` VT 物理页号，`5` VT mip（4/5 在 mode 0 下输出 0）。
- 当前地形只支持 realtime 渲染；offline-only 渲染遇到 terrain 直接失败。
- 地形碰撞已接入 World/Play：`WorldBuilder` 为每个 terrain placement 保留 plan+TRS，
  脚本 Play 在既有 `ScriptPhysicsWorld` 里创建静态 heightfield；编辑器/普通预览
  不创建物理世界。当前仍是整张地形一个 body；Sculpt 提交会更新高度图与分层
  覆盖页，碰撞仍只在下一次 Play 重建，Play 中不做实时 body 重建。编辑器已有
  TR 资产页、layered Sculpt/Paint 与高度图写回；分块碰撞仍未实现。
- page manager/GPU page 资源由 world-local `TerrainStreamingState` 管理；chunk 资源 key
  （`TerrainChunkResourceKey`）包含 `placementId`、TR 路径、高度图路径、size、
  heightScale/offset、grid/chunk 参数、`lod`/`lodStep` 与 chunk 坐标。同一 TR 的两处
  摆放因此不会共享或覆盖彼此的顶点/索引缓冲。

## 全局 layer catalog 与多 sourceSet（S1/S2）

S1 把“一个 World 只能有一套完全相同层定义”的限制换成 World 级全局层注册表；
S2 允许不同 biome 绑定各自的层源数组，但共享同一组层 atlas。

- `TerrainLayerRegistry`（`source/terrain/terrainLayerRegistry.*`）按场景对象顺序
  遍历所有带 `layers` 的 terrain placement：
  - 身份是 `layer.key`，缺省 `"<assetPath>#<name>"`；`sourceSet` 不参与 key；
  - 同 key 且参数一致 → 复用同一个 globalId（跨 TR 去重）；
  - 同 key 但参数不一致 → 加载失败，错误里同时给出两个 `assetPath`；
  - 不同 key 即使内容相同也不合并；
  - 分配顺序 = 场景对象顺序 × 每 TR `layers` 顺序首次出现，因此单 TR World
    的 `globalId` 恰好等于局部下标，旧 catalog/ID 图/截图逐字节不变；
  - 全局层上限 256（ID/权重图是 RGBA8）；超出、同 TR 内 key 重复、
    同一 placement 重复注册都会在加载期失败。
- `TerrainPageManager::RegisterTerrain(plan, layerIdMap, preload)` 保存每个
  placement 的 `local -> global` 映射。`TerrainWeightMapBuilder::Build/BuildPage/
  BuildVirtualTexture` 接受同一映射：权重与 top-4 排序仍按局部层（层参数与噪声
  种子是局部语义），写进 ID 图的层号与对外暴露的候选层列表是 **globalId**。
  空映射等价恒等映射，旧调用点行为不变。
- `RendererResourceLoadCoordinator` 用 registry 替换旧的
  `AreLayersIdentical` 直接抛错：registry 在注册任何页之前构建，冲突不会留下
  半注册状态。catalog、`TerrainLayerPageManager::ConfigureLayers`、
  `TerrainLayerVisibilityConfig::layerCount` 都使用全局层列表。
- `TerrainLayerSourceSets`（`source/terrain/terrainLayerSources.*`）是
  `map<sourceSet, TerrainSourceSetRecord>`。每个 record 保存
  albedo/normal/pbr `shared_ptr`、array layer 数、各图 mip 数/尺寸/格式，以及
  `layerSlices`：**global layerId → 该源数组的 slice 下标**。
  - 同一 `sourceSet` 必须始终指向同一组 `shared_ptr`，否则加载失败；
  - 不同 `sourceSet` 允许不同数组与不同 `sourceResolution`；
  - 层页上传按 `TerrainLayerPageManager::LayerSourceSet(layerId)` 选 record，
    再用 `layerSlices` 取源 slice；三张 atlas 仍只有一组，因此所有 set 的
    三张图格式必须一致（多格式组留给 S5）；
  - `TerrainLayerVisibilityConfig::sourceSets` 让每层按自己那一组源的 mip
    上限 clamp，而不是用全 World 的最小 mip。
- `terrainlayerstats` 增加
  `globalLayers / sourceSets / dedupedLayers / capacity`。

当前边界（S1/S2 不做）：>256 层与 16-bit ID（S3）、跨 World 持久化
registry / 页内容去重 / 编辑器全局层列表（S4）、多 atlas 格式组（S5）。
mode 0 仍按 globalId 直接采样每个 MI 绑定的 `sampler2DArray`：多 biome
mode 0 要求该 MI 的数组按 globalId 排布，否则应使用 mode 1；mode 1 的
source slice 由 `layerSlices` 还原，不受该限制。

## 层 catalog v2 与 CPU 层页管理器（L1）

L1 实现 CPU 数据与决策；L2 把层页真正送进 GPU；L3 让 shader 在
`u_terrainLayerMode=1` 时消费 catalog/页表/atlas。默认 mode 0 仍直接按 layerId
采样三张 `sampler2DArray`。

- `TerrainLayerTable::BuildImage` 输出 256×4 `RGBA32F`，行优先、每行 256 个 texel：
  - row0 = `heightMin / heightMax / slopeMin / slopeMax`
  - row1 = `noise / blendEdge(0.05) / uvScale / uvRotation(rad)`
  - row2 = `pagesPerSide(mip0) / pageTableBase / sourceSet / flags`
  - row3 = `avgColor.r / avgColor.g / avgColor.b / reserved(0)`
- `pagesPerSide(mip0) = sourceResolution / 128`：512→4、1024→8、2048→16。
  `pageTableBase` 按层顺序累加每层总页数：512² 层 21 页、1K 层 85 页、2K 层 341 页。
  这些整数在 float32 的 24-bit 尾数内精确可表示（256 层 2K 共 87,296 项）。
- `TerrainLayerTable::ReadLayer(image, layerId)` 是 CPU 往返 accessor；未使用层
  （`pagesPerSide == 0`）返回全零条目，`layerId >= 256` 抛 `std::out_of_range`。
  `AreLayersIdentical` 现在也比较 `sourceResolution`、`avgColor`、`sourceSet`、`flags`。
- 每层 mip 链固定按 128² 页切分：512 为 mip0..2（4/2/1 页/边，21 页），
  1024 为 mip0..3（8/4/2/1，85 页），2048 为 mip0..4（16/8/4/2/1，341 页；
  mip4 是 128² 尾页）。helper 在 `terrainAssetTypes.h`
  （`TerrainLayerMipCount` / `TerrainLayerPagesPerSide` / `TerrainLayerTotalPages`）。
- `TerrainLayerPageManager`（`source/terrain/terrainLayerPageManager.*`）管理
  `TerrainLayerPageKey { layerId, mip, pageX, pageY }` 到物理页号的映射：
  - 虚拟页号（设计 §4.4 的按 mip 分表）：
    `layerId * mipStride(mip) + pageY * pagesPerSide(mip) + pageX`。
    `mipStride(mip)` 是该 mip 下所有已配置层的最大 `pagesPerMip`，但当前实现
    按 256 层容量计算页表宽度/stride；全同分辨率时页表可能比“仅实际层数”的
    最小布局更大（例如全 512² 仍按 256 层布局）。CPU 与 shader 使用同一 stride，
    不同层的页号不会别名，只是页表内存不是最小。512/1K/2K 混用时取最大值。
    `VirtualPagesPerLayer`/`VirtualPageTableSize` 给出 L2 建页表所需的槽位数。
    另有 `FlatPageIndex` = `pageTableBase + mipBase + pageY * side + pageX`，
    对应 catalog 的扁平基址，供 L2 选择单表变体。
  - 物理页池可配置（默认 1024），最后一个槽位固定为 fallback，
    永不参与普通分配或回收；物理页号按 4096²/slice、32×32 页平铺，
    `DecodePhysicalPage` 给出 `(slice, tileX, tileY)`。
  - 驻留/淘汰语义与 `TerrainPageManager` 对齐：resident set、priority、
    `lastSeenEpoch`/`lastTouchedEpoch`、`referenceCount`、计数式
    `uploadPinCount`、`EvictionWindowFrames() = max(2, framesInFlight+1)`、
    `RequestPageAtEpoch` 安全窗、`SetResidentCapacityAtEpoch` 降预算、
    物理页 quarantine（`max(lastSeen, lastSubmitted) + framesInFlight + 1`）。
  - `BuildUploadBatch(maxPages)` 只收集“常驻且 `contentRevision > uploadedRevision`”
    的页，按物理页号升序输出并 pin 涉及页；每条 `PageUpload` 携带
    `key`、per-mip 的 `virtualPageIndex`、`physicalPage` 与 `revision`。
    `CommitUploadBatch` 只在页仍驻留、物理页与 revision 都未变时推进
    `uploadedRevision`；`AbortUploadBatch` 只解除本批 pin，保留脏状态。
  - L2 起 `BuildUploadBatch` 还输出 `pageTableUpdates`：常驻页的有效映射
    （R=physical、G=mip、B=1、A=priority）和淘汰页的失效项（四通道全零）。
    页映射版本 `pageTableRevision`/`pageTableUploadedRevision` 与像素内容版本
    分开；被 `maxPages` 截断的页不会先写页表，避免 GPU 指向尚未上传的
    atlas 页。
- 层页内容不在本层生成：`PageUpload` 不含像素，
  L2 才从源纹理做 GPU copy。L1 没有新增 runtime test flag，测试全部在
  `terrain_tests` 内。

## 层 atlas / 页表 / catalog 的 GPU 资源（L2）

L2 把层页与页表上传到 GPU；L2 自身不改变默认视觉路径，L3 才增加显式的
`u_terrainLayerMode=1` 采样分支。默认 mode 0 仍读取 `layerAlbedo/Normal/Pbr`
三张数组，因此 L2 的 atlas 可以只被创建、清空、写入和统计。L4 才会用 feedback
驱动层页驻留。

- World-local `TerrainStreamingState` 新增 `layerPageManager`、层源纹理描述和
  `TerrainLayerGpuResources`。它们随 `WorldLocalResourcePackage` 一起发布/退休；
  MI 预览与 Blueprint 扩展候选继续共享同一个 state，不会 reset。
- GPU 布局：
  - catalog：复用 `TerrainLayerTable::BuildImage` 的 256×4 `RGBA32F`。
  - 页表：每 mip 一张 `RGBA32F`、nearest、ClampToEdge、无 mip。宽度上限 4096，
    高度 = `ceil(VirtualPageTableSize(mip) / width)`；1K 层 mip0（16384 项）为
    4096×4，没有该 mip 的层得到 1×1 占位表。
  - atlas：`layerAlbedoAtlas`、`layerNormalAtlas`、`layerPbrAtlas`，都是
    `sampler2DArray`、4096×4096、无 mip，slice 数 = `ceil(physicalPageCount/1024)`。
    格式分别跟随源层纹理（例如 BC7 sRGB / BC5 / RGBA8）；物理页按
    `slice = page / 1024`、`tileX = page % 32`、`tileY = (page % 1024) / 32` 平铺。
- 页表 texel 编码：`R = physicalPage`、`G = 该虚拟页的有效 mip`、
  `B = valid(1)/invalid(0)`、`A = priority`。无效项四通道全零，shader 只能通过
  B 通道区分“有效映射”和“应走 fallback”。
- 层源纹理从 `M_terrain` 的 `layerAlbedo/layerNormal/layerPbr` 捕获；L2 要求
  一个 World 内所有 layer-table 地形共享**同一组** shared_ptr 纹理，否则加载失败
  （不同 `sourceSet` 暂不支持）。源图尺寸来自 backend 创建记录，
  catalog 的 `sourceResolution` 不参与 L2 的 copy 尺寸计算。
- 切页是 GPU→GPU `vkCmdCopyImage`：源 subresource = `(mip, layerId)`，
  源偏移 = `pageX/Y * 128`，目标 subresource = `(0, physicalSlice)`，
  目标偏移 = `tileX/Y * 128`。三张 atlas 的 region 各自按源 mip 的真实尺寸裁剪：
  现有验证资产只有 32×32 源图，因此 copy extent 可能小于 128×128，atlas 其余像素
  保持 clear 的零值。源/目标 image 都做了
  `ShaderReadOnly ↔ TransferSrc/TransferDst` 往返，录制在同一个 command buffer。
- 空 atlas 初始化走 record-only 路径，不构造 CPU 零图：
  - 非块格式：`vkCmdClearColorImage(0,0,0,0)`。
  - BC 格式：`vkCmdClearColorImage` 不允许压缩格式，改用一个 16 KiB 零页
    staging buffer 反复 copy 到每个物理页；buffer 活到 submit 完成之后。
- 上传路径：`BuildUploadBatch` → 页表 texel 打包到本次加载期 staging →
  同一 command buffer 录制“页表 copy + 三张 atlas copy” → 一次
  `BeginSingleTimeCommands/EndSingleTimeCommands` → 成功后 `CommitUploadBatch`。
  失败走 `AbortUploadBatch`，保留 dirty 与 invalidations；L2 没有逐帧上传 ring，
  也不在帧循环里创建/销毁 buffer。
- mode 0 初始测试集合：每个 layer 请求源 mip 链的尾页 `(page 0,0)`，请求前调用
  `SetGpuEpochContext(completedEpoch, framesInFlight)`；不去请求每层 85/341 页的全链。
  源 mip 不足时夹到可用 mip。
- 控制台 `terrainlayerstats` 打印层页 `resident/capacity/physical/free/
  quarantined/requests/hits/misses/evictions/uploadedPages/uploadedBytes/
  dirtyPages/dirtyPageTable`，以及 `atlasReady/slices/bytesPerMap/atlasBytes` 与三张
  atlas 的 `vk::Format` 字符串。它只读统计快照，不触发请求或上传。

L2 **没有**实现：shader 的层 VT 采样（由 L3 补齐）、层页 feedback/预取驱动驻留
（L4）、低分辨率 fallback/淡入、逐帧上传 ring、同一 TR 多摆放的层页内容去重、
不同 `sourceSet` 的多组源纹理。L3 另补了 catalog `sourceResolution` 与实际源图
尺寸的一致性校验。

## 层纹理 VT 采样（L3）

L3 在 `M_terrain` 内增加 `u_terrainLayerMode` 运行时分支；默认 0 仍走旧数组路径，
1 走 `top-4 ID → catalog → 每 mip 页表 → 物理页 → atlas`。L4a 起 mode 1 的页驻留
由覆盖页 feedback 驱动（见下一节）；L3 的加载期全量预取只保留为调试对照命令。

- 材质 schema：`M_terrain.json` 保留 `layerTable` 与旧
  `layerAlbedo/layerNormal/layerPbr`，新增 `layerPageTable0..4`（`sampler2D`）、
  `layerAlbedoAtlas/layerNormalAtlas/layerPbrAtlas`（`sampler2DArray`）。新增 float
  参数 `u_terrainLayerMode`（默认 0）、`u_layerPageResolution`（128）、
  `u_layerAtlasResolution`（4096）、`u_layerPagesPerAtlasSlice`（1024）、
  `u_layerTilesPerAtlasSide`（32）、`u_layerMaxMip`（4）。新纹理在 M_ 资产里带
  非空占位 default，保证旧 MI 不写新 binding 时仍能通过
  `MaterialDescriptorSchema::ValidateInstanceValues`；`RendererTerrainLoader` 在
  地形路径把它们覆盖为 World 级共享资源。`MaterialDescriptorSchema` 只使用现有
  `sampler2D`/`sampler2DArray` + float 参数，不需要 bindless 或整数采样器。
- 层 UV 与 mode 0 一致：catalog row1 的 `uvScale/uvRotation`，`context.texCoord`
  先乘 `u_uvScale`；mip 用层 UV 的 `dFdx/dFdy`：
  `texelsPerPixel = max(|ddx|,|ddy|) * pagesPerSide(mip0) * u_layerPageResolution`，
  `mip = clamp(round(log2(texelsPerPixel)), 0, maxMip)`，其中
  `maxMip = min(log2(pagesPerSide(mip0)), u_layerMaxMip)`。mip 由层自身纹素密度
  决定，不使用 atlas UV 的导数。
- `u_uvScale` 的**唯一来源是地形的 `M_terrain` 材质实例**。`RendererTerrainLoader`
  在注册第一个 layer-table 地形时把该值写进 `TerrainStreamingState::layerUvScale`
  并打印 `[TerrainLayer] u_uvScale=<v> resolved from M_terrain instance (<TR>)`；
  同一 World 内所有 layer-VT 地形必须给出相同的值，否则加载期直接抛错。
  `terrainFeedback` pass 的材质实例**不是**权威来源：`RenderSystem` 每帧把
  `layerUvScale` 推给 pass 材质，因此 `MI_terrainFeedback.json` 里不要重复写
  `u_uvScale`（写了也不生效，只会造成两个来源的错觉）。
- 虚拟页：`pagesPerSide(mip) = max(1, pagesPerSide(mip0) >> mip)`；
  先 `fract(layerUv)` 实现 repeat，再按该 mip 的 `pagesPerSide` 切页。页表 texel
  总量正好是 `perLayerStride(mip) * 256`，所以 shader 用
  `stride = textureSize(pageTable,0).x * y / 256` 还原 CPU 的
  `TerrainLayerPageManager::VirtualPagesPerLayer`，不需要额外 stride uniform。
  虚拟页号是 `layerId * stride(mip) + pageY * pagesPerSide + pageX`，
  texel 坐标是 `(virtualPage % width, virtualPage / width)`；mip 0..4 用 if/else
  选择页表，不做 sampler 数组动态索引。
- 页表 texel 语义沿用 L2：`R=physicalPage`、`G=mip`、`B=valid`、`A=priority`。
  `B<0.5` 时该层使用 catalog row3 `avgColor` + roughness 0.9 / metallic 0 / AO 1
  和几何法线，绝不回退到 layer 0。物理页映射：
  `slice = physicalPage / u_layerPagesPerAtlasSlice`、
  `tileX = (physicalPage % u_layerPagesPerAtlasSlice) % u_layerTilesPerAtlasSide`、
  `tileY = (physicalPage % u_layerPagesPerAtlasSlice) / u_layerTilesPerAtlasSide`；
  atlas UV = `(tile * u_layerPageResolution + localUv * u_layerPageResolution) /
  u_layerAtlasResolution`，localUv 先按半个页 texel 内缩。三张 atlas 共用同一
  物理页号。采样固定 `textureLod(..., 0.0)`：每个 mip 是独立 atlas 页，
  atlas 本身没有 mip，不能做页内隐式 mip。
- 绑定：`RendererTerrainLoader` 在 `usesLayerTable` 分支把
  `layerPageTable0..4`/三张 atlas 绑到 MI，并写入 128/4096/1024/32/4；不覆盖作者
  的 `u_terrainLayerMode`。L2 atlas 在首个 layer-table MI 载入后创建，随后立即绑定。
  若 MI 显式请求 mode 1 但 layer atlas 尚不可用，加载期失败而不是静默退回。
- `sourceResolution` 校验：catalog 的每层 `sourceResolution` 必须等于绑定数组纹理
  的真实边长，且三张源图必须是同尺寸正方形。默认 mode 0 下不一致只打印一次
  warning（现有 B32/M3 的 32² 资产就是这种情况）；若 MI 设置
  `u_terrainLayerMode=1`，加载期直接抛错并指出 TR 路径，禁止把 32² 当成 1K 采样。
- mode 1 的加载期只请求每层尾页（与 mode 0 相同的初始集合），随后由覆盖页
  feedback 逐帧请求；`terrainlayer preload` 仍可显式请求每层每个 mip 的全部页，
  作为与 L3 全量预取基线对照的调试路径。

## 层页 feedback 驱动驻留（L4a）

L4a 复用 P2/P3 的 128×128 覆盖页 feedback，不再新增 pass 或 runtime test flag。
可见层推导是纯 CPU 的 `TerrainLayerVisibilityResolver`
（`source/terrain/terrainLayerVisibility.*`）：

- 对每个有效覆盖页记录，`TerrainPageManager::FindPageByVirtual` 找到 placement/tile；
  未注册虚拟页跳过并计入 `skippedCoverPages`。
- 覆盖页的 top-4 ID/权重来自 `TerrainPageManager::ReadPageLayers`：常驻页直接读
  CPU atlas 镜像；非驻留页用同一 `TerrainWeightMapBuilder::BuildPage` kernel 临时
  重建（不写 atlas、不改变驻留状态）。重建每帧最多
  `maxCoverPageRebuildsPerFrame`（默认 4）张，其余下一帧再试。
- 每个 texel 的 4 个槽位按层号累加权重并除以 texel 数，得到该层的页内平均覆盖率；
  低于 `minLayerWeight`（0.05）的层丢弃，最多保留覆盖率最高的 4 层。
- 一个覆盖页的层集合按 `(placementId, tileX, tileZ)` 缓存——地形内容静态，同一页
  只需烘焙/统计一次；缓存上限是 World 的虚拟页数。
- mip 规则：覆盖页 `priority` 是“每页占多少屏幕像素”的对数压缩，越高越近。
  `>=192 → mip0`、`>=128 → mip1`、`>=64 → mip2`、`>=32 → mip3`、其余 `mip4`，
  再按该层 `TerrainLayerMipCount(sourceResolution)` 与三张源数组的最小 mip 数
  clamp。2K 层的 mip4 只有 1 页，远景必须能请求到，否则会永久停在 avgColor。
- 层纹理是平铺资产，选定 mip 后必须请求该 mip 的**全部**页
  （512² mip0 = 4×4 = 16 页）；否则 shader 的 UV 采样会落到未请求页。
  同一（layer, mip, page）在多个覆盖页重复出现时按最大 priority 去重。
- 层页 priority = 覆盖页 priority × `(0.5 + 0.5 × 层覆盖率)`，压回 0..255；
  大面积近景层优先驻留。
- 每帧最多 `maxLayerPageRequestsPerFrame`（16）个 `RequestPageAtEpoch`；失败项进入
  跨帧 `pendingLayerPageRequests`，按 priority 降序、同优先级按 `deferredRequestCount`
  轮转，不会丢失。
- 续命范围只覆盖本帧选中的完整页集：覆盖页路径里是“选中 mip 的全部页”，
  精确生产路径里是预算分配后每个选中 `(layer, mip)` 的全部页；
  `layerPageBudget=0` 的对照路径才是屏幕真正采样到的层页。不再刷新
  “可见层的所有历史 mip”——
  那会让每帧把整层所有 mip 刷成最新，capacity 对可见集完全失效（实测
  capacity 64 时 resident 长期停在 248）。
- 淘汰：`idleEvictionEpochs`（默认 180 epoch）之外未再被看到的层页在
  `EvictPage` 前先过 `CanEvictPageAt`（GPU 安全窗）；随后若
  `ResidentCount() > ResidentCapacity()` 再做 epoch-aware LRU 淘汰。
  这是软预算：**只要可见 working set 大于预算就持续超预算**（实测可见集 20，
  `capacity 8` 时为 `20/8` 并保持不变），capacity 只约束不再被本帧 feedback
  命中的页；不会为了立刻回到预算淘汰仍在采样或 in-flight 的页。
  `terrainlayer preload` 会关闭 idle 淘汰，`terrainlayer capacity N` 重新打开。
- 已驻留页如果不是本帧候选、但所属层仍在视野内（同层其它 mip、同 mip 未被采样的
  页），使用更短的 `unselectedMipIdleEvictionEpochs`（默认 16 epoch）而不是 180；
  完全离开视野的层仍走长 idle，避免相机小幅移动就丢掉整层。
- `TerrainLayerDesc::avgColor` 默认中性灰 0.18（旧默认是 0，mode 1 缺页会出纯黑块）。
- 逐帧上传：`TerrainLayerUploadGpuRing`（`source/render/resource/rendererTerrainLoader.*`）
  槽数 = frame fence 数，host-visible/coherent、常驻映射、预分配，只打包页表 texel
  （16 B/项，容量按所有 mip 页表项总数上界）。`RenderSystem` 在 scene pass 之前
  `BuildUploadBatch(maxLayerPageUploadsPerFrame)`（默认 8 页）→ 写入当前槽位 →
  `RecordTerrainLayerPageUploads` 把页表 copy 与三张 atlas 的 GPU→GPU copy 录制进
  主帧 command buffer；`SubmitFrame` 成功后 `TerrainLayerUploadTransaction::Commit`，
  异常/未提交由析构 `Abort` 保留 dirty 与 pin。不使用 `device.waitIdle()`，
  不每帧创建/销毁 buffer。
- 加载期同步入口 `ApplyTerrainLayerPageUploads` 仍保留给初始小集合与测试；它和逐帧
  路径共用同一套 region 生成与记录函数。
- 页表失效项与截断不变量沿用 L2：`maxPages` 截断时，指向被截断物理槽位的有效页表项
  一起延后，GPU 页表不会指向尚未上传的 atlas 页。
- shader 侧：选中 mip 的页表项无效时，`mf_terrainLayerVT.glsl` 会向更粗的 mip
  回退（最多到该层 `maxMip`），用第一条有效映射采样；全部无效才使用 catalog
  `avgColor`。这样逐帧收敛期间只是分辨率暂时偏低，不会整页变平均色。
- `terrainlayerstats` 第二行区分 `feedbackHits`（本帧命中并续命的已驻留页）、
  `requested`（本帧尝试请求数，含失败/deferred）、`accepted`（其中成功数）、
  `uploadedLast`、`pending` 与 mip0..4 的请求分布；第一行不再打印恒为 0 的
  `hits`（那是 `RequestPage` 命中已驻留页的次数，调用方在请求前已过滤）。
  `terrainlayer capacity N` / `terrainlayer preload` 都由 Console 写 atomic/flag，
  渲染线程帧首消费。

## 层页精确 feedback（L4b）

覆盖页 feedback 只能给出“页坐标 + priority”，mip 只能按 priority 近似。L4b 在同一个
`terrainFeedback` pass 上增加 4 张颜色附件，按 top-4 slot 逐像素报告真正的层页身份。

- 资源：`terrainLayerFeedback0..3`，各固定 128×128 `R8G8B8A8_UNORM`、
  `colorAttachment + transferSrc`；与覆盖页 RT 共用同一张 128×128 `D32_SFLOAT`
  深度（`less`），因此同一 texel 仍只记录最近的地形页。
- 编码（`source/terrain/terrainLayerFeedback.h`）：
  `R = layerId`、`G = pageIndex`（`pageY * side + pageX`，0..255；2K mip0 正好 256 页）、
  `B = bit0..2 mip | bit7 valid`、`A = frameTag`（与覆盖页同源，`(epoch % 255) + 1`，
  0 保留）。无效/关闭时四通道写 0。
- shader 与 `M_terrain` 共用 `shader/glsl/materialFunction/terrainLayerVTMath.glsl`：
  层 UV、导数选 mip、`fract` 切页、`pageIndex` 全部是同一份实现（CPU 参考是
  `terrain/terrainLayerVT.h`）。feedback 报告的是 shader **想要**的 mip，不是缺页
  回退后的粗 mip，否则更细的 mip 永远不会被请求。覆盖页缺失（`pageTable.a < 0.5`）
  时四张附件保持 invalid，层页驻留先由覆盖页驻留建立。
- 输入：pass 现在有 4 个 `worldTexture` 输入——`terrainPageTable`（binding 0）、
  `terrainLayerCatalog`（1）、`terrainLayerIds`（2）、`terrainLayerWeights`（3）。
  World 没有分层地形时后三者临时指向 page table，由 `u_layerFeedbackEnabled=0`
  保证不被采样。编译器合同：1..4 个 worldTexture 输入（binding 0..N-1）、
  1..5 张固定 128×128 RGBA8 颜色输出、恰好 1 张 D32 深度输出。
- CPU：`TerrainLayerFeedbackGpuRing`（`source/render/resource/terrainLayerFeedbackGpu.*`）
  每帧一张 host-visible/coherent/cached buffer（4×16 KiB；无 cached 型时退回
  coherent），4 张图在一次录制里
  `ColorAttachment → TransferSrc → copy → ColorAttachment`，epoch 门禁复用
  `TerrainFeedbackRingState`。`SummarizeTerrainLayerFeedback` 丢弃 valid=0 与
  frameTag 不匹配的 texel，按 `(layerId, mip, pageIndex)` 去重，用帧内最大
  sampleCount 归一到 1..255 的 priority。聚合使用 flat count 表而不是
  `std::map`；65,536 texel 的逐项树查找会让 CPU 回读固定多出几十毫秒。
- 导数作用域：GLSL 规定 `dFdx/dFdy` 在非 uniform 控制流里未定义，而
  `miss` 与 top-4 权重都是逐片元值。shader **无条件**算完 4 个 slot 的
  layerUv/mip/页号（固定 4 次迭代、无 break/continue），最后只用权重与
  enabled/miss 掩码选择写出的 slot；sampler 不做动态索引。
- 导数分辨率换算：feedback pass 固定 128×128，而 `M_terrain` 在全屏分辨率上
  取导数。`u_feedbackScaleX/Y = screen/128` 把 feedback 的每像素变化换算回主视图
  尺度（1280 宽时约 3.3 级 mip），否则层页 mip 会被系统性选粗、驻留欠配。
- `TerrainLayerVisibilityResolver::ApplyExact` 把 pageIndex 还原成 `(pageX,pageY)`，
  越界项计入 `outOfRangePages`。生产路径（`layerPageBudget != 0`）先按
  `(layer, targetMip)` 归并可见需求，再走“全局层页 mip 预算”一节；只有
  `layerPageBudget = 0` 的对照/测试路径才逐页请求，并在中心页缺页时按
  `exactFeedbackNeighborhoodRadius`（默认 1）扩一圈做 prefetch（邻域 priority
  减半），已驻留时不扩。两条路径的命中页都会推进
  `lastSeenEpoch`/`lastTouchedEpoch`，未驻留页进入与 L4a 相同的跨帧队列，
  每帧请求/上传预算、quarantine、Commit/Abort 语义完全一致。
- 精确 feedback 可用时它就是唯一驱动层页驻留的路径；L4a 的覆盖页推导只在
  ring 还没产出（启动、回读延迟）或精确 feedback 饥饿（见下一节）时作为
  回退/prefetch，不与精确路径同时写驻留。

## 目标 mip 缺页时的粗 mip 预取（L4c）

`M_terrain` 在目标 mip 的页表项无效时会向更粗 mip 回退（L4a 已有行为）；
但只有更粗 mip 的页也在驻留集里，回退才真的有内容可采，否则仍然是
catalog row3 的 `avgColor`。生产预算路径会为每个 `(layer, targetMip)` 需求
保留一个不低于目标的完整页集，因此回退目标是预算选中的更粗页集；下面这套
逐页粗 mip 扩容用于 `layerPageBudget = 0` 的对照/测试路径：

- 入口是 `TerrainLayerVisibilityResolver::ExpandCoarserFallbackCandidates`，
  L4a（覆盖页推导）与 L4b（`layerPageBudget = 0` 的精确 feedback 对照路径）
  共用；生产预算路径不调用它，避免绕过去重页集预算。
- 只对**缺页**的目标页扩容：目标页已驻留时不加粗页，否则候选集会凭空
  变大、capacity 被“假可见”页撑爆。
- 每粗一级的页坐标是 `(pageX >> level, pageY >> level)`，与 shader 在
  `TerrainLayerAddressAtMip` 里按 `pagesPerSide >> mip` 切页的结果一致。
- 级数与优先级：`TerrainLayerVisibilityConfig::fallbackPrefetchMipLevels`
  （默认 2，0 关闭）；`+1` 级 priority ×3/4、`+2` 级 ×1/2、更粗 ×1/4，
  最低 1。粗页只保证回退有内容可采，不得抢占细页的请求名额。
- 级数还会被该层允许的最大 mip 夹住（层自身 mip 链与源数组 mip 数取小），
  因此 mip 已经是最粗一级时不会产生额外候选。
- 预算不变：扩容只增加候选/pending 队列长度，每帧 `maxRequestsPerFrame`
  与 `maxLayerPageUploadsPerFrame` 仍严格生效。
- `terrainlayer stats` 新增回退与 mip 分布统计：`fallbackSlots`（本帧目标页
  缺页的 feedback texel 数）、`fallbackPages`（缺页目标页的 distinct 页数）、
  `fallbackLevels=+1/+2/+3`（本帧新加入的粗 mip 页数）、`fallbackPixels~`
  （`fallbackSlots / exactSlots × 当前渲染分辨率`，是“会走回退路径的像素数”
  上界估计，不是测量值）、`targetMips=0..4`（有效 feedback texel 的目标 mip
  分布）。这些值只在手动执行 `terrainlayer stats` 时打印，不逐帧刷屏。

本阶段的 shader 仍是硬切换；L4e 才加入按 mip 差的 `avgColor` 混合（见下一节）。

## 全局层页 mip 预算（生产路径）

层页是平铺资产，shader 的 UV 可能落到该 mip 的任意页；因此每个选中的
`(layer, mip)` 都是“全部页”的固定成本。多地形场景中同一全局层会在近处和
远处同时出现，shader 对不同像素会报告不同目标 mip。若每个 `(layer, targetMip)`
都独立按屏幕像素请求，7 个 1K tile × 4 层全部选 mip0 需要 1792 页，超过默认
`ResidentCapacity=512` 3.5 倍，每帧“请求 -> 淘汰 -> 再请求”无法收敛（实测
CPU 600 ms/帧）。生产路径由 `TerrainLayerMipBudget`
（`source/terrain/terrainLayerMipBudget.*`）在容量内统一分配：

- 输入是精确 feedback 归并出的 `(layer, targetMip)` 需求；同一层可以有多个
  目标 mip。需求的 priority 取该 `(layer, targetMip)` 在本帧反馈里的最大
  sampleCount 归一值。透视投影下近处地形占更多屏幕像素，因此高 priority 是
  “更近、应优先清晰”的排序键。
- 每个需求先从该层允许的最粗 `maxMip`（层自身 mip 链与 sourceSet 取小）起步；
  按 priority 降序依次向 `targetMip` 细化。细化一级只有在去重后的
  `(layer, mip)` 页集总页数仍不超过 `layerPageBudget` 时才提交；预算不足就停在
  当前 mip，继续下一个需求。
- 分配绝不越过 feedback 目标：shader 不会采样更细的 mip，给更细页只会白占
  容量。同一层的近处目标 mip0 与远处目标 mip3 可以同时保留，分别请求
  mip0 与 mip3 的完整页集。
- 每个参与分配的可见层始终保留其最粗 mip 的完整页集（所有支持分辨率下都是
  1 页）。128×128 feedback 欠采样漏掉某个远处目标 mip 时，shader 仍能沿
  mip 链回退到这张有内容的尾页，而不是 `avgColor`；成本是每层 1 页。
- 去重后的每个 `(layer, mip)` 都必须请求全部页并在本帧
  `MarkPageUsedAtEpoch`；旧的、不再被预算选中的 pending 请求会从队列删除，
  已驻留旧页交给 idle 安全窗淘汰。
- `RenderSystem` 每帧把 `layerPageBudget` 设为层页管理器的
  `ResidentCapacity()`。分配器同时写 `budgetExceeded`：如果所有需求落在最粗
  mip 仍然超过容量（例如容量被 console 降到低于可见需求数），调用方停止
  细化；已驻留选中页不淘汰，空槽不足的请求留在 pending，而不是继续淘汰
  选中页追预算。
- 选中页集通过 `TerrainLayerPageManager::SetProtectedPages` 排除在容量/LRU
  淘汰之外。正常预算内，页集加载完成后 resident 不再抖动；工作集大于容量时
  也不会出现“刚请求就被淘汰”。容量若在页集加载后降低，驻留会暂时超预算，
  直到可见页集缩小或用户恢复容量。
- `layerPageBudget = 0` 只保留给对照/测试，生产路径不会使用。

控制台 `terrainlayer stats` 的预算行：
`pageBudget`（容量上限）、`budgetPages`（去重页集总页数）、
`budgetSets`（去重 `(layer,mip)` 页集数）、`budgetClamped`（因预算不足停下的
细化次数）、`budgetExceeded`（最粗工作集仍超预算）、
`budgetEntries=0..4`（可见 `(layer,targetMip)` 需求最终落到的 mip 分布）、
`budgetMips=0..4`（最终驻留页集分布）。近处需求应优先出现在更小的 mip 编号。

## 精确 feedback 饥饿保护（L4d）

精确层 feedback 只有在覆盖页 resident（`pageTable.a >= 0.5`）时才会写出有效
texel。相机贴地/极端陡视角、地形被近裁剪面裁掉、或覆盖页整帧缺页时，一整帧
可能**没有任何有效 texel**。旧实现把它当成“所有层页都不可见”，180 epoch 后
把驻留集清空，覆盖页恢复后要重新请求/上传，这段时间里缺失页显示 `avgColor`。
L4d 在 `TerrainLayerVisibilityResolver` 里增加饥饿保护：

- 判定：`summary.validTexelCount < TerrainLayerVisibilityConfig::minValidFeedbackTexels`
  （默认 1，即只有完全空的精确 feedback 才算饥饿；调大可以覆盖只剩几个有效
  texel 的欠采样帧）。
- 饥饿帧：`ApplyExact` 只累加 `TerrainLayerFeedbackHealth::starvedFrames`，
  **不**续命、不入队、不淘汰；`lastSeenEpoch`/`lastTouchedEpoch` 一律不动，
  不会把“没看见”伪造成当前帧。pending 请求队列原样保留，恢复后按原优先级继续。
- idle 时钟：`starvationCreditEpochs = min(starvedFrames, starvationGraceFrames)`
  （默认 480 帧）直接加到 idle 窗口上，等于在宽限期内暂停老化；
  `CommitCandidates` 同时跳过 budget 淘汰，驻留集保持上一帧。
- 覆盖页回退：`ApplyTerrainLayerExactFeedback` 在饥饿时返回 false，
  `RenderSystem` 继续执行 L4a 的 `Apply`，用覆盖页 RT 重新估计可见层；
  `TerrainLayerVisibilityStats::usedCoverFallback` 标记这一帧的可见性来自回退。
  覆盖页重新 resident 后 shader 会再次写出有效 slot，精确路径自动接管。
- 慢速降级：连续饥饿超过 `starvationGraceFrames` 后补偿封顶，于是
  `idleEvictionEpochs + starvationGraceFrames` 之后驻留集开始老化；淘汰按
  `starvationEvictionPagesPerFrame`（默认 2 页/帧，idle 与预算淘汰共享名额）
  限速，既不会一次性清空，也不会永久锁死显存。
- 恢复：任一有效帧把 `starvedFrames` 清零、推进 `lastValidFeedbackEpoch`，并按
  `starvationCreditDecayPerFrame`（默认 1）逐帧衰减残留补偿；一次长时间饥饿
  不会永久放大 idle 窗口。
- 统计：`terrainlayerstats` 新增 `starvedFrames` / `lastValidEpoch` /
  `usedCoverFallback` / `idleGrace`，只在手动执行命令时打印。

## 层纹理质量：强制 mip、回退混合与相机预取（L4e）

### 强制目标 mip（调试入口）

- `M_terrain` 与 `M_terrainFeedback` 都有 `u_layerForceMip`（float，默认 `-1`，
  范围 `-1..4`）。`-1` 表示按屏幕导数自动选 mip；`0..4` 直接覆盖自动结果，
  再按该层实际 mip 链（`min(pagesPerSide 推导, u_layerMaxMip)`）clamp。
- Console：`terrainlayer forcemip N`，`N` 只接受 `-1` 或 `0..4`，其余打印
  usage 且不排队；`-1` 恢复自动。命令只写
  `TerrainStreamingState::layerForceMip`（atomic），渲染线程在帧首把值写入
  已登记的 `M_terrain` 实例与 feedback pass 材质。
- 一致性：shader 的目标 mip 覆盖规则在
  `materialFunction/terrainLayerVTMath.glsl::TerrainLayerTargetMip`，materials
  与 feedback pass 共用；feedback 报告的是**覆盖后的目标 mip**，不是回退后
  的粗 mip，否则 CPU 驻留会与 GPU 采样错位。CPU 参考实现是
  `terrain/terrainLayerVT.h::ResolveTargetMip`。
- 只影响 `u_terrainLayerMode = 1`；mode 0 不读该参数，像素不变。

### 回退混合

- 当实际采到的 `usedMip` 比 `targetMip` 更粗时，按 mip 差混合：
  `blend = clamp(0.2 * (usedMip - targetMip), 0, 0.6)`。差 0/1/2/3+ 级为
  `0/0.2/0.4/0.6`；完整驻留时 blend 为 0，采样路径与 L4b 逐字节一致。
- 混合对象：albedo → catalog row3 `avgColor`；pbr → 中性值
  `roughness 0.9 / metallic 0 / AO 1`（与无效页一致）；normal → 几何法线
  （粗 mip 不存在的细节按同一比例丢弃）。混合后的法线仍归一化。
- 目标 mip 与所有更粗 mip 都无效时仍整页 `avgColor`（混合等价于 1），
  绝不回落到 layer 0。`fallbackBlendPages` / `avgColorPages` 分别统计这两类
  目标缺页页数。
- CPU 参考：`terrainLayerVT.h::FallbackBlendForMipGap`；shader 为
  `TerrainLayerFallbackBlend`。测试锁定 0/1/2/3 级与上限。

### 相机前向预取

- 触发：渲染线程每帧比较本帧与上一提交帧的相机位置。前向位移
  `dot(delta, forward) > 0.5 * |delta|` 且 `|delta| > layerPrefetchMinForwardCm`
  （默认 5 cm）时置 `layerPrefetchMoving`；横向平移/转头不触发。
- 规则：`TerrainLayerVisibilityResolver::PrefetchFinerMipPages` 对本帧精确
  feedback 里**已驻留**的可见层页 `(layer, mip, x, y)`，请求细一级 mip
  `mip-1` 的 2×2 子页 `(2x+dx, 2y+dy)`。相机靠近时目标 mip 只会变细，
  提前一帧把子页放进 atlas 可以避免掉到粗页。
- 有界：每帧最多 `maxLayerPrefetchPagesPerFrame`（默认 4）个请求，名额独立
  于 feedback 的 `maxLayerPageRequestsPerFrame`，并且放在可见页提交之后执行，
  不会挤掉本帧真正要采样的页。每个请求仍走 `RequestPageAtEpoch` 的容量、
  quarantine 与 GPU 安全窗，不绕过预算。
- 当前页本身缺页时不预取细页（本帧已经在回退，细页会先占掉目标/粗页需要的
  物理槽位）；`mip == 0` 或达到该层最大 mip 时没有更细级别，直接跳过。
- 这是“细一级”的近似，不是基于视锥/距离的完整预取：不做地形 tile 级预测，
  也不按世界方向旋转到各层的 `uvRotation` 空间。

### 统计

`terrainlayer stats`（手动执行）新增一行：
`forceMip` / `prefetchMoving` / `prefetchPages` / `prefetchRequests` /
`prefetchAccepted` / `fallbackBlendPages` / `avgColorPages`。预取请求不计入
`mipRequests`（那是本帧可见页请求），也不计入 `feedbackHits`。生产预算路径的
`pageBudget` / `budgetPages` / `budgetSets` / `budgetClamped` /
`budgetExceeded` / `budgetEntries=0..4` / `budgetMips=0..4` 另有一行，见
“全局层页 mip 预算（生产路径）”。

## Feedback pass（P2/P3）

P2 增加了一个**独立的小 pass**，只产出可见页反馈；P3 由 CPU 端
`TerrainPageManager::ApplyFeedback` 消费它来请求驻留页：

- RenderGraph 资源 `terrainFeedback`：固定 128×128、`R8G8B8A8_UNORM`、
  `colorAttachment + transferSrc`；另有固定 128×128 的 `terrainFeedbackDepth`
  （`D32_SFLOAT`、`depthStencilAttachment`）。pass 类型 `terrainFeedback`，
  单颜色 + 单深度、无 MSAA、不写 swapChain/GBuffer。编译器要求
  `depthTestEnable/depthWriteEnable=true`、`depthCompareOp=less`，深度资源必须精确为
  `D32_SFLOAT`；会拒绝其他尺寸、非 D32 深度格式、MSAA、缺失/多余深度、
  第二个颜色输出和非 `worldTexture` 输入。
- draw domain 只遍历 `RenderDrawPacket::terrainFeedbackEligible`。该标记由
  `RendererTerrainLoader` 显式写入为 `usesLayerTable && TerrainUsesVirtualPages(plan)`：
  只有真正绑定 layer table 且声明了 `TR.layers`、已注册到 `TerrainPageManager` 的地形
  才进入 feedback。layerless 的 M_pbr/M_unlit 地形不写该标记，不会把可见页反馈
  误送给同 World 的分层地形。标记沿 `MeshObjectBuildPlan -> WorldMeshObject ->
  MeshDrawSnapshot -> RenderDrawPacket` 传播；不按材质名或资产路径猜测地形。
- feedback pass 复用每个 terrain draw 的 Set 2 `UBOModel` 来取 model 矩阵；
  因此地形材质必须让 Set 2 保持与 `commonUbo.glsl` 的 `UBOModel` 完全一致
  （当前 `M_terrain`/`M_pbr`/`M_unlit` 都满足）。若未来给地形 Set 2 增加
  skin/额外 SSBO，必须同时扩展 feedback 的 set 2 layout，否则描述符集不兼容。
- feedback 现在有深度测试（`less`）：每个 texel 只记录最靠近相机的可见页。
  这修复了“后绘制的远页覆盖近页 miss，导致近处页面永远不驻留”的缺口；
  被遮挡页不再进入统计。
- RenderGraph 当前统一使用 `0.2/0.2/0.2/1` 的 clear 值，所以非地形 texel 的 R 是 51、
  valid bit 为 0。CPU 靠 `valid` 位区分，不假设 clear 像素是零。
- 顶点直接使用地形 `texCoord1` 的全局页坐标，fragment 只读
  `terrainPageTable`（RenderGraph `worldTexture` 输入，与 `M_terrain` 同一张 32×32
  page table）；不采样 ID/weight atlas。
- `TerrainMeshBuilder` 已把整个地形的最后一列/最后一行顶点按半 texel 内缩到
  `pageOrigin + pageTilesPerSide - 0.5/128`。因此正常地形范围末端不会再采到邻居页；
  shader 仍保留全局 UV 恰为 1.0 时的半 texel 处理作为边界兜底。页索引按
  `floor(texCoord1 * pageTableSize)` 计算并 clamp 到 `0..size-1`；feedback 的
  `u_pageResolution` 由 `RenderSystem` 按 `PageResolution` 写入，使用同一顶点 UV，
  保证统计页与实际采样页一致。
- texel 格式版本 1（纯 CPU 定义在 `source/terrain/terrainFeedback.h`）：
  `R[0..4]=virtualPageX`、`R.bit5=miss`、`R.bit6=valid`、`R.bit7=0`；
  `G[0..4]=virtualPageY`；`B=priority`；`A=frameTag`。`frameTag = (submittedEpoch % 255) + 1`，
  0 保留为未写入；32×32 page table 恰好需要 5+5 bit，扩展到 64×64 必须升级格式版本。
- `TerrainStreamingState::feedbackRing` 是 World-local 的 host-visible/coherent/cached ring
  （无 cached 型时退回 coherent），
  槽数等于后端 `GetFrameFenceCount()`（当前 2），常驻映射、不每帧创建/销毁 buffer。
  帧末在所有 scene pass 之后把当前 swapchain image 的 `terrainFeedback` 从
  `ColorAttachmentOptimal` 转到 `TransferSrcOptimal`，`CopyImageToBuffer` 后转回
  `ColorAttachmentOptimal`；下一帧 render pass 仍以 `Undefined/clear` 取得内容。
- CPU 只在 `BeginFrame` 已等待该 frameIndex fence 后调用 `TryConsume`，并要求
  `copiedEpoch <= completedEpoch`；成功后立即清 pending，因此同一槽位只解析一次。
  未完成的槽位不会被读取。`SummarizeTerrainFeedback` 用固定 32×32 页槽聚合；
  直接逐字节解析未缓存 BAR 会让 64 KiB 回读产生约 11 ms 的 CPU 开销，因此
  readback ring 必须优先分配 cached 内存。该 ring 随
  `WorldLocalResourcePackage` 一起退休。
- 控制台 `terrainfeedback` 打印最近一次已完成回读的 `epoch/tag/validTexels/
  distinctPages/missPages/maxPriority/staleTexels` 和前 8 个高优先级页。
  `terrain stats` 打印驻留/预算/虚拟页/可立即分配空槽位/隔离槽位/请求/
  `requestHits`/未命中/淘汰/preload deferred/累计与最近一帧上传页数，以及最近一次
  ApplyFeedback 的 `feedbackHits`/missPages/queued/attempted/accepted/deferred/
  unregistered/pending。`requestHits` 只表示 `RequestPage` 命中；GPU 可见页命中应看
  `feedbackHits`（即 ApplyFeedback 的 markedPages）。stats 发布前会先回收已到期的
  quarantine，因此 `freeSlots`/`quarantined` 反映当前 completedEpoch 的真实状态。
  `terrain capacity N` 只把预算写入 streaming state 的 atomic，由渲染线程在帧首消费；
  它不在 Console/GT 线程直接修改 page manager。

P3/P3.1/L1/L2/L3/L4a/L4b/L4c/L4e **没有**实现：低分辨率 fallback 页（L4e 只做
`avgColor` 混合与细一级 mip 预取，没有降采样兜底页）、视锥/地形 tile 级预取、
硬预算淘汰（只要可见 working set 大于预算就持续超预算，capacity 只约束非可见页）、
同一 TR 多摆放的层页内容去重、不同 `sourceSet` 的多组源纹理、
Play 中实时碰撞重建。地形碰撞的 heightfield 模块已接入
World/Play（见下节），但仍是整张地形一个 body；Sculpt 的碰撞更新只在下次 Play
生效。
CPU 页面烘焙（`TerrainWeightMapBuilder::BuildPage`）仍在 `RequestPageAtEpoch` 内同步执行；
每帧 8 页是数量预算，不是时间预算，重载/快速移动时单帧仍可能出现烘焙耗时峰值。

## Jolt 碰撞（heightfield）

`source/terrain/terrainCollision.h/.cpp` 把一份已解码的 `TerrainBuildPlan` 变成
静态 Jolt `HeightFieldShape` body，加入**调用方已有的** `JPH::PhysicsSystem`；
它不创建第二套物理世界，也不改动渲染路径。

- 入口：`CreateTerrainCollisionBody(JPH::PhysicsSystem&, const TerrainBuildPlan&,
  const TerrainCollisionPlacement&, const TerrainCollisionSettings&)`。
  `TerrainCollisionPlacement.position` 单位 cm，`rotation` 单位度，顺序
  `Y*X*Z`（与 `CommonFunction::RotationToQuat` 一致），`scale` 无量纲；
  模块内部统一乘 `0.01` 进入 Jolt 米制。
- 局部空间：X/Z 以地形中心为原点，范围 `±size*scale/2`；高度为
  `heightmap * heightScale * scale.y + heightOffset`，与 `TerrainMeshBuilder`
  的顶点口径一致（u 对应 +X、v 对应 +Z）。placement 的旋转/平移由 body
  transform 承担，缩放烘焙进 heightfield 的 offset/scale，避免依赖 Jolt
  非均匀 body scale。
- 采样：默认 `sampleCount = TR.gridResolution + 1`，向上取整到 Jolt
  `blockSize(2)` 的倍数，保证覆盖范围不因向上取整而多出一圈样本；它与渲染网格
  **近似**同分辨率，采样点可能错开半格，起伏地形上碰撞表面与可见网格可有厘米级差异。
  允许 `[4, 4096]`。样本经 `TerrainHeightmap::SampleHeightCm` 双线性读取，
  与 CPU 采样同源。高度压缩用 `mBitsPerSample=16`、`mBlockSize=2`。
- body 固定为 `EMotionType::Static` + `EActivation::DontActivate`，
  `mAllowSleeping=false`、`mFriction=1.0`。object layer 默认 `0`
  （与 `ScriptPhysicsWorld::StaticLayer` 对齐）；`TerrainCollisionSettings`
  的 `objectLayerCount` 默认 2，`objectLayer >= objectLayerCount` 在创建前拒绝，
  调用方仍必须保证该层已在 PhysicsSystem 的 `ObjectLayerPairFilter` 中注册。
- `TerrainCollisionBody` 是 RAII：析构/`Reset()` 先 `RemoveBody` 再 `DestroyBody`，
  重复调用安全；句柄必须早于对应 PhysicsSystem 销毁。
  `GetBodyIdValue()` 返回 Jolt BodyID 的索引+序列号打包值（索引会被复用，
  序列号保证同一 world 内不重复），只用于日志/断言。
- 输入错误在创建 body 之前抛出 `std::invalid_argument`（空高度图、
  `<2x2` 高度图、非正/非有限 size 或 scale、`sampleCount` 越界、
  `bitsPerSample` 越界、非有限高度）；不会留下半创建的 body。
- 已知三角化差异：Jolt heightfield 每个 quad 沿 `(x,y)-(x+1,y+1)` 对角线切分，
  渲染网格沿另一条对角线；非平面 quad 内两者表面存在细微差异（平面/斜坡为 0）。
- `TerrainCollisionWorld` 是独立测试/离线夹具，内部持有唯一一个 PhysicsSystem
  （静态/移动两层配置与 `ScriptPhysicsWorld` 相同），提供 `AddTerrain`、
  `RaycastDown`、`Step`，只服务于模块测试和离线工具。
- Jolt 注册：`source/physics/joltRegistration.h/.cpp` 提供进程级
  `Physics::EnsureJoltRegistered()`，地形碰撞与 `ScriptPhysicsWorld` 共用同一份
  Factory/类型注册；任何入口都不得自行调用 `RegisterTypes()`。Jolt 的
  `Allocate()` 依赖 `RegisterDefaultAllocator()` 写入的函数指针，因此持有 Jolt
  分配器的对象必须保证注册先于任何成员分配完成。
- World 数据：`WorldBuilder` 用
  `BuildTerrainCollisionInstances(SceneAssetBuildPlan, sceneJson)`
  （`source/world/loading/terrainCollisionInstances.*`）从每个 `terrain`
  对象计划取 `TerrainBuildPlan`，并从作者场景 JSON 取 position/rotation/scale，
  写入 `World::GetTerrainCollisionInstances()`。同一 TR 多次摆放按
  `placementId = terrain|<name>` 各保留一份。缺少地形计划、TRS 非有限、
  scale 非正或 placementId 重复都会让 World 加载失败，不会拖到 Play。
- Play 生命周期：`ScriptRuntimeSubsystem::Control` 在每次 Play 创建
  `ScriptPhysicsWorld` 后调用 `SetTerrainCollisions(World instances)`；
  Stop / 重新 Play / World 换代都会销毁旧 body 再重建。编辑器与普通加载
  不创建物理世界，所以不会生成地形碰撞。`ScriptPhysicsWorld` 内
  `TerrainCollisionBody` 声明在 `JPH::PhysicsSystem` 之后，析构时先于物理世界。
  `SetTerrainCollisions` 是替换语义，空集合只清空；创建失败时旧集合保持有效。
- 查询身份：`ScriptPhysicsWorld::Raycast` / `ProbeGround` 命中地形时返回
  placementId（Raycast 额外带 `terrain=true`），不伪装成蓝图 id。接触/触发
  列表仍只发布脚本实体对。
- 碰撞分组：地形 body 使用 group `0`、subgroup 全开，脚本默认的
  `collisionLayer/collisionMask` 能查询和碰撞它；`QueryBodyFilter` 对
  Jolt 的 `cInvalidGroup/cInvalidSubGroup` 按全放行处理，避免移位越界。
- 可观测：Console `terraincollision stats` 只在手动输入时打印 Play 中的
  terrain body 数与 placement；非 Play 输出 “No physics world”。
- 构建：Jolt 已从 C# hosting 的 `if()` 中移出，是核心 CPU 依赖
  （`script/CMakeLists.txt` 无条件 FetchContent，锁定同一 commit）；
  `main` 与 `terrain_tests` 都链接 `Jolt`。

## 验证

- 纯 CPU：`terrain_tests`（采样、chunk 计数、法线、chunk 边界、TR 校验、加载器）。
- Jolt 碰撞：`terrain_tests --gtest_filter=TerrainCollision.*` 覆盖平地/斜坡
  射线命中与 CPU 采样误差、heightScale/heightOffset、placement 的平移/旋转/缩放、
  32 次销毁/重建无残留 body、非法输入拒绝与越界射线，以及动态球落到地表。
- World/Play 接入：`terrain_tests --gtest_filter=TerrainCollisionInstances.*` 覆盖
  每个 terrain placement 的计划/TRS/placementId 提取与非法记录在加载期失败；
  `script_tests --gtest_filter=WorldBuilderTerrainCollision.*:ScriptTerrainCollision.*`
  覆盖 WorldBuilder 生成的实例、`SetTerrainCollisions` 替换/清空、
  Raycast/ProbeGround 命中 heightfield 并与 `SampleHeightCm` 比较、普通 World
  不创建物理世界。当前 worktree 未开 C#，没有完整 Play 冒烟。
- CPU LOD/边界缝合：`terrain_tests` 钉住 LOD0 的顶点/索引与索引顺序（无额外几何）、
  LOD1..3 的格子数、边界环顶点数与过渡带三角形数、基础顶点在 LOD0 同名网格点的
  位置/UV（线性坡面下法线也一致）、边界环落在 chunk 边界线上且不超出 AABB、过渡带三角形与内部网格
  同向（法线朝下）、相邻 chunk 差一级 LOD 时共享边界逐点重合、粗 LOD 跨 chunk 边界法线连续、
  边界附近粗法线混向细法线而内部保持粗法线、不同 LOD 的边界环法线逐位一致、
  `chunkQuads` 无法被步长整除与 `lodLevel` 越界时抛错、`lodLevels`/`lodDistances`
  的校验与加载器往返、默认距离按地形最长边推导、距离→级别边界（含负距离）、
  距离 AABB 最近点、10% 双向滞回带、
  chunk key 区分 LOD 且不受 `lodDistances` 影响。
- 运行时选择：`editor_ui_tests --gtest_filter=TerrainLodSnapshot.*` 用真实
  `World` + `WorldSnapshotBuilder` 钉住“每组只输出一个 draw”、近/远级切换、
  RuntimeId 跨级稳定、两个组独立选择、无 LOD 组对象保持原 draw 与 RuntimeId，
  以及“目标级未驻留时回退到 resident 的更粗一级 / 只剩 coarsest 仍出图”
  和 `drawnChunks / drawnByLevel / residentObjectsByLevel / totalMeshDraws` 计数。
  组 AABB/距离/切换状态每帧按 group 只计算一次，RuntimeId 按 World 代际缓存；
  编辑器 picking RenderScene 只在开发者 UI 可见时重建，运行时 HUD 路径不生成。
- 几何 chunk 驻留：`terrain_tests --gtest_filter=TerrainChunkResidency.*`
  覆盖窗口默认推导（`lodDistances × 1.1`）、显式窗口优先、最粗一级恒驻留、
  闭区间边界、世界空间 AABB 距离、无相机时回到全量；
  `TerrainMeshBuilder.ComputedBoundsMatchLod0Geometry` /
  `TerrainMeshBuilder.FilterSkipsChunksWithoutChangingTheRest` /
  `TerrainChunkResidency.EstimatesMatchBuiltChunkCounts` 钉住“bounds 与 LOD0
  一致、过滤不改顶点/索引内容、跳过量估算与真实网格逐级一致”；
  `TerrainValidator.AcceptsChunkResidencyDistances` /
  `RejectsInvalidChunkResidencyDistances` 钉住资产校验。
- 运行时 chunk streaming：`ComputeTerrainChunkStreamingSet` 由
  `TerrainChunkResidency.StreamingSetKeepsCoarsestAndNearFineLevels` /
  `StreamingSetAppliesHysteresis` / `ParsesChunkGroupIdWithPipesInPlacement`
  钉住“最粗一级恒选、近处细级按距离排序、1.15 滞回、placementId 可含 `|`”。
  手工冒烟：`--no-dev-ui` 加载 `SC_terrain_blender_gn_pegasus.json`，用
  `camera pose 0 165000 2000000 0 0 0` 移到窗口外后 `terrainchunk stats`
  报告 `objectsByLevel=2:16`；再移回 `0 165000 330000 0 0 0` 后恢复
  `objectsByLevel=0:16,1:16,2:16`，画面无空洞。加载期 `chunks=48` 字节基线
  保持只读，运行时以 `objectsByLevel`/`drawnByLevel` 为准。
- LOD 冒烟：`Generated/Validation/TerrainLod/` 的
  `TR_terrain_lod_smoke.json`（3 级、4×4 chunk、距离 3000/8000cm）配
  `SC_terrain_lod_near|mid|far.json`；`terrainchunk stats` 分别报告
  32/20/16 个 chunk 对象（全量 48）、`drawnByLevel` 0:4/1:8/2:4、1:4/2:12、
  2:16，与改动前截图相比（排除左侧 UI）最大通道差 ≤9、平均 0.0、无 >8 的采样；
  stderr 为空、无 Vulkan validation error。B32/M3/BC7 旧场景截图逐字节不变。
- 16km 几何驻留冒烟：`SC_terrain_chunk16k.json`（16km、512 网格、8×8 chunk、
  4 级、距离 600/2000/6000m）报告 128/256 对象、7 616 512 B（全量
  53 276 672 B）；把 `chunkResidencyDistances` 调到 1e9 的
  `SC_terrain_chunk16k_all.json` 得到 256/256 全量基线，两张截图排除 UI 后
  最大通道差 8、平均 5e-5。运行时 `camera position 600000 20000 600000`
  把相机移到窗口外，画面连续、无空洞（回退到 resident 的 coarsest）。
- 运行时：`Generated/Validation/TerrainSmoke/SC_terrain_smoke.json` 加载 64×64 quad、
  2×2 chunk 地形并截图；见 [testing](testing.md)。
- Phase B/C1 32 层数组 + 256 层上限：`Generated/Validation/TerrainB32/SC_terrain_b32.json`，
  验证 tile 候选层、32×32 global page table/4096 atlas、`pageOrigin`/`texCoord1` 与
  32 层数组；128 page 在近景仍有块状。多地形 page 累积、P3 feedback 驱动驻留与
  quarantine 淘汰已落地；更高 page 分辨率与 256 层数组仍属于后续 Phase。
- 双地形共享：`Generated/Validation/TerrainB32/SC_terrain_b32_two.json`，两个地形分别用
  `pageOrigin [0,0]` 和 `[8,0]`，验证同一张全局 page table/atlas 上不重叠累积并同时出图。
- 同 TR 双摆放：`Generated/Validation/TerrainB32/SC_terrain_b32_same_tr.json`，同一个
  `TR_terrain_b32.json` 摆放两次，场景级 `pageOrigin` 覆盖为 `[0,0]` / `[8,0]`；
  验证 `placementId` 注册、页表映射和 chunk 资源互不覆盖，两处地形同时出图。
- L2 层 atlas：`terrain_tests` 覆盖页表布局/编码、淘汰失效项、Commit/Abort 与
  `maxPages` 截断；`vl_renderer_backend_tests` 的
  `TerrainLayerAtlasGpu.ClearsAtlasWritesPageTableAndCopiesLayersInOneSubmit`
  在 headless 设备上一次提交中验证 clear→0、RGBA32F 页表 texel 与两个源 layer
  到 atlas 两个 128×128 页的 GPU copy。
- L2 冒烟：`SC_terrain_b32.json`、`SC_terrain_m3.json` 截图与 L1 基线逐字节一致
  （`1D30D8…3F54` / `0E5D5F…BAAE`）；`SC_terrain_b32_bc7.json` 与
  `bc7_smoke2.bmp` 一致（`C3E77E…2BDB`），stderr 为空且无 Vulkan validation error。
  `terrainlayerstats` 在 B32 上报告 `resident=32/512`、`uploadedPages=32`、
  `uploadedBytes=6291456`、`atlasReady=1`、`slices=1`、`atlasBytes=201326592`；
  BC7 场景报告 `albedo=Bc7SrgbBlock`、`atlasBytes=150994944`。
- L3 材质 schema/CPU 参考：`material_schema_tests` 校验 15 个 binding（旧三数组、
  catalog、4 张页表、三张 atlas）、`sampler2D`/`sampler2DArray` 类型、默认参数 0/128/4
  与新纹理的非空占位 default。`terrain_tests` 的 `TerrainLayerVTL3` 覆盖
  maxMip/pagesPerSide、mip round/clamp、页表 texel 总数/256 = CPU stride、
  虚拟页号、页表 texel 坐标、atlas slice/tile/半 texel UV、repeat 的 `u=1.0`/负 UV、
  无效页表项全零，以及 catalog `sourceResolution` 与真实源边长的一致性。
- L3 mode 0 回归：`SC_terrain_b32.json`、`SC_terrain_m3.json`、`SC_terrain_b32_bc7.json`
  截图与 L2 基线逐字节一致（`1D30D8…3F54` / `0E5D5F…BAAE` / `C3E77E…2BDB`），
  stderr 为空且无 Vulkan validation error。
- L3 mode 1 冒烟：`Generated/Validation/TerrainLayerVT/SC_terrain_vt_mode1.json`
  使用 32 层 512² 源数组；`terrainlayerstats` 报告
  `resident=672/672`、`uploadedPages=672`、`uploadedBytes=132120576`、
  `atlasReady=1`、`slices=1`、`atlasBytes=201326592`，`dirtyPages=0`、
  `dirtyPageTable=0`。与同一资产的 mode 0 截图相比（排除左侧 UI 标题区），
  8-bit 通道平均绝对差 2.40、35.3% 像素有 >2 差异、3.83% >8、0.37% >16、
  最大 41；差异来自 mip 取整与页内过滤，不是页号错位。两次 stderr 均为空。
- L3 `sourceResolution` 失败路径：把 B32 的 32² 源数组 MI 设为
  `u_terrainLayerMode=1` 时，加载期报告
  `Terrain layer VT requires sourceResolution to match the bound layer texture extent`
  并终止 World 加载；mode 0 只打印 warning 并继续。
- 32×32 虚拟页按需注册：`Generated/Validation/TerrainB32/SC_terrain_b32_32x32.json`，
  同一 TR 使用 `pageTilesPerSide=32`。P3 下加载期只烘焙中心 2×2 页，
  `terrain stats` 应显示 `virtualPages=1024`、`preloadDeferred=0`；默认 512 预算下，
  当前宽视野 working set（实测约 415 页）应最终收敛到 `missPages=0`、`pending=0`、
  `resident<=512`，不存在加载期 46 秒全量 preload。
- CPU 页面生命周期：`terrain_tests` 覆盖 per-tile 烘焙与全量结果一致、请求/命中/未命中、
  LRU 淘汰、优先级/引用保护、淘汰后 page table 无效、最后一个物理槽位可分配和 pageOrigin 重叠拒绝；
  P0 另覆盖 epoch 安全窗、新页初始 epoch、pin 拒绝淘汰、页内容 revision
  与上传确认、state revision 幂等，以及 `SetGpuEpochContext` 不改变旧 LRU。P1 覆盖
  `BuildUploadBatch`/`CommitUploadBatch`/`AbortUploadBatch` 的 pin 生命周期、收集后
  再次变脏时旧批次不清 dirty，以及 `maxAtlasPages` 一致性截断后下一批继续收敛。
  P4 覆盖 fallback 物理/虚拟解耦、虚拟 `(31,31)` 可驻留、32×32 单地形注册、preload
  容量不足时 deferred、同一 TR 双 placement 的独立物理页与淘汰、placementId 回退
  assetPath，以及 chunk key / 场景 page 区域覆盖校验。
- P2.5 覆盖：新页从本帧已提交 epoch 起算、`RequestPageAtEpoch` 在安全窗内拒绝腾位/
  窗外成功淘汰、旧 `RequestPage` 无窗口 LRU 不变、重叠上传批次各自持有独立 pin
  且 Commit/Abort 不互相解 pin、`heightScale=0` 平地取 0.5、负值/超范围样本钳制到
  `[0,1]`，以及 MI 预览候选提交后仍指向同一个 `TerrainStreamingState`。
- P2 feedback：`terrain_tests` 覆盖 texel 编解码、frameTag 回绕、无效/过期 texel
  丢弃、distinct/miss 页聚合、ring 未完成不消费、完成后只消费一次、槽位复用和 tag 范围；
  `TerrainLayerFeedbackSummary.SummarizesFullFourSlotBufferAndResetsScratch` 覆盖
  完整 65,536 texel 与 flat scratch 复位，`RejectsMipOutsideEncodedRange` 覆盖
  越界 mip 的防御；
  `vl_renderer_backend_tests` 覆盖编译器的 128×128 单颜色 + 128×128 `D32_SFLOAT` 深度/
  禁 MSAA/swapChain/GBuffer/非 D32 格式合同、`ColorAttachment -> TransferSrc -> ColorAttachment` 往返与 headless 回读，
  并把读回内容交给同一 CPU summary 验证。手工 smoke 用 `terrainfeedback` 观察
  双地形与单地形页统计变化；P3 改为 `preload=false` 后不再要求截图与 P4 基线
  逐字节一致，验收改为稳定后无缺页块且 validation 无错误。
- P3.1 覆盖：feedback 命中同时推进 `lastSeenEpoch`/`lastTouchedEpoch` 并改变
  epoch-aware 淘汰顺序；有效安全窗按 `max(2, framesInFlight+1)` 计算；
  `SetResidentCapacityAtEpoch` 在窗口内/持续可见时保持超预算，只回收窗口外的页；隔离槽位到期后
  不分配也会被回收、分配时也会先回收再复用；capacity=1/8 压测终止；
  `materialUsesLayerTable && TerrainUsesVirtualPages` 的四组合 eligibility 纯函数；
  layerless 地形与分层地形同 World 时只计入 unregistered、不污染页驻留；
  网格最后列/行半 texel 内缩；编译器拒绝非 D32 feedback 深度。
- L4a：`tool/terrain-tests/terrainLayerVisibilityTests.cpp` 覆盖 priority→mip 阶梯、
  覆盖页 top-4→可见层、每个可见层请求该 mip 全部页、未注册页跳过、跨覆盖页去重、
  每帧请求预算与 deferred 轮转收敛、层 mip 链/源数组 mip 钳制、layerCount 越界跳过、
  常驻覆盖页不重建、离开视野后 idle 淘汰。逐帧上传 ring 复用 L2 已有的
  Commit/Abort、失效项与 `maxPages` 截断测试。
- L4a 冒烟：`SC_terrain_vt_mode1.json` 稳定后 `terrainlayerstats` 报告
  `resident≈248–268/512`、`visibleLayers=12`、`pending=0`、`dirtyPages=0`、
  `quarantined=0`，只有启动期少量 `evictions`；`terrainlayer preload` 后
  `resident=672/672`，与 L3 全量预取集合一致。两张截图对比 L3 基线
  `l3_final_mode1.bmp`：流式状态平均通道差 0.167、0.79% 像素 >8、最大 34；
  全量预取状态最大差 1（只剩 UI 文本），说明流式差异来自 mip/过滤，不是页号错位。
- L4a mode 0 回归：`SC_terrain_b32.json`、`SC_terrain_m3.json`、
  `SC_terrain_b32_bc7.json` 截图仍与 L2/L3 基线逐字节一致
  （`1D30D8…3F54` / `0E5D5F…BAAE` / `C3E77E…2BDB`），stderr 为空且无
  Vulkan validation error。
- L4b：`tool/terrain-tests/terrainLayerFeedbackTests.cpp` 覆盖 8/8/3+valid 位编解码、
  越界字段拒绝、跨 slot 去重、frameTag 过期丢弃、按覆盖率排序、pageIndex→
  (pageX,pageY) 还原、精确 mip 请求、缺页邻域扩圈（已驻留不扩）、越界 pageIndex、
  命中续命、每帧预算与跨帧收敛、同一层不同 mip 各自请求，以及 avgColor 默认 0.18。
  `terrainLayerVisibilityTests.cpp` 另覆盖 2K 层 mip4 请求和“capacity 只回收非候选页”。
  `vl_renderer_backend_tests` 的 `TerrainFeedbackGraph.*` 已按 5 颜色 + 1 深度合同更新。
- L4b 冒烟：`SC_terrain_vt_mode1.json` 稳定后 `terrainlayerstats` 报告
  `resident=20/512`、`visibleLayers=20`、`feedbackHits=20`、`exactSlots=2044`、
  `exactPages=20`、`requested=0`、`pending=0`、`dirtyPages=0`；该相机下 20 层的
  目标 mip 都已是该层最粗一级（每层 1 页），驻留集因此正好等于可见集。
  与 L3 全量预取基线 `l3_final_mode1.bmp` 相比（排除左侧 UI）最大通道差 15、
  平均 0.002、>8 像素占比 <0.01%，无整页缺失块。`terrainlayer capacity 8` 后为 `20/8`
  并持续（可见集大于预算的软预算行为，`accepted=0`、`pending=0`），不再像 L4a
  那样停在 248。
- L4b mode 0 回归：`SC_terrain_b32.json` 截图仍与 L2/L3 基线逐字节一致
  （`1D30D8…3F54`）；`SC_terrain_b32_two.json` 与 P3 基线相比平均通道差 0.001、
  无 >8 像素，差异来自逐帧流式的到达时刻。两者 stderr 均为空。
- L4c：`tool/terrain-tests/terrainLayerVisibilityTests.cpp` 覆盖
  “目标页缺页时 +1/+2 级粗 mip 页入队且优先级低于目标页”“目标页已驻留时不扩粗页”
  “粗页预取不绕过每帧请求预算（3 个候选 / budget 1 时只请求目标页、剩 2 个 pending）”。
  既有 L4a/L4b 用例的期望值同步改成含粗页的候选数（例如 512² mip0 两层：
  32 目标页 + 8 张 mip1 + 2 张 mip2 = 42）。
- L4c 冒烟（`SC_terrain_vt_mode1.json`，排除左侧 UI 后逐像素比较）：
  - 默认相机稳定态：`resident=20/512`、`fallbackSlots=0`、`fallbackPages=0`、
    `fallbackLevels=0/0/0`、`targetMips=0/0/2044/0/0`（20 层全部落在最粗 mip，
    每层 1 页且已驻留，因此不触发粗页预取）。
  - 与 L4b 基线二进制（同一份源码去掉 L4c）对比：mode 1、`SC_terrain_b32`、
    `SC_terrain_m3`、`SC_terrain_b32_bc7` 四张截图 **最大通道差 0**（逐字节一致）。
- 全局层页 mip 预算：`tool/terrain-tests/terrainLayerMipBudgetTests.cpp` 覆盖
  每层 mip 链/target 钳制、预算内收敛、近处优先、同层远近双 mip、重复需求
  去重、最粗工作集仍超预算的 `budgetExceeded` 路径；
  `terrainLayerVisibilityTests.cpp` 覆盖“feedback 只命中一页也请求选中 mip
  全部页”“目标 mip2 不装 mip0”“预算紧张时近处拿细 mip”“同一层 mip0+mip3
  同时驻留”“capacity 等于选中页集时后续帧 accepted=0/evictions=0”。
  `terrainLayerPageManagerTests.cpp` 覆盖受保护页集不被容量/LRU 淘汰。
  - `terrainlayer capacity 8` / `16`：`resident=20/8`、`resident=20/16`，
    没有新增淘汰或回退像素。可见页每帧都被 feedback 看到，落在 GPU 安全窗内，
    capacity 不能淘汰它们——因此在这个场景里 `capacity 8/16` **无法制造缺页**，
    只能证明“降预算不动可见集”。真正的缺页回退需要目标 mip 比最粗 mip 更细，
    该验证场景的 `u_uvScale=32` 使所有可见片元都停在该层最粗 mip。
  - 三次冒烟 stderr 均为 0 字节，无 Vulkan validation error。
