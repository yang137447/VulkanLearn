# GPU pass 与 CPU 阶段计时（`--gpu-stats` / `gpustats` / `perf json`）

> 状态：**当前契约**（实现已落地）。描述逐 pass GPU 时间戳采集、逐阶段 CPU 计时、
> 延迟读回、滚动平均、Performance 面板与日志/机器通道输出；每一条都可核对到源码或测试。

## 入口

- 启动参数 `--gpu-stats`（`source/engine/launchOptions.*`）：默认关闭；打开后每采集 120 帧向
  `Info` 级日志打印一张 `[GPU] frame avg over N: ...` 表。关闭且从未执行 `gpustats` 时**不创建**
  timestamp query pool；控制台按需启用后才创建。
- 启动参数 `--no-prerender-skin`（`source/engine/launchOptions.*`）：预蒙皮默认开启，用该关闭项回落到逐 pass 蒙皮；开启时每帧在
  `hairSimulation` 之后、pass 循环之前录制 `SkinDeformPass`，并在 GPU 表中新增
  `skinnedDeform` 一行。
- 控制台命令 `gpustats`（`source/debugConsole.cpp`）：把 `RuntimeCommandType::PrintGpuPassStats`
  排入 `CommandBus`，由 `RuntimeCommandExecutor` 在当前 GT tick 调用
  `RenderSystem::PrintGpuPassStats()`。它立即打印最近一次读回的窗口；没有可用样本时打印一行
  提示，并请求渲染线程在第一次读回后补打一张表。该命令无论 `--gpu-stats` 是否开启都会启用采集。
- 控制台命令 `perf json`：把 `RuntimeCommandType::PrintPerformanceJson` 排入 `CommandBus`，
  由 `RenderSystem::PrintPerformanceJson()` 在 GT 打印**恰好一行** JSON（无日志前缀），
  并启用采集。格式见“机器通道”。
- 启动参数 `--perf-jsonl <path>`（`source/engine/launchOptions.*`）：可选；按墙钟每秒向
  `<path>` 追加一行同样的 JSON，写入后立即 flush，并同时启用采集。父目录不存在时创建；
  创建或打开失败让启动失败并给出包含路径的错误信息，不静默丢弃时间序列。
- 启动脚本可复现：`--console-command gpustats --console-command-delay <frames>`。

## 时间戳写入位置

`RenderSystem::RecordAndSubmitCurrentRenderScene()` 在非 offline 路径中：

1. backend `BeginFrame` 返回后调用 `GpuPassTimer::BeginFrame`：读回该 frame slot 上一批结果，
   并在命令缓冲中 `vkCmdResetQueryPool` 重置该 slot 的查询范围。
2. 若 `HairSimulationPass::HasWork()` 为真，在 `RecordHairSimulation()` 前后各写一个
   timestamp，合成一项 `hairSimulation`。该阶段内部的所有 compute dispatch 都归入这一项。
   没有 hair dispatch 的帧不写 timestamp、不占 query 槽位。
3. 若预蒙皮开启且 `SkinDeformPass::HasWork()` 为真，在
   `RecordSkinDeform()` 前后各写一个 timestamp，合成一项 `skinnedDeform`。它覆盖本帧
   palette 上传之后的全部预蒙皮 dispatch；没有蒙皮对象或 flag 关闭时不写 timestamp。
4. 遍历 `renderGraph.GetRenderpassesOrdered()`，对每个**实际录制**的 pass 在
   `VulkanDebug::ScopedRegion` 之前写 begin、在 `PassRuntime::RecordPass()` 返回后写 end。
   被跳过的 pass（无环境时的 sky、非视口时的 editorGrid）不占用 query 槽位。
5. `SubmitFrame` 之前 `GpuPassTimer::EndFrame()` 把本帧名字列表交给延迟读回。

每个 timestamp 都写在 render pass 实例**外部**；pass 自己 `vkCmdBeginRenderPass` 时，begin
timestamp 已经在实例之前，end timestamp 在 `vkCmdEndRenderPass` 之后。

