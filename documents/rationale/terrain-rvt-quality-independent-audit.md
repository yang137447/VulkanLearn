# Terrain RVT 质量 / 加载状态独立中审

日期：2026-11-21。性质：独立只读审计的证据与推导，**不是实现契约，不是整阶段 PASS**。

复验状态：第 1–7 节保留原运行失败及整改前源码证据；第 8 节关闭 **F1
present old-swapchain 子项**；第 9 节接受 **C008/统一 Release core 的实际 full
Service Observe 初始放行范围**。两项均不改写原 raw，不表示 P3/P5 整体 PASS。

## 1. 结论

**已运行 scope：FAIL / 未释放 interactive。P3、P5 整体验收：pending。**

审查对象为 `p3-noether-game-min/run-20261121T035333501660Z`，不是此前
Cook、旧 V1 Full16 或其它运行。独立 CPU 解码全部 167 份二进制，确认最后
visible=3271、valid=3262、unresolved=0、incorrectIdentity=0、rootOnly=0，
但每份都有 9 个 claims=2 的像素，全部 complete=false。
3262/3271=99.7248547845% **不能覆盖完整性失败**；初始 86.748 s 门槛
实际触发 `Terrain.QualityConvergenceFailed`，进程 exit=2。

优先发现：

1. **[P1，实际运行证实] 固定机位的重复可见 claim 持续阻塞质量释放。**
   duplicate 冲突处理继续由 Hegel 整改；不能忽略冲突像素、修改分母、
   将 claims>1 当作合法、移除 complete 条件，或放宽冻结指标。
2. **[P1，当前源码可证；本 run 未执行 resize] 新 swapchain leaf 的延迟寿命
   与 resize 的 `oldSwapchain=nullptr` 相冲突。** Game loading cache 仍持有旧
   swapchain 时，detach 不会销毁/retire 它，却直接为同一 surface 创建新链。
   应作为下一次排窗前的 owner 整改项，不得用“有强引用所以 resize 安全”关闭。
3. **Game/UIOff 的最终黑清屏有真实录制路径，但本 run 没有截图或清屏事件。**
   不能把 quality 原始计数当作黑屏图像验收。错误退出与 teardown 日志有实证；
   loading 中人工输入/quit 的交互回归没有实证。

审计只读取指定 worktree 的 plan/P0Frozen、运行收据及 owner 源码；未启动
GPU、应用、编译或使用 `build-vt-release`，不占用 Cicero/Hegel 排窗。
唯一新增文件为本报告；未参与实现或修改冻结文件。

## 2. 范围、身份与复算方法

工作根：
`D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn`。

运行根：
`artifacts/terrain-rvt-production/p3-noether-game-min/run-20261121T035333501660Z`。
`launch.json`：独立 staged `main_game.exe`，worker=2，frame-limit=30，
validation，no-dev-ui，exit-after-frames=2700；实际 stage config 为 UI enabled=false，
CoverageAtlas Full4x4 R8192。没有 camera pose 指令序列。
`execution.json`：PID 76924，未 timeout、未自动重启，elapsed=88.3396397999 s，
正常观察到进程结束但 exitCode=2，不能称为成功正常退出。

只用 Node 的文件/字节读取在内存实现独立 decoder，**未调用项目 decoder
或将 runtime 汇总 JSON 当作结果**。读取全部 `capture-N.bin` 和对应 metadata：

- 校验 1,606,912 B、magic=0x31515654、version=1、capacity=16384。
- 固定 word 区：header=0..63，samples=64，results=131136，
  owners=393280，surfaces=401472；sample/owner/surface 为 32/64/64 B。
- 遍历 nonzero claims 重建 visible；claims≠1 作为 malformed，计数不接受该
  像素为 valid；检查有限 UV/majorUV、整数 bias∈[0,1]、surface/mip/尺寸。
- 遍历每个有效像素的最多 8 taps × 2 mip 分支，拆解 entry/mip/pageXY/flags，
  检查页范围、slot 1..512；逐字比较 owner 的 surface、mip、XY、generation、
  revision 和 256-bit identity 与 surface 字典，重建 missing/wrong/root/valid。
- 从 claims 与 header 检查 duplicate、visible/resolved、invalid/drop/overflow，
  独立决定 complete；核对 raw frame/pose/generation/epoch 与每份 metadata，
  以及 completedEpoch≥submissionEpoch。
- 直接相减原始 monotonic 时间；独立读取日志 clock、state、failure 与事件对照。

这复算的是 **raw 最终寻址结果/字典及门槛**，不是重新生成 GPU 页表、
base-channel 像素或 lit/TES 图像。未恢复第二个竞争 fragment 的属性，
所以不能用 missing=wrong=0 宣称九个冲突像素的双方 identity 均正确。

身份核对：

| 项目 | SHA256 / 核对 |
| --- | --- |
| 当前 P0Frozen bytes，匹配 `.sha256` | `43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd` |
| staged binary，实际读取匹配 launch | `74075c672f74668dade3269e65404d4d6b6935e218212112d9f8a363373ce783` |
| stage render graph，实际读取匹配 launch | `396b58f351b624f369c3112f60ff1fc9fe7a62f018cba3f0f96ee8b50abaa769` |
| 当前 Recorder，匹配 launch delta source | `d19cbfffbdde95d2f64956202991f280db7ad48a7c33ec2f1b0eea9bdcea654a` |
| stdout | `df66733c941015d4d8bddcdfc71886fecdac388f88ec458e494c62b8ab5772b0` |
| stderr | `f5a157308bd0cee90d95dd018cf7348963e40110a0b76144c2b45d7bdd95f746` |
| capture-1.bin | `29d846471bd03484f8db26b240d5a391500c48f989a209777ecd1846c3cbec45` |
| capture-167.bin | `db77a08e88a821433483eaa33b4e6be0e46353421f0fd77e12694a0e6a0a7f2e` |

Service/SourceSnapshots 两个实际 object 的 hash 匹配 launch requiredObjects；
当前 Service、QualityData、Budget header、QualityClock、SourceSnapshots h/cpp
均匹配 `r2-clock-metadata-mi-r3-service-handoff.json` 的 sourcesAfter。
这支持该组源码与 run 的绑定，但不把未逐项重验的整个 support archive 的
源码闭包、Present 新代码或后续修改自动认定为已运行回归。

## 3. Raw 结果与已证故障

原始二进制合计 268,354,304 B；167 份均 visible=3271、header duplicates=9，
9 个像素各 claims=2，重复额外 claim 总数也是 9；全部 complete=false，
有效像素的 missing/wrong 最大值均为 0，bias 始终为 0。

| 捕获 | epoch | valid | rootOnly | complete |
| --- | --- | --- | --- | --- |
| 1 | 1 | 0 | 3262 | false |
| 5（首次 rootOnly=0） | 22 | 3214 | 0 | false |
| 7（首次 valid=3262 且 rootOnly=0） | 36 | 3262 | 0 | false |
| 167 | 1143 | 3262 | 0 | false |

因此“root0”只适用于收敛后的捕获，不适用于整个运行；初始明确以根兜底。
第 7 份在 process-start 后约 44.0000144 s 达到非冲突像素的最高 valid，
但仍不是 quality ready；全部 state 的 interactive=false。

最后九个冲突坐标，128×128 feedback 像素域：

| x,y | 首个记录的 surface index | targetMip |
| --- | --- | --- |
| 47,46 | 0 | 6.09116268 |
| 80,46 | 3 | 6.05468845 |
| 49,51 | 0 | 6.12685156 |
| 53,71 | 9 | 5.84126282 |
| 89,83 | 11 | 5.82980680 |
| 48,91 | 13 | 5.63533497 |
| 75,92 | 14 | 5.70909739 |
| 43,103 | 12 | 4.53834152 |
| 80,103 | 15 | 4.52773905 |

源码定位：`shader/glsl/terrain/terrainCompositedQualityVisible.glsl`
先以 fragment depth 精确等于 final feedback depth 筛选，再 atomicAdd claims；
prior≠0 递增 header duplicate 并返回，只保存第一方属性。
`source/render/resource/terrainCompositedQualityData.h:126` 拒绝 claims≠1，
`:212` 要求 duplicate=0，`:90` 又要求 complete。
这是 **实际存在的 ambiguous visibility/replay 冲突**，不是细页尚未加载：
最高 valid 已稳定，duplicate 不随页生产消失。raw 无第二方 draw/primitive
标识，不能在本审计凭 UV 接近分块界线就断言哪两个 chunk 重绘，或假定双方相同
而去重。Hegel 应消除/明确解决实际 draw 可见冲突并保留 strict 冲突检测后复验。

## 4. 同 draw epoch 字典、GPU 读回与寿命

**有限源码核对：RecordedOwners 是 committed + 当前 pending overlay，不是
只取上次 completed 的字典。**
`source/render/terrain/terrainCompositedRuntime.cpp:677` 从共享 ResidencyPool
的 CommittedOwners 起步，对本次 pending removals 置 invalid，对 uploads
写入当前 slot/token。`terrainCompositedResidency.cpp:451` 返回的是全共享池，
不是第一 surface 的私有子集。Record 同时把 removals/新 entry 写入实际页表，
transfer→shader transition 在反馈 draw 前完成。

`source/render/resource/terrainCompositedService.cpp:2222` 只在 recording 中抓取
runtime Binding、Token、RecordedOwners；字典序与 surface index 一致。
Recorder 使用 currentFrame+1，Quality Submitted 要求 epoch 精确等于 snapshot
drawSubmissionEpoch；runtime Commit 在成功 Submit 后才确认本批。
167 份 raw/header 与 metadata epoch/world/pose 的匹配和有效样本 identity=0
是该机位的实证，不证明未执行的 World replacement、刷新与 eviction 压力组合。

GPU ABI/同步核对：

- output 为 device-local SSBO/transfer src+dst，host 为 host-visible/coherent
  transfer dst；无需 noncoherent invalidate。Owner update=32768 B，小于
  updateBuffer 的 65536 B 限制；header/surface/selector 的 offset/size 四字节对齐。
- replay 保留实际 feedback sets，pass set3 的 0..4 为原图输入，5=SSBO，
  6=普通 sampler2D final depth；compute 是 sampler2DArray 页表，不是 shadow sampler。
  Graph 对 terrainFeedbackDepth 使用普通 depth sampler，单采样 sampled D32。
- `terrainCompositedQualityGpu.cpp:402` 建立 fragment→compute、compute→transfer、
  transfer→host barrier，copy 完整固定 buffer，timestamp 后结束。
  `:450` 必须 completedEpoch 达到 submittedEpoch 且 query 可读才能访问 host。
  Backend BeginFrame 在复用 frame slot 前等待真实 fence，再推进 completed epoch。
- Impl/snapshot/readback 互相不是强引用循环：Impl outstanding 为 weak，readback
  storage 强持 Impl；snapshot 强持生产 inputs、页表与 graph depth leaf。
  Frame readinessService/readinessValidation、active State 为 weak。
  depth resolver 是无捕获 lambda 查 leaf registry，未发现捕获旧 World 的 callback。
- Frame 的固定寿命槽把 Impl 与 loading Entry 保留至 submission fence；
  State 本身另由 retire queue 保留。depth registry 在 graph destroy 时 release，
  leaf 销毁实际 image/views/sampler，不强持 World/完整 graph。

