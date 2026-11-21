# 地形 RVT 实施与独立验收台账

> 状态：实施进行中，未完成交付。2026-11-21。本文是执行记录，不是当前实现契约。

执行范围以 [按需生产方案](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)
为准：P0–P5 全部完成，P6 给出评估结论。完整 Full16 飞马岛、独立离线 Game、
视觉与性能证据均属于交付，接口、单页测试或文档不能替代阶段完成。

## 初始状态与保留边界

- 复用 `terrain-openworld/VulkanLearn`，分支 `codex/terrain-openworld`，
  初始 HEAD `5df9fbc0af122581260d614b3b9f35d8a4d8bacd`。
- 复用兄弟 `VukanLearnResources`，分支 `codex/terrain-openworld-assets`，
  初始 HEAD `e665a40fca8833252e72c9e5f92063ff331ed374`。
- 代码已有大量 tracked/untracked 改动；资源 `VT/` 未跟踪，
  `Generated/` 被忽略。未提交状态和忽略资源均不能被当作可丢弃内容。
- 初始 tracked patch、status、untracked 清单位于
  `artifacts/terrain-rvt-production/initial-state/`；
  清单中 117 项现存未跟踪代码文件另保存于其中的 `untracked-code/`。
  该目录不是全部资源数据的备份。
- 初始文档门禁运行通过，原始日志为
  `artifacts/terrain-rvt-production/initial-state/docs-validation.log`。
- 既有 Full16 是 V1 完整页发布；已有 Game 读取路径不具备缺页生产。
  不将历史完整页 Cook 或单页 compute 收据计为本轮 RVT 完成。

## 阶段状态

### 最新整合检查点（历史记录仍保留）

2026-11-21 接续核对：P0 冻结文件及门槛未修改，P6 不采用压缩的结论不变。
下列子项已有独立复核，但 P1–P5 整阶段均未关闭：

- P3 双模式真实输入压力共 10,936 页，production/copy/combined 无冻结上限违例；
  实际 GPU 原评价器对照共 384 页，三通道最大误差 `[0,0,1]`。
  证据见 `p3-real10936/runs/run-001` 与 `p3-real-numeric-c008/runs/`。
- 初始质量实测 3271 可见样本、3269 合格、完整性 true，解除黑屏约 32.95 秒；
  此有限初始视角通过不代表全岛巡游、TES 或正式性能通过。
- C009a Coverage 安装包 worker 1/2 及只读 worker 2 初始启动已独立复核；
  L009 Local 安装包 worker 1/2 初始启动已独立复核。两包仍需最终源码重导出，
  Local 只读、完整文件读取轨迹、全岛视觉与性能尚未完成。
- TES coverage 坐标修复的真实 A/B 实验由独立审阅者接受其有限消除黑条效果；
  尚不能替代 24 条内边、9 个交点及最终包验证。
- FrameAbandon R2 三帧 Vulkan 同步恢复及 9 项 CPU 检查已独立接受，
  不证明旧 World 可继续绘制或完整 Game 故障矩阵通过。

