# Terrain：几何、作者数据、编辑与碰撞

> 状态：**当前契约**。本文件覆盖 CPU 高度图几何、TR/场景身份、Editor、
> Paint/coverage 与宏高度碰撞。地形唯一材质运行时标准是 compositedVT +
> 常驻 tiling height；资源、Cook、反馈、预算、采样和发布规则统一见
> [合成地形 VT 合同](terrain-composited-vt.md)。

## 合同边界与数据流

```text
SC_*.json terrain
  -> SceneAssetValidator::BuildLoadPlan
  -> TerrainAssetLoader::Prepare
       -> TR 校验、高度图解码、Paint/sidecar 恢复
  -> RendererTerrainLoader
       -> TerrainMeshBuilder / chunk residency
       -> composited surface 注册
  -> WorldBuilder / World mesh objects
  -> WorldSnapshotBuilder 选择 LOD
  -> geometry pass / compositedVT 采样
```

地形通过普通 World mesh object、scene object identity 和 geometry pass 绘制；
顶点/索引由 `TerrainMeshBuilder` 生成。合成表面的反馈与流式生命周期由
`TerrainCompositedService` / `TerrainCompositedRuntime` 拥有。
几何 chunk 驻留与材质页面驻留是不同职责。

旧 direct 数组与 layer source VT 双路径不属于受支持的运行时。
保留下来的层 catalog、源数据、覆盖率页、Paint 和高度缓存为合成烘焙、
近场细节、TES 或编辑提供输入，不构成另一条基础材质采样路径。
退役实现和旧截图/数值记录见
[历史快照](../rationale/terrain-legacy-runtime-contract-snapshot.md)，其中的历史证据
不能作为当前实现或 full16 验收结果。

## TR JSON

以下是作者数据示例；包路径声明本身不代表包已经 Cook 或校验成功：

```json
{
    "name": "TR_terrain",
    "type": "terrain",
    "heightmapPath": "Maps/SC_x/Source/heightmap.png",
    "size": [10000.0, 10000.0],
    "heightScale": 2000.0,
    "heightOffset": 0.0,
    "gridResolution": 64,
    "chunkQuads": 32,
    "materialInstancePath": "Maps/SC_x/Materials/MI_terrain.json",
    "compositedVT": {
        "enabled": true,
        "packagePath": "VT/worldV2/SC_x/manifest.json",
        "productionSourcePath": "Maps/SC_x/world.author.json",
        "virtualResolution": [1024, 1024]
    }
}
```

- `size` 是 X/Z 全宽；`heightScale`、`heightOffset` 与尺寸均以 cm 表示。
  几何高度为 `sample * heightScale + heightOffset`，允许零或负 `heightScale`。
  权重求值的归一化高度钳制到 `[0,1]`；零 scale 的归一化高度取 0.5。
- `gridResolution` 是总 quad 数，`chunkQuads` 是每块 quad 数；均为正整数，
  `chunkQuads <= gridResolution` 且能整除总 quad 数。
- `heightmapMargin` 默认 0，范围 0..16；非零时高度图必须恰为
  `gridResolution + 2*heightmapMargin + 1` 方形样本。mesh 覆盖仍是 `size`，
  高度/法线、权重和碰撞把 mesh UV 映射到内圈，以使用边界外 overscan 样本。
  Editor 笔刷仍按完整高度图 UV 计算，未抵消 overscan。
- `lodLevels` 默认 1，范围 1..4。最粗步长 `2^(lodLevels-1)` 必须整除
  `chunkQuads`。`lodDistances` 是长度 `lodLevels-1` 的严格升序正数表，
  单位 cm；未写时按地形最长边均分。
- `chunkResidencyDistances` 与 LOD 距离表同长度、同单位且严格升序；
  未写时取 `lodDistances * 1.1`，单级地形允许空表。
- `pageOrigin` 默认 `[0,0]`，`pageTilesPerSide` 默认 8；覆盖率的全局虚拟区域
  满足 `pageOrigin + pageTilesPerSide <= 32`。它们不是 compositedVT 材质页尺寸。
- `heightmapPath`、`materialInstancePath` 是资源根相对路径。
  `detailMaterialInstancePath` 可选，供 LOD0 使用，必须与基础 MI 不同；
  LOD1..N 使用基础 MI。多级地形的基础 MI 不允许开启 tessellation，
  避免粗 LOD 被错误绑定为 patch 拓扑；显式 detail MI 加载失败是硬错误。
