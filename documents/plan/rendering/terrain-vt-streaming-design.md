# 地形 VT 流式闭环设计（GPU feedback → CPU 驻留 → 子区域上传）

> **状态：未来工作，非契约。** 本文只描述下一阶段计划，不表示任何能力已经实现。
> 当前运行时行为以 [`contracts/terrain.md`](../../contracts/terrain.md) 和源码为准；
> 本文中的格式、预算、接口和阶段都可以在实现前调整。

**实施状态（2026-10-30）**：P0（World-local state/epoch 管道）、P1（批量上传、
record-only 入口）、P2（`terrainFeedback` pass、回读 ring、只读统计）、P2.5
（每帧 epoch 回灌、feedback 只更新 `lastSeenEpoch`、`RequestPageAtEpoch` 安全窗、
上传 pin 计数、MI 预览共享 state、heightScale=0/负值与缺页中性表面）和 P4
（placementId、fallback 解耦、32×32 注册）已落地并写入 `contracts/terrain.md`；
P3（feedback 驱动驻留、真实 GPU 淘汰）与 P3.1（非 VT 地形隔离、epoch-aware LRU、
按 epoch 降预算、quarantine 主动回收、512 默认预算、D32 深度合同、页范围末端内缩）
已于 2026-10-30 落地并写入 `contracts/terrain.md`。本文件其余内容仍是计划与证据，
不是当前行为。

| 项目 | 内容 |
| --- | --- |
| 类型 | 实施计划 / 架构决策 |
| 状态 | P0–P4 + P3.1 已实现：World-local state、batch 上传、feedback 回读、`preload=false`、feedback 驱动请求/淘汰、每帧上传 ring、物理槽位 quarantine/回收与 epoch-aware LRU |
| 修订 | 2026-10-29 第 1 版 |
| 目标 | 16km 级大世界、256 全局层、每 tile 32 候选层、128² page、32×32 page table / 4096² atlas |
| 本期范围 | 只做“GPU feedback → CPU 读取 → page manager 驻留/LRU → 子区域上传”的闭环 |
| 非目标 | GPU-driven 几何、LOD/skirt、地形碰撞、地形编辑、albedo/normal/pbr 的完整 VT、256+ 层纹理数组 |
| 关联 | [地形大世界与上百层地表](terrain-large-world-layers-plan.md)、[地形复刻计划](terrain-openworld-demo-plan.md)、[terrain 契约](../../contracts/terrain.md)、[测试契约](../../contracts/testing.md)、[GBuffer 通道所有权](../../contracts/gbuffer-channel-ownership.md) |

## 0. 结论

### 0.1 推荐路线

采用 **方案 A：专用低分辨率 feedback RT + 异步回读**。它是一个独立的小颜色附件，
不是 GBuffer 的附加通道，也不改变 deferred 的 8 张颜色附件。

闭环形状：

```text
terrainFeedback pass（128×128 RGBA8）
  -> 帧末 CopyImageToBuffer 到 host-visible ring
  -> 等该帧 frame fence 完成后 CPU 解析
  -> TerrainPageManager 请求/命中/LRU/淘汰
  -> 下一帧提交前把 dirty page table + atlas 页批量上传
  -> geometry pass 读取新的 page table/atlas
```

### 0.2 三方案结论

| 方案 | 结论 | 原因摘要 |
| --- | --- | --- |
| A. 独立小 RT + 异步回读 | **推荐** | 不碰 GBuffer、带宽小、可复用现有 frame fence；代价是需要新增一个专用 pass |
| B. 复用 GBuffer/现有 pass 的 MRT 附加 attachment | **否决** | `geometry` 已经写满 8 张颜色附件；`forwardOpaque` 只允许 Eye；借用现有通道会破坏 deferred/选中/速度语义 |
| C. compute pass | **暂缓** | 当前 RenderGraph 没有通用 compute pass；compute 不知道光栅化后的可见页，除非先有 visibility/page 列表，基础设施大于本期收益 |

### 0.3 “不新增 pass”能做什么

**不能在不新增 pass 的前提下得到干净的逐像素 feedback。**

- `geometry` pass 的 8 张颜色附件已经占满；再加第 9 张会违反当前附件上限，也会改变所有 GBuffer 材质的输出壳。
- 把 feedback 塞进 `gbufferVelocity`、`sceneColorBase.a` 或其它现有通道，会与 deferred 的通道所有权冲突。
- `forwardOpaque` 在当前材质校验中只允许 Eye；`forwardTransparent` 不绘制 terrain；`postProcess` 只绘制 pass material 的全屏三角形，不遍历 terrain draw packet。
- 如果强制“不新增 pass”，只能退化为 CPU 视锥/距离预取，或者用 occlusion query 得到 tile 级可见性；它们都不是本文定义的逐像素 GPU feedback。

因此本期接受一个**很小的专用 pass**：固定 128×128、单颜色附件 + 128×128 深度、
无 MSAA。P3 的深度测试用于让最近页赢得 feedback texel。

## 1. 事实基线（2026-10-29）

这一节只记录源码里能核对到的现状，供后续 PR 判断“已经有什么、还缺什么”。

### 1.1 当前地形数据流

```text
SC_*.json (type: "terrain")
  -> SceneAssetValidator::BuildLoadPlan
  -> TerrainAssetLoader::Prepare(terrainPath)
  -> RendererResourceLoadCoordinator::LoadRendererResources
  -> RendererTerrainLoader::LoadTerrainObject
  -> TerrainPageManager::RegisterTerrain(preload=true)
  -> CreateTerrainPageGpuResources + ApplyTerrainPageUploads
  -> geometry pass
```

- 场景对象类型、TR 校验和 chunk 网格生成仍在 `source/scene/validation/sceneAssetValidator.cpp`、
  `source/terrain/terrainAssetLoader.*`、`source/terrain/terrainMeshBuilder.*`。
- 地形没有独立 World 类型，仍复用 `MeshObjectBuildPlan`、mesh object 和 geometry pass；
  这一事实由 `documents/contracts/terrain.md` 的“数据流”和“网格”两节固定。
- `RendererTerrainLoader::LoadTerrainObject` 只生成 chunk 计划，并把 `pageTable`/`layerIds`/
  `layerWeights` 绑定到材质；它没有逐帧入口。
  （`source/render/resource/rendererTerrainLoader.cpp::LoadTerrainObject`）

### 1.2 页管理器的虚拟页/物理槽位模型

`TerrainPageManager` 已经是 CPU-only 的页管理器，不持有 Vulkan 对象：

- `PageTableSize = TerrainPageTableSize = 32`、`PageResolution = 128`、
  `AtlasResolution = 4096`、`PageSlotCount = 1024`。
- `FallbackPageX/Y = 31/31`，`UsablePhysicalSlotCount = PageSlotCount - 1 = 1023`。
- `PageState` 同时保存 `virtualPageX/Y`（页表 texel 坐标）和 `physicalPageX/Y`
  （atlas 槽位）；`MakeResident` 在虚拟坐标处写页表项，内容是该页的物理槽位。
- `PageTableUpload.virtualPageX/Y` 是页表 texel 的 image offset；
  `AtlasPageUpload.physicalPageX/Y` 是 atlas 的物理页坐标。
- `EvictPageAt` 清零 atlas 页、失效页表项，并释放物理槽位；`AllocatePhysicalSlot`
  从空闲槽位分配；`FindEvictionCandidateIndex` 先看 priority，再看 `lastUsedFrame`。

核对点：

- `source/terrain/terrainPageManager.h`：`PageTableSize`、`PageResolution`、
  `AtlasResolution`、`FallbackPageX/Y`、`UsablePhysicalSlotCount`、`PageTableUpload`、
  `AtlasPageUpload`、`CollectDirtyUploads`、`CommitDirtyUploads`。
- `source/terrain/terrainPageManager.cpp`：`EnsureStorage`、`RegisterTerrain`、
  `MakeResident`、`RequestPage`、`EvictPageAt`、`AllocatePhysicalSlot`、
  `SetResidentCapacity`。

### 1.3 当前注册与预算的硬限制

这些限制会直接影响 16km 目标；P0/P4 后状态如下：

1. **同一资产可以多处摆放。** P4 后注册身份是场景 `placementId`；同一 TR 用不同对象名
   摆放两次是合法的，只有重复 `placementId`/虚拟区域重叠才失败。
2. **fallback 不再占用虚拟页。** P4 后 fallback 只保留物理 atlas 槽位 `(31,31)`；
   虚拟 `(31,31)` 是普通页，`pageTilesPerSide=32` 的单地形可以注册。
3. **可驻留页最多 1023。** 32×32 全页表有 1024 个虚拟页，但 fallback 占一个物理槽位；
   因此 1024 页仍需 `preload=false`/feedback 淘汰，不能同时全部常驻。
4. **preload 失败已可观测。** P4 后 `RegisterTerrain(preload=true)` 在容量不足且无法
   淘汰时记入 `Stats::preloadDeferredCount` 并继续，不会静默死循环。