没有在这个 run 发现 copy/fence/ABI 错读的实证。Query 的 quality replay+resolve+copy
时间为 0.373408..28.98432 ms（首份 28.98432 ms）；这不是单页 production/copy
计时，也不是 P0 steady geometry/GPU pass sum，不能混用 4/6 ms 生产门槛或由此
宣布性能通过。metadata Workspace observed peak=25,798,688 B 也不是 WDDM
或完整进程/跨 World 瞬时高水位。

## 5. Present leaf 的 resize 缺陷与 device 顺序

**[P1，源码可证、GPU resize 未复现]**

1. Game/UIOff 无 viewportOutput 时，Recorder `:952` 把
   CapturePresentTargetLifetime 的真实 swapchainOwner 交给 LoadingTarget。
2. `terrainCompositedLoading.cpp:110` 的 cache Entry 强持 target，空槽优先；
   frame 完成不会清空缓存。即使没有 frame fence 引用，Entry 仍持 old owner。
   State 又在 `terrainCompositedQualityFrame.cpp:329` 被每帧 retire 到
   drawEpoch+2；最近一帧后仅 collect(lastSubmittedEpoch) 不足以全部释放 State。
3. resize 安全点只 WaitIdle、shutdown graph/frame resources，没有清空这个
   loading cache。`VulkanManager::ReCreateSwapChain` `:136` 调用 detach；
   `presentTarget.cpp:46` 只移除 target 的自身引用。上述 Entry 仍在，旧
   swapchain 的析构没有执行。**device idle 不会使 shared_ptr 自动析构。**
4. `VulkanManager::CreateVkSwapChain` `:479` 仍 setOldSwapchain(nullptr)，
   `:138` 立即为同一 surface 创建新链，没有传旧链使它 retired。
   与“surface 不能另关联未退休 swapchain”的 WSI 生命周期条件冲突，
   可导致 native-window-in-use/校验失败。Backend 的 collect 还在 RHI recreate
   **之后**（`:431`），不构成创建之前的保护。

无需 depth attachment 即可成立；这恰是无 depth Game 最终色目标的 owner 路径。
“保留旧 views 防悬空”是正确局部目的，但必须同时满足 swapchain retirement。
整改方向是显式 oldSwapchain handoff/retirement，或在安全点、所有消费者解除后
先确实销毁旧链再新建；不能仅把 detach 当 destruction。此处只记录，不实施。

析构局部次序正确：`PresentSwapchainResources::~PresentSwapchainResources`
先 views 后 swapchain。正常 EngineLoop shutdown 先 Stop RT/WaitIdle，
清 World/cache/graph，再 ForceReleaseAll；VulkanManager 随后 detach 各 target、
destroy device。该路径未发现明确的强 leaf 循环或正常 teardown 的倒置。
但 leaf 只借用裸 vk::Device；外部逃逸引用、异常提前销毁 backend/device 并无
类型级保护。本 run 的 teardown complete 日志不是每个资源析构的逐项收据，
也没有覆盖上述 resize/异常寿命，不能扩大为 device-lifetime 全面 PASS。

## 6. Loading、输入、deadline 与退出

### Loading 黑清屏

Recorder 在场景 passes 后、UI overlay 前调用 RecordLoading；Game/UIOff
直接选 swapchain view/format/extent、PresentSrcKHR layout 和真实 present owner。
Loading pass 是 eClear/eStore、RGBA=(0,0,0,1)，无 depth attachment，
初始/最终保留目标 layout，具有前后依赖，真正 begin/end render pass；
不是只计算 loading=true。Editor viewport 分支在 shader-read 转换/UI 采样前清色。

该 run 的 UI=false、所有 interactive=false、成功提交到 epoch1148，结合相同
Recorder 源码支持“未 ready 帧走黑清屏录制分支”的判断；但运行目录仅有
167 bin、169 json、2 log，**无 PNG/BMP/其它截图，也没有 loading-clear
专用执行事件**。不能称为已亲眼/像素验黑，不能用 quality capture（页表结果，
不是 color image）补这个缺口。UIOn/Editor 截图与首帧黑屏图像仍 pending。

### 时间与 request arrival

日志 clock：processStarted=606728413.8106 ms，
deadline=606815161.8106 ms，差恰为 86748 ms，不是从 World commit/首 capture 起算。

| 原始事件 | process-start 后秒数 / 差值 |
| --- | --- |
| world_frame_arrived | 39.292895600 s（不是 World commit） |
| 首次 capturedMonotonicMs | 42.237271400 s |
| 第7份完成，非冲突 valid 饱和 | 44.000014400 s |
| quality_failed | 86.759398900 s，超 deadline 11.398900 ms |
| 最后 readback completion−pose arrival | 47217.627200 ms，精确匹配最后 state elapsedMs |

最后 failure requested=168、completed=167；epoch1148 已提交，但没有第168份完成
raw，不得补算为已捕获。最后完成 epoch1143。capture-1 到 capture-167 的 capture
跨度=44,218.816500 ms。

所有 capture_completed 事件比 metadata.completedMonotonicMs 晚约
65.6958..100.2991 ms：事件是在 readback/Service 重算之后发出，
**不是 fence 物理完成瞬间**。门槛使用 TryConsume 观测时间；应保留该差异，
不能拿事件时间替换 metadata、或减掉 pipeline/CPU 处理以“通过”初始门槛。
进程 observer 的 88.3396 s 又是 launch 到终止观测，不能混作 quality deadline。

QualityClock 在初始 ready 前即使 SetPose/资源换绑也保留 initialDeadline；
ready 后新 pose 用 arrived+10000 ms；Failed 会在无 readback 时被 Frame/Service
查询推进，不能靠停止 capture 永久留在 Loading。
明确 camera setter 在 **执行并成功改变当前 camera 后**才调用
RequestTerrainCompositedQualityRevalidation，发 pose_arrived 的 steady-clock 时间；
GetCameraState 不触发。这里 arrival 是命令实际执行，不是消息入队/人点击时间，
也不是 delayed readback 时间。本 run 只有 pose=1，不能声称实际 teleport 10 s 回归。
资源/页表换绑的隐式 arrival 是 Frame 首次观察时刻，不能泛称所有资源请求都已
按 GT request arrival 计时；未验组合保持 pending。

Validation Begin 的非显式 view 检测以 ready 后的 512 cm/15° 邻域为容许域，
不是逐帧像素相等验证；不 ready 时普通 view 改动不会自动另开 pose。
这需在实际移动/teleport 测试中验证，但本 run camera 不动，不能凭此构造本轮
“已经发生误释放”的结论。

### 输入 / clean error / quit

`engineFramePhases.cpp:577` 在 UpdateActionState 前判 terrain loading，
禁 game keyboard/pointer/relative mouse，simulationDelta=0；update 路径另挡
autoplay、gameplay 与摄像机/粒子推进。console、UI action、runtime command
先处理，不会因 loading 将 quit 一并封死；每帧 UI policy 先更新，ready 后
不是永久留下本帧禁用状态。未实际注入按键，不能据源码宣称完整交互测试通过。

错误传播有实际证据：Frame 发布质量错误并 throw，RT fatal 被 GT poll，
exitCode=2/shouldClose=true；stderr 明确 Render thread failed，
stdout 有 EngineLoop teardown complete，execution 观察到结束且非 timeout。
这证明本轮没有“永远黑屏、不报错”。
普通 quit 的命令路径设 quitRequested，GT 返回 exit=0，并走同一 shutdown；
**本 run 未发 quit，不能把失败 exit2 当 quitclean exit0 验收**。

## 7. 中审裁决与关闭条件

已获得的是：worker2/CoverageAtlas/单初始固定机位下实际 final-depth replay、
提交/完成 epoch、raw identity/mip、strict duplicate 拒绝以及初始 deadline 的
失败证据；不是整个 P3/P5 的通过证据。

首先关闭可证故障而非改门槛：Hegel 解决 duplicate 竞争，复验完整 raw 的
complete=true；Present resize 明确退役旧链并在无 depth Game loading 情况验证。
下一收据须保留 staged binary/源码闭包/hash、未改 P0Frozen、真实 loading 截图、
interactive 事件以及 raw 完整性，不接受手工 gate=true。

实际 full53poses、worker1 与 worker2 的配对完整巡游、LocalLayerArray、
teleport/World replacement/resize 的 request-arrival 收敛、TES/动态和 lit 图像、
根恢复、独立 Game package 离线闭包、完整 timing/进程峰值及编辑闭环仍 pending。
这些缺项不稀释已证 FAIL，也不能拿本 partial raw 的零 wrong/root 反向宣布
P3/P5 PASS。计划的阶段目标、冻结质量指标与现存旧证据严格分开。

## 8. F1 present old-swapchain 独立复验：子项关闭

日期：2026-11-21。本节 F1 指第 5 节的 present old-swapchain 问题，
**不是第 3 节 duplicate 故障**。裁决：**F1 CLOSED / 有限模块 PASS**。
此前“detach 后仍以空 oldSwapchain 创建新链”的反例已被当前源码修正；
实际 native resize/清屏模块证据支持关闭该子项，不升级整阶段结论。

本次未启动任何 GPU/native harness，不重跑当前主运行窗口、不编译、
不使用 `build-vt-release` 构建或运行，不改实现/配置/raw。
独立重跑已有 CPU spy executable 的 10 项测试，并直接解码已有 BMP、
核对源码/对象/实际 native executable hash、日志与生命周期调用链；
唯一写入仍为本报告。下述 GPU 结果是对**已完成运行**的证据复验，
不是本审计新跑 GPU。

### 8.1 整改调用链与生命周期

- `source/vulkanManager.cpp:136` 调用 DetachSwapchainForReplacement，
  清 target 的借用 aliases，但返回真实 oldOwner；`:138` 的强引用跨越
  CreateVkSwapChain，`:487` 继续交给 PresentSwapchainResources::Create。
- `source/render/rhi/vulkan/presentTarget.cpp:42` 在 native create 前验证
  old leaf 存活且同 device，并覆盖 createInfo.oldSwapchain 为该 leaf 的真实
  handle；初建才为空。oldOwner 参数一直存活到 create 返回/异常展开，
  原 F1 的“旧链尚活而 native create 收到 null”不再成立。
- 新链成功后，Loading cache/frame 的引用可以继续保留**已退休**旧链及 views。
  新链不反向强持 oldOwner，registry 也是非拥有 raw-pointer 索引，不形成旧链链式
  保活或 World 循环。最后引用释放才正常按 views→swapchain 顺序销毁。
- native replacement 失败不 republish 旧链；partial candidate 析构清理，
  aliases 保持 detached，异常交回既有 resize failure/shutdown 路径。
  不把已 retired 的旧链当作可回滚 active swapchain。
- `VulkanManager::DestroyVkDevice` `:359` 在 device.destroy 之前调用
  ReleasePresentSwapchainsForDevice。registry 在锁内遍历仍存活的 leaf，
  ReleaseNativeResources 先销毁 views/链，然后清空 views、swapchain、device。
  晚 alias 析构时 native release 发现 device=null，不能再次调用 dead device。
  析构 unregister 同样受锁保护；registry 不拥有 leaf。
- 原 GT/RT resize safe point、WaitIdle 与 framebuffer/cache 的完成引用规则仍须
  满足；本整改没有把 ordinary-frame wait 或额外 swapchain 创建引入质量流程。
  此关闭不证明任意外部消费者可在 device shutdown 后继续使用被清空的 aliases。

### 8.2 CPU spy：独立执行 10/10