- `compositedVT` 必须显式 `enabled=true` 且具有非空资源根相对 `packagePath`。
  同一 World 全部 terrain 指向同一 V2 manifest；Editor/Cook 还要求共享非空
  `productionSourcePath` 作者配方，surface 集合恰等于场景 placement 身份。
  Game 不读取配方。旧 `residentPages`/`heightBudgetBytes` 被拒绝，
  共池/高度/输入预算由固定 P0Frozen 统一控制，不由 TR 自定。
  解析校验不要求包已经存在，稀疏发布、实际输入消费前校验及 Cook 规则见
  [合成地形 VT 合同](terrain-composited-vt.md)。disabled 的内部默认结构不是作者兼容路径。
- `paintPath` 可选，指向资源根相对 `.vlpaint` 文件，要求至少一层 `layers`。
  文件不存在时只是保存目标；存在但格式、层数或 sidecar 非法则拒绝 World 候选。
- `layers` 存在时为 1..256 层，必有 `name`、`heightRange`、`slopeRange`。
  可选 `noise`、`uvScale`、`uvRotation`、`sourceResolution`（512/1024/2048，
  默认 1024）、`avgColor`（默认线性 0.18）、`sourceSet`（0..255）、
  `flags`（0..65535）、`displacementM`（非负有限米数，默认 0）、
  `hasHeight`（默认 false）。
- 层 `key` 可选，长度 1..256，缺省 `<assetPath>#<name>`。
  `TerrainLayerRegistry` 按 World 顺序分配 globalId；同 key 同参数去重，
  参数冲突、TR 内重复 key 或超过 256 个全局层均失败。
  coverage 写 globalId；sourceSet/slice 映射用于作者源与烘焙输入。
- 未知字段、缺失必填项、非有限数与非法范围在预检阶段拒绝。

## 高度图、CPU 网格与 LOD

`TerrainAssetLoader::Prepare` 使用 `TerrainHeightmapIO::Load`；
不改变通用 `TextureIO` 语义。

- 16-bit grayscale PNG 通过 `stbi_load_16` 解码为 `R32_FLOAT`，除以 65535，
  保留完整精度。RGB/RGBA 16-bit PNG、损坏或截断数据会拒绝，不降为 8-bit。
  无头 raw 16-bit 不受支持。
- 其他格式走 `TextureIO`：8-bit PNG/BMP/TGA 归一化到 0..1；
  EXR/HDR 保留 float。EXR 沿用 RGBA 读取，单通道 EXR 不受支持。
  高度图至少 2×2，CPU 高度采样使用双线性插值。
- UV 的 u 对应 +X、v 对应 +Z；位置是
  `((u-0.5)*size.x, sample*heightScale+heightOffset, (v-0.5)*size.y)`。
  高度图不套用材质纹理的固定 V 翻转。
- LOD0 每块 `(chunkQuads+1)^2` 顶点、`chunkQuads^2*2` 三角形，uint32 索引；
  UV 是整块地形 `[0,1]` 坐标，切线 `(1,0,0,1)`。
  `texCoord1` 携带覆盖率虚拟区域坐标。
- 法线来自全局高度图中心差分。粗级内部用 `step=2^lod`；
  距边界三个粗格内平滑混向 `step=1`，边界环恒用 `step=1`。
  相邻 chunk 共享全局采样点，避免法线接缝。
- 第 lod 级合并步长 `2^lod`。粗级没有 skirt，而是追加
  `4*chunkQuads` 个按 LOD0 间距采样的边界环顶点，再用过渡三角形连接内圈；
  不同级共享边界位置，无需知道邻居级或 shader morph。
  `TerrainChunkVertexCount` / `TerrainChunkIndexCount` 是预算与测试共用的计数入口。
- chunk 有局部 AABB；资源 key 含级号与步长，距离表和 build filter 不进 key。
  单级地形只生成 LOD0。

加载期 `RendererTerrainLoader` 按初始 camera 到 chunk 世界 AABB 的最近距离，
使用 `TerrainChunkResidencyDistancesCm` 决定细级是否生成；最粗级恒保留。
`ComputeChunkBounds` 不生成法线，`TerrainChunkBuildFilter` 只控制生成集合。
没有 camera 或单级地形时不启用窗口裁剪；terrain 不支持 offline-only 渲染。

