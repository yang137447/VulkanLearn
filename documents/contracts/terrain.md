# Terrain Runtime

> 状态：**当前契约**。本文件只描述 `source/` 与场景/TR 资产现在的行为；LOD、地形
> chunk 卸载、低分辨率 fallback/淡入、碰撞与地形编辑等未来能力不在这里承诺。

## 数据流

```text
SC_*.json (type: "terrain")
  -> SceneAssetValidator::BuildLoadPlan
  -> TerrainAssetLoader::Prepare(terrainPath)
       -> TR_*.json 解析与校验
       -> 高度图解码为 HostImage
  -> RendererResourceLoadCoordinator
  -> RendererTerrainLoader
       -> TerrainMeshBuilder::BuildChunks
       -> RenderableObject / MeshObjectBuildPlan
  -> WorldBuilder -> World mesh objects -> geometry pass
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
- `pageOrigin`（可选，默认 `[0,0]`）和 `pageTilesPerSide`（可选，默认 8）决定该地形
  在全局 32×32 page table 中占用的 tile 区域；`pageOrigin + pageTilesPerSide <= 32`。
- 场景里的 `terrain` 对象可以再次覆盖 `pageOrigin` / `pageTilesPerSide`（见“场景对象”）；
  覆盖只改该次摆放的虚拟页区域，不修改 TR 资产本身。
- `heightmapPath`、`materialInstancePath` 都是资源根相对路径；材质在预检阶段至少必须可解析。
- `layers` 可选：1..256 层。每层必有 `name`、`heightRange`、`slopeRange`；可选
  `noise`、`uvScale`、`uvRotation` 与 L1 新增的 `sourceResolution`（512/1024/2048，
  默认 1024）、`avgColor`（3 个 0..1 线性色分量，默认 0）、`sourceSet`（0..255，
  默认 0）、`flags`（0..65535，默认 0）。未知字段、越界值与非有限数都会被拒绝；
  旧 TR 不写新字段时行为与默认值不变。
- `M_terrain` 使用这些层生成 256×4 `RGBA32F` layer table（catalog），未使用的层
  四行全零；布局与 CPU accessor 见“层 catalog v2”。
- 未知字段、缺失字段、非有限数值、非法范围都会在场景预检阶段失败。

## 高度图

`TerrainAssetLoader::Prepare` 使用 `TextureIO::Load` 的 linear、无翻转路径解码高度图：

- EXR/HDR 走 float 路径；PNG/BMP/TGA 等 8-bit 图像按 0..1 归一化。
  当前 EXR 沿用 `TextureIO` 的 `RgbaInputFile`，因此需要 RGBA EXR；单通道 EXR 尚未支持。
- 高度图至少 2×2；采样使用双线性插值。
- 地形 UV `u` 对应 +X，`v` 对应 +Z；顶点 `x = (u-0.5)*size.x`、
  `z = (v-0.5)*size.y`、`y = sample*heightScale + heightOffset`。
- 高度图不套用材质纹理的固定 V 翻转；翻转会同时改变高度场方向。
- 当前支持 `TextureIO` 实际产出的 `RGBA32_FLOAT`、`R32_FLOAT`、`RGBA8_UNORM/SRGB`、
  `R8_*`、`RG8_*` 标量读取；其他格式在加载阶段报错。

## 网格

- 每块有 `(chunkQuads+1)²` 个顶点、`chunkQuads²×2` 个三角形、uint32 索引。
- 顶点 UV 是整块地形的 `[0,1]` 全局坐标，留给材质乘 `uvScale`；切线为 `(1,0,0,1)`。
- 法线用全局高度图中心差分：`n = normalize(-dh/dx, 1, -dh/dz)`。相邻 chunk 共享的
  边界采样同一个全局 UV，因此不会出现 chunk 内法线接缝。
- 每个 chunk 计算局部 AABB，交给 World/渲染快照做视锥与阴影裁剪。
- 索引绕序与引擎的 Clockwise front face 对齐；材质可以使用 Back culling。
- 没有 LOD、没有 skirt、没有地形 chunk 卸载；所有几何 chunk 在加载期一次性生成。
  地表 ID/权重 atlas 的页内容已按需流式驻留，top-4 混合已实现，但几何本身不流式。

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
仍是地形对象名，编辑器 actor 树因此把多个 chunk 聚合成一个 mesh actor。

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
  期间有效。fallback 只保留**物理 atlas 槽位** `(31,31)`：页表里不再有该虚拟坐标的保留项，
  fallback 页也不进入 dirty/resident 统计；虚拟 `(31,31)` 现在是普通页，`pageTilesPerSide=32`
  的单地形因此可以注册。
- `TerrainPageManager` 的注册身份是 `placementId`（场景对象身份），不是 `assetPath`。
  同一 `assetPath` 可以注册多次；同一 `placementId` 重复注册、或虚拟区域重叠都会在
  `RegisterTerrain` 抛错，错误信息同时带 `assetPath` 与 `placementId`。所有公开页 API
  的第一个字符串参数都是 `placementId`。
- `RegisterTerrain(preload=true)` 仍是测试/小场景兼容入口：逐页请求常驻，容量不足或候选页
  被 pin/引用时 `RequestPage` 返回 false，注册继续并把次数记入
  `Stats::preloadDeferredCount`，不会死循环。生产 renderer 在 P3 改为
  `RegisterTerrain(preload=false)`：注册只建立虚拟页与物理槽位身份，不烘焙页面。
  每个 placement 只预取中心 2×2 页（clamp 到地形范围）作为初始可见集合，其余页由
  feedback 逐帧请求。resident budget 默认 `TerrainPageManager::DefaultResidentCapacity`
  = 512，仍小于 32×32 的 1024 个虚拟页与 1023 个可用物理槽位。512 来自实测：
  2km/32×32 宽视野 working set 约 415 页。budget 必须大于实际可见 working set；
  不足时安全窗内的页不能被淘汰，未驻留可见页会保持中性表面，直到通过
  `terrain capacity N` 提高预算（这是预算配置问题，不是 feedback 失效）。
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
  resident 占用或仍在 quarantine 的槽位都不计入。fallback 物理槽位永远不参与分配/回收。
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
  已过安全窗的页；窗口内没有安全候选时允许暂时超预算，后续帧继续尝试收敛。
  旧 `SetResidentCapacity` 仍保留无窗口 LRU 语义，只供加载期/测试使用。
  本帧请求页在 `BuildUploadBatch` 中被 pin，不会同帧被淘汰；淘汰出的物理槽位
  始终走 quarantine，不会让 in-flight 帧采样到被覆盖的 atlas。
- page table 的无效项是 `A=0`。shader 缺页时仍把页坐标 remap 到物理 `(31,31)`
  （该槽位不参与分配，atlas 零初始化），但最终输出用缺页掩码替换为中性表面：
  baseColor 0.18、roughness 0.9、metallic 0、AO 1、几何法线/切线。有效页的
  `invalidPage=0`，输出与原采样路径逐位一致。物理 `(31,31)` 继续保留不分配，
  对应虚拟坐标可以正常注册。
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
- 没有地形碰撞、没有 heightfield 物理体、没有地形编辑和绘制工具。
- page manager/GPU page 资源由 world-local `TerrainStreamingState` 管理；chunk 资源 key
  （`TerrainChunkResourceKey`）包含 `placementId`、TR 路径、高度图路径、size、
  heightScale/offset、grid/chunk 参数与 chunk 坐标。同一 TR 的两处摆放因此不会共享或
  覆盖彼此的顶点/索引缓冲。

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
    `mipStride(mip)` 是该 mip 下所有已配置层的最大 `pagesPerMip`：全同分辨率时
    等于设计公式（1K 的 mip0 stride 64、2K 256），512/1K/2K 混用时取最大值，
    因此不同层的页号不会别名。`VirtualPagesPerLayer`/`VirtualPageTableSize`
    给出 L2 建页表所需的槽位数。
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
1 走 `top-4 ID → catalog → 每 mip 页表 → 物理页 → atlas`。这是可选的正确性/可视化
路径，不是 L4 的 feedback 驻留策略。

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
- L3 验证路径没有 L4 feedback：当任一 MI 请求 mode 1 时，加载期
  `RequestAllTerrainLayerPages` 会请求每层每个 mip 的全部页，并把层页容量提升到
  `min(可用物理页, max(总页数, 默认 512))`，随后一次 `ApplyTerrainLayerStreamingUploads`
  上传。mode 0 仍只请求每层尾页。全量预取是 L3 的验证行为，不是生产 streaming 策略。

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
- `TerrainStreamingState::feedbackRing` 是 World-local 的 host-visible/coherent ring，
  槽数等于后端 `GetFrameFenceCount()`（当前 2），常驻映射、不每帧创建/销毁 buffer。
  帧末在所有 scene pass 之后把当前 swapchain image 的 `terrainFeedback` 从
  `ColorAttachmentOptimal` 转到 `TransferSrcOptimal`，`CopyImageToBuffer` 后转回
  `ColorAttachmentOptimal`；下一帧 render pass 仍以 `Undefined/clear` 取得内容。
- CPU 只在 `BeginFrame` 已等待该 frameIndex fence 后调用 `TryConsume`，并要求
  `copiedEpoch <= completedEpoch`；成功后立即清 pending，因此同一槽位只解析一次。
  未完成的槽位不会被读取。该 ring 随 `WorldLocalResourcePackage` 一起退休。
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

P3/P3.1/L1/L2/L3 **没有**实现：低分辨率 fallback/淡入、相机预取、LOD/skirt、
地形 chunk 卸载、层页 feedback 驱动的驻留（L4；L3 只有加载期全量预取）、
同一 TR 多摆放的层页内容去重、不同 `sourceSet` 的多组源纹理、地形碰撞/编辑。
CPU 页面烘焙（`TerrainWeightMapBuilder::BuildPage`）仍在 `RequestPageAtEpoch` 内同步执行；
每帧 8 页是数量预算，不是时间预算，重载/快速移动时单帧仍可能出现烘焙耗时峰值。

## 验证

- 纯 CPU：`terrain_tests`（采样、chunk 计数、法线、chunk 边界、TR 校验、加载器）。
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
  LRU 淘汰、优先级/引用保护、淘汰后 page table 无效、fallback 物理槽位保留和 pageOrigin 重叠拒绝；
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
  `vl_renderer_backend_tests` 覆盖编译器的 128×128 单颜色 + 128×128 `D32_SFLOAT` 深度/
  禁 MSAA/swapChain/GBuffer/非 D32 格式合同、`ColorAttachment -> TransferSrc -> ColorAttachment` 往返与 headless 回读，
  并把读回内容交给同一 CPU summary 验证。手工 smoke 用 `terrainfeedback` 观察
  双地形与单地形页统计变化；P3 改为 `preload=false` 后不再要求截图与 P4 基线
  逐字节一致，验收改为稳定后无缺页块且 validation 无错误。
- P3.1 覆盖：feedback 命中同时推进 `lastSeenEpoch`/`lastTouchedEpoch` 并改变
  epoch-aware 淘汰顺序；有效安全窗按 `max(2, framesInFlight+1)` 计算；
  `SetResidentCapacityAtEpoch` 在窗口内允许暂时超预算、窗口过后收敛；隔离槽位到期后
  不分配也会被回收、分配时也会先回收再复用且 fallback 永不回收；capacity=1/8 压测终止；
  `materialUsesLayerTable && TerrainUsesVirtualPages` 的四组合 eligibility 纯函数；
  layerless 地形与分层地形同 World 时只计入 unregistered、不污染页驻留；
  网格最后列/行半 texel 内缩；编译器拒绝非 D32 feedback 深度。