审读 `tool/validation/terrain/noether_present_owner_tests.cpp`：
其 vkCreateSwapchainKHR spy 记录实际 createInfo.oldSwapchain，
destroy spies 检查 deviceAlive 并记录每个 view/chain 的顺序；
没有 SDL/window/Vulkan initialization。已有 executable 的 import table
无 Vulkan loader，只有运行库/系统库。本次执行：

```text
%TEMP%/vulkanlearn-swapchain-owner-f1/present_owner_tests.exe --gtest_filter=PresentOwner.*
10 tests ran; 10 passed; exit 0.
```

覆盖 actual factory 的旧 handle=100、初建 null、跨 device 在 native call 前拒绝、
native create 失败、candidate publish 失败、重复 detach、active owner 防覆盖、
generation 切换、epoch19 不释放/epoch20 才按 views→chain 释放，以及 deviceAlive
置 false 后晚 aliases reset 不再 destroy。结果与已有 cpu-tests.xml 的 10/0 对上。
CPU executable SHA256：
`0eb6bf23af90136c2585a2cc536ce61d6665c6e7c6976c0d359770e1de2c1f03`。
这是复用现存 spy executable 的独立执行，不声称本次从源码重新编译或
CPU 模拟可替代真实 WSI；当前整改源码绑定另由下述 prepared native inventory 支撑。

### 8.3 实际 native 运行身份，不能混淆 Release link-only

证据根：
`artifacts/terrain-rvt-production/noether-swapchain-owner-f1`，
实际 run：
`run-1795237381947678100`。

当前四个整改文件 SHA256 均匹配 production-object-handoff.json 和 build.json：

| 文件 | SHA256 |
| --- | --- |
| VulkanManager.h | `0774baa8bd61b7344638c324eaff07f7302b8870a6f5cd7602bb3f2ae8f9aeba` |
| vulkanManager.cpp | `9e6e1581c6ba44313a8f531e7a4d83b7ed88c1dc88097ede71e7af69ae7f9f51` |
| presentTarget.h | `a56c7659b31974b9e66274b5b2ee7c80d1e081d68a10d614521fea8ad18ae8c8` |
| presentTarget.cpp | `97cd3f39380899147e71aa1e12e825ddf293f5a999bc37e282112ee4dee8a338` |

实际读取 launch.argv[0] 的 `swapchain_resize_check.exe`，
SHA256=`b43e21faa87a5b8a03de082459019c1d6b89663bc6a66f8693fc6760e5fb6664`，
匹配 launch 与 build；prepared 7 个 object hash 全匹配。
prepared native harness 的源码 inventory 除 TerrainRvtBudget.h 外均匹配当前文件。
该 header prepared=`693169667da37d3979cf34d73d7b2c0fa6a661c3ca4350c0358b3483e7dbf5cb`，
当前=`f987b01d07b9ad3987692c253fdca8de95d1fab3e66157ea884cfb894054b102`；
launch.preparedInventoryDifferences 已明确保留同一差异，运行策略是未改 prepared
binary/support snapshot。这不影响四个 F1 文件绑定，但不能宣称验证当前整套
Budget ABI/生产总账或后续 native core。

handoff 中两个 Release production object 的实际 hash 都匹配
`vulkanManager.cpp.obj` / `presentTarget.cpp.obj`；
Release-linked harness hash=`5d87b474d8db2ba5016c629396b13db96eba065ee1d542f354f33810542d4e00`
也匹配，但 handoff.executed=false。**实际 GPU 跑的是上述 b43e… prepared harness，
不是 5d87… Release link-only executable，也不是 full main_game。**
prepared link.map 的 native Vulkan import 关联实际对象；未见 CPU spy 测试源码进入
该 map。不能把 link-only 收据说成 Release GPU 已运行。

### 8.4 三次 resize、四次白底→黑清屏：独立像素复算

审读 native harness `tool/validation/terrain/noether_swapchain_resize_check.cpp`：
每帧先用真实 swapchain framebuffer 清为白色，再调用现有 Loading::Record
清黑，随后 RecordSwapchainScreenshot、SubmitFrame、CompleteSwapchainScreenshot。
没有 depth attachment/terrain/UI；因此全黑不是“本来没画东西”的黑底假阳性。
三次 resize 跨越 1280×720→960×540→800×600→1280×720。
report generation 为 1/2/3/4，oldGeneration 为 0/1/2/3。

本次另写内存 decoder，未调用 runner.check_black：校验 BMP magic/DIB、planes=1、
24-bit BI_RGB、尺寸/offset/stride/文件长度，逐个像素检查 B/G/R，不计行 padding。
四张图合计 **2,841,600 像素，非黑像素=0**，尺寸和所有 SHA256 对上 execution：

| 图 | 尺寸 / 像素数 | 非黑 | SHA256 |
| --- | --- | --- | --- |
| black-0.bmp | 1280×720 / 921600 | 0 | `d43ec90910e666b315661be75dfea0af9722ff471aa30593999567dffc53ac9c` |
| black-1.bmp | 960×540 / 518400 | 0 | `74b7fa6d14444bcc61896f4466d86b7f2399b65fcc86364408156e0c826a4d2c` |
| black-2.bmp | 800×600 / 480000 | 0 | `778f0a8fe7571a631823c081e4f4b4d6eb2093553249633a5059a5bd36e35485` |
| black-3.bmp | 1280×720 / 921600 | 0 | `d43ec90910e666b315661be75dfea0af9722ff471aa30593999567dffc53ac9c` |

source 检查了 screenshot 完成 fence 后才释放 frame 引用；三份 old weak owners
在清掉 seed/external 引用后仍由 Loading cache 保活，loading.reset 后必须全部
expired，失败会使进程返回 1。report 的 oldOwnersReleasedAfterCacheReset=true
与该实际 assertion 路径一致。全局 lateShutdownOwner 在 main 前构造，实际
VulkanManager 函数静态实例在运行中才构造：正常退出时 Manager 先析构并清 native，
late alias 后析构。不能把 report 写入时的布尔值单独当 destructor 收据；
本次以源码顺序、真实 process exit0 和 CPU dead-device spy 联合支持此边界。

stdout 明确 `Validation layer: on, debug utils: on`，四次 present mode 日志；
独立扫描 stdout/stderr，VUID/Validation Error/错误文本均为 0，stderr 为 0 B。
execution PID23960、exitCode=0、timedOut=false、gpuProcessEnded=true；
launch→end 观测间隔=3.028757334 s，不是 terrain process-to-ready 指标。

| 证据 | SHA256 |
| --- | --- |
| execution.json | `518cf3dc40038b35678fc2ac2bb33c58a9e6a94998a890cf2d5e332a85068d80` |
| report.json | `e707577c6588bbb1a21b3ac9e16676e4d0860cd801451d3eb5d2a282aeca64f8` |
| stdout.log | `87864c8097a538f5ec3eb9e0870c490503e274e6cc05b97e202ff5a493c13c2c` |
| stderr.log | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |

### 8.5 关闭边界：不等于 full quality ready

关闭的是 **真实 oldSwapchain handoff、旧 views/cache/fence 保活与释放、
device-before-late-alias 清理及无 depth 清屏模块**。这补充了模块级黑清屏图像
证据，不给原 `run-20261121T035333501660Z` 补造截图/成功结果。
原 167 captures、9 duplicates、initial deadline failure/exit2 仍为历史实证。

用户本轮提供的 winner7MRT/2capture dup0 module PASS 是另一个整改状态；
本次未读取其专属 raw/执行文件闭包，不重新签发该项独立质量 PASS，
也不否定其模块收据。实际全 Game Observe 与新 native core 的组合待 Euclid。
这里 native harness 不含 TerrainCompositedQualityFrame/Service Observe/真实 terrain
working set，不能外推为 interactive 已释放、10 s teleport 或 Full53 已通过。
**P3/P5 整体继续 pending**，worker1/2 全量、双权重、TES/lit/fullready 和其它
第 7 节未完成验收不因 F1 关闭而消失。

## 9. 统一 Release core / C008 初始 full Observe：独立接受

日期：2026-11-21。裁决：**INITIAL SCOPE ACCEPTED / CLOSED**。
对象为实际 Game 首次加载、GPU winner/raw、完整 Service::ObserveQuality、
加载黑屏解除与退出，不再是 only-module 或 overlay harness。
**P3/P5 整体仍 pending**；本节不覆盖后续 53 poses、worker1/2 配对全程、
steady/performance、WDDM/整进程峰值、readonly 包或 P4 编辑。

本次只读已有证据、内存 CPU 解码与图像查看；未启动 GPU/游戏、未编译、
未改 source/config/raw。仅追加本报告。

### 9.1 实际执行文件、冻结 source 与 C008 绑定

运行：
`artifacts/terrain-rvt-production/p3-core-full-observe/run-1795239270837751400`。
argv 是 private stage `p5s/QO-93f6b735ec/bin/main_game.exe`，
worker2、30 Hz、validation、no-dev-ui、exit-after-frames=3000、gpu-stats。
stage config 明确 UI=false、resourcePath=`p5r/C008`，初始 scene 为
`RVTv2_Full4x4_R8192_Default_CoverageAtlas/SC_composited_vt_4x4.json`。

独立 hash 实际 argv executable，匹配 launch/execution/acceptance 及
`core-native-r2-frozen/checkpoint.json`：

| 身份 | SHA256 |
| --- | --- |
| 实际 main_game.exe | `ce9c1814d4813a94473193dc365fb235eeec318d6665e9b0a062ad35fee332d3` |
| core checkpoint | `62cbd0a2292bb3a573c4d6a93e3d492b8c4bf89c196372a9e0df672143cb2612` |
| core source-before manifest | `e8a621f27a12c9060ec98ab1ce0fe637b71621b93392cac414028890e6dec6ee` |
| private stage config | `902f5b04f966926200edf0d0f3aeb0cbdd7939200a3c2edf9b57791824f165e1` |
| private stage graph | `15b16e55db18cbab443ec16836c2daa2ca98cf6a77dc4f9ef5a935fdf51f7c24` |

独立遍历 **1073 个冻结 source 文件**，全部匹配 source-before manifest；
checkpoint 列出的 14 个 binary/object/archive hash 也全部匹配。
mainGameLinkInputs 是普通 CMake `libvl_runtime.a` 及依赖链接，prepared/receipt
objectOverlays=[]、moduleOnly=false，所有 capture metadata.moduleOnly=false；
不是 F1 harness 或临时替换 Service object 的旧运行。

绑定以冻结 core 为准：审计时 live Service/Recorder 已与冻结版本不同，不能
用后续 live source 冒充 ce9c… 的已运行源码。实际审读的是冻结
`source/render/resource/terrainCompositedService.cpp:2815` 的 Observe 和
`:125` 的 process-start clock，以及冻结 Frame/QualityGpu。
QualityData/Gpu/Frame/Clock 当前 hash 仍与冻结值一致。
stage 的 Quality compute、terrainFeedback fragment、Footprint shader 也分别
匹配 prepared.shaderSources，避免把 C++ manifest（不含这些 shader key）误作
完整 shader 绑定。未对后来修改的 native core 签发回归通过。