5. **page manager 已是 World-local。** P0 后 `TerrainPageManager` 与 GPU page 资源都在
   `TerrainStreamingState`，随 WorldLocalResourcePackage 发布/退休。

核对点：

- `source/render/resource/rendererResourceLoadCoordinator.cpp::LoadRendererResources`：
  `TerrainStreamingState`、`RegisterTerrain(terrainPlan)`、
  `CreateTerrainPageTableGpuResource`/`CreateTerrainPageAtlasGpuResources`、
  `ApplyTerrainStreamingUploads`。
- `source/terrain/terrainPageManager.cpp::RegisterTerrain` 的重复注册和重叠检查。
- `source/terrain/terrainPageManager.cpp::EnsureStorage` 的 fallback 预留。

### 1.4 当前上传路径

`ApplyTerrainPageUploads` 是加载期同步路径：

- 它 `CollectDirtyUploads` 后创建一个覆盖本批全部数据的 staging buffer：页表项、
  ID atlas 页和权重 atlas 页按 16 字节对齐分段打包，只 map/unmap 一次。
- P1 后三张 image 的 transition + 批量 copy 都录制进同一个 command buffer：
  页表、`layerIds`、`layerWeights` 共用一次 `BeginSingleTimeCommands` +
  `EndSingleTimeCommands`，即 1 次 submit + wait。
- `CopyBufferToImageRegions` 内部仍是一个 command buffer、一次
  `BeginSingleTimeCommands` + `EndSingleTimeCommands`；调用方负责 layout 往返。
- 全部成功后才 `CommitDirtyUploads`；失败路径销毁 staging 并保留 dirty。

核对点：

- `source/render/resource/rendererTerrainLoader.cpp::ApplyTerrainPageUploads`。
- `source/commonFunction.h::CopyBufferToImageRegion`。
- `source/commonFunction.h::CopyBufferToImageRegions`：所有 region 共用同一
  command buffer，一次 submit + wait；`vl_renderer_backend_tests` 的
  `BufferToImageRegions.*` 覆盖空列表/零 extent 拒绝和多 region 回读。

另外：

- `CreateTerrainPageAtlasGpuResources` 用 4096² 的全零 `HostImage` 创建两张 RGBA8 atlas；
  主机侧各 64 MiB，合计约 128 MiB 的零填充输入；page table 现在始终单独创建（4 KiB）。
- 页面表本身只有 32×32×4 = 4 KiB。
- `RendererBackendVulkan::CopyImageToBuffer` 仍是同步 `BeginSingleTimeCommands` 路径；
  P1/P2 的帧内回读使用 record-only 的 `RecordCopyImageToBuffer`，不再走这个同步入口。

核对点：

- `source/render/resource/rendererTerrainLoader.cpp::CreateTerrainPageGpuResources`。
- `source/render/backend/rendererBackendVulkan.cpp::CopyImageToBuffer`。
- `source/commonFunction.h::CopyImageToBuffer`。

### 1.5 Shader 现状

`M_terrain` 当前已经消费 page table 和 ID/weight atlas；P2 另有独立的
`shader/glsl/pass/M_terrainFeedback.json` + `terrainFeedback.vert/frag`：

- `M_terrain.json` 声明 `layerTable`、`pageTable`、`layerIds`、`layerWeights`、
  三张 `sampler2DArray`，以及 `u_pageTableSize`、`u_pageResolution`、`u_uvScale`、`u_debugMode`。
- `mf_terrainLayerBlend.glsl` 用 `texelFetch(pageTable, tile, 0)` 查页表；
  `page.a < 0.5` 时回退到 `pageTableSize - 1`（当前 31）。
- 页表坐标来自顶点 `texCoord1`：`tile = floor(texCoord1 * u_pageTableSize)`；
  本地 UV 用 `fract(texCoord1 * u_pageTableSize)`。
- `M_terrain` 本身没有 feedback 输出；`terrainFeedback` pass 用独立 shader 输出
  page 坐标/valid/miss/priority/frameTag 到 128×128 RGBA8 RT，不改变 `M_terrain`。

核对点：

- `shader/glsl/M_terrain.json`。
- `shader/glsl/M_terrain.surface.glsl`、`shader/glsl/M_terrain.vertex.glsl`。
- `shader/glsl/materialFunction/mf_terrainLayerBlend.glsl` 的 `TerrainLayerExtra`、
  `EvaluateMFTerrainInputs`。
- `shader/glsl/pass/M_terrainFeedback.json`、`pass/terrainFeedback.vert/frag`。

### 1.6 RenderGraph 与附件现状

- `config/renderGraphConfig.json` 的 `geometry` pass 写出 8 张颜色附件
  （`gbufferA`..`gbufferF`、`gbufferVelocity`、`sceneColorBase`）加 `sceneDepth`。
- `shader/glsl/engine/passTemplate/base.frag.glsl` 明确写着 “Geometry 已占满设备允许的 8 个颜色附件”，
  选中标记复用 `gbufferVelocity.z`。
- `documents/contracts/gbuffer-channel-ownership.md` §1 把“8 个颜色附件已经占满”定义为硬约束；
  新增 deferred 模型不得新增附件，必须复用已有通道。
- `forwardOpaque` pass 当前只服务 Eye；`source/material/validation/materialAssetValidator.cpp`
  对非 Eye 的 `ForwardOpaque` 直接报错。
- `PassRuntime::RecordPostProcessPass` 只绑定 pass material 并调用 `renderPass.Draw`，
  不遍历 scene draw packet；因此不能把 feedback 伪装成 postProcess pass。
- `RenderGraphPassType` 现在有 `Shadow`、`Geometry`、`ForwardOpaque`、`ForwardTransparent`、
  `PostProcess`、`EditorGrid`、`AutoExposure`、`TerrainFeedback` 八种；仍没有通用 compute pass。
  `AutoExposure` 是编译器里特判的 1 输入/1 输出 `R32_SFLOAT` 节点。
- `TerrainFeedback` 是编译器特判的固定 128×128 单颜色节点；P2 只输出统计。
- 固定尺寸资源是支持的：`widthSize`/`heightSize` 在 `renderGraphCompiler.cpp` 编译，
  `renderGraph.cpp` 用第一个 output 的 `resource.width/height` 作为 pass 尺寸。
- pass input 支持 `source: "worldTexture"`；因此 feedback pass 可以把 World 级
  `pageTable` 作为输入采样，而不需要把 page table 提升成 graph-owned 资源。

核对点：

- `config/renderGraphConfig.json` 的 `geometry` pass。
- `source/render/rendergraph/renderGraphPassType.h`。
- `source/render/rendergraph/renderGraphCompiler.cpp::ParseRenderGraphPassType`、
  `CompileResourceSize`、`CompilePassInputs`、AutoExposure 特判。
- `source/renderGraph.cpp::CreateRenderpass` 中 `renderpass.width = resources[0].width`。
- `source/render/pass/passRuntime.cpp::RecordPostProcessPass`、`RecordGeometryPass`。
- `source/material/validation/materialAssetValidator.cpp` 的 `usesForwardOpaqueRenderMode` 校验。

### 1.7 Frame fence 与 ResourceRetireQueue 现状

- `MAX_FRAMES_IN_FLIGHT` 在 `source/commonFunction.h` 中是 2。
- `RendererBackendVulkan::BeginFrame` 等待当前 `frameIndex` 的 `taskFinishedFence`，
  然后对 `frameFenceEpochs[frameIndex]` 调
  `ResourceRetireQueue::CollectCompletedEpoch`，再清空该槽位。
- `RendererBackendVulkan::SubmitFrame` 设置
  `submittedEpoch = currentFrame + 1`，写入 `frameFenceEpochs[frameIndex]`，
  并调 `ResourceRetireQueue::MarkFrameSubmitted(submittedEpoch)`。
- `ResourceRetireQueue` 的释放谓词是 `lastUsedEpoch <= lastCompletedEpoch`；
  它不是“当前帧开始就全部完成”，而是按已完成 epoch 递增。

核对点：

- `source/render/backend/rendererBackendVulkan.cpp::BeginFrame`、`SubmitFrame`。
- `source/render/resource/resourceRetireQueue.h`、`.cpp`
  `RetireShared`、`MarkFrameSubmitted`、`CollectCompletedEpoch`、`GetLastSubmittedEpoch`。

### 1.8 现状直接推出的缺口

| 缺口 | 现状依据 | 本期需要的动作 |
| --- | --- | --- |
| 没有 feedback 载体 | P2 已新增 `terrainFeedback` pass/RT 与回读 ring | 已解决；剩余是 P3 的驻留闭环 |
| CPU 驻留状态不持久 | P0 已提升为 World-local `TerrainStreamingState` | 已解决；剩余是帧内决策 |
| 上传不是每帧增量 | P1 已有 `RecordTerrainPageUploads` record-only 入口 | 已解决；剩余是每帧限流/批量 ring |
| 回读没有 ring | P2 已有 host-visible feedback ring | 已解决；剩余是驻留消费 |
| 淘汰没有 GPU 保护 | `EvictPageAt` 只检查 CPU `referenceCount` | 增加 GPU epoch 安全窗 + barrier |
| 全量 preload 与 16km 冲突 | fallback 已在 P4 解耦；仍 `preload=true` | P3 改为按需注册/feedback 驱动 |