Editor E003 的 216 页候选全部完成，但因反馈 PassMI 不声明 `layerIds`
而未发布，原失败保留；WorldBindings 事务修复已进入 R8 普通全量构建。
R8 已核对 1100 份源码 before/after 相同并冻结完整 Game/Editor 与库；
E004 使用同 ABI 的新 wrapper 完成编译，随后 CPU baseline 预检明确失败：
旧 C008 捕获的 `mf_terrainLayerBlend.glsl` 身份与实际 R8 TES 修复源码不同。
该次未启动 renderer、未进行 Paint，不计为 GPU 事务失败或通过。
证据保留在工作树父目录 `p4e/E004/preflight-blocker.json`。
后续经受支持发布流程生成源一致闭包，再进行真正 Paint 发布及 Undo；
不篡改 C008 baseline 或手工重哈希旧内容寻址发布。未以候选页成功代替联合发布。
C010 随后由冻结 R8 真正重新烘焙 448 页并通过 Store 全闭包、32 个 MI、
16 个作者摘要及 Monitor baseline 验证；manifest SHA256 为
`1f92ee6690d1c026bc03aad135cbf196e8263929ba0f8388e7db3d9db6ec2e9e`。
原有粗页数值与新烘焙一致，未修改旧发布。
E005 使用 C010 后实际发布 Paint revision 1，但 Undo revision 2 停在
198 个已完成生产、216 个 required、16/16 根、零在途任务，
最终触发 120 秒诊断阶段 watchdog；这次整体仍失败。
专用事件序列 1965 记录失败，1968 仍显示 required/table fence 未完成，
详见工作树父目录 `p4e/E005/evidence/runtime-events.jsonl`。
旧当前画质就绪不代表 Undo 成功；后续根因修复不能增加冻结槽数或延长等待放行。
Hume 用实际 Runtime/Residency 与材质快照类型复现了精确停滞：
512 槽中 94 Retiring、414 Resident、4 Free；作者快照意外持有初始
`compositedPageTable`，Undo 另有 18 页排队。释放作者快照但保留正常 descriptor
仍不能复用；最后一个正常引用释放并经过 CollectRetired 后才完成余页。
wrapper 只保持 CPU 输入证明，已排除其持有 table；Runtime 阻止活 owner 下复用是正确的。
Service 作者态快照与运行时绑定所有权分离的修复及 E006 GPU 复验尚待完成。
R9 v2 随后完成 1107 份源码、37 个 CMake 输入的 before/after 相同锁定，
checkpoint SHA256 为 `b7f71acfbee7a30143cbb6a62cbdec539c68241e18a141a3d42bba2b5247db4a`。
E006 Coverage 使用完整同 ABI 库实际完成 Paint revision 1 与 Undo revision 2，
descriptor revision 10/11、233 次真实测量、footer 1332 incomplete=false，
三张 BMP 完整；该单项 runnerAccepted=true，整 P4 仍未完成。
对称 E006-local 随后真实失败：Paint 候选报
`local weights require an authored texture identity`，没有 Paint/Undo 联合发布。
作者快照丢弃生成 overlay 时还必须保留真实 authored weight identity，
后续独立构建及 Local 实机复验待完成；不放宽必需身份校验。
Local 修复随后进入独立 R10 小核心，1107 源及 37 CMake 输入前后一致，
checkpoint SHA256 为 `dd973e1d9fc9bef61ec48406deb49670a494c0ee164f814e776ce7899e750480`。
E007-local 实际 Paint/Undo 均发布 revision 1/2、descriptor 10/11，
268 次完成测量、footer 1520 incomplete=false、validation/runtime errors=0，
L010 258 个文件未变。Darwin 已独立接受 E006 Coverage 与 E007-local
这两个 worker2 单项，包括全部 capture 身份/fence、旧输入及 CPU 所有权回归；
没有将静态 BMP 差值当作 lit/TES 视觉验收。
随后完整 27 步 Coverage worker2 F001 实测完成前 11 步后，
因私有 wrapper 错误要求邻块作者 ticket 在 boundary_paint 超时。
实际邻块 72 页均以新输入 token 生产，并联合提交；仍缺独立输出像素验证。
原 F001 保留失败，新 F005 用明确的作者/邻块依赖/联合提交/fence 分离谓词重新实测。
F005 完成 12 步（含边界 Paint）后在边界 Sculpt 再次超时：作者 ticket 已 Published，
邻块 72 个输入 token 均更新并联合提交，但调用方一直使用提交前 71/72、
completed2907/table2908 的旧 candidate 采样。后续 active 观察为 resident/selected72、
queued0、quality 完成 epoch3130；原 71 不能事后改成 72。
后续需真实提交后 per-surface 状态事件，另验宏几何、frame、碰撞与边界像素。
原 F005 仍失败，不据后续成功事件补写其缺失观察或声称完整 27 步通过。
Local L010 也已实际重烘并通过 448 页、75 对象、32 MI、16 作者/Monitor baseline，
以及 16 份 distinct R32（118,820,800 B）逐 float 验证；manifest SHA256 为
`576bac2a8510600e190467de717d7c6386ff0b33152e8b0f653f546ef041f75d`。
正式性能还需真实 CPU/GPU 帧身份和质量计算计时；独立事件文件与读文件
轨迹补全正在接线。旧发布资源暂不删除，最终交付继续 pending。

后续 R9 源码检查点：Service 作者态绑定分离已通过 11 项实际 CPU injection，
active fault owner 注册/退场通过 2 项生命周期测试，Runtime 四文件保持不变；
普通完整 Game/Editor 构建正在进行，尚未完成源码 before/after 最终锁定。
Dirac 真实帧身份、quality GPU timer 和故障入口的 69 项 CPU、
Turing 捕获关闭的 32 项 CPU 已分别由 Euler 独立复跑接受其有限范围。
Raman 独立发现暖缓存 `HashFile` 与 manifest Load 漏读取日志，两处整改后
同一 86-entry cache 反例已独立复验关闭；实际 R9 只读 Game 读取负证据仍未产生。