C008 publication receipt SHA256
`a727baf6affa63fff74a21a77a8d2672a6a0b40e89452e38e19b947584dee8ac`，
实际 manifest SHA256
`4cbfbc0671bacee39cf478a34be517bba15863bbedad800755f14fe5b0a9143f`，
均与 prepared/publication receipt 对上。独立数 manifest 与实际文件：
16 surfaces、16 roots、**67 production objects**（实际字节总数 390,874,396 B，
每项大小相符）、**448 唯一 published page keys**、99,434,496 B 页 payload，
实际 package=99,434,512 B，所有页 offset/size 在 package 内，无重复 key。
不是把当前新对象数量套到旧 C006/C007 包。
此处验证文件身份/索引/字节与 current token 绑定，不重做全对象 BLAKE3
Store integrity 验收；publication 原 nativeStoreValidateAll 收据仍是该项证据，
不能把本节升级为资源全闭包/readonly acceptance。

### 9.2 独立 TVQ2 decoder：14 captures，逐 tap low/high owner

独立读取全部 **14×1,619,200=22,668,800 B** raw；没有调用项目 decoder，
没有以 observations 或 receipt 的 accepted=true 代替复算。
检查 version=2、固定 sample/result/owner/surface offset、256×12 words
duplicate appendix、frame/pose/world/sequence/epoch 与对应 metadata。
逐 nonzero claims 像素重建分母；检查 claims=1、有限 UV/majorUV/target，
整数 bias∈[0,1]，surface 尺寸/flags/maxMip/generation。

对每个 tap 的 low/high branch，按 floor(target)+bias 与下一 mip 分支核对
requested/实际 mip、entry/slot、pageXY/flags、owner valid、surface、generation、
revision、256-bit identity；检查实际 mip 是否不粗于该分支 target+1。
累计独立检查 **208,894 个分支**。另外由 raw UV/majorUV/tap 数复算采样位置，
按 anchored center 或 tap 的 clamp/repeat 坐标核对 pageXY，失配=0。
这核验的是已导出的最终 GPU page-table 地址，不声称重建未导出的整张 GPU 页表
或独立 base-channel 像素参考。

第 6 份的 16 个 raw surface identity **逐项等于 C008 manifest.contentHash**；
generation 全为 1、revision 全为 0。每个 sampled owner 与对应 surface 的
generation/revision/hash 相等；初始无编辑，冻结 Residency 用初始 revision/token。
Observe 还在运行时重新取 current runtime.Token，比对 snapshot identity/revision，
并要求 RootsCurrent、所需 height atlas/LUT 存在、无 budgetInsufficient、
无 convergence failure，才接受 counts。不存在“只信完整 readback 就 ready”的旁路。

| capture | frame / epoch | visible | valid | rootOnly | complete / meets |
| --- | --- | --- | --- | --- | --- |
| 1 | 0 / 1 | 3271 | 0 | 3271 | true / false |
| 2 | 2 / 3 | 3271 | 0 | 3271 | true / false |
| 3 | 8 / 9 | 3271 | 1494 | 1742 | true / false |
| 4 | 15 / 16 | 3271 | 2757 | 461 | true / false |
| 5 | 22 / 23 | 3271 | 3225 | 0 | true / false |
| 6 | 29 / 30 | 3271 | 3269 | 0 | true / true |
| 7–14 | 32..82 / 33..83 | 3271 | 3271 | 0 | true / true |

所有 14 份 duplicate/invalid/drop/overflow、duplicate witnesses/overflow、
unresolved/incorrectIdentity/uninitializedDraws 均为 0，bias=0；
visible=header visible=resolved，分母没有排除“不够细”的像素。
每份 hash/counts/complete 与 observations 对上；每份独立 meets 判定与
**实际 stdout Service interactive** 对上。初始只有完整性通过，根兜底仍禁止放行；
第 5 份虽 root0，也因 valid 不足而没有提前释放。

第 6 份分母 3271，99% 所需最少 valid=3239，实际 **3269/3271
=99.93885661877103%**。两份不够细像素均 surface7：
`(84,57)` target=4.97234201、`(84,62)` target=4.73863649，各 4 taps；
low requested=4 仍到 mip6/slot80/page(0,0)，超过 low+1，故按像素各扣 1，
**没有忽略或修改分母**。其 high branch、identity/解析仍合法，不是 unresolved。
下一份 valid=3271。第 6 份 raw SHA256：
`b850ba497bd13ce11101cd70d1a3cc42f526bc523816c7f47a98b33712325c5e`。

当前路径从原 depth-tested feedback 的两个 RGBA32Uint MRT winner 直接导入，
不是再次 redraw 精确等深度的多 fragment claim。compute 对 winner valid/depth
位、surface 范围不符仍记 invalid/drop，完整性门槛没有放宽。
本运行的 winner/raw 结果足以关闭**这一初始固定机位**的旧 duplicate 阻塞，
不表示全部边/交点、相机姿态和动态场景都已覆盖。

### 9.3 初始 clock、Service 接受与黑屏解除

冻结 Service 的 process-start 使用 GetProcessTimes 的 creation FILETIME，
以当前精确系统时间与 steady clock 映射到 monotonic epoch；
不是 World commit、Frame arrival 或 pipeline 完成才开始计时。
这是对源码与原事件的一致性核对，未重新查询已退出 PID 的 creation timestamp。
单个初始 clock 及所有 state deadline 相同：

```text
processStartedMonotonicMs = 612785623.0095
initialLimitMs           = 86748
deadlineMonotonicMs      = 612872371.0095
```

初始 clock 不因 SetPose/绑定发现而后移；初始 ready 前保持 initialDeadline。
日志只有 world=1/pose=1，无 pose_arrived/reset，全部 state 未报 failed。

| 事件 | process-start 后 |
| --- | --- |
| world_frame_arrived（不是 commit） | 31.213559400 s |
| 第6份 raw completed，epoch30 | **32.864526900 s** |
| frame31/epoch32 loading_released | **32.954422700 s** |
| release 时距离 initial deadline | **53.793577300 s** |

Frame 先 TryConsume 完成 epoch/query，**先写出 raw 再 ObserveQuality**；
Observe 重新 Recompute 并校验 current token/height/roots/budget，随后
Validation::Observe 接受相同 stamp/camera/epoch，loading 使用二者共同的 ready。
不是 runner 修改 gate，也不是 module-only 把 counts 直接赋给 interactive。

实际 Service 第一条 interactive=true 对应 epoch30 的 3269/3271，elapsedMs
=1650.95（自 Frame pose arrival），不能把它当 process-to-ready=1.65 s。
loading_released 事件在 frame31/epoch32，明确引用 observationFrame29、
observationSubmissionEpoch30、observationCompletedEpoch30；之后有对应 screenshot
saved 日志。raw completion→release=89.895800 ms，含 CPU 导出/Observe/调度，
未从初始时间中扣除。ready screenshot 是 epoch32，不偷换为 epoch30 即呈现。
截图队列事件本身不是 fence 完成；本节结合 saved 文件、像素和 clean exit 验证。

### 9.4 图像独立像素与人工模型检查

直接解码两张原 BMP 的 header/24-bit BI_RGB/offset/stride/完整文件长度，
逐像素检查 B/G/R，且通过 `view_image` 人工模型分别查看：

| 原截图 | 独立结果 | SHA256 |
| --- | --- | --- |
| first-black.bmp，frame0/epoch1 | 1280×720，921600 像素全黑；maxChannel=0 | `d43ec90910e666b315661be75dfea0af9722ff471aa30593999567dffc53ac9c` |
| first-ready.bmp，frame31/epoch32 | 1280×720，非黑像素921600，非零通道2764800，maxChannel=237 | `110b9e2095f8fa3c3ee8a73597d4c2e68de2425691b3fe945c820eb1b8703051` |

人工图像检查：black 是整幅纯黑；ready 不是仍黑或空窗口，中央可见俯视透视的
起伏地形、绿色/黄褐/灰色地表细节及浅色矩形边界，周围蓝色水面/背景，无 dev UI。
确认**实际黑屏→可见 terrain**，不只是非零背景颜色。
这不是 lit ROI 数值误差、TES silhouette/height oracle、近场纹理质量或 53 姿态
审美验收；图像可见性 PASS 不代替那些比较。

stdout 明确 validation on；独立扫描 VUID/Validation Error/quality failure/
incomplete 均为 0，stderr=0 B。日志有实际 `quit`、`Quit requested.`、
`EngineLoop teardown complete.`；execution PID7280、quit injection exit0、
process exit0、gpuProcessEnded=true、无 autoRestart，elapsed=36.8620165 s。
这次可支持 quit-clean，不再把此前 failure exit2 当成普通 quit 通过。

### 9.5 预算、性能口径与总体未验边界

14 份 metadata 的 Workspace observed peak 均 **27,396,128 B < 33,554,432 B**；
这只证明该 owner 观测口径。质量分配回执含 output/host 各 1,619,200 B、
16 selectors、6 imported winner images 实际 query=1,572,864 B，
quality admitted GPU=4,942,592 B。不能把 Workspace 或 CPU metadata peak
等同于 WDDM/全进程/跨 World 真峰值。

冻结 QualityGpu 有自己的 timestamps，Frame 的 RecordResolveAndCopy 尚无
GpuPassTimer named entry；**本轮 quality import/resolve/copy 没有被完整纳入
GpuPassSum 正式口径**。这影响正式性能统计完整性，需 Pasteur 的 named-entry
整改与绑定新源码/二进制后复验；14 个 capture 不能签发 steady P99 或性能 PASS。
它不使 raw owner/counts 失效，也不改变本次实际 wall-clock 初始 deadline 的证明。

证据身份保留：

| 文件 | SHA256 |
| --- | --- |
| acceptance-receipt.json | `0c4318b1f3c0ea77ff67e2e485e4a22ec4de0d6d742d0db5d1992e3c7163bd4f` |
| execution.json | `3c8d5bd38e2db7639a613cf021ae9e410706a7ffdfcc8b0e26393da53f53c131` |
| observations.json | `bc7a7fb91d7070bbc3718f3d8fbc47492df03691ddbc9ef182ad68343c813ace` |
| launch.json | `9cd1ed49c5d9c297eed6a6ff5c360beea388719247c8fba93bc2e1c6c407ae9a` |
| stdout.log | `2377a6b3a87b49df16596936f391cca1c3c70cddb8ae174888ea1b88c479fbbb` |

**接受并关闭初始 actual full Observe/quality counts/deadline/黑屏放行/quit 子项**。
这是 ce9c… core+C008、worker2、单初始固定机位的正式有限收据。
第 1–7 节的 167 raw/9 duplicates/86.748 failure/exit2 不改写、不删除、不补造；
第 8 节的 F1 module closure 也不扩大。
后续全 53 poses、两 thread 模式、双权重/Local、steady、WDDM、readonly、
P4 编辑/刷新及其新 native core 仍 pending，**不能因本初始范围接受而宣布整个
P3/P5 PASS**。

## 10. FrameAbandon short3-r2-01：native frame-sync 子项接受

日期：2026-11-21。裁决：**NATIVE FRAME-SYNC MODULE ACCEPTED / CLOSED**。
本节只接受 acquire 后废弃命令、真实恢复 submit/present、下一普通帧 fence 完成。
本次未启 GPU/游戏、未编译、未改实现或 raw，仅只读核对并重跑纯 CPU 状态测试；
不干扰 current R8 fullcore build，也不把尚待 main 排窗的 native World fault 算作通过。

### 实际收据与历史失败