## 2. 目标、预算与边界

### 2.1 目标数字

| 项目 | 目标值 | 说明 |
| --- | --- | --- |
| 世界尺寸 | 16 km × 16 km（1,600,000 cm） | 沿用大世界计划的 16km 目标 |
| 全局层数 | 256 | layer table 上限仍为 256 |
| 每 tile 候选层 | 32 | 由 CPU 烘焙阶段筛选，不进入逐像素循环 |
| 每像素有效层 | top-4 | ID/weight 图每 texel 4 个 ID + 4 个权重 |
| tile 尺寸 | 512 m（51,200 cm） | 32×32 tile 覆盖 16.384 km，取整覆盖 16km |
| page 大小 | 128×128 | `TerrainPageManager::PageResolution` |
| page table | 32×32（1024 虚拟页） | `TerrainPageManager::PageTableSize` |
| atlas | 4096×4096 | 32 × 128 |
| ID atlas | RGBA8，64 MiB | 每 texel 4 个 8-bit layer ID |
| weight atlas | RGBA8，64 MiB | 每 texel 4 个 8-bit 权重 |
| 物理页槽位 | 1023（当前 fallback 占 1） | 目标模型应把 fallback 与虚拟页表解耦，见 §8 |
| feedback RT | 128×128 RGBA8，64 KiB | 默认推荐；64×64 是低成本档 |
| feedback ring | 2 帧 | 等于当前 `GetFrameFenceCount()`；随 frame-in-flight 调整 |
| upload staging | 每帧最多 16 页，约 2.1 MiB | 16 × (64 KiB IDs + 64 KiB weights) + page table |
| upload ring | 2 帧，约 4.2 MiB | 与 frame-in-flight 对齐 |

### 2.2 预算结论

- 固定 VT 资源约为 **128 MiB atlas + 4 KiB page table + 8 KiB layer table**，
  再加 feedback/upload ring 约 **4.3 MiB**；本期不改变这个数量级。
- 16km 的 32×32 虚拟页可以覆盖世界，但**不能同时全部常驻**：
  1024 个虚拟页对 1023 个物理槽位，且流式本来就不需要全驻留。
- 因此 `preload=true` 只能作为小场景/测试兼容路径；16km 必须使用
  `RegisterTerrain(preload=false)` + feedback/预取驱动 `RequestPage`。
- P4 已解开 fallback 与虚拟 `(31,31)` 的耦合；在此之前的 P0–P3 走查场景应使用
  `pageTilesPerSide <= 31`，或把多个地形放在不覆盖 `(31,31)` 的 `pageOrigin` 区域；
  不要把“32×32 单地形注册失败”误判成 feedback 闭环失败。

### 2.3 本期非目标

- 不做 GPU-driven 几何、indirect draw、meshlet、GPU LOD 选择。
- 不做 terrain chunk 的按需生成/卸载；`TerrainMeshBuilder::BuildChunks` 仍在加载期生成全部 chunk。
- 不做 albedo/normal/pbr 三张纹理数组的按需 VT；本期只让 **ID/weight atlas** 流式。
- 不做地形碰撞、heightfield、地形编辑、绘制工具。
- 不把 128² page 扩成 256²；不把 32×32 page table 扩成 64×64。
- 不把 feedback 当作正确性来源；缺页由 fallback、预取和下一帧反馈兜底。

## 3. Feedback 载体方案对比

### 3.1 方案 A：专用小 RT + 异步回读（推荐）

**形状**

```text
terrainFeedback pass（128×128 RGBA8，单颜色附件，无 MSAA）
  -> 对每个 terrain fragment 编码 virtualPageX/Y + priority + frameTag + miss
  -> 帧末 CopyImageToBuffer 到 host-visible ring[frameIndex]
  -> BeginFrame 等到该槽 fence 后 CPU 解析
```

**与现有 RenderGraph/attachment 合同**

- 不触碰 `geometry` 的 8 张颜色附件，不改变 deferred 通道所有权。
- 复用固定尺寸资源能力：`widthSize: 128`、`heightSize: 128`；
  `renderGraph.cpp` 会以第一个 output 的尺寸建立 pass framebuffer。
- 只有一个颜色附件，不需要深度；不会与 `sceneDepth`、`selectionMask` 竞争。
- 需要新增一个 `RenderGraphPassType::TerrainFeedback`，因为原有七种 pass 都不能
  “遍历 terrain draw packet 并写一个专用小 RT”。

**同步成本**

- 每帧读取 128×128×4 = 64 KiB；按 2 帧 ring 约 128 KiB。
- 回读不阻塞提交：CPU 只在对应 frame fence 完成后读旧槽位。
- 相比把 feedback 塞进全分辨率 GBuffer，带宽小两个数量级以上。

**绘制成本**

- P2 可以先用现有 terrain chunk draw packet 做正确性参考，确认 page 编码和
  page table 读取无误。
- 16km 走查前应改成“每页 1 个 CPU-built proxy quad”：按 page 的 XZ 包围盒和
  高度范围铺一个粗四边形，只画视锥内的页。一个 32×32 页表最多 1024 个 quad，
  不会把完整地形顶点成本翻倍；页级反馈本来就是启发式，不需要逐三角形精度。
- 两种绘制方式必须写同一套 virtual page 编码，不能用不同 page 坐标约定。

**改动面**

- RenderGraph：新增资源、pass 类型、编译器校验、`PassRuntime` case。
- 绘制：新增 terrain feedback draw domain，只遍历带 terrain 标记的 draw packet。
- Shader：新增 feedback pass material / 输出壳，读取 `pageTable`（`worldTexture` 输入）。
- 渲染循环：feedback copy、ring、`completedEpoch` 消费点。
- 不动 geometry 的 8 个输出，不动 deferredLighting。

### 3.2 方案 B：复用 GBuffer/现有 pass 的 MRT 附加 attachment

**为什么看起来可行**

- `geometry` 已经是多 RenderTarget pass，表面上“再加一张小 RT”很自然。
- `forwardOpaque`/`forwardTransparent` 也有颜色附件。

**为什么否决**

1. `geometry` 已经是 **8 张颜色 + 1 张深度**；当前设备和合同都把 8 张颜色附件视为上限。
   `shader/glsl/engine/passTemplate/base.frag.glsl` 的注释和
   `documents/contracts/gbuffer-channel-ownership.md` §1 都明确记录了这一点。
2. 如果硬加第 9 张颜色附件：
   - 需要改 `base.frag.glsl` 的所有 GBuffer 输出；
   - 需要改 geometry 的 render pass/framebuffer/管线兼容性键；
   - 需要重新验证所有 deferred 模型和 forward 路径；
   - 还可能直接超过设备的 `maxColorAttachments`。
3. 如果借用现有通道：
   - `gbufferVelocity.z` 已被选中标记占用；
   - `sceneColorBase.a` 在 Subsurface/Eye 等路径有明确语义；
   - `gbufferD/E/F` 按 ShadingModelID 有所有权账本；
   - 任何“临时借用”都会让 deferred 读回错误数据。
4. `forwardOpaque` 当前只允许 Eye（`materialAssetValidator.cpp`），terrain 不走它；
   `forwardTransparent` 不绘制 opaque terrain；`postProcess` 不遍历 terrain draw packet。

**结论**：在现有合同下不可行。若未来设备确实支持更多颜色附件，也应作为独立的
GBuffer 布局迁移处理，而不是本期的 feedback 捷径。

### 3.3 方案 C：compute pass

**优点**

- 不需要颜色附件；可以直接写 storage image/buffer。
- 如果已经有 visibility buffer 或 page 列表，compute 可以做去重、排序、优先队列。

**当前不选的原因**

- `RenderGraphPassType` 没有通用 compute；`AutoExposure` 是编译器特判的
  1 输入/1 输出 `R32_SFLOAT` 节点，不是可复用的 compute authoring 路径。
- compute 没有光栅化覆盖率：它不知道哪些 page 在屏幕上真正被 terrain 覆盖。
  只按相机距离/视锥算，得到的是 CPU 预取，不是 GPU feedback。
- 要做“compute 读 visibility buffer → 生成 page 列表”，需要先有 visibility buffer
  或 depth/page proxy pass，基础设施比方案 A 大。
- 需要 storage image/buffer 描述符、barrier、material/pass 绑定和测试，
  还会引入 GPU 写入与 CPU 回读的额外同步。

**结论**：作为后续优化保留。等本期 feedback 闭环稳定、需要 GPU 侧聚合/去重时再评估。

### 3.4 其它捷径

