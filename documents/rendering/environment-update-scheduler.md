# 环境派生资源分帧更新契约

## 1. 适用范围

本文定义当前已实现的环境派生资源更新规则，覆盖：

- 程序化天空 environment cubemap
- 9 项球谐系数（SH9）
- specular IBL prefilter cubemap
- dirty 代际、分帧预算、active/pending 资源切换
- Vulkan 可见性与 GPU timestamp 诊断

Sky Pass 的可见背景不属于低频派生资源。程序化天空背景直接读取每帧最新
`SkyParametersGPU`，而 cubemap、SH 和 prefilter 按本文规则渐进收敛。

## 2. 职责边界

| 组件 | 当前职责 |
| --- | --- |
| `EnvironmentUpdateState` | 比较环境源快照，维护 requested generation 与 dirty |
| `EnvironmentUpdateScheduler` | 冻结 pending 快照，维护 face/SH/mip 游标、预算和 commit 阶段 |
| `ProceduralSkyCubeGenerator` | 按 face 写 pending cubemap，并在 commit 时复制到稳定 active cubemap |
| `EnvironmentIblBaker` | 对冻结 cubemap 执行一次 SH 投影、逐 mip prefilter，并提交 active IBL |
| `EnvironmentGpuTimer` | 按 swapchain image 管理 timestamp query，记录四类环境 GPU 时间 |
| `RenderSystem` | 观察 dirty、取得帧计划、按依赖顺序录制工作并发布诊断快照 |

调度器不拥有 Vulkan 对象，generator/baker 不决定代际推进。该拆分保证状态机、
资源生命周期和命令录制可以分别审查。

## 3. Dirty 输入规则

以下变化会创建新代际：

- 环境类型变化
- 程序化天空的 `SkyParametersGPU` 任一字段变化
- HDRI cubemap 的逻辑资源 key 或 generation 变化

以下变化不会创建新代际：

- 相机移动
- `environmentIntensity` 变化
- `prefilteredCube` 输出句柄变化

`environmentIntensity` 在 Sky 与 lighting 消费端统一相乘，因此不应重复生成
cubemap、SH 或 prefilter。

## 4. 分帧状态机

默认预算为：

- cubemap：每帧 1 个 face
- SH：依赖满足后执行 1 次
- prefilter：每帧 1 个 mip
- commit：依赖满足后执行 1 次

状态顺序为：

```text
Idle
  -> Cubemap(face 0..5，仅程序化天空)
  -> SphericalHarmonics(0/1)
  -> Prefilter(mip 0..N-1)
  -> Commit
  -> Idle
```

各类预算独立。若某一帧刚好完成最后一个 face，剩余的 SH 和 prefilter 预算
可以在同一命令缓冲中继续使用，但不会突破各自上限。

同一帧允许生成多个 cubemap face。每个 swapchain image × face 使用独立的参数
UBO/descriptor，避免命令提交前多次写同一映射内存导致所有 dispatch 读取最后一个
`faceIndex`。

## 5. 冻结快照与取消语义

新代际开始时，调度器按值保存 `EnvironmentSnapshot`。程序化 cubemap 每个 face
都读取该冻结快照，而不是读取随后可能继续变化的 frame-local Global UBO，避免
六个 face 来自不同天空参数。

若 pending 尚未完成时收到更新代际：

1. active generation 保持不变；
2. pending face、SH 和 mip 游标归零；
3. 新冻结快照替换旧快照；
4. pending 资源在同一 graphics queue 上建立读/写到覆盖写的 barrier 后复用。

旧任务不会清除新请求的 dirty。只有 generation token 与最新 requested generation
一致时，`EnvironmentUpdateState` 才允许完成 dirty。

## 6. Active / Pending 资源

程序化 cubemap 与 prefilter cubemap 都使用两份 image：

- `active`：graphics descriptor 长期引用的稳定对象；
- `pending`：compute 分帧写入的工作对象。

运行期不通过交换 `shared_ptr` 或批量更新 graphics descriptor 切代。完整 pending
结果在 commit 中复制到 active，因此所有已提交和将提交的 descriptor 始终引用同一
active image/view/sampler 地址。