证据：`artifacts/terrain-rvt-production/fault-hooks/short3-r2-01.log`、
`short3-r2-01.stderr.log`、`short3-r2-01.process.json`。
独立解析原日志的五个 JSON 行，与 process.events 逐项相等：
normal-submit-present frame0 → recovery-submit-present frame1
（discardedSceneSubmits=0）→ normal-submit-present frame2 →
final-continuation-fence-completed frame2 → abandonedFrames=1、
continuationFrames=1、validationErrors=0。
stdout/stderr 实际 hash 对上，stderr=0 B，无 VUID/SYNC-HAZARD/WRITE_AFTER_READ/
validation 消息。PID53656、exit0、timedOut=false、
elapsed=1.094810100 s，另 `.log.exit.txt`=0；支持本有限运行没有挂起。

| 身份 | SHA256 |
| --- | --- |
| 实际 argv `artifacts/frame-abandon-cpu/framesubmitabort_short.exe` | `c6a891588084f213125cbe1560c2ff142358c73ae8cd4e620e5c8578ed6d5320` |
| 当前 production `frameAbandonVulkan.cpp`，匹配 R1/R2 process 与 R2 ready | `21659a499a3897bc1eed65302e02d57c82c53f501c979a413ab18f2ef2f50c57` |
| short3-r2-01.log | `9aff37bd62670e03849fc2a15d22b539bae890329bec25e467411a5f2bfbddba` |
| short3-r2-01.process.json | `22e55b16e3e574a1e67e2a940caae8087a478f85ff91415c78909bd10cd8f490` |
| 本次审读 harness source | `a6db2b37c51ba11f25ee14dfa4d02840bc932888c68819aca7bf75b8ea6ae355` |

实际 exe 存在 RecoverAcquiredFrame 定义及 vkQueueSubmit/vkQueuePresentKHR
native import；CMake short target/objects1.rsp 仅列 harness 与 production helper，
VL_FRAME_ABORT_SHORT=1。不是 CPU mock 或完整 FrameRecorder executable。
process+ready 绑定实际 binary/helper hash；这里没有宣称整套 Game 源码清单绑定。

原 `short3-actual-01` **仍是 FAIL**：原 exe=e6d637…、exit1、1.380332900 s，
stdout 只报告 frame1 recovery，stderr 原文件 hash 对上，独立数到两次
WRITE_AFTER_READ acquire→normal transfer-layout-transition hazard。
R2 改的是 harness 普通帧 waitStage 从 TRANSFER 到 ALL_COMMANDS，
且增加 normal/final-fence 事件；production helper hash 未变。
validation layer 仍强制要求存在，仍显式启用 synchronization validation，
error callback 累加计数，cleanup 后仍要求 validationErrors=0；
不是关闭 validation、忽略旧日志或把 R1 重新标 PASS。

### acquire / fence / image layout / epoch 核对

`source/render/backend/frameAbandonVulkan.cpp:5`：
必须 Discarded 且 live device/command/image/view/acquired/finished/fence/extent，
否则拒绝。harness 先等前一 fence、acquire、reset command；explicitdrop frame1
再次 reset **丢弃未提交的 transfer/scene 命令**，标 Discarded，调用同一 helper。

helper 重新录制单采样 color-only 黑清屏 render pass，initialLayout=Undefined、
finalLayout=PresentSrcKHR。Undefined 是有意丢弃 acquired image 旧内容，
不是声称当前布局原来就是 Undefined；render pass 完成实际 color-attachment/
present layout 转换，无需保留被废弃场景的布局更改。
acquire semaphore 在恢复 queue.submit 中以 **ALL_COMMANDS** 等待，覆盖初始
layout transition/clear；submit signals 该 image 的 finished semaphore 和真实
frame fence，present 等 finished，之后 waitForFences 最多 5 s。
局部 framebuffer/renderpass 直到 GPU fence 完成才析构；失败终止路径尝试 idle。

因此恢复是 **有清屏 command buffer 的真实提交**，不是仅 signal fence 的
empty submission，也不能把“fenceSignaledEmptySubmission”当作本 run 的实证。
没有 host 伪造 fence 完成，也没有把废弃 RVT/场景 command buffer 提交。
正常帧也使用 ALL_COMMANDS acquire wait，其 transfer clear 最后 barrier 转
PresentSrcKHR；frame2 之后显式等最终 fence，证明有一次真实普通 native continuation。
该 harness 使用同一 graphics/present queue family；跨 queue/swapchain-out-of-date/
recovery timeout/真实 device-lost 的 native 组合未覆盖。

状态顺序是 Discarded→record→resetFence→submit→RecoverySubmitted→present→Recovered；
submit/present 抛出的真实异常原样传播，不能生成 TaskFrameAborted 成功票据。
当前 backend facade 的 `FrameAbandonAfterAcquire` 返回后才登记
currentFrame+1 的 recovery epoch/retire queue，Frame 随后推进 currentFrame；
正常下一帧必须使用新 epoch。**short3 日志只有 frame0/1/2，没有 facade 的
frameFenceEpochs 数值或 World epoch**；不能把源码接线当成此 run 已验证 backend
epoch/RT mailbox/Game owner 全链。该边界仅由下面 CPU epoch 状态测试补充。

### CPU 阴性状态机与关闭边界

独立重跑已有 `artifacts/frame-abandon-cpu/frame_abandon_cpu_tests.exe`
`--gtest_filter=FrameAbandon.*`：**9/9 PASS、exit0**；未初始化 Vulkan。
审读测试确实拒绝 Recording 直接恢复、重复 dispose/recover、
Submitted 再 abandon；device-lost submit 不调用 present 且保持 Discarded，
present failure 保持 RecoverySubmitted、不声称 Recovered。
另外验证 record/reset/submit/present 顺序、31/32 两个 epoch 分开、
显式 abort 归还 frame budget/下一 mailbox slot、thread-local worker 标志恢复、
默认 missing provider 不 arm。CPU executable SHA256：
`8ead52724d4fbe2cbd5e3e3865642ad0950eeebd3a2de94b9101b2956bf9efcc`。
这是状态机/异常策略测试，不是 native invalid-sync-handle、真实 device-loss
或正在运行的 RenderThread 测试。未发现本限定 helper/short3 路径的新可证 bug。

**只关闭 native framesync 恢复模块。** harness 无 World、无 SPV、无完整
FrameRecorder、RTGameOwner/Service abort 与 commit 观测，既没有 oldWorldUsable
实证，也没有六项 Game fault 矩阵完成证据；R8 build/CPU link 不改变这个限制。
真实 World task token、资源 Abort/无发布、retained old World 后续 scene draw、
facade epochs/RT continuation 必须由之后 main 安排的完整 native Game fault
运行另验。第 1–9 节历史与有限关闭范围保持，P3/P5/P4 总体不扩大。

## 11. R9 Frame / controller / real-frame identity：有限 CPU 验收

日期：2026-11-21。裁决：**R9 DIRAC LOCKED CPU SCOPE ACCEPTED**。
接受 source-locked Frame 身份、查询提交/放弃状态、descriptor retry 分类、
controller ingress 与真实 CPU pool/sink 的有限验证；**不签发 R9 Game fault、
当前 Service 或正式性能 GPU PASS**。本次未启动 GPU、未修改生产源码、
未启动 shared build、未编译 Service；只重跑已有独立 CPU binaries 并追加本报告。

### 身份 / 编译 / 独立复跑

最终收据：
`artifacts/terrain-rvt-production/r9-dirac-ingress-cpu/checkpoint.json`，
SHA256=`ca1ccc83508dbc50e409e50190cf20c3fb5f3070d3df7eb4a614bc371e892c32`。
逐项 hash 当前 **19 个 Dirac 锁定源码/头文件/测试**，全部与 checkpoint 相等，
复验末次检查仍相等。5 个实际 CPU executable 也逐项匹配 checkpoint。
compile-report 中 9 个生产 TU 的 argv/exitCode 与 checkpoint 对上，全部 exit0；
这是保存的独立编译证据，本次未重新编译，更不是统一 Game link。
boundary-audit 原日志末尾明确 passed；未拿编译/门禁当 native fault 运行。

独立复跑，均 exit0：

| 实际 CPU binary | 通过测试数 |
| --- | --- |
| terrain_fault_injection_tests.exe | 34 |
| terrain_fault_slot_tests.exe | 8 |
| frame_identity_cpu_tests.exe | 10 |
| frame_abandon_cpu_tests.exe | 13 |
| frame_evidence_sink_tests.exe | 4 |
| 合计 | **69** |

本节使用新的 ingress 69CPU 收据，不用先前 60CPU checkpoint 给新 selector/action
源码背书。frame sink 测试写自己的临时 CPU fixture，未触碰保存的正式运行 raw。
先前 R2 short3 native 同步子项仍沿用第 10 节，不重跑 GPU。

### real-frame identity / CPU_RAW / GPU_RAW / 质量计时

`gpuFrameIdentity.h` 从冻结 context.currentFrame 与 renderScene.worldGeneration
生成 renderFrame/world/submitEpoch=currentFrame+1。Frame 在 acquire 之后固定身份，
将同一副本传给 CPU payload 与 GpuPassTimer；slot、readbackSequence 是独立字段。
GPU slot identity 将 recording/prepared/pending 分离，只有 backend SubmitFrame
成功返回后的 `FrameSubmitted()` 才使新 batch 可读。读回先要求
completedEpoch≥pending.submitEpoch；Abort 清掉未提交录制，不把它伪装成 GPU row。
独立 identity tests 包含不同 World/帧、801→802 fence 边界、空 batch、重复 submit、
当前 batch abort 与旧 pending 保留，不能用计数器替换 original renderFrame。

CPU_RAW 的原始 interval 在曝光 clamp 之前取 steady-clock 差值；
sampleMonotonicNs 没有在读回时重新制造 arrival。注意其语义：
**record-begin、submitEpochPlanned=true 是计划录制样本，不是成功 scene submit**。
acquire-not-ready 行为 frame-skipped，interval/slot 为空，无 query batch；
重试不会把 acquire 失败推进为正常已提交帧。首个曝光 clock 初始化仍没有完整
CPU_RAW interval；正式逐帧样本统计须尊重初始化/缺样及计划样本状态。

GPU_RAW 写原 submitted identity，单独记录 readbackSequence/fenceCompletedEpoch；
RuntimeEvidence sink 与兼容 stdout 都使用同一身份序列化。CPU sink 测试用
明确标注的 synthetic CPU metadata 验 lossless uint64、未 clamp interval、
stdout 被抑制时仍落盘、close 后写入失败。这是**实际 CPU 文件写入**，
不是本次真实 GPU timestamp 数值证据。

质量计时口径源码整改存在：Frame 先结束 terrainFeedback 的 named timer，
再用独立 `terrainQualityAfterFeedback` 包住整个 RecordAfterFeedback，
其中 winner import/resolve/copy 均落在该区间；原 feedback 的 MRT winner 写入
仍包含在 terrainFeedback 项中。该新项进入相同 pending names/durations/
timingRing；快照 RecomputePerformanceSnapshotTotals 汇总所有已测项，
没有把质量段排除在 GpuPassSum 外。
因此第 9 节的遗漏已有**source/compile 边界整改**，尚无 R9 实际 GPU_RAW/
正式 steady/P99 的独立闭环，不回改 ce9c… 原性能口径或签发性能 PASS。

### descriptor 异常与真实 skipped/aborted