| 捷径 | 为什么不做 |
| --- | --- |
| 在 terrain fragment shader 里直接写 storage image | 当前材质 texture schema 只覆盖 sampler；要新增 storage image 绑定、pass input 语义和写 hazard 规则，改动面不小于方案 A |
| 用 occlusion query 代替 feedback | 只能得到 tile 级“是否可见”，没有屏幕覆盖/优先级；查询数量和读回节奏也不好控制 |
| 用全分辨率 GBuffer 回读 | 1080p RGBA8 约 8 MiB/帧，4K 约 33 MiB/帧；带宽和 CPU 解析都不可接受 |
| 用 CPU 视锥/距离预取 | 可以作为兜底，但不知道遮挡和实际屏幕覆盖，容易过度驻留或漏页 |

### 3.5 对比表

| 维度 | A. 专用小 RT | B. 现有 pass MRT | C. compute pass |
| --- | --- | --- | --- |
| 附件合同 | 新增 1 张独立小 RT，不碰 GBuffer | 与 8 张颜色上限/通道所有权冲突 | 不用颜色附件，但要 storage 资源 |
| RenderGraph 兼容 | 固定尺寸资源已支持；需新增 pass 类型 | 需要改 geometry 输出壳/管线/所有 GBuffer 材质 | 需要新增通用 compute pass 类型/描述符 |
| 同步成本 | 64 KiB/帧，异步 fence 回读 | 全分辨率回读，MiB/帧 | 取决于 visibility/page 列表；可能更低 |
| 改动面 | 中 | 大且跨合同 | 大 |
| 本期结论 | **推荐** | 否决 | 暂缓 |

## 4. Feedback 数据格式

### 4.1 RT 规格

| 项目 | 值 |
| --- | --- |
| 资源名 | `terrainFeedback` |
| 格式 | `R8G8B8A8_UNORM` |
| 尺寸 | 128×128（固定 `widthSize`/`heightSize`） |
| usage | `colorAttachment`、`transferSrc` |
| MSAA | 否 |
| 深度 | 无 |
| loadOp | `clear` |
| storeOp | `store` |
| 颜色 | 只写 terrain fragment；非 terrain 像素保持 clear 的 0 |

建议同时保留一个 64×64 档位用于低端机/调试；默认 128×128。

### 4.2 Texel 位布局（格式版本 1）

每个 texel 4 字节：

| 通道 | 位 | 含义 |
| --- | --- | --- |
| R | 0..4 | `virtualPageX`（0..31） |
| R | 5 | `missFlag`：采样 page table 时 `page.a < 0.5` |
| R | 6 | `validFlag`：该像素确实被 terrain fragment 覆盖 |
| R | 7 | 保留，写 0 |
| G | 0..4 | `virtualPageY`（0..31） |
| G | 5..7 | 保留，写 0 |
| B | 0..7 | `priority`（0..255，越大越应驻留） |
| A | 0..7 | `frameTag`：`(submittedEpoch % 255) + 1`；0 表示无效 |

说明：

- 32×32 page table 只需要 5+5 bit；把 `miss/valid` 放进 R 的高位，避免额外通道。
- `frameTag` 用 8 bit 是为了让 CPU 丢弃过期 ring 数据；0 保留为“未写入/无效”。
  255 帧的回绕远大于本期 2 帧 ring 延迟。
- 如果未来 page table 扩到 64×64，5 bit 不够；必须同时升级本格式版本、packer、
  shader 和 `TerrainPageManager`，不能静默复用 bit 位。

### 4.3 Priority 定义

priority 是“屏幕覆盖 + 缺页偏置”的启发式，不是精确覆盖率：

```glsl
vec2 pageUv = context.texCoord1 * u_pageTableSize;
float pageDerivative = max(fwidth(pageUv.x), fwidth(pageUv.y));
float pagePixels = 1.0 / max(pageDerivative, 1.0e-4);
float coverage = clamp(log2(pagePixels + 1.0) / 8.0, 0.0, 1.0);
uint priority = uint(round(255.0 * coverage));
if (miss)
    priority = max(priority, 192u);
```

- 远处页面的 `pagePixels` 小，priority 低，优先被淘汰。
- 近处/斜视角覆盖大的页面 priority 高。
- 缺页页至少给 192，保证下一帧优先请求。
- 这只是反馈信号；最终是否驻留仍由 `TerrainPageManager` 的容量、refcount 和
  GPU barrier 的读写顺序决定。

### 4.4 降采样策略

| 反馈分辨率 | 每帧字节 | 1080p 下每 texel 覆盖 | 4K 下每 texel 覆盖 | 用途 |
| --- | --- | --- | --- | --- |
| 64×64 | 16 KiB | 约 30×30 px | 约 60×60 px | 低端/快速验证 |
| 128×128 | 64 KiB | 约 15×15 px | 约 30×30 px | **默认** |
| 256×256 | 256 KiB | 约 7.5×7.5 px | 约 15×15 px | 高精度调试 |

降采样不保证每个可见页都出现一次；多个页落到同一 texel 时会有碰撞。
这是可接受的：feedback 是驻留提示，漏页由 fallback、预取和下一帧反馈兜底。
如果碰撞导致明显缺页，再把 128×128 提升到 256×256，而不是把 feedback 塞进 GBuffer。

### 4.5 CPU 回读 buffer 与 ring

```cpp
// N = rendererBackend.GetFrameFenceCount()，当前为 MAX_FRAMES_IN_FLIGHT = 2
struct TerrainFeedbackRing
{
    std::array<vk::Buffer, N> buffers;
    std::array<vk::DeviceMemory, N> memories;
    std::array<void*, N> mapped;
    std::array<bool, N> pending;
    std::array<uint64_t, N> copiedEpoch;
};
```

- 每个槽位 128×128×4 = 65,536 字节；N=2 时总计 131,072 字节。
- buffer 使用 `eTransferDst | eHostVisible | eHostCoherent`，常驻映射。
- `RecordFeedbackCopy` 在帧末把 `terrainFeedback` 拷到 `buffers[frameIndex]`。
- `pending[frameIndex] = true`、`copiedEpoch[frameIndex] = submittedEpoch` 在
  `SubmitFrame` 成功后登记。
- 下一次使用同一 `frameIndex` 的 `BeginFrame` 已经等待过该槽 fence；
  此时才允许 map/parse，然后清 `pending`。

### 4.6 读回节流

- 默认 `FeedbackIntervalFrames = 2`，即每 2 帧消费一次。
- 约束：`FeedbackIntervalFrames` 必须是 `GetFrameFenceCount()` 的整数倍；
  这样消费的 ring 槽位正好是 `BeginFrame` 已等待 fence 的槽位。
- 可配置为 2/4/6；不要配置成 1 以外的非整数倍，否则需要额外的“哪个槽已完成”查询。
- 回读本身可以在每次消费帧才录制 `CopyImageToBuffer`，其余帧不产生回读带宽。
- 低帧率/后台窗口可以提高到 4 或 6，减少 CPU 解析；前台走查用 2。

## 5. 同步与生命周期

### 5.1 帧内顺序

```text
BeginFrame(currentFrame)
  -> wait taskFinishedFence[frameIndex]
  -> completedEpoch = frameFenceEpochs[frameIndex]
  -> ResourceRetireQueue::CollectCompletedEpoch(completedEpoch)

TerrainStreaming::ConsumeCompletedFeedback(frameIndex, completedEpoch)
  -> 只消费 pending && copiedEpoch <= completedEpoch 的槽
  -> 解析 texel，按 virtual page 聚合
  -> TerrainPageManager::ApplyFeedback(...)
  -> 生成 request/hit/miss/eviction 决策

TerrainStreaming::BuildUploadBatch(...)
  -> 受 MaxUploadPagesPerFrame 限制
  -> 返回 page table + atlas 上传描述和 revision

RecordTerrainPageUploads(commandBuffer, batch)
  -> 在 geometry/terrain feedback 之前
  -> pageTable/atlas: ShaderReadOnly -> TransferDst -> ShaderReadOnly
  -> 单 command buffer、单次 submit（随主帧提交）

RecordScenePasses(...)
  -> geometry 读取新的 page table/atlas
  -> terrainFeedback pass 读取同一 page table

RecordFeedbackCopy(commandBuffer, frameIndex)
  -> terrainFeedback: ColorAttachment -> TransferSrc
  -> CopyImageToBuffer -> ring[frameIndex]
  -> TransferSrc -> ColorAttachment（或显式 initialLayout）

SubmitFrame(...)
  -> submittedEpoch = currentFrame + 1
  -> MarkFrameSubmitted(submittedEpoch)
  -> 成功后 CommitUploadBatch(batch)；失败则 AbortUploadBatch
```

### 5.2 三条硬规则

#### 规则 1：回读完成前不得复用 staging/fence

- feedback ring 和 upload staging ring 都按 `frameIndex` 分槽，槽位数等于
  `GetFrameFenceCount()`。
- 同一个槽位只有在该槽的 `taskFinishedFence` 完成后才能写入/映射。
  `BeginFrame` 的 fence wait 就是这个保证。
- CPU 解析 feedback 前必须检查 `copiedEpoch <= completedEpoch`；
  不满足就保留 `pending`，不能读半完成的 buffer。