C010r8 独立诊断包已原生 Export/Verify，但巡游 batch01 的末后台捕获缺结束事件，
整体保留失败。batch04 在 `center_0_2` 又实际触发 10 秒质量失败：
16384 visible、14095 合格，complete=true、identity/unresolved/rootOnly 均零；
该失败不同于退出事件缺口，仍须按 raw 查明，不能自动归因于已知旧页表引用问题。
其他点继续作为诊断收集，不能宣告整个巡游或最终双模式包通过。
后续 C010r8 worker2 诊断批次继续保留失败：case01 仍有原生质量失败，
case02–04 各自有未闭合的提交/完成捕获对，未被 parser 修补。
这些结果用于暴露运行时收口和跨姿态收敛问题，不构成 P5 视觉或性能通过。
完整 C010r8 Coverage worker2 的 53 个诊断机位现已全部实际尝试：
51 个达到质量 ready，`center_0_2` 与 `center_1_2` 分别再次触发约 10 秒
收敛失败（后者最后 2816/16384，前者 fresh process 仅 11/16384）。
每个 ready 点都有 dedicated submitted/completed 与同 frame/pose/epoch 对齐；
失败点、每批 footer、BMP、inventory 和原始 stderr 均保留。该结果仍是
R8/C010 诊断，不能替代 R9、Local、worker1、正式性能或独立 lit/TES 视觉验收。
R10 新包独立 fresh 两点仍失败：`center_0_2` 末 0/16384、
`center_1_2` 末 14/16384，实际反馈对照所需 mip1 四页位于
`(15,15)/(15,16)/(16,15)/(16,16)`，分别属于 surface 8/9。
独立数学复算推导完整需求为 114/230 个键，低于 492 的活动目标容量，
未发现旧 ceil shader 或显式请求截断；实际反馈 bytes、selector 与队列状态
尚待 R11 有界探针采集，不据此猜测预算耗尽或调度根因。
R11r2 两 main、实际 draw 诊断 target 及五个必要 CPU 测试目标均构建成功，
测试分别 9/2/17/6/6 通过；因构建期间一个未注册到 binary 的 804B lit 工具入口
变化，完整 before/after 锁失败，未签发 checkpoint。所有共享写入已暂停，
新的 build-only 执行者以当前实际源码建立 R11r3 锁并重做正常增量构建。
已独立发现的 metadata 写失败仍置 complete 缺陷也已通过 checked writer
与 6 项阴性/成功 CPU 用例整改，实际 capture GPU 验收尚未进行。
随后 R11r4 正常重建并锁定 1113 源、41 CMake 与全部工具输入，
checkpoint `71752867c800de317c31efc2974f3d70b48abc21828e9d1e123993cd9f751400`。
首个真实 LOD0 patch 产生 116 条有限、非零位移记录，无 overflow，
232 次独立高度比较最大 0.003735423089 R16 单位；Hypatia 已独立接受有限子项。
这不代表其余边角、table/LOD、scene-lit 或整个 P5 通过。

R11 两失败视角的实际探针已证明目标页反馈 bytes/fence 正确且有 297 空槽，
但 10 秒只完成 56/59 个生产任务。CPU 反例进一步复现原升序 pending 加整表轮转
导致 mip1 覆盖迟于大量 mip0；新策略有根优先、namespace 公平与细页进度，
Darwin 独立 9 项 CPU 复核通过。质量回读新增一次有账缓存复制的候选也由
Euler 独立验证 85/85 raw/count/export 等价、14 项守卫与预算/寿命检查。
两修复进入一致 R13 核心，checkpoint 为
`24c96cf967648c8483f1497ab645e6924d689ce83cac3e15c8c3f5c87572adcb`。
Hypatia 随后独立接受两原视角真实收敛：1.776293 秒 16384/16384、
6.502141 秒 16304/16384，最终两者均全有效；全部 72 份 raw 身份与 epoch 核对。
短 validation-on run 的 CPU 改善已有证据，但不能签正式 1000 帧/validation-off 性能。
P0、512 槽、1 页/帧、4 task 不变；新增 1,619,200 CPU 字节已入账，
捕获 GPU Workspace 峰值 27,396,128 字节不变。
整合 owner 原 Euclid 因服务端 429 重试上限终止，Copernicus 接手，全部旧成果保留。
R12 真实提交后观察已签发；F007 当前在冻结 R13 上继续完整 Editor 行为矩阵。
清退审计已识别资产工作树内 16 payload + 16 manifest、共 19,438,418,160 B，
并分别列出需保留的 sidecar 与未知发布；目前未删除任何候选。

| 阶段 | 执行 | 独立验收 | 当前结论 |
| --- | --- | --- | --- |
| P0 | Planck | Boole（最终审计） | PASS；冻结哈希见下文 |
| P1 | Mencius/Averroes 初始实现；Pascal/Arendt/Meitner 整改；Rawls GPU 执行 | Tesla、Dalton 等独立子项审阅者 | pending；V2 双模式闭包与真实安装初始启动已有证据，故障矩阵及最终闭包验收未关闭 |
| P2 | Nietzsche 初始实现；Euclid 接手共享资源、预算与调度 | Darwin | pending；有界 owner、叶快照及 CPU 联合事务子项接受，真实竞争/退休/跨 World 峰值未关闭 |
| P3 | Euclid 运行时事务；Cicero 邻接 Baker/compute | Hypatia、Euler（质量子项） | pending；10936 页性能、384 页数值和初始质量子项接受，全 Game 正式性能及故障未关闭 |
| P4 | Archimedes/Arendt 初始实现；Euclid/Hume 事务；Peirce 实机执行 | 独立事务与 WorldBindings 审阅者 | pending；E003 未发布，R8 冻结后执行 E004，完整 Editor 行为未完成 |
| P5 | Anscombe 初始工具；Pascal 交付；Ptolemy 巡游工具 | Raman（安装子项）、Hypatia（有限 TES 对照） | pending；双模式诊断包存在，完整最终包/全岛视觉/性能及旧资源清退未完成 |
| P6 | Boole | Tesla | PASS：本轮不采用 BC5/BC7 |

主线程仅负责管理、接口裁决、整合及证据核对；subagent 执行实现。
阶段只有在实际退出条件和独立复核同时满足后才能关闭。

P0 中间检查点：旧 Full16 cold 与 raw GPU timestamp 巡游均正常退出，
分别位于 `artifacts/terrain-rvt-production/p0/cold/` 和 `raw-cold/`。
两者尚不代表 P0 通过。独立中审复算确认 PID 和内存峰值汇总正确，
同时要求纠正实际 WDDM 采样间隔、末条缺样、滚动平均分位数、
曝光 delta 钳制、账本漏项及历史比较分母。