`RunWorldDescriptorOperation` 只将显式 admission retry、std::bad_alloc，
以及 Vulkan host/device/pool-memory、fragmented-pool 四类结果转成 typed retry；
DeviceLost、structural runtime/logic error 和未知异常继续原样 throw。
独立 CPU 负例确认 DeviceLost 不转成可恢复事件，原 VkResult/binding identity 保留。

Frame 在被保护 pre-submit 区域检查 retry 的 acquired image/operation，
先 DiscardFrameCommands/AbortFrame，再让 Terrain/Quality/Service RAII 展开，
最后恢复 acquire 同步。恢复成功后记录 frameSkipped/recovery epoch，
不走正常 SubmitFrame、FrameSubmitted、Service Commit、submittedFrameSerial
或 post-normal-scene witness。RT 只捕获显式 TaskFrameAborted/
DescriptorFrameSkipped，普通异常仍 StoreFatalError，不泛化吞掉 DeviceLost。

descriptor 后续 witness 不是“world ID 相等就成功”：须真实正常 scene submit 后
读取当 image 的 World-consuming pass snapshot，全项 identity 一致且有实际
binding owner，才记 renderedBindingWorld/revision；随后 completed fence 才输出
descriptor-retry-frame-completed。没有正常提交/owner 时保持 unobserved，另有
timeout/superseded 事件。CPU 验证“completed epoch 很大但尚无 submitted witness”
也不能完成；actual rendered revision 与 requested revision 分字段保留。
正式 CPU 统计必须把 record-begin planned row 与 FRAME_SKIPPED/
submit-abort-observed 按真实身份关联，不能仅凭 event=frame-timing 计作正常成功帧。

### selector / SlotArm-Pump-Cancel / action owner

SubmitFail ingress 不构造伪 token：Queue 只收显式 World/page/revision selector，
要求 live registration；ConsumeBeforeSubmit 只有实际 owner observation 精确匹配
World/page/rev 且 native token generation/content/epoch 合法才绑定 pendingSubmit，
发一次 abortTicket。exact QueueSubmit 路径保留；重复 consume、错误 World/page/
revision/token、取消的 selector 均被 CPU 负例拒绝。
Frame 还要求实际 ownerLease/observeAfterAbort，并校验回传 task identity；
注入结果记 injectedSubmitResult，rawSubmitResult=null，不把它称作真实 Vulkan
返回错误。ObserveSubmitAbort 保留原计数，包括失败计数，不伪造 published=0。

Slot callback 由实际 Runtime owner提供。Pump 在串行边界先检查 slot/reservation/
published token/writeEpoch；charged arm 失败记录 admission-rejected，不称成功。
armed 时必须能 poll，原 reservation/hash 不得换；cancel/timeout 先调用实际
hooks.cancel 再清控制请求，teardown 同样释放 watch；replacementObserved 才记录
replacement-observed，但具体安全条件仍由真实 pool diagnostics 字段证明。

原实际 CPU sink `slot-watch-20261121T092546300.jsonl` hash
`a237f7d70f67ccda64cbc4ca0a6d774773807cdc745a2fa957c2d1fb7a9185cd`
与 checkpoint 对上，15 行 sequence 连续，terminal incomplete=false。
独立解析：原 reservation1；中间 blockedByEpoch=1、blockedByOwner=1；
releaseCompletedEpoch=retire=reference=12 后，replacement reservation3、
releasedAtBoundary/replacementObserved=true。另有 identity/admission rejection、
cancel、timeout、owner-teardown；取消/拒绝未产生 replacement-success。
对应独立 CPU process receipt exit0/GPUStarted=false；不能把这些 CPU fixture
World7/epoch12 当 Game fault 原始数据。

Budget/Stale action 只提供 ReadAction/ConsumeAction/FinishAction 的有界控制：
精确 owner kind/World，stale exact page/rev、单次 consume；cancel/timeout 不静默
删除已持有动作身份，等待串行 owner 实际清理后 FinishAction。
FinishAction/queued 本身无 success/pass；controller 不修改冻结 cap、不重写 stale
task token、不持 Vulkan owner。严格更小的 budget test limit 与实际 candidate
cost、stale 真任务持有/新 revision 联合提交/旧结果拒绝仍属于 Service 原生证据。
本次 controller CPU 通过不代替这些 Service/GPU 效果验收。

### main / Euclid 集成边界与总体裁决

**不验收正在修改的 Service。** checkpoint 的 Service hash/callsite counts
明确只是 source inspection（bb259…），不是编译/执行源锁。
审计期间 live Service 从早先 SetWorldGeneration 注册四 hook、Runtime 后加 slot
的版本，更新为 RegisterActiveFaultHooks/ResetFaultHooks 的 active 生命周期版本。
旧版本每 Service 5 registrations、两重叠 Service 共10>MaxOwners8，
且失败返回0不处理的缺口已即时提醒 main；不能把该旧反例继续称为最新稳定源码 bug。
最新观察的 active 注册实现仍须 Euclid/main 冻结后验证：候选/旧 World 切换顺序、
所有注册结果非零、退役/失败 owner 清理、取消/超时 held leases 真释放。
**不要增大 MaxOwners=8 或 pending=1，也不要以缺 hook 的 pendinghookmissing 伪造通过。**

Dirac 19 项锁定源码与本次 69CPU 未发现新的可证缺陷，故接受其有限 CPU 范围。
Service E005 Undo owner 泄漏及其修复不由本审计代修/关闭；统一 native R9/R8
完整 build/link、真正 Game fault 六项、retained old World scene 后续可用、
actual raw timings/sum/fence 与 owner 峰值仍 pending。
历史初始放行与 R2 native recovery 的有限通过保留，P3/P5/P4 不整体扩大。

## 12. R9 capture lifecycle close：有限 CPU / 调用边界接受

日期：2026-11-21。裁决：**CAPTURE LIFECYCLE LOCKED CPU SCOPE ACCEPTED**。
接受末尾 capture 的 completed/aborted 分离、原 identity 保留、正常 shutdown/
替换调用边界与 fail-closed footer；**真正新 R9 Game 端到端仍 pending**。
本次未启动 GPU/游戏、未改生产源码、未启动 shared build，也没有编译共享 core。
仅只读源码/保存的原生 object 与 XML，重跑已有私有 CPU executable，追加本报告。

### 源锁与独立 CPU

以下六个生产文件的当前 SHA256 与
`artifacts/terrain-rvt-production/core-native-r9-owner-fix-final/source-before.json`
及该目录对应源码副本全部一致；测试源码另单列，不谎称它包含在生产 source-before：

| 文件 | SHA256 |
| --- | --- |
| source/core/runtimeEvidence.h | `cf1797e9d06f8e1489ac63d05e69f5a79dff52f5491241cfbbca57b1c00e02ac` |
| source/core/runtimeEvidence.cpp | `6074315ce45a0f558dc07f41ed0d8dc6a0a7f5537da91f20243a69a8e12718f0` |
| source/render/frame/terrainCompositedCaptureLifecycle.h | `1e9aeae1f90799b12486393606867d7f29d05aa59b96bab717f2a28d8fd01384` |
| source/render/frame/terrainCompositedQualityFrame.h | `25187a44d108390020007c53a6209c276c3cf81081993e75d1deec4f882925d7` |
| source/render/frame/terrainCompositedQualityFrame.cpp | `360587e7c8360a85af8f139cf81eed35e9db5884c45f83f4f1dfaea301524269` |
| source/renderSystem.cpp | `3636dfe832278c0e697fd03c2e3bc1c630dca0c01eed54cfc2967e9eaa6f298c` |
| tool/terrain-tests/runtimeEvidenceTests.cpp | `7e6eeaa17c0ac359eee47cb15930e4d3af49a7774a9ce1e7365fc6362382aba5` |

私有目录：
`%TEMP%/vl-evidence-cpu-0b5556620f3e44cab10844c1abf491e2`。
实际 `runtime_evidence_tests.exe` SHA256
`4d30017d59d5ac2727d34e477954ec40afda90cded7c268fae480fc2a41141ee`。
独立执行 `--gtest_filter=RuntimeEvidenceTest.CaptureLifecycle*`：**9/9 PASS、exit0**；
再执行完整 suite：**32/32 PASS、exit0**。私有测试只操作临时 CPU 文件。
保存的 results.xml 是旧 23/0，lifecycle-results.xml 是新 32/0，未把旧 XML
说成含新增九项。实际两个原生 object 均可读为 pe-bigobj-x86-64：

- terrainCompositedQualityFrame.obj：
  `afeb5f7f4ad5361b85b28235ff7c1936c3c6a6733c2ad1856d2b3ab79977fd60`。
- renderSystem.obj：
  `df0cda652510a362bb7c5a7e156c238063032035b53732484fa52d8eed049611`。

这支持已有 QualityFrame/RenderSystem 编译对象及 CPU 结果，不升级为本次
重新编译、统一 core link、真实 State/GPU readback 或实际 Game 退出收据。

### completed、aborted、未提交身份

Lifecycle.Recorded 保存实际 handle/world/pose/frame/drawEpoch，单 pending；
Submitted 要求相同 handle 且实际 epoch==drawEpoch，才记录 submissionEpoch。
QualityFrame 先成功调用 QualityGpu::Submitted 后才推进 lifecycle，不能将
仅录制计划当成 actual submitted capture。

Completed 必须逐项匹配原 handle/world/pose/frame/submissionEpoch，
且 completedEpoch≥submissionEpoch；其唯一生产调用来自
`QualityFrame::State::Consume` 的实际 `quality->TryConsume` 返回对象。
TryConsume 本身要求真实提交已完成、timestamp query 可读、raw ABI/identity
复核通过。Completed 的 measurementProvided=true，不是 shutdown 时用最后一份
旧测量填补，也不是 World/pose 变化后换一个 identity 冒充原 capture。

Close 只生成 capture_aborted，measurementProvided=false、明确 reason，
保留原 world/pose/frame/handle/drawSubmissionEpoch。
未提交的 capture 保留 submissionEpoch=0、submitted=false；**不能用 drawEpoch
伪配一对 capture_submitted/completed**。无 pending 或已经消费的 lifecycle
Close 返回 false，不造终结事件；重复 close 不重发，旧 identity 不能完成新 pending。
Emit 失败时 pending 不清空，便于 fail-closed，而不是丢掉无法落盘的 identity。

新增九项负例覆盖空 owner、unsubmitted abort、World replacement、swapchain、
错误/重复/unfenced completion、emit failure、closure 写失败与 disabled sink。
其中“八份真 completion + 末份 submitted 仅 aborted”测试明确断言 completed=8，
尾 aborted 不含 completedEpoch，footer 在 aborted **之后**。
这只证明清理协议闭合，不把 aborted 当作第九份质量测量通过。

### Stop / WaitIdle / WorldGraphSwap / frameAbort / shutdown

- 正常 shutdown：EngineLoop 先 Stop/join RT、backend WaitIdle，随后
  RenderSystem::ShutdownRenderObject 调用 CloseAfterDeviceIdle，再清 graph/
  World/cache/retire queue。ApplicationScope.Finish 在 RunApplicationBody 的
  EngineLoop shutdown 返回后才关闭 sink、写 footer。
- resize：既有窗口生命周期先 WaitForEngineRenderThreadIdle、WaitIdle；
  ReleaseSwapchainDependentResources 入口以 reason=swapchain_teardown 关闭。
  不把普通帧 pending 以“resize 即将发生”为由提前当成完成。