首个代际完成前，active cubemap、active prefilter 和 Global UBO 的 SH 区间均初始化为
确定的黑色/零值，禁止采样未定义 layout 或未初始化数据。

HDRI 本身已经是完整输入，不经过程序化 cubemap face 阶段；其 SH 与 prefilter 仍然
使用相同 pending/active 提交流程。

## 7. Swapchain 与 SH 广播

SH 投影写入一份共享的 144 字节 pending buffer。commit 时只执行 buffer copy，
把相同 SH9 结果写入全部 frame-local Global UBO：

```text
一次 SH compute
  -> EnvironmentSHPendingOutput
  -> copy 144 bytes 到 GlobalUBO[0..swapchainImageCount-1]
```

因此 swapchain image 数量只影响很小的复制工作，不会导致 SH 重复投影。常规 CPU
Global UBO 上传显式跳过 `environmentSH` 区间，避免覆盖 GPU 已提交结果。
初始化时会校验 Global UBO 数量与 swapchain image 数严格一致，且每个 buffer range
完整覆盖 `environmentSH`；该约束禁止某个 frame-local UBO 漏掉代际提交。

## 8. Vulkan 同步契约

当前所有环境工作与 graphics pass 位于同一 graphics queue。关键依赖如下：

| 生产者 | 消费者 | 主要 stage/access 与 layout |
| --- | --- | --- |
| cubemap face compute | SH/prefilter compute | `ComputeShader/ShaderWrite -> ComputeShader/ShaderRead`，`General -> ShaderReadOnlyOptimal` |
| pending prefilter compute | commit copy | `ComputeShader/ShaderWrite -> Transfer/TransferRead`，`General -> TransferSrcOptimal` |
| 旧 active graphics 采样 | commit copy | `FragmentShader/ShaderRead -> Transfer/TransferWrite`，`ShaderReadOnlyOptimal -> TransferDstOptimal` |
| commit copy | 新 active graphics 采样 | `Transfer/TransferWrite -> FragmentShader/ShaderRead`，`TransferDstOptimal -> ShaderReadOnlyOptimal` |
| SH compute | SH 广播 copy | `ComputeShader/ShaderWrite -> Transfer/TransferRead` |
| SH 广播 copy | Global UBO 消费 | `Transfer/TransferWrite -> Vertex/Fragment/Compute Shader/UniformRead` |

对所有 swapchain Global UBO 的写入通过同一 queue 的 barrier 排在旧 uniform read 之后，
再排在后续 shader read 之前。当前实现不依赖跨 queue ownership transfer。

## 9. GPU 时间与进度诊断

每个 swapchain image 独占一组 timestamp query。该 image 再次 acquire 时，其 fence 已经
等待完成，CPU 可无阻塞读取上一轮结果，然后在当前命令缓冲中 reset 并复用槽位。

记录的产品为：

- cubemap face budget 总时间
- SH 投影时间
- prefilter mip budget 总时间
- active commit 与 SH 广播时间

时间戳根据物理设备 `timestampPeriod` 换算为毫秒，并保留 last、average、max 与
sample count。启用 Tracy 时，毫秒值同时写入对应 Tracy Plot；Vulkan debug region
保留在 RenderDoc/Nsight GPU 时间线上。

开发者 UI 显示 active/pending generation、当前阶段、face/SH/mip 进度、是否仍在使用
旧资源，以及四类 GPU 毫秒值。

## 10. 必须保持的约束

- 不得恢复“每个 swapchain image 各执行一次 SH 投影”的路径。
- 不得在 pending 未完成时让 graphics descriptor 指向 pending。
- 不得让分帧 cubemap face 读取会随帧变化的未冻结参数。
- 不得用 `environmentIntensity` 触发派生资源重建。
- 新增 queue 或 async compute 前，必须重新设计 ownership transfer 与跨 queue semaphore。
- 修改预算、资源数或提交顺序时，必须同步更新本文和对应的模块测试。