运行时 `TerrainChunkStreamer` 在 snapshot 前读取相机，用 worker 构建 CPU chunk；
GT 在安全边界上传、加入 World。离开窗口的 chunk 从 World/cache 取出并经
`ResourceRetireQueue` 按 epoch 退休。World 换代和编辑更新的失效处理由 streamer
生命周期实现负责，不得把旧高度或旧 World 的 worker 结果重新发布。

`WorldSnapshotBuilder::Build` 是唯一 LOD 选择点：按世界 AABB 最近距离与 10%
滞回选择，每组只输出一个 draw。变粗阈值为 `distance >= threshold*1.1`，
变细为 `distance < threshold*0.9`；目标未驻留时沿更粗方向找 resident 级。
组身份是 `placementId|chunkX|chunkZ`，组内级别共享 `RuntimeId`。
阴影、拾取和材质上下文消费同一 snapshot。
`terrainlod stats` / `terrainchunk stats` 提供选择、resident 位图与几何字节观测。

## 近场细分与 coverage 依赖

LOD0 的可选 `VL_TERRAIN_TESSELLATION=1` 由设备能力约束；
不支持 tessellation 时宏归零。管线使用四控制点 quad patch；
缺少 patch 索引时跳过绘制，不能把三角索引送入 PATCH_LIST。
LOD1..N 保持三角拓扑。

TCS 按屏幕边长与 `u_terrainTessTargetPixels` 计算因子，
由 `u_terrainTessMax` 限制，半径远端平滑退为 1。
TES 使用 `layout(quads, equal_spacing, cw)`，outer 边必须与参数域对应；
宏位置按 CPU 的对角线分片线性求值，再追加 tiling 位移。

TES 与近场 detail 使用 height-blend 前的原始 top-4 权重。
CoverageAtlas 按 coverage 身份合并；LocalLayerArray 按冻结的 R32_FLOAT 逐层
overlay 和 local→global 映射解析，不能用 Coverage 的测试代签 Local。
`terrainCoverage.glsl` 的双线性读取会跨虚拟页取邻域，合并相同 globalId 的贡献，
重新选 top-4 并归一化；placement 边界须 clamp。
coverage page table、ID/权重 atlas、`layerTable` 和逐 placement 的权重输入
仍是真实依赖，不能随退役的层源页表一并删除。
显式 `layerWeightMaps` 输入具有 placement 身份，不可因层表共享而跨 placement 复用。

各层 detail UV 为 `texCoord*u_uvScale*layerUvScale*u_detailUvScale`。
高度使用常驻 tiling height atlas 的源 mip2，按当前权重路径混合
`(height-0.5)*displacementM`，再乘 `u_layerDisplacementScale` 与距离淡出。
高度 atlas/LUT 的预算、格式与包验证以合成 VT 合同为准。
基础 composited 表面、近场 detail 和 TES 的输入关系不构成旧基础材质旁路。
独立高度闭包不随基础色细页淘汰；根页可用不证明高度已就绪或近场置换通过。
非零基础 height blend 仍读取源 mip3，源高度数组并未全部删除。

tiling 位移只影响视觉，Jolt 与脚 IK 仍使用宏高度；阴影 pass 使用宏网格。
位移应处于鞋子/脚 IK 可吸收的厘米级容差内。
相关 detail MI 参数包括 `u_detailUvScale`、`u_layerDisplacementScale`、
`u_terrainTessRadiusM`、`u_terrainTessMax`、`u_terrainTessTargetPixels`。
Landscape 层详情按厘米显示逐层 `displacementM`，存储仍为米。

## 场景身份

`terrain` 对象要求 `terrainPath`，并具有普通 position/rotation/scale。
`placementId=terrain|<name>`；同 TR 可重复摆放，对象名必须不同。
可选 `pageOrigin` / `pageTilesPerSide` 在 TR 预检后覆盖本次 placement，
不改写 TR，最终覆盖率区域仍必须合法。
空 placementId 的内部调用回退到 assetPath。
所有 chunk 共享对象的 scene identity，Editor actor 树聚合为一个 Terrain actor；
World 中的渲染对象仍是 chunk mesh。

## Editor 资产与 Landscape