- CloseAfterDeviceIdle 只在这个已 idle 前提下尝试 Consume(lastEpoch)，
  消费不到真实 readback 则中止而非强造 completed；之后释放完成 lifetime，
  对仍 pending quality 可 reset。它自身**不执行 WaitIdle**，调用方必须遵守前提。
- 普通 World/graph replacement：事务在 RT safe point swap，旧 World/graph/
  package 进入正常退休队列；Quality BeginFrame 对旧 session 先尝试以真实
  completedEpoch 消费，再以 world_replaced/graph_replaced 关闭。
  该分支只清 **证据 lifecycle**，不 reset QualityGpu；在途 snapshot/Impl/
  depth/winner owners 仍由 frameLifetimes 和退休 State 强引用保活至完成 epoch。
- 切 offline、World unavailable、failed session 同样只逻辑中止；不以缺 World
  作为 GPU 已完成的证据。若 swap 后立即 quit、没有下一 BeginFrame，shutdown
  仍从旧 State 保存的 identity 终结，不用当前空 World 或新 pose 覆盖它。
- frameAbort：先 discard 未提交 command，再让 RAII QualityFrame::Abort 调
  QualityGpu::Abort（明确拒绝 submitted capture），关闭为 frame_aborted。
  已 submitted 的 capture 必须等 fence/idle 退休，不能走这条早释放分支。

这是**源码调用链审阅**，新增 CPU suite 未实际运行完整 QualityFrame、
RenderSystem、WorldGraphSwap、RT Stop 或 Vulkan owner。正常 lifetime 安全性
不能被称作已做新 GPU regression；异常 driver/退出交错仍需新 Game 收据。

### footer、析构与范围

State 析构发现未闭 pending 时只调用 RuntimeEvidence::MarkIncomplete，
该函数为 noexcept atomic sticky flag，不做 JSON 分配/文件写入、不从析构抛出。
正常 explicit close 的 Record 发生在 footer 前；closure/Consume 写出失败
会 MarkIncomplete，保留 pending 并传播到显式调用方，不能静默作为 exit0 PASS。
Abort 的 noexcept 路径捕获异常并 mark，不把异常漏到 transaction 析构。
全局 ApplicationScope.Finish 检查 sticky incomplete，原 exit0 转为非零并写
incomplete footer；晚析构 mark 是失败兜底，不是允许把终结事件写到 footer 后。
局部 Sink 测试还验证真实文件/flush/close 失败不能报告成功。

R8 tour b01 末后台 capture 未 completion/abort 的既有 whole-failed 事实保留；
本次未重跑/改写其 raw，也未独立重新解析 b01 的全 tour 输出。
本修复给出了末 pending **没有测量时显式 aborted** 的正确清理通道，
不补齐任何正式 pose 所需测量。新的统一 R9 Game 必须另验：
每个实际 submitted capture 恰有一个真实 completed 或无测量 aborted，
所有 record 都在 footer 前、末 owner 不提前释放、footer 完整性及实际退出。
正式 tour 所需质量 measurements/53 poses 缺失不能靠 cleanup closure 变 PASS。

本限定源码/CPU 范围未发现新的可证 bug，给予有限接受。
真正新 Game 端到端、pending GPU 退出/World swap 回归及 P3/P5/P4 整体验收继续
pending；不碰 Euclid Service、source-before/shared core 锁或正在安排的 GPU 窗口。

## 13. R11 actual-draw capture 推广：有限安全审阅与写出缺陷

日期：2026-11-21。裁决：**有限 Frame lifecycle CPU 安全范围接受；完整 capture
证据验收不关闭**。发现下述 P2 metadata 写出缺陷，已立即提醒 main。
本次未启 GPU、未编译 shared core、未改生产/工具 capture 实现；仅只读审阅、
复跑已有私有 CPU lifecycle 测试、在 `%TEMP%` 新建一个无 GPU 的小型 I/O 反例，
追加本报告。未重测 69CPU 或 legacy R2。

### 13.1 可证 P2：native metadata 写失败仍置 complete

文件：`tool/validation/terrain/native-capture/nativeTerrainCaptureAdapter.cpp:250`。
ConsumeCompleted 调用 `std::ofstream(metadataPath) << metadata.dump(2)`，
没有检查 open/write/flush/close 状态或开启 stream exceptions，随后 `:252`
设置 complete=true、`:253` 开始释放 capture/witness/material/pipeline owners。
因此 **metadata 没有实际成为有效文件，也可能走到“完成”分支**。
raw writer 的检查不能替代这第二个文件的检查，stdout 中有 JSON 也不能替代
要求落盘的 `.native.json`。

私有 CPU 反例：
`%TEMP%/terrain-capture-safety-metadata-probe.cpp`。
模拟同一 exists-check/write 模式：初始 raw/metadata 都不存在，通过初始检查；
写出 CPU raw fixture 后，在 metadata open 前把目标建立为目录（受控模拟
check→open 间的文件系统变化）。执行相同 ofstream 表达式，不启用 exceptions，
随后设置 complete。实际输出：

```json
{"scope":"CPU metadata writer expression only; no GPU","bothAbsentAtInitialCheck":true,"complete":true,"threw":false,"metadataRegularFile":false,"counterexampleConfirmed":true}
```

反例编译/执行均 exit0（表示反例确认），不是 Game/GPU PASS。
这是 writer 表达式/控制流的实际 CPU 反例，**未声称运行了整个 native adapter
或真实 GPU readback**；初始 exists guard 本身不能保证随后 I/O 成功。
源码 SHA256=`73c00ff5b7d72ae30bd67ce1b15cf6145ddb87e675b661663a7a65baa925a1c0`，
exe=`c9c667c27639d213b70b521a91146db17b49e98883b4a0ca75519bdd9a96f11d`。

给 main/Avicenna 的精确关闭条件：检查 metadata 的 open/write/flush/close，
在输出成功前不得置 complete/丢弃重试所需证据状态；失败必须可见、不能当完整
capture 成功。整改后更新工具源锁及其反例收据，再进行正式 TES/scene-lit 验收。
最终 runner 若检查文件缺失，仍可能拒绝整次运行；本发现不声称已经发生
“整个 Game 伪 PASS”，但不能据当前 adapter 完成标志接受完整证据协议。

### 13.2 推广身份与动态源码边界

推广 receipt：
`artifacts/terrain-rvt-production/r11-capture-promotion/receipt.json`，
SHA256=`80183e5989aa96f176b3938e29e72728851d1176653741a9bd57cd127124a651`。
两份实际 patch hash 对上，receipt 明确普通 apply_patch 推广、非 object overlay。
私有来源使用真实路径 `r10-actual-draw-private-r3/final-handoff.json` 和
`r10-dirac-frame-private/checkpoint.json`，不是把 R10 compile 收据当 R11 执行收据。

receipt 的 15 个推广文件中，审阅时 13 个仍匹配 after hash；两项已继续变化：

| live 文件 | 审阅 SHA256（不是原 receipt after） |
| --- | --- |
| source/render/frame/renderFrameRecorder.cpp | `733bf62edf4be69f6e9ef5e5044240b00b3fb3c3a415753ff665f05a8aa51798` |
| source/render/frontend/rendererFrontend.cpp | `201c912ab98a2d6ed5d17e21039427906ae3f12e08e9bbb8f0d62e9b94861378` |

明确以本次读到的 live 调用链做源码审阅，不说这些新版本已由旧 private TU
receipt 编译/运行。FrameLifecycle header、bridge、source-owned interface 等
仍匹配推广 after；上述有缺陷 adapter 仍匹配 final-tool-source-lock 的
`86d8923db58a2ab89275c5e9d8255d7c5c471bba03e91dba219121da1b0cd5cb`。
不冻结/改动 Euclid 正在接入的后续 probe 或统一 source-before。

### 13.3 默认路径、device features 与 runtime/tool 边界

source-owned hooks 默认为空；普通 Frame 只查询空注册指针，不创建 bridge，
不读 capture case/manifest/shader 文件。live frontend 已将 terrain lodGroupId/
lodLevel 复制限定到有 hook 的情况，普通 default-null 路径不因此复制额外 LOD
字符串。Draw 中空 inputs/shared_ptr 为栈上空对象；真正冻结 scene/owners、
编译 diagnostic shader、读验证文件都在显式 observer 分支或薄 NativeGame 中。

审查 capture 范围源码 include 未见 Runtime include `tool/native-capture`：
Runtime 依赖 `source/render/diagnostics/terrainCaptureHooks.*`，
外部 adapter 由薄 NativeGame 显式注册；不是 source 自动扫描工具或环境文件。
注册约定为 device 创建前的一次串行配置，本节不证明并发注册或任意 observer
热替换安全。

VulkanManager 只在注册 observer RequiresTesVertexStores=true 时额外请求
vertexPipelineStoresAndAtomics，并先检查物理支持、TES、maxBoundDescriptorSets≥5；
普通路径不因此开启该 feature。GraphicsPipeline 只在显式诊断启用且真实
device feature 标记存在时允许 set4/binding0、单 storage buffer、
TES-evaluation stage、准确 capture 名称；其它越界 binding 拒绝。
本节没有声称所有设备均支持诊断，缺能力应拒绝而非暗降级。

### 13.4 lifecycle 与跨 World owner：17 项 CPU 复跑

独立校验并运行
`r10-dirac-frame-private/frame-lifecycle-tests.exe`：
SHA256=`f37b8f42dd119bfcb302b5155b275ec261df7b4bb001bd073eea3181107fd245`，
**17/17 PASS、exit0**，对应 XML/CPU receipt hash 也匹配。
测试是 controlled hooks 加真实 ResourceRetireQueue，不是 Vulkan/完整 Frame GPU 运行。

Prepare 将实际 frameIndex/epoch、hook、immutable frame/graph owner 放入固定 8
flight 槽。BeforeGeometry/AfterGeometry/SubmitAttempted/Submitted 顺序受检查；
只有 backend phase=Submitted 且 exact epoch 匹配才 accepted。
已知未提交必须先 Discard，AbortProvenUnsubmitted 才调用 AbortUnsubmitted；
submitAttempted 即使未确认 accepted 也不可推断“未提交”，不会提前清 storage。
submit/present/回调异常保持 fatal，不以空成功回调替代真实后端结果。

私有负例覆盖 ambiguous queue result、accepted 后 present 抛错、重复回调、
wrong phase/epoch、slot premature reuse、Consume failure 与 confirmed-idle close。
当前 Frame bridge 提交前将 Flight 入退休队列；slot 也保留 owners，
不因 DetachCurrent 丢掉在途存储。Complete 只对 accepted 且 fence epoch 满足
的 flight 调 hook；读取/解码失败继续保活并传播。Idle close 才允许释放
ambiguous/未完成 storage，不把它记成 measurement。

FrozenCaptureFrame 保留实际 terrain packet 索引、camera/resolved draws、
actual Service 页表 snapshot；FrozenCaptureGraph 持 graph leaf、实际 geometry
World descriptor snapshot、present owner。World/graph swap 的其它资源继续依赖
既有正常 epoch retirement，不把这组 leaf 冒称整个 graph 的副本。
跨 World 旧 descriptor snapshot/输入/实际 draw resource 的强引用不因新 World
发布而提前释放；后续 UBO/资源复用仍须遵守正常 frame-slot fence 规则。

RenderSystem shutdown/resize 已接 FrameRecorder close；正常 EngineLoop Stop RT、
WaitIdle 后调用，ApplicationScope footer 在其后。CloseAfterDeviceIdle 本身
没有代调用 WaitIdle；异常退出/多个 observer/新 probe 组合仍需正式端到端回归。