P0 执行者随后确认 baseline 源码、私有输入和计时二进制已隔离，
允许后续替换源树的旧 Store；此确认是基线隔离点，不是预算冻结。
CPU raw Game 摘要为
`cb95bacad01ad42d92cf2826f061c03d71fbb51d85f504efad4c2d1f9a9091f8`，
重链清单位于 `artifacts/terrain-rvt-production/p0/raw-cpu-link/build.json`。
`fr`、P0 stage 和私有 producer 输入继续保持原状。
GPU 测量窗口仍由 P0 独占。

后续检查点：正式冻结文件已签发为
`config/terrainRvtP0Frozen.json`，SHA256 为
`43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd`。
该文件保持不变，独立最终审计由未参与 P0 实现的 Boole 执行；
审计通过前不将 P0 标记完成。quiet-final 的 370 个主机审计样本未观察到构建进程。

P1 CPU 子项第一次集成结果为 165 项中 162 通过、2 跳过、1 失败；
失败项是加载根页 epoch 0 重试与预览 epoch 0 拒绝的状态语义，
已交运行时写者修复，未降低断言掩盖失败。
exporter 子项为 72 项中 71 通过、1 符号链接能力跳过，
交付工具 25 项通过。ProductionGpu allocation 查询和转换子项后续达到
16/16 CPU 通过，但 GPU roundtrip 尚未验证。
这些均不替代 P1 整阶段或 Full16 Game 验收。

部分原执行 agent 因服务端 `429 Too Many Requests` 结束，
主线程已保留其源码和证据，由接替执行者继续同一工作树：
Euclid 负责运行时整合，Arendt 负责 Editor，Pascal 负责交付工具，
Cicero 负责邻接 GPU producer；Tesla 独立核查 P1。
不因执行 agent 更换重新生成或放宽 P0 冻结配置。

P0 最终独立审计已通过，见
[P0 独立验收](terrain-rvt-p0-independent-audit.md)。
27 个证据文件的实际长度和摘要一致，原始统计复算一致；
结论只放行 P0，不代表 V2 Game 已通过。
P6 执行侧完成 [压缩评估](terrain-rvt-p6-compression-evaluation.md)，
本轮不采用 BC5/BC7；独立复核仍待完成。
Rawls 获得下一 GPU 窗口，验证实际 ProductionGpu 输入恢复与 roundtrip，
并非运行独立 Vulkan 模拟后冒充生产模块验证。

Rawls 随后完成实际模块 GPU 执行：3 项 allocation、58 次 roundtrip、
278 个 mip/layer、4 个双模式 placement、21 项拒绝和2组租约检查全部通过，
validation 无错误。证据为 `p1-gpu/runs/run-002/`。
这仍不证明无作者目录 Game、Full16 或性能。
Cicero 的首轮真实路由验证随后完成：2,222,424 条严格 CPU Resolve 对照与
7,398,400 条生产 Address.comp route 对照均为零差异。
实际跨块完整产页、GPU atlas copy 和性能仍待验证。

P1 独立中审发现并阻断了临时文件硬链接覆盖、MI 路径基准、
NTFS 非 ASCII 大小写别名及 albedo 传输格式不一致问题。
相应修复和回归已分派；不能用早期 CPU 通过覆盖这些发现。
P6 的独立复核已通过，见
[P6 独立验收](terrain-rvt-p6-independent-audit.md)；
结论只针对本轮不采用的决策，不宣称所有 BC 编码器不可行。

## 已作接口裁决

- V2 使用一个 World 发布 manifest，包含多 surface、共享不可变输入对象和显式邻接；
  每个 placement 按稳定 surface 身份选取输入。
- Startup 不扫描全部可选页面 payload；实际输入消费前必须完成验证。
  安装、发布与导出执行完整实际闭包验证。
- 合法缺页、随包页损坏、可丢弃缓存损坏和生产输入损坏分别表达；
  不通过通用 IO 异常猜测合法缺页。
- Production/Adjacency 纯 CPU 协议由 P1 单一写者负责；
  Service 将其恢复成版本化 GPU 输入，P2/P3 不另建第二套 JSON 解析。
- TR 的 `packagePath` 指向共享 World 发布；作者态增加
  `productionSourcePath` 指向完整 surface 集合及显式邻接配方。
  surface 身份沿用 `TerrainPlacementIdentity(plan)`。Game 不读取作者配方；
  Cook 收集整个 World 后一次原子发布。TR 不再决定物理池预算。
- 包内 base/detail MI 是动态参数权威；Game 在 MI 纹理源读取前注入
  生产快照纹理，避免另装一份作者源图再替换。
- World 共享三通道物理池、任务槽和上传环；跨 World 准入 owner
  必须同时覆盖 active、candidate、在途和退休资源。
- P0 实测后的冻结配置是后续验收唯一预算与性能门槛。
  旧 cache 1 GiB 口径不能冒充新地形总账，不允许为通过测试放宽。

## 已确认的验收风险

- 历史 GPU 报告只覆盖部分 passes；120 帧滚动平均不能推导逐帧 p99
  或不可抢占单页最坏 GPU 时间。