begin/end 都使用 `VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT`：begin 等待上一个 pass 完整结束，
end 等待本 pass 完整结束，因此 `duration` 是"该 pass 的串行 GPU 时间"，而不是上一个 pass 的
尾部时间。代价是采集期间每个 pass 边界都会 drain 一次流水线。

## Query pool 设计

- 每个 frame-in-flight 槽位（`MAX_FRAMES_IN_FLIGHT = 2`）独占一个
  `vk::QueryType::eTimestamp` query pool，每个 pool 有
  `kGpuPassTimingMaxEntries (64) × 2 = 128` 个 query。
- 启用采集后的第一帧、且在本帧 `BeginFrame` **之前**（`GpuPassTimer::PrepareFrame`）创建该
  slot 的 pool；`RenderSystem::InitializeFrameResources()` / `ShutdownFrameResources()` 随帧资源
  创建/销毁，swapchain 重建（`ReleaseSwapchainDependentResources` → `RebuildSwapchainDependentResources`）
  会先销毁再重建。
- 每帧最多记录 64 项（含 `hairSimulation` / `skinnedDeform`）。超过时只记录前 64 项并写一次
  `[GPU] pass timing capped at 64 entries per frame` 警告；不重建 pool，避免与仍在飞行的查询冲突。
- `vkCmdResetQueryPool` 在每个 slot 复用时写入该帧命令缓冲，位于该帧任何 timestamp 之前。

## 延迟读回（不阻塞）

读回发生在**同一个 frame slot 下一次被复用**时。此时 backend `BeginFrame` 已经等待该 slot 的
`taskFinishedFence`，所以上一批查询必然完成。读取使用
`RendererBackendVulkan::TryReadTimestampQueryPair()`：

- `vkGetQueryPoolResults` 只带 `VK_QUERY_RESULT_64_BIT`，**不带**
  `VK_QUERY_RESULT_WAIT_BIT`；
- `VK_NOT_READY` 返回 `false`，该样本记为 `n/a`，绝不阻塞渲染线程；
- 其他错误抛出运行时错误。

每帧读回后把 `(begin, end)` 换算成毫秒并推入 120 帧环形窗口；每 120 个采集帧打印一次。
当前表也会在锁保护下发布给控制台线程；`gpustats` 只读这份快照，不等待渲染线程。

## `timestampPeriod` 与有效性

- 支持条件（`GpuPassTimer::Initialize` 一次读取）：
  - `vk::PhysicalDeviceLimits::timestampComputeAndGraphics != 0`；
  - graphics queue 的 `timestampValidBits > 0`；
  - `vk::PhysicalDeviceLimits::timestampPeriod > 0`。
- 任一不满足时采集整体禁用，只写一行
  `[GPU] pass timing disabled: timestamp queries unavailable (...)` 警告，不创建 query pool。
- 换算：`duration_ns = (end - begin) × timestampPeriod`，输出毫秒。`end < begin`
  （计数器回绕、被禁用）或 `timestampPeriod <= 0` 时该样本为 `n/a`，绝不输出负数。
- 不对 tick 差做 `timestampValidBits` 掩码：掩码会把回绕变成一个巨大的正数；本实现按需求把
  回绕如实报告为 `n/a`。

## CPU 阶段计时（GT）

`source/core/cpuFrameProfiler.h` 是 GT 独占的 120 帧环形计时器，与 GPU 表使用同一套窗口
规则（固定名字顺序、部分窗口平均、无样本输出 `n/a`）。`EngineLoop::Tick()` 在开头调用
`BeginFrame()`，在渲染段结束后调用 `CommitFrame()`，随后把窗口平均样本交给
`RenderSystem::PublishCpuStageSamples()`，由 `GpuPassTimer` 在同一把 `snapshotMutex` 下
并入唯一 `PerformanceSnapshot`。CPU 通道不依赖 `--gpu-stats` / `--perf-jsonl`：每一帧都采集，
每个阶段只有两次 `steady_clock::now()` 和一次加法，不分配、不打日志。

`CpuFrameStageScope` 允许同一阶段在一帧内出现多个不连续区间（`input`、`scripts` 就是如此），
区间耗时累加；`CommitFrame()` 每帧只做一次定长数组平均与值拷贝，返回的样本向量复用名字字符串。

