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

- `tool/renderer-backend-tests/gpu_pass_timings_test.cpp`（`vl_renderer_backend_tests`，51 项）：
  tick→毫秒换算、`end < begin` 与非法 period、部分窗口平均、环形窗口淘汰、名字顺序稳定、
  重配置清窗、已知样本表的逐字字符串、`n/a` 与空表。
- `tool/engine-launch-options-tests/launch_options_test.cpp`（`engine_launch_options_tests`，4 项）：
  `--gpu-stats` 默认关闭/可接受/与既有参数组合，未知参数仍被拒绝。