- 旧交付脚本默认 stage 指向 2×2，实际现包为 Full16，必须绑定实际身份。
- 主线程实际查看旧 Full16 `cross_tile_corner.png`，存在矩形材质硬边；
  根因待区分作者输入与覆盖、frame、边界寻址和过滤，旧图不构成无缝通过证据。
- 完整网格验收至少包括 24 段内部邻边、9 个四块交点、外部四角及逐块近景。
- 被替代的约 19.42 GB V1 页面只在 V2 独立 Game、视觉、性能和预算验收通过后，
  按精确旧发布清单清退；历史日志和未知 stage 不在自动删除范围。

## 追加：本轮契约与操作文档同步检查点

2026-11-21，文档单写者只读指定工作树当前源码、P0Frozen 和独立审计，
保留其他 owner 的 dirty 成果；未改源码/脚本/预算/资产，未启动 GPU。
V2 Store 检查时机、真实 production objects、World 共池/跨 World epoch、
gen3 与当前 World+WorldPadding 两 active compute、rawRGBA8、双权重和独立高度、
作者配方/默认切换操作已同步到契约与操作入口。
这些是可核对的源码声明，不关闭 P1/P2/P3/P4/P5。

quality 仍在并行接线：当前帧录制器可见 BeginFrame、AfterFeedback、Submitted 调用，
quality 模块有环境变量启用、完成 epoch 消费、visible replay 和读回写出；
Service stats 仍为 qualityReadiness=unverified。深度资源 owner、原始事件、
采样 identity/有效 mip 与 interactive 事件到 P0 门槛的完整集成和实际运行尚待证据。
不要在 contracts 承诺自动质量就绪或将 decoder/接口存在写成完整交付。

[P3 独立中审](terrain-rvt-p3-independent-audit.md) 的 Repeat129末页真实消费反例已交
producer owner；Clamp 支持域证明不关闭 Repeat 缺陷。
当前两新 shader 的 GPU数值/性能、176 sampler 实际能力准入及最终包闭包均待复验。
早期五阶段/gen2 收据与同 gen3 不同 source hash 的诊断二进制不混用。

该中审三处 acceptance 链接失败发生于文档替换期间目标文件暂时缺失；
目标操作页已恢复，保留原引用和独立失败日志，不改写审计历史。
最终整树 docs gate 以文档任务结束时的真实日志为准。
主线程 Game 运行后最终校对：两权重/两worker离线包、根恢复、全岛边/角/TES、
raw timing/CPU/WDDM/全部 allocation、Editor真实动作及默认无参数启动。
当前 `config/config.json` 仍指向旧 Full4x4 默认；本轮没有进行默认切换或 V1 清退。

## 追加：真实 Full16 首轮诊断与后续阻断

`cook-gen3-coverage-run001` 已实际启动并退出 1：新 Coverage MI 显式重复
`u_terrainWeightMapMode=0` 默认值，被现有材质校验拒绝。
生成器和新 V2 作者命名空间已修正；旧作者 Height/Paint 与 V1 发布保留，
没有通过放宽材质校验解决。

`cook-gen3-coverage-run002` 随后越过材质加载，退出 1：
CaptureGpuImage 为 64 MiB coverage 图申请整图 GPU 临时读回，
不满足冻结的 32 MiB Workspace。Rawls 将输入捕获和恢复统一改成
至多 1 MiB block-row 分块。实际 `p1-gpu/runs/run-006` 验证
116 次 roundtrip、868 个 subresource、1626 个 chunk 零字节差异；
预占 28 MiB 后 Workspace GPU 峰值为 29 MiB，validation 无错误。
该执行没有记录整进程 CPU 峰值，不能据此宣称 CPU 总账验收完成。

P1 文件 API 整改由未参与实现的 Tesla 独立复跑：
124 项中 121 通过、3 个符号链接条件跳过。
具体已关闭的问题和 Windows 本地覆盖边界见 P1 独立审计；
它不替代双模式离线 Game 或根页损坏重建证据。

P3 单 dispatch 的 Repeat/limits 修复已实际运行，仍未通过最终门槛：
70 个数值页中有 4 个 albedo 字节误差为 2，超过冻结的 1 byte；
352 页有限压力的生产时间也有超过 4 ms 的记录。
关键 Repeat 反例点单独通过，不代表整页通过。原始失败保留在
`p3-baker-world-repeat-fix/runs/`。正在以实际 GPU 中间采样值与并行子样本
prototype 定位，未调整参考值、误差容限或性能上限。

QualityFrame 的单一 readiness getter 已由接替实现者 Hegel 补齐并独立编译；
状态只由已完成 fence 的实际 GPU counts 和 Service 身份核验更新。
完整深度 owner、瞬移/revision 失效、加载遮蔽及真实 Game 行为仍待运行验证。
最新整程序重链和 snapshot-002 是 run003 的前提，尚不构成正式 Cook 或交付通过。

## 最终交付核对表

下表约束最终关闭目标；子模块通过不能替代对应整阶段证据。
“待验”不表示可以省略，实际收据必须绑定二进制、shader、输入和冻结配置身份。