| 阶段 | 测量位置 | 含义 |
| --- | --- | --- |
| `input` | Tick 内 `PumpPlatformEvents()` 与 `InputSubsystem::UpdateActionState()` 两段 | SDL 事件轮询/分发与动作状态更新 |
| `scripts` | Update 块内三段：`TickBlueprintAssets()`..`scripts.Tick()`、`DebugDraw::AdvanceGameTime()`、`PollBlueprintAssetChanges()` | 蓝图资产、自动 Play、编辑控制器、脚本 Tick 与资产变更轮询 |
| `animation` | `World::TickAnimations()` | 骨骼动画求值、CPU 蒙皮与姿势包围盒 |
| `hairSimulation` | `RendererResourceCache::AdvanceTressfxSimulations()` | GT 侧 TressFX 步进；GPU guide simulation 不在这里 |
| `worldSnapshot` | `RenderSystem::PublishSnapshotFromActiveWorld()` | 世界快照构建与发布 |
| `uiViewModel` | `EngineLoop::UpdateUiViewModel()` | UI 可见性决策、逐帧 DrawDebug/相机冻结、按分节版本重建的变化分节、`UiSubsystem::Update`（分节发布/脏集、RmlUi 更新与绘制快照） |
| `renderRecord` | RT 模式：`renderThread->SubmitFrame()`；单线程：`RenderLatestSnapshotOrLastGood()` | 命令录制入口（含 hair sim dispatch 录制） |
| `submitPresent` | RT 模式：`WaitForRenderThreadIdle()`；单线程：无独立样本 | 等待渲染线程完成消费、录制、提交、present 与帧槽 fence |
| `other` | 派生项 | `cpuMs - 已测阶段之和`，钳制到 `>= 0` |

### `other` 规则与对账

`other` 不是独立计时区间，而是发布时用同一快照里的 `cpuMs` 减去所有已测阶段（不含 `other`）
的平均值之和：

- `other = max(0, cpuMs - sum(measured stages))`；
- 测量值总和超过 `cpuMs`（窗口跨越节拍变化、采集边界或单帧异常）时输出 `0.000`，绝不输出负数；
- `cpuMs` 缺失时 `other` 也是 `null`；
- `other` 始终是 `cpuStages` 的最后一行；没有已测阶段时 `cpuStages` 为空，不伪造 `other`。

因此面板/JSON 中 `cpuStages` 各行之和加上 `other` 与 `cpuMs` 对账。逐帧求和再取中位数时，
`cpuMs` 的中位数与各行中位数之和可能差一个亚毫秒量级（中位数不可加、且单帧钳制会丢掉超出的部分），
所以契约以**每帧求和的中位数**作为对账口径。

### 归属与交接

- 计时器对象由 `EngineLoop` 独占，只在 GT 的 Tick 生命周期内读写；`CommitFrame()` 之后
  EngineLoop 只把值语义样本交出，不再持有窗口。
- `GpuPassTimer` 是唯一快照的持有者：GPU 读回、CPU 帧时长与 CPU 阶段样本都在它的
  `snapshotMutex` 下更新。面板、`perf json` 与 `--perf-jsonl` 都只读这一份副本，不存在
  第二套计时、第二个窗口或第二套取整规则。
- `PerformanceSnapshot` 的 `valid` 仍只描述 GPU 通道；CPU 阶段在 GPU timestamp 不可用、
  尚未读回时仍照常发布和显示。

### `uiViewModel` 的构成与可见性

`uiViewModel` 不再等价于"每帧构建完整编辑器视图模型"，读表时按可见性与更新调度解释：

- **开发者工作区可见且静止**：`source/ui/uiViewModelSections.h` 的九个分节各自按版本
  变化重建与重脏；`source/ui/uiUpdateScheduler.h` 只在分节变化、RmlUi 输入、DOM 突变、
  表面切换或 10 Hz 兜底到期时执行 `rmlContext->Update()` 与快照发布。静止帧两者都跳过，
  RT 继续绘制上一份不可变快照。2026-10-23 的 Release skinned 实测中位数为
  **约 0.11 ms**（每秒约 10 次更新、约 170 次跳过），而第 2 层是每帧更新（约 0.53 ms）。