- 上传 staging 在提交前由 CPU 填充；提交后 GPU 读取。下一次填充同一槽必须等
  `frameIndex` 的 fence，不得提前覆盖。

#### 规则 2：驻留变更只在下一次提交前应用

- `ApplyFeedback`、`RequestPage`、`EvictPage*` 只允许发生在 `BeginFrame` 之后、
  录制 scene pass 之前。
- `SubmitFrame` 之后不再修改 page manager，不再改 page table/atlas 的 dirty 状态。
- 这样同一帧的 geometry 看到的是同一份页表；不会出现“前半帧旧页表、后半帧新页表”。
- World 切换/重建时，旧的 streaming state 通过 World-local package 的 shared_ptr
  保持到帧末，再由 `ResourceRetireQueue` 退休；不能直接 delete。

#### 规则 3：淘汰页在 GPU 仍可能采样的帧内不能清零

仅有 CPU `referenceCount` 不够，因为 2 帧 in-flight 时，上一帧可能仍在采样某个页。

保护分两层：

1. **CPU 安全窗（只减少抖动，不单独保证正确性）**：记录每页的 `lastSeenEpoch`，
   并记录已经消费到的 `lastConsumedFeedbackEpoch`。只有
   `lastSeenEpoch + HysteresisFrames <= lastConsumedFeedbackEpoch` 的页才允许成为淘汰候选；
   没有安全候选时 `RequestPage` 返回 false，保留 miss，下一帧再试。
   这个窗口避免“刚看见就淘汰”的抖动，但它无法知道尚未消费的 in-flight 帧是否正在采样该页，
   所以不能把它当成唯一保护。
2. **GPU barrier（正确性保证）**：真正清零 atlas 页/失效页表项之前，在帧 command buffer 中插入
   覆盖此前所有提交的读写屏障（`eShaderRead`/`eColorAttachmentRead` →
   `eTransferWrite`，`eAllCommands` 或等价的 stage/access 组合），再执行 clear/copy。
   这样即使某个 in-flight 帧仍在采样旧页，清零也会排在它的读取之后。

对应到现有语义：

- `ResourceRetireQueue::GetLastSubmittedEpoch()` 提供 `lastSubmittedEpoch`；
  `TerrainStreamingState` 另记 `lastConsumedFeedbackEpoch` 作为 CPU 淘汰窗的输入。
- `RendererFrameContext` 增加 `completedEpoch`（纯 CPU 字段），由
  `frameFenceEpochs[frameIndex]` 填入，供 feedback 消费和测试使用。
- 页面内容的“退休”不是销毁资源，而是页表/atlas 内容变更；因此不走
  `ResourceRetireQueue::RetireShared`，而是走上面的 GPU barrier，并用 CPU
  安全窗减少无谓同步。只有 World-local package（包含 page manager 和 GPU
  textures）整体替换时才走 `RetireShared`。

### 5.3 失效场景与规避

| 失效场景 | 后果 | 规避 |
| --- | --- | --- |
| 读取尚未完成的 feedback 槽 | 读到旧帧/半写数据，误请求或误淘汰 | `pending` + `copiedEpoch <= completedEpoch` 双条件 |
| feedback buffer 在 GPU 回读前被覆盖 | 帧号/页号错乱 | ring 按 `frameIndex` 分槽；fence 完成后才复用 |
| staging 在 GPU 读取前被 CPU 重写 | 上传内容错页/撕裂 | upload ring 同样按 `frameIndex`，fence 完成后复用 |
| 淘汰页仍在 in-flight 帧中被采样 | 采样到清零 atlas，出现黑洞 | 全队列 barrier 保证读写顺序；CPU 安全窗只减少抖动；无安全候选时延迟淘汰 |
| 同一帧先请求后淘汰同一页 | 刚上传的页被自己清掉 | 本帧请求页加 pin（`referenceCount`/`pinnedForUpload`），淘汰跳过 pin |
| 上传批次失败/提交失败 | dirty 被误清，页永远不更新 | `CommitUploadBatch` 只在 `SubmitFrame` 成功后调用；失败走 `Abort`，保留 dirty/revision |
| 部分 region 成功、部分失败 | 页表与 atlas 不一致 | 一个 command buffer 批量 copy；失败时整批不提交，下一帧整批重试 |
| World 重载时旧 state 被销毁 | 渲染线程悬空 | streaming state 放进 World-local package；按帧捕获 shared_ptr，旧 package 走现有 epoch 退休 |
| `frameFenceCount` 改变/swapchain 重建 | ring 槽位错配 | 重建 ring；`WaitIdle` 后重置 `pending/copiedEpoch` |
| 截图路径 `WaitIdle` 插队 | 重复消费同一槽 | `pending` 消费后立即清零；`WaitIdle` 不改变 `copiedEpoch` 语义 |

## 6. API 形状（伪代码，不实现）

### 6.1 TerrainPageManager 新增/调整

`TerrainPageManager` 继续保持 CPU-only：它只输出 `PageTableUpload`/
`AtlasPageUpload`/revision，不持有 `vk::Image`、`vk::Buffer` 或 command buffer。
Vulkan 录制、staging ring 和 fence 都在 renderer/backend 层完成；这是现有
`terrain_tests` 能纯 CPU 覆盖页生命周期的前提。

```cpp
struct TerrainFeedbackPage
{
    uint32_t virtualPageX = 0;
    uint32_t virtualPageY = 0;
    uint32_t priority = 0;
    bool miss = false;
    uint64_t frameTag = 0;
};

struct TerrainFeedbackFrame
{
    uint64_t epoch = 0;
    std::vector<TerrainFeedbackPage> pages;
};

struct TerrainUploadBatch
{
    uint64_t batchId = 0;
    std::vector<PageTableUpload> pageTableUploads;
    std::vector<AtlasPageUpload> atlasUploads;
    std::vector<uint32_t> pageTableRevisions; // 与 pageTableUploads 对齐
    std::vector<uint32_t> atlasRevisions;     // 与 atlasUploads 对齐
    vk::DeviceSize stagingBytes = 0;
};

// 现有 PageState 增加：
//   uint64_t lastSeenEpoch;
//   uint32_t contentRevision;
//   uint32_t uploadedRevision;
//   bool pinnedForUpload;

void SetGpuEpochContext(
    uint64_t lastSubmittedEpoch,
    uint64_t lastConsumedFeedbackEpoch,
    uint32_t framesInFlight);

// 把一帧 feedback 合并进驻留决策；不直接碰 Vulkan。
void ApplyFeedback(const TerrainFeedbackFrame& frame);

// 显式安全窗淘汰；没有安全候选时返回 false，不强制清页。
bool EvictLeastRecentlyUsed(uint64_t safeEvictionEpoch);

// 按预算收集 dirty 页；返回的 revision 用于提交后确认。
TerrainUploadBatch BuildUploadBatch(uint32_t maxAtlasPages);

// 主帧提交成功后调用；只有 contentRevision 未变的页才清 dirty。
void CommitUploadBatch(const TerrainUploadBatch& batch);

// 提交失败时调用；保留 dirty/revision，下一帧重试。
void AbortUploadBatch(const TerrainUploadBatch& batch);

// 虚拟坐标 -> PageKey，feedback 解析用。
const PageState* FindPageByVirtual(uint32_t virtualPageX, uint32_t virtualPageY) const;
```

约束：

- `ApplyFeedback` 先做 request/hit/miss，再做淘汰；本帧请求的页先 pin。
- `RequestPage` 在容量不足且没有安全候选时返回 false，不抛异常；
  由调用方保留 miss 统计，下一帧重试。
- `SetResidentCapacityAtEpoch` 降低预算时不能立刻清不安全页；可以暂时 over-budget，
  由后续帧继续用 `EvictLeastRecentlyUsed(safeEpoch)` 收敛。旧 `SetResidentCapacity`
  只保留给加载期/测试的无窗口语义。
- `CommitUploadBatch` 不能简单清所有 dirty；必须比较 `contentRevision`，
  防止“收集后被重新标脏”的页被误清。
- `BuildUploadBatch` 必须按 **physical slot** 合并 atlas 上传：同一帧内
  “淘汰旧页 → 复用槽位给新页”只产生一次最终内容上传；page table 仍按
  virtual slot 分别产生失效/有效 entry。

### 6.2 World-local streaming state

把 page manager、GPU page textures、feedback ring、upload ring 收进一个
World-local 对象：

```cpp
struct TerrainStreamingState
{
    TerrainPageManager pageManager;
    TerrainPageGpuResources pageResources;
    TerrainLayerGpuResources layerResources;
    TerrainFeedbackRing feedbackRing;
    TerrainUploadRing uploadRing;
    uint64_t lastConsumedFeedbackEpoch = 0;
    uint64_t ownerGeneration = 0;
};
```

生命周期：

- `RendererResourceCache::WorldLocalResourcePackage` 增加
  `std::shared_ptr<TerrainStreamingState> terrainStreaming;`
  （当前 package 只有 texture/material/mesh/object 表；
  核对：`source/render/resource/rendererResourceCache.h::WorldLocalResourcePackage`）。