| 要求 | 当前证据/状态 | 最终关闭依据 |
| --- | --- | --- |
| P0 实测冻结、以后不放宽 | 独立 PASS，固定 SHA256 | 最终包与运行日志仍使用原始冻结摘要 |
| V2 稀疏协议与完整实际生产输入 | CPU/文件 API/输入往返子项通过，P1 待验 | 双权重无作者目录独立 Game、安装完整验证及根恢复/输入坏分类 |
| World 三通道共池、统一 coverage 与有界任务/上传 | 已接代码，P2 待验 | Full16 实际竞争，所有物理资源实际分配账本及跨 World 峰值不超冻结 cap |
| 槽、在途任务与旧帧退休 | CPU 复验部分通过 | 实际提交失败、取消、迟到 revision、旧 epoch 及复用竞争无错误发布 |
| 公平调度与画质下限 | 待实际 Game 验证 | 512 槽内无永久饥饿，bias≤1，超能力明确失败而不是降低标准 |
| Game GPU 缺页生产 | 已接 Record/copy/status/页表事务，未正式通过 | 空细页缓存与任意未访问位置真实收敛，无 V1/直接 evaluator 自动回退 |
| 显式邻接与边界积分 | 路由有限 GPU 对照通过，完整产页仍有失败 | 24 段内部边、9 个四块交点、外角、混合权重/映射及粗 mip 正确 |
| 初次可交互画质 | 质量/加载模块整合中 | roots/高度闭包有效；实际 GPU 样本≥99% raw target+1、零未知/错误身份，无 root-only 放行 |
| Paint/Sculpt/参数/热重载/Undo/Redo/Cancel | CPU 子项通过，P4 待验 | 新输入不可变/COW、旧任务不能回写；实际 Editor 过程与独立审计 |
| 作者保存、预览与游戏发布分离 | 接口/状态存在，待实机 | 保存不误清其他 dirty，预览不冒充游戏已发布，重开与重载证据 |
| 高度与两种 TES 权重模式 | CPU/独立高度输入部分证明 | 生效距离下真实非零置换、缺高度诊断，基础细页缺失时仍正确 |
| 全岛视觉数值 | 尚无本轮最终图 | 全岛/近场/接缝/四角同相机参考、三通道与 lit ROI/TES 误差均达冻结标准 |
| 性能与内存 | P0 分母已冻结，新实现仍有性能失败 | 冷/热/连续/瞬移/重载 raw GPU/CPU、10936 页、进程 private/WDDM 与实际分配同时达标 |
| 运行故障分类 | CPU 负例部分通过 | 生产非零 status、坏缓存、坏根、坏输入、预算拒绝均有真实可追溯结果 |
| 两线程模式与 World 替换 | 待实际 Game | worker1/2、A→B→A、候选失败保旧、退休重叠和 validation 零错误 |
| 独立离线 Game | 尚未交付 | 双权重闭包及独立安装，零 shader miss，禁止工作区依赖，安装只读验证 |
| 标准入口及旧路径清退 | 旧默认/V1 保留中 | 新包全部通过后切默认，旧配置/材质开关拒绝，删除被替代产物精确集合 |
| P6 采用决策 | 独立 PASS：本轮不采用 | 保持结论及原实验，不将块大小算术当压缩包或性能实测 |
| 各阶段独立验收 | P0/P6 PASS，其余 pending | 每阶段由未参与该阶段实现者签署，最终按完整范围复核 |

run003 已实际执行并失败于 shader 输入路径基准；真实私有 cache 的两个 World compute、
primary/include/SPV/debug 共 11 个文件摘要 CPU 核对通过后修正。
run004 随后实际执行，失败于统一 archive 将 ShaderSource/ShaderArtifact 传入
仅接受 Image 的 WriteInputObject 校验。该调用边界正在按真实对象 kind 修正，
没有放松 bytes/hash、路径或不可变对象冲突检查。两轮失败继续保留。

## 实际发布与首次 Game 联调

`cook-world-chunk-coverage-run006` 实际 exit0 并提交 World。
原生 Store 全量检查确认 16 surfaces、448 个 mip4…13 页、16 roots、
66 objects、1120 条作者依赖；发布目录 100 文件共 490,973,049 B。
这是 Coverage 完整岛的发布结构子项，不是质量、性能或 P5 通过。

首次质量 Game 使用的旧支持库不识别 RawAuthorSource kind3，实际启动被拒绝；
更新匹配的 Production/Store 对象后，第二次启动揭示归档 MI 的空 textures 字段
违反现有材质规范。32/32 原发布 MI 经原生校验均被拒绝。
源生成端已改为省略空字段，并在归档前调用原生 resolver。

`runtime-mi-actual32-r1` 随后在全新资源根执行实际 CPU Store 原子重发布：
32 个新 MI 原生校验通过，448 页摘要、全部基础 surface 身份及包 bytes 不变，
旧发布的 100 个文件 hash 未变。新 manifest SHA256 为
`9578503372de07f7744f984116e2cf8316437393ba511718e02f4d186fb24668`。
这不是原地修补内容寻址文件，也没有冒称重新完成 GPU 烘焙。

实际导出另暴露了旧 compute whitelist 和不可达旧水材质 cache 记录问题。
未消费旧五阶段已从 required runtime whitelist 删除；过期 cache 不伪造摘要，
计划在新的专用私有 Release stage 使用实际原生编译/加载生成一致缓存，
原锁定 stage 和失败导出证据保留。当前尚无可交付的独立 Game 包。