- **工作区可见且持续输入**：每个输入事件都强制同帧更新，成本回到"每次更新一次"。
  指针约 18 次/秒移动时实测约 0.17 ms；约 55 次/秒移动时约 0.26 ms。
- **`--no-dev-ui`**：只冻结运行时 HUD 的场景名/游戏文本，不构建编辑器分节。HUD 与
  工作区共用同一个 RmlUi Context，但 HUD 文本按值比较后才置脏，通常约 0.04 ms。
- **沉浸模式/UI 完全隐藏**：不构建编辑器分节、不更新 RmlUi 文档，只发布仅
  DrawDebug 的绘制快照，通常约 0.02 ms。
- **UI 子系统未初始化**：`UpdateUiViewModel` 直接返回，本阶段没有样本或接近 0。

`uiViewModel` 的可见态数值现在取决于采样窗口内"更新了几帧"，是均摊值而不是每帧成本；
单帧更新的实际成本仍约为 0.4–0.6 ms（RmlUi 渲染 + 快照构建）。因此隐藏 UI 后该行接近 0
是预期结果，不是计时缺失。`perf json` / `--perf-jsonl` 读取的是 `GpuPassTimer` 的
渲染/CPU 计时快照，不经过 UI 快照，UI 节流或隐藏不会改变这些通道的值。逐帧渲染的
`other` 会相应吸收 UI 侧减少的 GT 工作。

`--gpu-stats` 每秒额外输出一行 `[UI] sections/s=… panelBuilds/s=… runtimeViews/s=…
bindingDirty/s=… rmlUpdates/s=… snapshotPublishes/s=… skipped/s=…`（附按分节拆分的
发布次数），用于验证"静止帧只重采样时间序列节、不重建面板、不推进上下文/快照"。
该计数不改变任何数据路径。

### 与等待重叠的边界

`workerThreadCount=2`（默认）时，GT 只发布世界快照并唤醒渲染线程，真正的
`ConsumeLatestSnapshotIntoRenderScene()`、命令录制、`BeginFrame()` 的 fence 等待、
`SubmitFrame()` 与 present 都在 RT 上完成。GT 无法在不拆分线程边界的情况下观测这些内部区间，
因此它们全部被 `submitPresent` 的等待吸收；此时 `renderRecord` 只记录 GT 的唤醒调用。
`workerThreadCount=1` 时全部工作在 GT 上串行，`RenderLatestSnapshotOrLastGood()` 的
consume + 录制 + 提交 + present 统一计入 `renderRecord`，`submitPresent` 没有独立样本（`n/a`）。

这条归属意味着：RT 模式下 `submitPresent` 既可能包含真实的 GPU/present 背压，也可能包含
渲染线程自己的 CPU 录制。用 `--frame-limit` 给 GPU 留出余量可以区分两者——若 `submitPresent`
随帧率上限明显下降，说明它此前主要是在等 GPU/swapchain；若基本不变，则剩余部分更可能是
渲染线程 CPU 工作。契约只记录这一归属，不在运行时尝试进一步拆分。

## 共享快照（CPU + GPU）

`source/core/performanceSnapshot.h` 定义唯一快照 `PerformanceSnapshot`；`GpuPassTimer::Snapshot()`
在既有 mutex 下复制一份值语义副本。面板、`perf json` 和 `--perf-jsonl` 都只消费这一种类型，
不存在第二套计时、第二套取整或第二套字段顺序。

| 字段 | 来源 | 空值语义 |
| --- | --- | --- |
| `fps` | `1 / cpuFrameSeconds` | 尚未采样或 delta 非法 |
| `cpuFrameMs` | `cpuFrameSeconds × 1000` | 尚未采样或 delta 非法 |
| `gpuTotalMs` | 所有有效 pass 平均耗时之和 | 窗口内没有任何有效 pass |
| `passes[]` | 读回窗口的名字顺序（不是按耗时排序） | 每项 `ms` 独立为空 |
| `cpuStages[]` | CPU 窗口的固定阶段顺序，最后是派生 `other` | 每项 `ms` 独立为空；`other` 依赖 `cpuMs` |
| `gpuTimingSupported` | 设备 timestamp 能力 | `false` 时 GPU 通道整体无效 |
| `collecting` | 至少完成过一次读回 | 尚未读回时 GPU 通道无效 |
| `windowFrameCount` | GPU 已进入 120 帧窗口的帧数 | 用于 GPU 状态文案，不进入 JSON |
| `cpuWindowFrameCount` | CPU 已进入 120 帧窗口的帧数 | 用于 CPU 状态文案，不进入 JSON |