- `RendererResourceLoadCoordinator` 创建 state，`RegisterTerrain(preload=false)`，
  创建 GPU 资源，然后把它放进 candidate package。
- 为此增加最小的 cache 接口（伪代码）：

```cpp
void RendererResourceCache::BindTerrainStreaming(
    std::shared_ptr<TerrainStreamingState> state);
std::shared_ptr<TerrainStreamingState>
RendererResourceCache::GetTerrainStreaming() const;
```

- `RendererResourceCache::BeginCandidate` 当前按值复制 package；`terrainStreaming`
  不能在 candidate 与 active 之间无条件共享。完整 World 候选必须创建**新的**
  `TerrainStreamingState`；MI 预览/增量扩展如果不需要改地形页，只共享只读视图或在
  提交后继承 active state。否则候选准备会在渲染线程同时改同一份 page manager。
- `RenderSystem` 每帧从 active package 捕获 shared_ptr；不要在帧中间直接引用
  全局 cache 的可变成员。
- World 替换时旧 package 走现有 `ResourceRetireQueue`；streaming state 的 GPU
  textures 仍是 shared_ptr，退休后自然释放。

### 6.3 RendererTerrainLoader / 上传入口

`RendererTerrainLoader` 不再持有 `const TerrainPageManager&` 作为唯一状态来源，
而是接收 `TerrainStreamingState&`：

```cpp
RendererTerrainLoadResult LoadTerrainObject(
    const nlohmann::json& node,
    const TerrainBuildPlan& plan,
    TerrainStreamingState& streaming) const;
```

加载期：

- 注册 terrain（`preload=false`），记录 `registrationId = assetPath + 场景对象身份`。
- 绑定 `pageTable`/`layerIds`/`layerWeights` 到 MI。
- 把 page table 同时 `BindWorldTexture("terrainPageTable", pageTable)`，
  供 feedback pass 的 `worldTexture` 输入使用。
- 如果 P2 先用 chunk 参考路径，为 terrain draw packet 打上
  `terrainFeedbackEligible = true`；当前 `RenderDrawPacket` 没有这个字段，
  需要从 `MeshObjectBuildPlan` 传下来。核对：
  `source/render/frontend/renderScene.h::RenderDrawPacket`、
  `source/scene/sceneAssetTypes.h::MeshObjectBuildPlan`。
- 如果直接走 page proxy，则不需要改 `RenderDrawPacket`；feedback pass 从
  `TerrainStreamingState` 读取页列表并生成 quad。

每帧入口：

```cpp
void TerrainStreaming::RecordFrame(
    vk::CommandBuffer commandBuffer,
    uint32_t frameIndex,
    uint64_t completedEpoch,
    const RenderScene& renderScene);
```

它只做四件事：

1. 消费已完成的 feedback 槽。
2. `ApplyFeedback` + `BuildUploadBatch`。
3. `RecordTerrainPageUploads`（在 scene pass 之前）。
4. 登记下一帧的 feedback copy（在 scene pass 之后）。

`ApplyTerrainPageUploads` 保留为加载期/测试用同步入口，但内部应改成
“收集一批 + 一次批量 copy + 一次 submit”，不要继续逐 region submit。

### 6.4 批量上传

```cpp
void RecordTerrainPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainPageGpuResources& resources,
    const TerrainUploadBatch& batch);
```

流程：

1. 断言 `pageTable/ids/weights` 存在，batch 非空。
2. `TransitionImageLayout(pageTable, ShaderReadOnly -> TransferDst)`。
3. 用 `RecordCopyBufferToImageRegions` 一次提交所有 page table texel 的
   `vk::BufferImageCopy`。
4. `TransitionImageLayout` 回 `ShaderReadOnly`。
5. 对 ID atlas、weight atlas 各重复一次 transition + 批量 copy。
6. 所有 copy 都在同一个 `commandBuffer`，不做单独 submit/wait。

staging 布局：

```text
[page table entries: N * 4 B]
[id page 0: 64 KiB][id page 1: 64 KiB]...
[weight page 0: 64 KiB][weight page 1: 64 KiB]...
```

每个 `vk::BufferImageCopy` 的 `bufferOffset` 指向对应块；`bufferRowLength = 0`、
`imageExtent = {width, height, 1}`。不要为每页单独创建 staging buffer。

### 6.5 Backend/RHI 需要补的 record-only 接口

现有同步 API 不能直接用于主帧录制：

```cpp
// 只录制，不 submit/wait；调用方负责 command buffer 生命周期。
void RecordTransitionImageLayout(
    vk::CommandBuffer commandBuffer,
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    uint32_t baseArrayLayer = 0,
    uint32_t layerCount = 1);

void RecordCopyBufferToImageRegions(
    vk::CommandBuffer commandBuffer,
    vk::Buffer buffer,
    vk::Image image,
    const std::vector<vk::BufferImageCopy>& regions);

void RecordCopyImageToBuffer(
    vk::CommandBuffer commandBuffer,
    vk::Image image,
    vk::Buffer buffer,
    uint32_t width,
    uint32_t height,
    bool flipY = false,
    vk::DeviceSize rowBytes = 0);
```

现有 `CopyBufferToImageRegions`/`CopyImageToBuffer` 可以改为
“BeginSingleTimeCommands + RecordX + EndSingleTimeCommands”的薄包装，
保持加载期/测试行为不变。

`RendererFrameContext` 增加：

```cpp
uint64_t completedEpoch = 0; // 该 frameIndex 上一次提交的 epoch，已完成
```

由 `BeginFrame` 在清空 `frameFenceEpochs[frameIndex]` 之前填入。

### 6.6 RenderGraph / pass / shader

新增资源：

```json
{
    "name": "terrainFeedback",
    "format": "R8G8B8A8_UNORM",
    "widthSize": 128,
    "heightSize": 128,
    "usage": ["colorAttachment", "transferSrc"]
}
```

新增 pass（放在 `geometry` 之后；当前 terrain 是 DefaultLit/Opaque，走 geometry）：

```json
{
    "name": "terrainFeedback",
    "type": "terrainFeedback",
    "needMsaa": false,
    "pipelineState": {
        "useVertexInput": true,
        "depthTestEnable": false,
        "depthWriteEnable": false,
        "depthCompareOp": "always"
    },
    "input": [
        { "resource": "terrainPageTable", "source": "worldTexture", "binding": 0 }
    ],
    "output": [
        { "resource": "terrainFeedback", "loadOp": "clear", "storeOp": "store" }
    ]
}
```

编译器校验：

- `terrainFeedback` 资源必须是 `texture2D`、`R8G8B8A8_UNORM`、固定 128×128
  （或配置的固定尺寸）、`colorAttachment` + `transferSrc`。
- 只能有一个 `terrainFeedback` pass。
- pass 不能写 `swapChain`、不能写 GBuffer、不能有深度输出、不能 `needMsaa`。
- 它必须出现在绘制 terrain 的 pass 之后；当前是 `geometry`。

Shader/材质：

- 新增 pass-only material `M_terrainFeedback`，不要复用 `M_terrain` 的 GBuffer 输出壳。
- 新增一个 feedback 输出模式（例如 `VL_MATERIAL_OUTPUT_FEEDBACK`），只声明
  `layout(location = 0) out vec4 outTerrainFeedback;`。
- feedback surface 从 `context.texCoord1` 计算 virtual page，从 `pageTable` 读 miss；
  输出 §4.2 的 bit 布局。
- `M_terrainFeedback` 的 vertex 路径复用默认 vertex；draw 时只遍历
  `terrainFeedbackEligible` 的 terrain draw packet。
- page table 作为 `worldTexture` pass input 绑定；binding 顺序必须与
  `TerrainStreaming` 的 `BindWorldTexture` 名称一致。

绘制入口：

```cpp
void RendererDrawExecutor::DrawTerrainFeedbackScene(
    RendererDrawContext& context) const;
```

它类似 `DrawShadowScene`：绑定一个 common feedback pipeline，遍历 terrain draw
packet，使用现有 per-object UBO，不写 sceneColor/GBuffer。

## 7. 分步实施与验收

每一步都可以独立分派；括号里是依赖关系。
所有新模块测试都走现有 root `vulkanlearn_add_gtest(...)` 目标，不新增任何
runtime test flag；现有 `--shader-reload-test`、`--shader-compute-reload-test`、
`--world-graph-transaction-test` 三个入口不变。本文的 smoke 截图是手工/脚本采集，
不是 runtime test。

### P0：World-local streaming state 与 frame epoch 管道

**目标**：不改变画面，只把状态和同步骨架准备好。

**触碰文件**

- `source/render/resource/rendererResourceCache.h/.cpp`
- `source/render/resource/rendererResourceLoadCoordinator.h/.cpp`
- `source/render/resource/rendererTerrainLoader.h/.cpp`
- `source/render/backend/rendererBackendVulkan.h/.cpp`
- `source/terrain/terrainPageManager.h/.cpp`
- `tool/terrain-tests/terrainTests.cpp`