### 13.5 sets0..3、实际 UBO/VB/IB/LOD、set4 恢复

实际 geometry draw 的 MaterialInstance UBO/Object UBO 更新保持原位置。
BuildTerrainCaptureDrawInputs 从被选 resolved draw 的 object descriptors 取
sets0..2、从实际 pass 取 set3；保留 mask 和实际 image/动态 frame-slot。
diagnostic layout 从原 pipeline layout 拷贝前四 sets，并检查反射类型/count/
stage 兼容；只追加 set4。DrawExecutor 切诊断 pipeline 时重绑实际 sets0..3，
hook 只绑定诊断 set4；记录完同一次 Draw 后恢复原 pipeline 与前四 sets。
normal pipeline 不消费残留 set4，下一诊断 draw 必须重新绑定它。

RenderableObject::DescribeDrawBindings 与 Draw 共用同一解析规则：
实际动态 slot VB/offset、instance VB、patch 或真实 IB/indexCount、instanceCount。
真实 Draw 只调用一次 drawIndexed；不是另画参考 mesh 替代当前 draw。
adapter 将实际 LOD0、PATCH_LIST、primitive interval、drawPacketIndex、buffers/
offset/count、sets/mask 与正常 pipeline 逐项比对；重复匹配 draw 拒绝。
GT RenderScene.frameIndex 不被拿来要求等于 RT submission frame，
避免 worker2 的合法双域差异；witness 使用实际 frame/epoch。
这些是 source/CPU contract 检查，不是已验证的 TES GPU 记录或 scene-lit ROI。

### 13.6 VkReq 预算与结论边界

TES helper 在分配前查询对应 usages 的 device/host buffer memoryRequirements.size，
检查 storage range、capacity1..4096、descriptor/CPU metadata cost；
向实际 World Workspace/outer owner TryReserve，host 对齐量也进入 CPU 账。
不能用 payload bytes 代替 VkReq，不能增加32MiB Workspace cap。
reference atlas API 查询真实 image/staging VkReq，限制 pool≤224MiB、
Workspace≤32MiB，使用真实 child/outer lease；缺额度拒绝而非提高 caps。
本节只审这些分配入口，不声称编译器/全部 driver transient 的实测峰值通过。

因此给予**有限默认路径/Frame lifecycle CPU 安全接受**；P2 metadata 写出问题
在完整 capture 证据验收前必须关闭。真正新统一 R11 build/link、首次非空有限
TES sample/独立 R16 oracle、scene-lit reference 同 epoch/ROI、实际跨 World/退出
及 full53/modes/workers/dynamics 继续 pending。既有章节历史不改写，不代修实现。

## 14. Quality CPU staging 候选：有限语义 / 寿命 / 准入接受

日期：2026-11-21。裁决：**CPU STAGING CANDIDATE LIMITED SCOPE ACCEPTED**。
未发现该单 CPP 候选在当前 Frame→WriteReadback→Service::ObserveQuality 的
shared-readback 调用链中新增的可证 blocker。**真实 R13 Vulkan mapped memory
性能、实际 Game 和组合 priority-root 修复效果不据此通过**。
本次不修改生产源码、public ABI 或 raw，不启 GPU/shared build；
只读私有 patch/当前推广，运行最小已有 CPU 测试和一个私有 CPU 寿命 probe。

### 身份与严格单文件范围

实际资料路径为：
`artifacts/terrain-rvt-production/quality-cpu-cost-investigation-r1`。
HANDOFF 和 final-handoff-sha256 清单逐项 hash 匹配；早期
build-output-sha256 是先前版本，不用其中旧 patch hash 替代最终源锁。

| 对象 | SHA256 |
| --- | --- |
| 最终 cpu-staging-private.patch | `7b617bc329e7b9b5c3b956d4d7296a64752d79ff6b0e71aec801c5ee3d247253` |
| 完整候选 QualityGpu.cpp | `604fcaa85a0987100d4176485400c0f6c2533a57f10cfbde749b4e65a45ca11a` |
| 当前推广 QualityGpu.cpp（整文件等于候选，复验末仍相等） | `604fcaa85a0987100d4176485400c0f6c2533a57f10cfbde749b4e65a45ca11a` |
| candidate-writer-tests.exe | `32616573d42c43713c3221b4984809669afdb28997a2a8c7827c53e8e746a980` |
| budget-tests.exe | `67aa2c549ddbea423cbc060d3df179b83cde07d7391a2630e23a643ef0ebc5a5` |

patch 只改 cpp-defined Impl、构造准入后的 resize、CPU quote、TryConsume 的一次
memcpy/words 地址、WriteReadback owned-pointer guard。
与冻结 `core-native-r11-capture-probe-r4` 比较，public QualityGpu.h
仍为 `b959d2a650fabb5d8e2441923be8a84543c6211173a1b17ace49ed7e700abae1`，
QualityData.h 仍为
`f9beb42f7789fe30b01b7959d36e303068382a6bc7780763d1d1cca103c559b7`。
readback struct / 函数签名 / ABI version2 / 1,619,200 B payload、
sample/result/owner/surface/duplicate appendix、raw identity 和门槛均未改。
不是 cached-count shortcut，也没有删除第二次独立 Recompute。

### fenced copy、cached words 与实际消费寿命

Impl.cpuReadback 是构造时一次性 resize 的 uint32_t vector，
元素数=404800。TryConsume 仍先要求实际 submittedEpoch、completedEpoch 到达，
时间合法且 timestamp query 可读，随后才 memcpy(mapped→cpuReadback，精确
1,619,200 B)。GPU buffer/host-coherent mapping/copy/barrier/queue/fence 顺序不变。
第一遍 Recompute、header/epoch/world/pose 核对、导出、Service 的第二遍 Recompute
均读取 result.words 指向的 heap buffer，不再散读 mapped。

正常 Frame Consume 的 `result` 是返回的 shared readback，整个 export、同步
ObserveQuality、Validation、lifecycle completion 期间都保持强引用。
Record 的 snapshot/outstanding/global capture-gate 规则未改；
outstanding 未 expired 时拒绝下次录制，故不会异步复用同一 heap 做新 capture。
readback 还持有原 captureGate，保持全局 gate，阻止其它 QualityGpu 实例并发占用。
成功 TryConsume 又清 submittedEpoch，不能反复消费同一提交。
readback.storage 强持 Impl，使 facade/Frame quality 对象释放后 backing vector
和预算 lease 仍活至最后 readback owner 释放。

这里区分两种保证：**storage 保证内存寿命；原 returned shared readback 的
outstanding guard 保证内容不被下一 capture 覆写。** 本节不扩展原 API 为
“任意用户自行复制的 readback 值/裸 words 指针只持 Impl 即可长期稳定”。
生产 Frame 已核对保持原 shared result；任意并发/脱离该约定的 reader 未验收。

异常路径仍保留原 fail-closed 行为：decode/export 出错不发布新 observation，
Frame 标 failed/incomplete，pending capture 由原 lifecycle aborted/idle 规则关闭；
WriteReadback 流错误保持本实例及全局 exportFailed latch。
constructor 在 reserve 后 resize，若分配抛错由构造成员/Impl 释放 lease；
没有把未完成 memcpy 或失败解析标为 completed。footer/Abort 协议未被 patch 改写。

### 最小独立 CPU 复跑：不重做 242 落盘

用新 `%TEMP%/quality-staging-independent-parity-cbcc57fcfed34bf5a715d2f8d1e4f646`
目录执行 candidate-writer-tests 的 **parity-only**：

- **85/85** 实际输入 raw 的 staged counts、binary bytes、metadata strings 与
  原 writer 一致；两者使用相同 supplied accounting inputs。
- **14/14** baseline/candidate guard 通过：foreign storage、wrong pointer、
  incomplete epoch、duplicate sequence、capture cap、byte cap 拒绝而不新增 latch；
  failing output stream 拒绝并保持 latch。
- 输出 exports 目录 **0 文件**，本次 mutation=0；不重跑旧 242 disk exports/
  6800 mutations。新小型报告 SHA256
  `35a39521bec33939a40e7de5bf7e096e42a0e1158e756075009d96299b43c2c1`。

已有私有 writer harness 直接包含冻结原/candidate writer bodies和真实 Recompute，
但 backend getter/metadata 是 CPU fixture，不是完整 QualityGpu Vulkan 对象。
85 inputs 不等于 85 native Service pairs；原收据仅 84 个 native counts 可比，
缺 native quality-state 的一个不能补造为 native parity。

另加并运行 `%TEMP%/quality-staging-lifetime-audit.cpp`，使用实际 public
TerrainCompositedQualityReadback/Abi::Recompute，模拟相同 vector/weak-outstanding/
storage pattern（**不是调用真实 Vulkan TryConsume**）。六项均 true、exit0：
MinGW vector capacity bytes 精确等于 payload；原 shared readback 在时拒绝复用；
facade owner 释放后 Impl 仍活；words pointer 稳定；第二解析不受已复制源代理改变
影响；最后 readback 释放才析构 owner。
probe source SHA256
`db27a1b2ea95e8983e0b52f480aa70056a28dfc6626c524f63282f1c60d556d9`，
exe=`412e37ceddbf76ecc99c79194667071dbd93ec508082edbe6f94b54d01fe90e6`。
不把标准库 CPU pattern 测试称为实际 mapped buffer/fence 回归。

### 真实 BudgetOwner 准入与不变预算

独立执行已有 budget-tests.exe，链接真实冻结 TerrainRvtBudget 实现，exit0：
新增 **1,619,200 CPU bytes** 进入 Workspace candidate lease；
原 quote CPU=1,881,344 → **3,500,544 B**，并保留每 lease 256 B 元数据记账。
先 reserve 成功才 resize，private Impl 增大的 vector 对象本身仍由 sizeof-based
metadata admission 覆盖。observed MinGW capacity 没有额外 vector 预留增长；
本节不签发任意其它标准库 allocator 或整进程精确 heap 高水位。

total CPU / Workspace CPU / Workspace GPU 各少一字节时实际拒绝；
Active→Retiring→销毁后所有 charges 回零。GPU allocation/quote 没增加。
27,396,128 B GPU Workspace 与32MiB cap 的旧参考差值仍是6,158,304 B；
不是把 GPU headroom转作CPU额度，更不是本次实测 R13 peak。
真实 post-integration CPU/owner overlap/进程峰值另验。

### 性能和组合 R13 的边界

缓存内存下两遍 parse+export 约4ms、PAGE_NOCACHE/WRITECOMBINE 代理约223→11ms
属于 Feynman 保存的私有 CPU 测量，本次没有重跑性能批量。
它们不是实际 Vulkan memoryType/cache 属性或 live TryConsume 性能证明；
cached/write-back 已显示没有明确收益，不能预填正式 Game speedup。

当前推广单文件与最终候选相同，故接受上述有限语义/寿命/准入候选。
Copernicus 正常构建中结合的 production priority-root 修复不属于本节审查；
后续 R13 必须绑定新的 combined source/binary，实测 copy/parse/export、原始
counts、真实 Service/current identity、CPU/Workspace peak及正常失败/退出生命周期。
不能把组合改善全部归因于 staging。无新 scoped blocker，不持有 GPU 等待，
不宣布 actual Game / live perf / Full53 / P3/P5 通过。