producer 的独立参考完成两页 identity byte-exact 校准后，
完整 adaptive 两批各 70 numeric + 352 pressure 已有有效参考，
三通道最大误差 [0,1,0]，有限数值门槛通过；
validation-off 最大 production 5.456256 ms，仍有 5 页超过 4 ms，
因此尚不推广该候选，也未完成 10936 页正式压力。
double 过滤偏差继续留作诊断；独立审定明确原冻结比较权威为原实际 GPU evaluator，
不将主线程额外增加的 double 门槛追溯解释为 P0 要求。

R1 预算登记表的有界修复已由原独立审阅者复核关闭，
原 10000 次短租约负载、21 Budget / 77 联合测试及 400 万次并发负载通过。
R2 像素叶 owner registry 已完成 15 项 CPU 子模块测试，正在 Service 接线；
仅模块存在不关闭编辑源快照重叠计账门禁。

## 质量捕获与收敛的实际失败记录

Hegel 的首个有效质量 Game 运行 `run-20261121T034340809005Z`
完成 27 次 GPU 读回，但 3271 个可见样本中 3262 个仍为 root-only，
并有 9 次重复 claim，完整性为 false。该运行明确失败并正常 teardown。
随后确认帧末合成反馈回读被错误包在已退役 coverage feedback ring 的存在条件内；
Game 删除旧 ring 后，新合成 ring 也不再回读。已拆开独立条件，没有恢复旧运行路径。

`run-20261121T035333501660Z` 使用上述修复与真实初始时钟，
完成 167 次 GPU 读回：3271 visible、3262 满足 raw target+1、
unresolved=0、incorrectIdentity=0、rootOnly=0。
每份仍有 9 个重复 claim，因此 complete=false，不能放行。
进程在初始 deadline 后明确报告 Terrain.QualityConvergenceFailed，
exit2 并完成 teardown；没有靠黑屏等待掩盖失败。
原始读回由未参与实现的 Euler 独立解码确认，
见 [质量独立审计](terrain-rvt-quality-independent-audit.md)。
本运行没有截图，不能据代码存在宣称已验证实际黑底像素。

R2 的 CPU 像素快照重叠范围也已由 Darwin 独立复核：
28 项与额外容量/父账用例通过，Service 的 Retain、candidate、PageManager alias
及终态释放接线已核对。GPU COW 候选根/status/fence 与输入/页表/MI 原子切换、
真实 Full16 竞争及跨 World 峰值仍是未关闭门禁。
P0 与 P6 以外的整阶段继续 pending。

文档整合后源码继续变化：末次读取已见 Repeat 用完整136×136评价域，
Baker构造接入 selected-device ValidateDeviceLimits；
这些标为整改迹象/待独立复验，不改中审F1/F2历史结论。
quality 的 SetDepthOwnerResolver 当前只搜索到声明/定义，尚无调用方证据；
帧接线存在不能当作整个可见深度/quality/interactive链已完成。
本轮整树 `py -3 tool/validate-docs.py` exit0，149条路由行、94条生成规则、
712个树内相对链接通过；224个出仓知识库链接仅提示需主checkout核对，
不宣称本轮已验证这些外部路径。最终再次运行的结果由任务回复给出。

## 追加：R14 最终包运行、53 机位与性能/Editor 实测（2026-11-22）

本轮在 **R14 冻结核心**（`core-native-r14-lit-r1/checkpoint.json`，SHA256
`fffc42482f7bbedda03b5817c0ebf63a42952e148ae348d200ae7fbcfa977c0d`，1116 源文件
manifest `e6e00918…103489`）与两份同版本最终包上执行。P0 冻结文件未改。
主线程负责窗口与整合，运行/审计由未参与该子项实现的 agent 执行。

### 环境与工具修正（不改变门槛）

- 本机无 `pwsh`（只有 Windows PowerShell 5.1），所有依赖 `console_inject.ps1` 的相机/截图/
  `quit`/`loadworld` 注入都会 `FileNotFoundError`。用 `powershell.exe` 副本作为
  `final-delivery/bin/pwsh.exe` 加入 PATH 后恢复；这是测试环境前提。
- `tool/validation/terrain/rvt_formal_perf.py` 修正四处证据汇编缺陷（不改阈值）：
  R13/R14 checkpoint 的 `build-input-*.json` 是 120 项而工具只接受 41 项；
  `incomplete` 对 dict problem 直接 `set()` 崩溃；相机日志实际为
  `position=(x, y, z), forward=(…)` 而旧正则从未匹配；`execute` 会按 `evidenceRoot`
  重拼 P0 命令而 `fixed_route_window` 读 plan 的 `reference`，导致收尾帧延长时二者分叉。
- 正式性能必须在锁定时钟下测量：未锁时 GPU 在 210–1800 MHz 间漂移，同一包的
  p50 会翻倍以上；P0 的 p50 只在 1800 MHz 量级可复现。本轮用 `nvidia-smi -lgc 1800,1800`
  测量、结束后 `-rgc` 恢复。

### 已实际通过的子项