TR 是标准资产文档，使用 `terrain.get_document/get_capabilities/modify/validate/
save/revert/reload` 和 `AssetDocumentService` 的 baseline/draft/revision、
源摘要冲突检查与原子保存。能力与可编辑字段以 `get_capabilities` 返回为准，
校验复用 `TerrainAssetValidator`；文档保存不等于完成 Cook 或 World 事务。
层编辑以 `layerIndex+expectedLayerName` 校验身份，避免重排后写错层。

Terrain 标签页提供 TR 字段、层表与 heightmap 预览；关闭标签不自动提交草稿。
PNG/JPG/BMP/TGA 由 RmlUi 解码，EXR 不显示预览。
Terrain Material 区块通过 `material.open` 打开关联 MI，
`u_uvScale` 使用 MI 文档的 `material.set_parameter`、revision 与保存流程；
不在 TR 中写第二份全局 tiling。Landscape 优先使用选中 terrain，
否则取场景首个 placement。

层库 CPU 预览从 MI 的 `layerAlbedo/layerNormal/layerPbr` 作者描述符解析源文件；
slice 是同 sourceSet 中早于本层的层数。缺失项保留空位，三组 slice 必须一致。
`displacementSources` 是与 albedo sources 同序同长度的 16-bit grayscale 输入，
预览不读取 GPU。声明后缺失/损坏不得静默降精度；未声明的旧预览可用 alpha/亮度近似。
球体/平面缓存 key 含四类源路径、大小、mtime、尺寸、着色版本与位移参数，
写入 `Generated/TerrainPreview/PreviewCache`；只请求可见层卡，不遍历全部层。
预览失败退回颜色/诊断，不修改 TR、MI 或 runtime 纹理。

### Sculpt

Landscape 在 authoring Editing、非 offline、非模型预览场景可用。
Play 销毁编辑会话。`TerrainHeightmapEditor` 支持 Raise/Lower/Smooth、
Circle/Square、Strength/Falloff；半径由 cm 按地形 X 跨度换为 UV，
非方形地形的世界足迹因此为椭圆。
一次拖动对应 Begin/Apply/EndStroke；Esc、失焦、选择或 World 换代取消。
Undo/Redo 属会话局部，不合并 Scene 历史。

authoring chunk 使用三槽动态顶点缓冲。Sculpt 冻结更新携带 World/placement/
revision 身份，经 `TerrainCompositedSculptApply`、World commit 和顶点更新队列发布；
RT 只回放当前安全槽。提交更新实际存在的 chunk/AABB 与 World 高度图，
并通知合成服务刷新该 placement；失败保留 dirty 和诊断，不把失败当成功。
Game 使用静态顶点缓冲。具体提交、回滚与队列所有权见合成 VT 合同。
下一次 Play 消费更新后的宏高度图；编辑器不实时重建 Play 中的 Jolt body。

### Paint

`TerrainPaintSession` 独立拥有 layer-major weight/opacity。
UI 提供 Radius、Hardness、Opacity、Flow、Spray、Paint/Erase 与 Circle/Square。
`falloff=clamp(1-hardness)`；Flow 是每 stamp 的推进量，
Opacity 是 stroke 覆盖上限；Spray 按 texel 与 seed 确定性求值。
Erase 降低 opacity、保留 weight；Sculpt 不共享这些 Opacity/Flow/Spray 状态。

`TerrainWeightMapBuilder` 双线性混合 Paint 与高度/坡度自动权重，
tile 候选集补入 Paint 命中的层。没有持久化数据时按层数分配空白覆盖；
已有合法 paint 成为会话起点，首次笔画不清空它。
提交通过 `UpdateTerrainPlacementLayerPaint` 更新 World 的不可变快照；
`PaintData` 按 revision 缓存副本，后续 stamp/Undo 不修改已交给 World/RT 的数据。
合成服务的覆盖率刷新和材质重烘焙边界见合成 VT 合同。
Play/Stop 或换 actor 后继续使用 World 最新快照，重载 World 恢复磁盘状态。

### Save Heightmap / Save Paint

Save Heightmap 写回 `heightmapPath`，不写 TR、不自动重载，也不保存 Paint。
PNG 默认保持已有 16-bit 目标的精度，新文件/8-bit 目标默认 8-bit；
`WriteOptions::PngBitDepth::Sixteen` 可显式写 grayscale 16-bit。
BMP 写 24-bit grayscale；EXR 转 RGBA16_FLOAT，不钳制 HDR/越界高度。
写入使用同目录临时文件与原子替换。