**内容**

- 新增 `TerrainStreamingState`，放进 `WorldLocalResourcePackage`。
- coordinator 不再使用局部 `TerrainPageManager`；加载后 state 随 package 发布。
- `RendererFrameContext` 增加 `completedEpoch`。
- page manager 增加 `lastSeenEpoch`/`contentRevision`/`uploadedRevision` 字段和
  `SetGpuEpochContext`；暂不改变 `preload=true` 行为。
- 保持 `SC_terrain_b32_two.json` 画面不变。

**GoogleTest**

- `TerrainPageManager`：state 拷入/拷出 package 后页表内容不变；
  `contentRevision` 在 dirty 时递增；`SetGpuEpochContext` 不改变现有 LRU 结果。
- `vl_renderer_backend_tests`：`completedEpoch` 在 `BeginFrame` 后等于该槽上次提交 epoch；
  无 fence 完成时不消费。

**Smoke/截图**

```powershell
build\bin\main.exe --initial-scene Generated/Validation/TerrainB32/SC_terrain_b32_two.json `
  --no-dev-ui --console-command "screenshot terrain_p0.bmp" `
  --console-command-delay 120 --frame-limit 60
```

与改动前的双地形截图做像素对比；允许 0 差异（P0 不应改变渲染）。

**依赖**：无。

### P1：批量上传与帧内 record-only 入口（依赖 P0）

**目标**：把加载期逐 region submit 改成一批一次提交，并为流式准备每帧上传入口。

**触碰文件**

- `source/commonFunction.h`
- `source/render/rhi/vulkan/rhiDeviceVulkan.h/.cpp`
- `source/render/backend/rendererBackendVulkan.h/.cpp`
- `source/render/resource/rendererTerrainLoader.h/.cpp`
- `source/terrain/terrainPageManager.h/.cpp`
- `tool/renderer-backend-tests/buffer_to_image_regions_test.cpp`
- `tool/terrain-tests/terrainTests.cpp`

**内容**

- 新增 `RecordTransitionImageLayout`、`RecordCopyBufferToImageRegions`、
  `RecordCopyImageToBuffer`。
- `ApplyTerrainPageUploads` 改为：收集一批 → 一次批量 copy → 一次提交/等待。
- 新增 `RecordTerrainPageUploads`，只录制，不 submit。
- page manager 的 `BuildUploadBatch`/`CommitUploadBatch`/`AbortUploadBatch` 支持
  revision 和每帧页数上限。
- 加载期 atlas 零填充可改为 `vkCmdClearColorImage`（可选，但应在 P1 一起测量）。

**GoogleTest**

- 批量 region 的 offset/rowLength/extent 正确；空 region 拒绝；
- 同一批包含 page table + IDs + weights 时，三个 image 的 copy 顺序和 offset 不串；
- `CommitUploadBatch` 不清理在收集后再次变脏的页；
- `AbortUploadBatch` 保留全部 dirty；
- 单帧 `maxAtlasPages` 截断时，剩余 dirty 下一批继续。

**Smoke/截图**

- `SC_terrain_b32_two.json` 静态截图与 P0 一致；
- 启动 stderr 无 Vulkan validation error；
- 记录加载耗时/提交次数，确认不再按页 submit。

**依赖**：P0。

### P2：Feedback pass + 回读 ring（依赖 P1）

**目标**：GPU 能看到 terrain 可见页，但只打印统计，不改变驻留。

**状态（2026-10-29）**：已实现并写入 `contracts/terrain.md`。当前：

- `terrainFeedback` pass 固定 128×128 RGBA8 + 128×128 深度、无 MSAA，不写
  swapChain/GBuffer；编译器拒绝其他尺寸/格式/附件。
- 只绘制 `RenderDrawPacket::terrainFeedbackEligible` 的地形 chunk；标记由
  `RendererTerrainLoader` 显式写入并沿 WorldSnapshot 传播。
- texel 格式：`R=pageX+miss+valid`、`G=pageY`、`B=priority`、`A=frameTag`；
  P3 起启用深度测试（`less`），每个 texel 只记录最近可见页。
- 回读 ring 槽数等于 frame fence 数（2），host-visible/coherent 常驻映射；帧末
  ColorAttachment→TransferSrc→copy→ColorAttachment，CPU 仅在
  `copiedEpoch <= completedEpoch` 时消费一次。
- 控制台 `terrainfeedback` 只打印统计；没有 `ApplyFeedback`、`RequestPage`、
  `Evict`，`preload=true` 与画面不变。

**P3 已补**：feedback→request/miss、safe-epoch LRU、物理槽位 quarantine
（CPU 安全窗代替 override barrier）、`preload=false`、每帧上传限流。

**触碰文件**

- `source/render/rendergraph/renderGraphPassType.h`
- `source/render/rendergraph/renderGraphCompiler.h/.cpp`
- `source/renderGraph.h/.cpp`
- `source/render/pass/passRuntime.h/.cpp`
- `source/render/backend/rendererDrawExecutor.h/.cpp`
- `source/render/backend/rendererBackendVulkan.h/.cpp`
- `source/renderSystem.cpp`
- `shader/glsl/pass/M_terrainFeedback.json`、`pass/terrainFeedback.vert/frag`
- `shader/glsl/engine/materialPass.glsl`、`engine/passTemplate/base.frag.glsl`
- `config/renderGraphConfig.json`
- `tool/renderer-backend-tests/*`、`tool/terrain-tests/terrainTests.cpp`

**内容**

- 新增 `TerrainFeedback` pass 类型、资源、编译校验。
- 新增 `M_terrainFeedback` 材质/输出壳和 terrain feedback draw domain。
  P2 先用 terrain chunk draw packet 验证；16km 走查前改为 page proxy quad
  （由 `TerrainStreamingState` 提供视锥内的页列表）。
- 新增 feedback ring、`RecordFeedbackCopy`、CPU 解析和 `terrainfeedback` 普通
  console 诊断命令（不是 runtime test flag）。
- 只统计 `visiblePages/missPages/invalidTexels`，不调用 `RequestPage`/`Evict`。
- 画面必须与 P1 完全一致；feedback 不参与 shading。

**GoogleTest**

- 编译器：固定 128×128、单输出、禁止 swapChain/GBuffer/MSAA/depth；
- feedback bit pack/unpack：page 坐标、miss、valid、priority、frameTag 往返；
- frameTag=0 的 texel 被忽略；过期 frameTag 被忽略；
- ring 未完成时 CPU 不消费；完成后只消费一次；
- `terrainfeedback` 的计数与合成 feedback buffer 一致。

**Smoke/截图**

```powershell
build\bin\main.exe --initial-scene Generated/Validation/TerrainB32/SC_terrain_b32_two.json `
  --no-dev-ui --console-command "terrainfeedback" `
  --console-command "screenshot terrain_p2.bmp" `
  --console-command-delay 120 --frame-limit 60
```

- 截图与 P1 做像素对比，要求 0 差异；
- `terrainfeedback` 显示可见页数量合理，且 feedback pass 后无 validation error；
- 用 `debugview`/现有 debug mode 检查 terrain 仍正常出图。

**依赖**：P1。

### P3：驻留决策、LRU 与按需上传（依赖 P2）

**目标**：真正闭环；`preload=false`，feedback 驱动 request/miss/evict。

**状态（2026-10-30）**：已实现。生产路径改为 `RegisterTerrain(preload=false)`，
每个 placement 只请求中心 2×2 页；`TerrainPageManager::ApplyFeedback` 消费
`MarkPageUsedAtEpoch`/`RequestPageAtEpoch`，每帧最多 8 个请求并按 deferred 轮转；
`TerrainUploadGpuRing` 按 frame fence 分槽，主帧 scene pass 前录制、提交成功后
Commit；淘汰不再清 atlas，物理槽位按
`max(lastSeenEpoch, submittedEpoch) + framesInFlight + 1` quarantine 后再复用。
feedback pass 增加 128×128 深度（`less`），避免远页覆盖近页 miss。
控制台 `terrain stats` / `terrain capacity N` 提供验收入口。低分辨率 fallback
与淡入仍留给后续阶段。

**触碰文件**

- `source/terrain/terrainPageManager.h/.cpp`
- `source/render/resource/rendererTerrainLoader.h/.cpp`
- `source/renderSystem.cpp`
- `source/render/resource/rendererResourceCache.h/.cpp`
- `tool/terrain-tests/terrainTests.cpp`
- `documents/contracts/terrain.md`（实现落地后更新，不在本文这一步改）

**内容**

- `ApplyFeedback` + `RequestPage` + safe-epoch `EvictLeastRecentlyUsed`。
- 本帧请求页 pin；没有安全候选时保留 miss，不强制清页。
- `RegisterTerrain(preload=false)`；初始只请求相机附近的页，其余靠 feedback/预取。
- 每帧 `MaxUploadPagesPerFrame` 限流；miss 统计/eviction 统计暴露给 `terrainfeedback`。
- fallback 仍保留；本期不升级低分辨率 fallback 内容。

**GoogleTest**

- feedback miss 触发 `RequestPage`；hit 更新 `lastSeenEpoch`；
- 容量满且所有页都在安全窗外时，`RequestPage` 返回 false，不淘汰；
- 安全窗内的页可以被 LRU 淘汰；priority 高的页更晚淘汰；
- 本帧请求页不会被同帧淘汰；
- 淘汰后 page table 无效、atlas 清零、物理槽位可复用；
- 同一页重复 miss 不会重复分配物理槽位；
- `CommitUploadBatch` 的 revision 语义在“收集后再次标脏”时保持 dirty。

**Smoke/截图**

- `SC_terrain_b32_two.json` 静态近景：无黑洞，`terrainfeedback` resident 稳定；
- 用 `camera pose` 在三个位置各跑一次，截图检查远景/近景切换：

```powershell
build\bin\main.exe --initial-scene Generated/Validation/TerrainB32/SC_terrain_b32_two.json `
  --no-dev-ui --console-command "camera pose 0 5000 0 0 0 80000" `
  --console-command "screenshot terrain_p3_far.bmp" `
  --console-command-delay 180 --frame-limit 60
```