- **CoverageAtlas / worker2 / 53 机位全岛巡游**：`final-delivery/s2-cov-w2/cov-w2-batch01..07`，
  7 批全部 exit0、clean teardown、`packageUnchanged=true`、安装闭包 native verify exit0。
  53/53 ready；最大到达→就绪 6413.4 ms（`center_1_2`），最小有效比例 99.512%；
  `bias=0`、`unresolved=0`、`incorrectIdentity=0`、`rootOnly=0`、`uninitialized=0`。
  独立审计（未参与运行）逐份重解码 563 份 TVQ1 raw、对照 553 条 native STATE，
  0 数值不符、53/53 ACCEPT，见 `final-delivery/s2-audit/independent-audit.md`。
- **LocalLayerArray / worker2 / 53 机位全岛巡游**：`final-delivery/s3-loc-w2/loc-w2-batch01..07`，
  53/53 ready，最小有效比例 99.768%，最大 6436.9 ms，其余条件同上。双权重模式均实跑通过。
- **worker1 核心/瞬移**：Coverage 与 Local 的 `core`（5 机位）与 `teleport`（4 机位）均 ready；
  `mid` 在 worker1 下 6.6–7.5 s 收敛，仍 <10 s。
- **P4 Editor 27 步矩阵**：R14 ABI 适配器重建（`p4-adapter-r14/editor_s4.exe`
  SHA256 `e43d0b6e…f7fba`），修正资源根后 27/27 步全部执行、54/54 surface 快照、
  27/27 BMP、child exit0（422 s）。

### 未通过 / 未关闭（保留原始失败，不修饰）

- **正式性能 FAIL**（`final-delivery/s5-formal-performance-record.md`）：
  同一 P0 九段路线、锁 1800 MHz、1088 帧测量窗口、35/35 相机确认。
  steady GPU pass sum p99 **12.589 ms** vs 冻结 4.618；geometry+TES p99 **9.060 ms**
  vs 3.754；CPU interval p99（含 4 Hz quality 回读）**77.752 ms** vs 72.105；p95 40.6 通过。
  p50 2.545 ms 与 P0 2.059 ms 同量级；尖峰集中在两段近地机位的 6 帧簇
  （geometry 9.1 ms、feedback 2.7 ms），与 quality 捕获无关，LOD 计数与 TES 宏与 P0 一致。
  启动 18.6 s、private 5.50 GB、WDDM 5.07 GB 均达标。**不得靠降 tessellation/质量或改门槛通过**。
- **连续移动 FAIL**：`s3-w1-cov-continuous` 在 `tile00_near→near` 直线插值的中段
  `QualityGpu visible/resolved counters differ or no visible terrain`（相机穿过山体/地下，
  0 可见）。该路线不是贴地路径，属测试路线缺陷；需要一条真实贴地移动路线才能验收“快速移动”，
  不能用瞬移结果代替。
- **P4 四个阻断**（`final-delivery/p4-adapter-r14-run-report.md`）：
  A) 热重载重新编译 debug SPIR-V，与 Cook 时 debug artifact 永不相等，而
  `ValidateProductionGenerator` 连 debug 产物一起比较 → `reload_inflight`/
  `old_world_action`/`reloaded` 失败、World generation 不前进；
  B) save_b 的 height 保存被上一次 sculpt 失败重试阻断（paint `.vlpaint` 已写）；
  C) `paint_undo`/`paint_redo`/`paint_pending_cancel`/`paint_cancel` 4 张截图落在
  重烘窗口内、近黑（mean byte error 81.6）；
  D) `native-refresh.jsonl` 无发射方，12 类 native 事件仍缺。
- **lit ROI / TES 视觉门禁未实现**：R14 已链接 native lit capture（`ee90b975…`），
  但 lit case（`native-terrain-lit-replay-v1`）、固定 masks、ROI 实际 slot footprint、
  atlas format/capacity dump、独立参考页绑定和 candidate-vs-actual 校准均无生产者；
  见 `final-delivery/lit-gate-plan.md`。TES 单项仍沿用 R11r4 已接受的 0.003735 R16。
- **P1/P2 负例**：`rvtfault` 5/6 hook 已有真实入口（仅 candidate_retention 解析后
  `pendinghookmissing`），scenario fixtures 已产出并 CPU plan 通过 23/24，
  但 `run_rvt_case.verify_case` 要求的 `[RVT_EVIDENCE]` 事件族无发射方，
  因此任何 execute 都只会得到 `incomplete`；尚未产生可签署的 P1/P2 负例收据。
- **readonly 安装 / 文件读取轨迹 / 默认入口切换 / 19.42 GB 清退全部未执行**：
  `VukanLearnResources` 根没有 `VT/worldV2/...` 发布与匹配 shader 闭包，
  直接把 `config.json -> initScene` 指向 V2 变体会让仓库默认入口立即因
  publication identity 失败；清退前门禁 D01–D10 仍为 false，旧 19.42 GB 保留。

### 结论

P0、53 机位双权重画质/收敛、启动与内存已经实测成立；**正式性能、连续移动、
P1/P2 负例、P4 完成矩阵、lit/TES 视觉门禁、readonly 取证与旧路径清退仍未关闭**。
整 P1–P5 继续 pending，不因本轮 53 机位或 Editor 27 步执行而宣告完成。