Save Paint 要求无活动 stroke、有数据且 TR 声明 `paintPath`。
`ResolveTerrainPaintPath` weak canonical 后必须仍在资源根内，
拒绝绝对路径、逃逸与指向根外的 junction/symlink。
`SaveTerrainLayerPaintBundle` 用两文件事务写 `.vlpaint` 与 `<paint>.meta.json`，
失败尽量回滚，不能报告保存成功；不修改 TR 或自动重载 World。

VLP1 固定小端布局：24-byte 头包含 magic `VLP1`、width、height、
layerCount、flags=0、reserved=0；后接 layer-major float32 weights，
再接等量 uint8 opacity。边长 2..4096 且为正方形、层数 1..256 并与 TR 一致；
权重必须有限且在 `[0,1]`，长度精确匹配，负载不超过 512 MiB。
存在但不是常规文件或损坏数据不能当作“没有 Paint”。

VLP2 sidecar 不改变 VLP1 格式：version=2、kind=`terrainLayerWeights`，
携带 tileX/tileY/tileGrid、width/height/layerCount、顺序敏感的 layerKeys、
featherTexels 和 contentSha256。loader 校验形状与层身份，解析但不计算 SHA-256；
缺 sidecar 的 VLP1 会打印 legacy asset 诊断并加载。
保存写实际字节哈希，已有 tile 元数据保留，缺失时默认单 tile。
进程崩溃可能留下 rollback 文件，loader 不自动恢复。
多 tile 的完整身份、边界与哈希检查使用
`tool/terrain-houdini/paint_manifest.py write/validate <terrain-set>`。

## Jolt 宏高度碰撞

`terrainCollision.*` 将 `TerrainBuildPlan` 转为调用方已有 PhysicsSystem 中的
静态 HeightFieldShape；不创建第二套游戏物理世界。
`TerrainCollisionProxy` 只保留 placement、世界跨度、宏高度样本与 AABB，
不依赖材质、层页或渲染 LOD。

- 默认采样 `gridResolution+1`，向上对齐 Jolt blockSize=2，允许 4..4096；
  双线性读取宏高度，16-bit 压缩。采样点可能与可见网格错位，
  且 Jolt/渲染 quad 对角线不同，非平面 quad 存在误差。
- position 以 cm、rotation 以度，旋转顺序 `Y*X*Z`，scale 必须为正且有限；
  内部转米。缩放烘焙进 heightfield，平移/旋转由 body transform 承担。
- `TerrainCollisionBody` RAII 先 RemoveBody 再 DestroyBody，必须先于物理世界销毁。
  非法输入在创建前拒绝。进程 Jolt 注册复用 `EnsureJoltRegistered()`。
- World 为每 placement 保留 plan/TRS；Play 创建 `ScriptPhysicsWorld` 后调用
  `SetTerrainCollisions`，Stop、重 Play、World 换代销毁旧 body。
  普通 Editor 加载不创建物理世界。替换失败保持旧集合。
- `TerrainCollisionPatchStreamer` 在角色周围使用 64 m、0.5 m 采样的
  130² 宏高度 patch，中心按 32 m 吸附；worker 构建，GT 创建/替换 body。
  patch 不完整落在代理范围内时卸载并回退宏 body；边缘 6 m 混向宏高度。
- 宏 body 使用 group 14，角色近场 patch 使用 group 15；角色在 patch 内切换 mask，
  同时只使用一套地形碰撞。tiling height/detail/TES 不进入碰撞或脚 IK。
- Raycast/ProbeGround 返回 terrain placement 身份；`terraincollision stats`
  观测 body/proxy/patch 数，非 Play 显示 No physics world。
  `TerrainCollisionWorld` 只用于测试/离线夹具。

## 验证入口与证据边界

验证命令和构建前提见 [测试合同](testing.md)；本文件不维护第二份通过计数。
`terrain_tests` 覆盖高度图、TR、mesh/LOD、coverage/Paint、碰撞代理与 patch；
`script_tests` 覆盖 World/Play 接入，Editor 测试覆盖资产文档与编辑提交。
这些入口的存在不等于本次已经运行。
compositedVT、常驻高度、Cook、预算与运行验收以合成 VT 合同及其证据入口为准。
旧场景的截图哈希、旧模式回归与历史测试结果不能证明 full16 当前通过。