CPU 帧时长来自渲染器自动曝光路径的 `RenderSystem::UpdateExposureFrameTiming()`：
它用既有的 `steady_clock` 计算 `exposureDeltaSeconds`（首帧 1/60，之后收敛到
1/240..0.1 s），并在同一处调用 `GpuPassTimer::SetCpuFrameTiming()`。因此 FPS/CPU ms
和眼适应使用同一个 frame delta，没有第二个帧率时钟；GPU 采集关闭时 CPU 字段仍每帧更新。

`valid` 只描述 GPU 通道：`gpuTimingSupported && collecting && gpuTotalMs 有值`。
设备不支持 timestamp、尚未读回、或窗口内没有任何有效样本时为 `false`；此时 CPU 数字与
`cpuStages` 仍照常显示。

Performance 页的 CPU 表只消费这份快照：标题与表头把 CPU 阶段和 GPU pass 分开，行/条形样式
与 GPU 表相同，百分比以 `cpuMs` 为分母（因此各行百分比与 `other` 合计为 100%），`other` 行
用独立颜色提示它是派生 remainder。面板仍按 5 Hz 从冻结副本刷新，首帧空快照不伪造 0。

## 机器通道（`perf json` / `--perf-jsonl`）

两个通道由 `FormatPerformanceJson()` 序列化，固定键顺序、固定 3 位小数、无效值一律写
`null`，不输出 NaN/Inf，也不使用指数记法：

```json
{"fps":56.559,"cpuMs":17.681,"gpuTotalMs":14.290,"valid":true,"passes":[{"name":"hairSimulation","ms":1.258},{"name":"shadowCascade0","ms":2.519}],"cpuStages":[{"name":"input","ms":0.046},{"name":"scripts","ms":0.009},{"name":"animation","ms":0.135},{"name":"hairSimulation","ms":0.001},{"name":"worldSnapshot","ms":0.022},{"name":"uiViewModel","ms":1.268},{"name":"renderRecord","ms":0.006},{"name":"submitPresent","ms":3.481},{"name":"other","ms":0.026}]}
```

- `fps`、`cpuMs`、`gpuTotalMs` 是数字或 `null`；`valid` 是布尔值。
- `passes` 始终存在，顺序等于录制顺序：有 hair 时 `hairSimulation` 最前，随后是开启预蒙皮时的 `skinnedDeform`，其余按 RenderGraph 录制顺序。
- `cpuStages` 始终存在，位于 `passes` 之后；顺序固定为 `input`、`scripts`、`animation`、
  `hairSimulation`、`worldSnapshot`、`uiViewModel`、`renderRecord`、`submitPresent`、`other`。
- 单个 pass 或 CPU 阶段没有有效样本时写 `"ms":null`，不是 0；`cpuMs` 缺失时 `other` 也是
  `null`。
- `--perf-jsonl` 每行一个完整 JSON、单个 `\n`、逐行 flush；`Init` 完成满 1 秒后的第一个
  更新帧写出首行，之后维持墙钟节奏，渲染变慢不补写历史行。

**数值一致性**：面板、`perf json` 与 `--perf-jsonl` 共用 `PerformanceSnapshot` 和
`FormatPerformanceNumber()`；同一个快照在三处产生相同数字。面板按 5 Hz 从自己冻结的副本
刷新，`perf json`/JSONL 读取命令执行时刻的当前副本，因此不同输出时刻可能对应相邻快照，
但不存在数值规则差异。

## 表格与滚动平均

纯逻辑在 `source/render/backend/gpuPassTimings.h`，CPU 环形平均在
`source/core/cpuFrameProfiler.h`，两者使用同一套窗口规则：