- 检查 stderr 无 validation error/泄漏；
- 记录 miss/eviction 曲线，确认不会每帧抖动全量淘汰。

**依赖**：P2。

### P4：16km 注册模型、fallback 与多放置收口（依赖 P3）

**目标**：解决当前 32×32 全页表与 fallback/重复 TR 的冲突，让 16km 单地形和
同一 TR 多处摆放可用。

**状态（2026-10-29）**：已实现，不依赖 P3 的部分先行落地。当前行为：
`TerrainBuildPlan::placementId` = 场景 `terrain|<name>`（空则回退 assetPath）；
`TerrainPageManager` 以 placementId 注册，同一 assetPath 可多次注册；fallback 只保留
物理 atlas 槽位 `(31,31)`，虚拟 `(31,31)` 是普通页；场景对象可覆盖
`pageOrigin`/`pageTilesPerSide`；32×32 单地形可注册，preload 容量不足时记录 deferred
并继续；chunk key 含 placementId。完整的“页内容按 assetPath+tile 去重”仍未做。

**触碰文件**

- `source/terrain/terrainAssetTypes.h`
- `source/terrain/terrainPageManager.h/.cpp`
- `source/render/resource/rendererResourceLoadCoordinator.cpp`
- `source/render/resource/rendererTerrainLoader.cpp`
- `shader/glsl/materialFunction/mf_terrainLayerBlend.glsl`
- `documents/contracts/terrain.md`（实现后）
- `tool/terrain-tests/terrainTests.cpp`

**内容**

- 注册身份从 `assetPath` 改为 `assetPath + placementId`；同一 TR 可注册多次，
  page content 可按 `assetPath + tile` 去重（可选优化）。
- fallback 从虚拟 `(31,31)` 解耦：推荐保留一个物理 fallback slot 或独立
  fallback texture，不再占用虚拟页表坐标；这样 `pageTilesPerSide=32` 可注册。
- 页表无效项仍由 shader 分支回退到 fallback；不要回退到可能属于其它地形的 page 0。
- 更新 `TR.layers`/pageOrigin 校验和测试。

**GoogleTest**

- 同一 TR 两个 placement、不同 pageOrigin 可同时注册；
- 32×32 `pageTilesPerSide=32` 注册成功（fallback 不再冲突）；
- fallback 页不参与 resident 统计；`IsResident(31,31)` 语义明确；
- 重叠虚拟区域仍拒绝；
- 多 placement 的页内容一致时，可选共享物理页的 refcount 正确（如果本期做去重）。

**Smoke/截图**

- 现有 `SC_terrain_b32_two.json` 双地形不回归；
- 新建一个同 TR 两 placement 的 smoke 场景（普通场景资产，不是 runtime test flag），
  两个 pageOrigin 同时出图；
- 16km 单地形（`pageTilesPerSide=32`）加载/走查，截图检查无整块缺失。

**依赖**：P3。

## 8. 风险清单

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| 虚拟页/物理槽位映射错误 | 页表指向错误 atlas 页，地形花屏 | `PageTableUpload` 只写虚拟坐标，`AtlasPageUpload` 只写物理坐标；单测覆盖“淘汰后槽位复用”和“页表 entry=物理槽位” |
| 同一 TR 多处摆放 | assetPath 曾是唯一注册键，第二处直接失败 | P4 已改为 placementId；每处摆放独立物理页，内容去重仍未做 |
| fallback 页占用虚拟 `(31,31)` | 32×32 单地形无法注册；`IsResident(31,31)` 语义混乱 | P4 已解耦为“只保留物理槽位”；虚拟 `(31,31)` 是普通 resident page |
| 1024 虚拟页 vs 1023 物理槽位 | 全量 preload 必然缺一页；容量满时静默失败 | 16km 使用 `preload=false`；`RequestPage` 失败要进入 miss 统计；禁止静默 full preload |
| 128 MiB atlas 固定分配 | 显存预算被固定占用，不能按页释放 | 本期接受固定 atlas；后续若需要更大世界，再评估 256² page/分块 atlas/压缩 |
| staging/上传预算过大 | 每帧 upload 峰值造成卡顿 | `MaxUploadPagesPerFrame` 限流；priority 队列；一次批量 copy |
| 上传/反馈读写 hazard | 页表/atlas 在上传时被采样，或反馈图被覆盖 | upload 在 scene pass 之前；feedback copy 后显式 layout 转换；淘汰页加 barrier |
| 淘汰页仍在 in-flight 帧采样 | atlas 清零导致黑洞/闪烁 | 全队列 barrier 保证清零排在旧帧读取之后；CPU 安全窗（`lastSeenEpoch + HysteresisFrames <= lastConsumedFeedbackEpoch`）只减少抖动；无安全候选时延迟淘汰 |
| 回读延迟导致抖动 | 相机快速移动时页请求滞后，画面短暂缺页 | feedback 2 帧延迟 + 预取（相机前方/大 priority）；miss 页保留到下一次反馈；fallback 兜底 |
| frameTag 回绕 | 过期 feedback 被当成当前帧 | 8-bit tag + 0 无效；ring 延迟远小于 255；消费时校验 `copiedEpoch` |
| `fract(texCoord1 * pageTableSize)` 在边界 1.0 | 本地 UV 回绕到 0，页边界采样错页 | 对 `texCoord1` 做 `min(..., nextafter(1.0, 0.0))` 或从 page 坐标计算 local UV；加边界单测/截图 |
| feedback 分辨率不足/碰撞 | 远处页漏报，驻留不足 | 128×128 默认；必要时 256×256；miss/priority 偏置；fallback |
| 同一帧请求又淘汰 | 新上传页被自己清掉 | 本帧请求页 pin；淘汰在请求之后且跳过 pin |
| `ConsumeDirtyUploads` 便捷入口部分清 dirty | 异常时页永远不重传 | 流式路径只用 `BuildUploadBatch` + `Commit/Abort`；`ConsumeDirtyUploads` 仅测试/一次性同步 |
| World 重载/streaming state 悬空 | 渲染线程访问已销毁 page manager | state 放 World-local package；每帧捕获 shared_ptr；旧 package 走 `ResourceRetireQueue` |
| `CreateTerrainPageGpuResources` 忽略 `pageManager` 参数 | 后续误以为尺寸/预算来自 manager | P1 删除未使用参数或改为显式 `TerrainPageGpuConfig`；尺寸只从常量/配置读取 |
| 初始全零 atlas 上传 128 MiB | 加载慢、内存峰值高 | P1 用 `vkCmdClearColorImage` 或一次性 GPU clear；不要在 CPU 构造两张 64 MiB 零图 |
| barrier mip 数硬编码 1 | 未来 atlas 有 mip 时漏转换 | 本期 atlas 无 mip；若加 mip，barrier/transition 使用实际 mip 数 |

## 9. 验收口径

本期完成的标准不是“画面看起来有草”，而是：

1. **反馈闭环可观测**：`terrainfeedback` 能报告 visible/miss/request/hit/eviction，
   且数值随相机移动变化。
2. **驻留真的变化**：`residentCount` 在预算内波动，`evictionCount > 0`，
   不是永远全量常驻。
3. **无 GPU 危险**：Vulkan validation 无 layout/ hazard 错误；淘汰/上传帧无黑洞。
4. **画面不回退**：P1/P2 的静态截图与改动前一致；P3/P4 的走查截图无整块缺失。
5. **模块测试**：`terrain_tests`、`vl_renderer_backend_tests` 通过；
   若改材质 schema，再跑 `material_schema_tests`。
6. **文档门禁**：改 `documents/**` 后运行 `py -3 tool/validate-docs.py`。

实现落地后再更新 `documents/contracts/terrain.md`、`documents/contracts/testing.md`
和 `documents/routing.md`；本文在实现前始终保持“未来工作，非契约”的状态。
