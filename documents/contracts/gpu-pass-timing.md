# GPU pass timing（`--gpu-stats` / `gpustats`）

> 状态：**当前契约**（实现已落地）。描述逐 pass GPU 时间戳采集、延迟读回、滚动平均与日志输出；
> 每一条都可核对到源码或测试。

## 入口

- 启动参数 `--gpu-stats`（`source/engine/launchOptions.*`）：默认关闭；打开后每采集 120 帧向
  `Info` 级日志打印一张 `[GPU] frame avg over N: ...` 表。关闭且从未执行 `gpustats` 时**不创建**
  timestamp query pool；控制台按需启用后才创建。
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
3. 遍历 `renderGraph.GetRenderpassesOrdered()`，对每个**实际录制**的 pass 在
   `VulkanDebug::ScopedRegion` 之前写 begin、在 `PassRuntime::RecordPass()` 返回后写 end。
   被跳过的 pass（无环境时的 sky、非视口时的 editorGrid）不占用 query 槽位。
4. `SubmitFrame` 之前 `GpuPassTimer::EndFrame()` 把本帧名字列表交给延迟读回。

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
- 每帧最多记录 64 项（含 `hairSimulation`）。超过时只记录前 64 项并写一次
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
| `gpuTimingSupported` | 设备 timestamp 能力 | `false` 时 GPU 通道整体无效 |
| `collecting` | 至少完成过一次读回 | 尚未读回时 GPU 通道无效 |
| `windowFrameCount` | 已进入 120 帧窗口的帧数 | 用于状态文案，不进入 JSON |

CPU 帧时长来自渲染器自动曝光路径的 `RenderSystem::UpdateExposureFrameTiming()`：
它用既有的 `steady_clock` 计算 `exposureDeltaSeconds`（首帧 1/60，之后收敛到
1/240..0.1 s），并在同一处调用 `GpuPassTimer::SetCpuFrameTiming()`。因此 FPS/CPU ms
和眼适应使用同一个 frame delta，没有第二个帧率时钟；GPU 采集关闭时 CPU 字段仍每帧更新。

`valid` 只描述 GPU 通道：`gpuTimingSupported && collecting && gpuTotalMs 有值`。
设备不支持 timestamp、尚未读回、或窗口内没有任何有效样本时为 `false`；此时 CPU 数字仍可显示。

## 机器通道（`perf json` / `--perf-jsonl`）

两个通道由 `FormatPerformanceJson()` 序列化，固定键顺序、固定 3 位小数、无效值一律写
`null`，不输出 NaN/Inf，也不使用指数记法：

```json
{"fps":56.559,"cpuMs":17.681,"gpuTotalMs":14.290,"valid":true,"passes":[{"name":"hairSimulation","ms":1.258},{"name":"shadowCascade0","ms":2.519}]}
```

- `fps`、`cpuMs`、`gpuTotalMs` 是数字或 `null`；`valid` 是布尔值。
- `passes` 始终存在，顺序等于 RenderGraph 录制顺序（有 hair 时 `hairSimulation` 最前）。
- 单个 pass 没有有效样本时写 `"ms":null`，不是 0。
- `--perf-jsonl` 每行一个完整 JSON、单个 `\n`、逐行 flush；`Init` 完成满 1 秒后的第一个
  更新帧写出首行，之后维持墙钟节奏，渲染变慢不补写历史行。

**数值一致性**：面板、`perf json` 与 `--perf-jsonl` 共用 `PerformanceSnapshot` 和
`FormatPerformanceNumber()`；同一个快照在三处产生相同数字。面板按 5 Hz 从自己冻结的副本
刷新，`perf json`/JSONL 读取命令执行时刻的当前副本，因此不同输出时刻可能对应相邻快照，
但不存在数值规则差异。

## 表格与滚动平均

纯逻辑在 `source/render/backend/gpuPassTimings.h`：

- 名字顺序 = 录制顺序：有 hair 时 `hairSimulation` 在最前，其余按 RenderGraph pass 顺序；
- 窗口容量 120 帧；`avg over N` 的 `N` 是当前窗口内已采集的帧数（不足 120 时为实际帧数）；
- 某一项在窗口内没有任何有效样本时输出 `n/a`；部分样本为 `n/a` 时该行按自己的有效样本平均，
  表头 `N` 仍是窗口内已采集的帧数；
- `total` 是各行平均值之和；没有任何有效行时输出 `total n/a`；
- 固定两位小数，例如：

```text
[GPU] frame avg over 120: hairSimulation 0.18 ms | shadowCascade0 0.42 ms | ... | total 12.30 ms
```

## 数字含义与已知边界

- **各 pass 时间之和不是帧时间**。表格不包含未计时的环境 IBL 计算、UI overlay、present、
  CPU 提交间隙和 vsync 等待；`total` 只是"被计时 pass 的串行时间之和"。
- timestamp 本身会改变流水线行为：begin/end 的 `BOTTOM_OF_PIPE` 让相邻 pass 之间 drain，
  短 pass 可能被相邻长 pass 的同步开销放大；不开 `--gpu-stats` 时没有这些 timestamp。
- begin/end 的 `BOTTOM_OF_PIPE` 把每个 pass 串行化，因此 `duration` 是"该 pass 独占 GPU 的
  时间"，不是异步 overlap 后的 wall-clock 贡献。真实帧时间里 compute/graphics 若有 overlap，
  各 pass 之和会大于这部分实际占用。
- `hairSimulation` 覆盖 `HairSimulationPass::Record()` 内部所有 dispatch（9 个 compute pass，
  含可选的 SDF Clear/Splat/Collide），不是逐 dispatch 拆分。
- 录制路径不逐帧分配堆内存：slot 名字数组、持续时间数组在 pool 创建后按上限预留；日志每
  120 帧一次，不逐帧打印。

## 相关测试

- `tool/renderer-backend-tests/gpu_pass_timings_test.cpp`（`vl_renderer_backend_tests`）：
  tick→毫秒换算、`end < begin` 与非法 period、部分窗口平均、环形窗口淘汰、名字顺序稳定、
  重配置清窗、已知样本表的逐字字符串、`n/a` 与空表。
- `tool/renderer-backend-tests/performance_snapshot_test.cpp`（同一套件，12 项）：
  JSON 逐字字符串与固定键序、3 位小数、`null` 与 NaN/Inf 拒绝、CPU/GPU 合并、
  部分窗口、缺失/负值 pass、`valid` 规则、快照副本独立、百分比、表格还原。
- `tool/engine-launch-options-tests/launch_options_test.cpp`（`engine_launch_options_tests`，6 项）：
  `--gpu-stats` 默认关闭/可接受/与既有参数组合、`--perf-jsonl` 默认关闭/路径必填/
  与 `--gpu-stats` 组合，未知参数仍被拒绝。
- `tool/editor-ui-tests/performancePanelTests.cpp`（`editor_ui_tests`，7 项）：
  摘要/百分比/条形映射、图序而非成本序、5 Hz 节流与可见性立即刷新、不支持/未采样/无有效
  样本的空状态，以及首帧空快照不伪造 0。`DockRmlTest` 另验证 Performance 标签注册在
  Log/Renderer 同组并消费冻结快照。