- 名字顺序 = 稳定输出顺序：GPU 有 hair 时 `hairSimulation` 在最前，开启预蒙皮时 `skinnedDeform` 紧随其后，其余按 RenderGraph pass
  顺序；CPU 固定为 `input`..`submitPresent`，`other` 最后；
- 窗口容量 120 帧；`avg over N` 的 `N` 是当前窗口内已采集的帧数（不足 120 时为实际帧数）；
- 某一项在窗口内没有任何有效样本时输出 `n/a`；部分样本为 `n/a` 时该行按自己的有效样本平均，
  表头 `N` 仍是窗口内已采集的帧数；
- `total` 是各行平均值之和；没有任何有效行时输出 `total n/a`；
- 固定两位小数，例如：

```text
[GPU] frame avg over 120: hairSimulation 0.18 ms | shadowCascade0 0.42 ms | ... | total 12.30 ms
```

## 阴影 caster 剔除诊断

打开 `--gpu-stats` 时，阴影 pass 每 120 帧额外向 `Info` 日志打印一行：

```text
[GPU] shadow caster culling (drawn/candidates): cascade0 0/4 cascade1 4/4 cascade2 0/4 cascade3 0/4
```

`candidates` 是本帧材质路由为 shadow caster 的 draw packet 数，`drawn` 是逐级联剔除后
实际提交的数量。计数取**最近一帧**而不是窗口平均：级联覆盖是离散的，平均值会掩盖
“某级联 0 draw”这一关键证据。该行只写日志，不进入 `PerformanceSnapshot` / perf JSON；
JSON 的固定键序与 pass 列表保持不变。关闭 `--gpu-stats` 时不打印，也不做额外扫描。

判据与保守规则归 `source/render/shadow/shadowCasterCull.*` 所有；本文件只记录输出格式。

## 数字含义与已知边界

- **各 pass 时间之和不是帧时间**。表格不包含未计时的环境 IBL 计算、UI overlay、present、
  CPU 提交间隙和 vsync 等待；`total` 只是"被计时 pass 的串行时间之和"。
- timestamp 本身会改变流水线行为：begin/end 的 `BOTTOM_OF_PIPE` 让相邻 pass 之间 drain，
  短 pass 可能被相邻长 pass 的同步开销放大；不开 `--gpu-stats` 时没有这些 timestamp。
- begin/end 的 `BOTTOM_OF_PIPE` 把每个 pass 串行化，因此 `duration` 是"该 pass 独占 GPU 的
  时间"，不是异步 overlap 后的 wall-clock 贡献。真实帧时间里 compute/graphics 若有 overlap，
  各 pass 之和会大于这部分实际占用。
- `skinnedDeform` 覆盖 `SkinDeformPass::Record()` 内部所有逐对象 dispatch（一个对象一个 dispatch），不是逐顶点拆分。`--no-prerender-skin` 时该行不出现。\n- `hairSimulation` 覆盖 `HairSimulationPass::Record()` 内部所有 dispatch（9 个 compute pass，
  含可选的 SDF Clear/Splat/Collide），不是逐 dispatch 拆分。
- CPU 阶段只覆盖 GT Tick 的指定区间；`cpuMs` 本身是渲染/曝光帧 delta，包含帧率上限的等待、
  UI 线程调度与宿主噪声。`other` 因此既可能是未计时的小段 GT 工作，也可能是 pacing/调度空档；
  单帧 `other` 的均值容易被宿主噪声放大，定位时应优先看中位数与逐帧 `sum`。
- `submitPresent` 与等待重叠：RT 模式下它吸收渲染线程的录制、提交、present 与 fence 等待；
  单线程模式下它没有独立样本。详见“与等待重叠的边界”。
- 录制路径不逐帧分配堆内存：slot 名字数组、持续时间数组在 pool 创建后按上限预留；CPU 阶段
  使用定长数组并在 `CommitFrame()` 复用名字字符串；日志每 120 帧一次，不逐帧打印。

## 相关测试

- `tool/renderer-backend-tests/gpu_pass_timings_test.cpp`（`vl_renderer_backend_tests`）：
  tick→毫秒换算、`end < begin` 与非法 period、部分窗口平均、环形窗口淘汰、名字顺序稳定、
  重配置清窗、已知样本表的逐字字符串、`n/a` 与空表。
- `tool/renderer-backend-tests/performance_snapshot_test.cpp`（同一套件，13 项）：
  JSON 逐字字符串与固定键序、3 位小数、`null` 与 NaN/Inf 拒绝、CPU/GPU 合并、
  部分窗口、缺失/负值 pass、`valid` 规则、快照副本独立、百分比、表格还原，以及
  `cpuStages` 顺序/`other` 派生与 JSON 键序。
- `tool/renderer-backend-tests/cpu_frame_profiler_test.cpp`（同一套件，10 项）：
  部分窗口逐阶段样本数、未采样阶段为 `null`、120 帧环形淘汰、阶段名字/顺序稳定、
  同一阶段多区间累加、`other` 减法与 `>= 0` 钳制、`cpuMs` 缺失时为 `null`、
  空 CPU 通道不伪造 `other`、`other` 原地更新且始终最后。
- `tool/renderer-backend-tests/shadow_caster_cull_test.cpp`（同一套件，20 项）：
  光源视锥六个平面的内/外/跨越/相切、退化与倒置包围盒、非有限输入、padding，
  当前 skinPose 包围盒替换 bind pose 的用例，以及接收面光源空间 XY 预筛的重叠/分离/
  跨越/退化输入。
- `tool/renderer-backend-tests/skin_deform_test.cpp`（同一套件，10 项）：预蒙皮输出 stride/offset 与 64 B/顶点、dispatch 上取整、CPU 参考的 identity/rotation/translation/multi-influence 与逆转置法线、镜像 handedness。
- `tool/material-schema-tests/skinDeformMaterialTests.cpp`（`material_schema_tests`，3 项）：引擎宏只加到蒙皮材质、进入 variant/material 缓存身份、资产自声明被拒绝。
- `tool/engine-launch-options-tests/launch_options_test.cpp`（`engine_launch_options_tests`，8 项）：
  `--gpu-stats` 默认关闭/可接受/与既有参数组合、`--perf-jsonl` 默认关闭/路径必填/
  与 `--gpu-stats` 组合，未知参数仍被拒绝。
- `tool/editor-ui-tests/uiViewModelRefreshTests.cpp`（`editor_ui_tests`，11 项）：
  可见性档位（未初始化/沉浸/`--no-dev-ui`/工作区可见）、首帧到期、间隔等待、
  事件强制刷新、`MarkRefreshed` 重新起窗、自定义 15 Hz 节拍，以及"隐藏时即使
  强制也不构建编辑器快照"的判定。
- `tool/editor-ui-tests/uiViewModelSectionsTests.cpp`（`editor_ui_tests`，17 项）：
  分节版本语义（同 revision 只发布一次、变化只递增一次）、在途消费者持有旧
  `shared_ptr` 的不可变性、资产/日志/缩略图按库 revision 发布（5 次相同 Poll →
  1 次发布）、性能节每次采样发布、`ChangedFrom` 只报告变化位、两个 owner 的
  版本合并，以及到期窗口重建计划（静止只重采样周期节、选中只重建大纲、首帧/
  World 换代重建全部；Performance/动画不触发面板 Build）。
- `tool/editor-ui-tests/uiUpdateSchedulerTests.cpp`（`editor_ui_tests`，14 项）：
  第 3 层更新调度的空闲跳过、分节变化/输入事件/DOM 突变/World 换代/标签切换/
  本地化提交/10 Hz 节拍强制更新、隐藏态不更新、表面切换与隐藏后首帧强制完整
  更新、DrawDebug 只发布不更新，以及跳过发布时上一份不可变快照身份保留、RT
  队列保持空、隐藏后首帧发布完整快照。
- `tool/editor-ui-tests/performancePanelTests.cpp`（`editor_ui_tests`，8 项）：
  摘要/百分比/条形映射、图序而非成本序、5 Hz 节流与可见性立即刷新、不支持/未采样/无有效
  样本的空状态、首帧空快照不伪造 0，以及 CPU 阶段表按 `cpuMs` 归一、`other` 行与独立颜色。
  `DockRmlTest` 另验证 Performance 标签注册在 Log/Renderer 同组并消费冻结快照。
