# GASP P5 Motion Matching 执行记录

本文记录 2026-10-18 的 P5 实施范围、审查决策和验收边界，不是接口契约。
当前行为以 [轨迹与搜索](../contracts/animation-motion-features.md) 和
[动画运行时](../contracts/animation-runtime-skinning.md) 为准。

本轮按用户要求由 subagent 执行，主线程负责规划、接口审查与独立验收。
P5 基础搜索、局部交叉混合及调试已完成本轮技术验收；
最终构建、模块、正式控制器和实窗截图分别登记，不把先前版本测试通过当作最终结果。

## 实施范围

- 搜索核心 `source/animation/motionMatching.*`：冻结数据库、同代查询归一化、
  加权平方距离、轨迹/姿态分项、稳定决胜、有界 topK 和继续/重选策略。
- 共用 `motionRuntimeDatabase.*`：工具构建逻辑提升到动画资产模块，
  工具保留适配，运行时核对模型输入、骨架与数据库身份后缓存 const 资源。
- 原生 `motionRuntime.*` 和 World 桥接：C# 提交单源数据库及 clip ID 子集，
  原生建立实际历史查询、维护搜索/播放时钟和交叉混合。
- `SkeletalPlayback` 使用 ozz 局部姿态混合，再统一层级求值和发布；
  没有把矩阵直接逐元素混合作为骨架过渡。
- 只读候选面板：查询序号、同代 DB/统计、完整 raw/normalized 查询表、
  Selected/Continue/前五名的分项代价、策略代价与拒绝原因。
  固定七个两行槽位、10 Hz 或变更刷新、暂停冻结、短视口滚动。
  MM 底部只保留单行状态/颜色图例，不重复候选内容；P4 原摘要单独保留。
  完整值的用户入口是列表图标展开的滚动通道表；title 仅为附带元数据，
  不将其当作 RmlUi 原生 tooltip。

原 UE 工程 `D:\YYBWorkSpace\UnrealProject\gasp` 仍作为只读输入；
资源侧正式 GASP 目录承载 gameplay 与动画数据，不把游戏源码放进引擎示例目录。
本实现以小库的精确搜索建立基线，不引入近似索引、IK、惯性化或 P6 步幅修正。

## 审查决策

1. C# 负责动作许可和动作锁，原生只执行数值筛选、稳定决胜和连续性策略。
   空集合始终无候选，不允许继续帧或默认回退扩大 C# 的业务范围。
2. 准确继续特征按实际播放时间从同一导入片段提取；最近库帧只作为明确量化的 API 路径，
   半采样间隔是其误差边界。准确半帧继续不能误删不同时间的有效库候选。
3. 非循环尾端没有完整未来窗口，不能伪造搜索代价。已锁定且仍被许可的动作
   通过运行时完成路径继续到 Completed，并清空查询/排名。
4. 查询与代价必须配对：节流不搜索时继续展示上次搜索的 raw/normalized，
   不把新查询与旧候选排名拼在同一视图。空候选的原因也不能被节流状态掩盖。
5. 搜索前根已由本帧物理更新，显示姿态尚来自最近采样。
   局部速度使用最近两次实际混合姿态的差分，转向后替换根速度，
   避免重复姿态的临时观察抹掉动画速度。
6. topK 保留低代价但被策略拒绝的行，Selected 和 Continue 另外发布，
   诊断者可以直接区分数值最优与实际执行结果。
7. 非循环尾窗没有可评分继续帧，但不能因此丢失当前播放的时间身份。
   同库仍允许的当前非循环片段以 `nonloop-tail-rewind` 拒绝回跳早帧，
   其他片段仍按成本竞争；不限制为单 clip，不伪造尾端特征。

## 验收矩阵

| 对象 | 验收效果与证据 |
| --- | --- |
| 搜索数值 | 独立手算 72 总代价；固定随机小表的独立全扫描排名；不用生产 distance 函数生成预期 |
| 筛选与确定性 | 空/未知集合不扩大；低代价禁用 clip 被排除；同成本 clip/member/frame 稳定决胜 |
| 连续性 | 精确/量化继续、同片段近邻限制、最低改善、重选窗口、动作锁、跨库重新绑定 |
| 时间与身份 | 首次零 delta、暂停/零倍率不搜索；筛选变化及时生效；传送/Stop/World 换代清理 |
| 原生混合 | ozz 局部姿态交叉混合；打断过渡使用实际显示姿态；冻结 palette/models/bounds 独立 |
| CLR/Jolt | C# 高层状态改变允许集合；真实角色位移/碰墙；空集合冻结；锁定一次性动作可完成 |
| 冻结 UI | query/search 配对、完整通道值、前五名及分项、拒绝原因、文本转义与固定槽位 |
| 布局 | 960×540、1280×720、1920×1080；候选表滚动且不穿透场景，角色中心与工具条保留 |
| 实际场景 | 起步、匀速、停止、左右转向、跳跃/落地；候选和实际动作都可见 |
| 渲染生命周期 | 单/双线程截图；本轮 World/Graph gate；截图强制终止不等同正常 teardown |

主线程最终核对的日志、数量、退出码、像素范围和未覆盖项见下文。

## 初验与修正

本节保留初验结果，不把中间版本或失败后重新生成的报告冒充最终通过。
`artifacts/` 不入库，下列路径用于本地证据定位；结论保存在本文。

- `artifacts/gasp-p5-capture-smoke-20261018-a/capture.json` 的四项
  walk/start/stop/reverse 均完成截图但动作类别验收失败。
  夹具随后改为实际查询历史就绪后开始输入，避免启动期大 dt 跳过起步/预走阶段。
- 正式 schema 的 trajectoryPosition 从 1 调为 20，其余三组仍为 1。
  这是资源源数据变更，自动改变输入身份；没有在搜索器按片段名硬编码加分或缩小许可集合。
  `...smoke-20261018-b/capture.json` 中 walk/start/stop 通过其阶段类别检查，
  reverse 仍失败，整份报告仍为失败。它们不是最终动作矩阵结果。
- 初始实窗中底部 summary 重复所有候选，右表又因三行高行距只露出少数项。
  修正为底部单行状态/颜色图例及右侧 26px 两行固定槽位，
  Selected/Continue/top3 的 720 高度可见性和短视口滚动分别有 UI 断言。
- `artifacts/gasp-p5-animation-tests-final.log/xml` 记录中间版 101 项中的
  两项 `MotionRuntimeQuery` 手算夹具失败；夹具修订与最终重跑单独登记。
  文件名中的 final 不改变该报告的失败性质。
- 主线程追踪较长 Start/Stop 尾段发现：完整未来窗口消失时继续候选失效，
  原搜索器随即放开同片段早帧，可能重复重启非循环动作。
  修正新增同库/允许成员限定的尾窗回跳拒绝及六项独立策略回归；
  长阶段正式控制器仍需以最终 trace 验收，不把该推断直接写成效果已通过。
- 进一步检查本地 RmlUi 6.2 源码确认，Core 不读取元素 title 以生成 tooltip。
  初版完整查询虽存在于 DOM title，但没有用户可见入口；修正为真实列表图标展开的
  固定通道节点、完整身份和滚动区域，候选全文与图标提示用实际 CSS hover。
  鼠标移动/按下/抬起和展开状态暂停静默有独立 UI 回归；最终构建结果另记。
- 严格正式控制器长阶段初验的五个末秒窗口已分别达到 WalkLoop/Idle/WalkLoop/
  Run 或 Sprint/Idle，各 60 帧；同轮墙前 case 的末秒 Idle 为 0，仍失败。
  资源侧 C# 据实际水平速度进入 Idle 后限制为 Idle/WalkStop，移动族保持宽集；
  这是明确的业务状态筛选，不再扩展原生数值策略来硬选 Idle。
  修改后的正式墙前结果单独登记，保留此前失败报告。
- 进一步墙前复查显示，仅用 Idle/Stop 许可族仍会因查询未来沿 desired 穿墙，
  重选 Stop 早帧。修正仅在已观察到 grounded、水平 desired≥2 且 actual<2 cm/s
  时以实际水平速度构造 MM 查询；自由/空中维持原预测，没有新增未来碰撞系统。
  根参考系、未来世界点与 blocked 标志随最后搜索的 raw/normalized/排名一起冻结，
  红色绘制查询、琥珀保留实时 desired/no-collision；UI 查询行显示 blocked。
  新增数值区分与 Pause/节流配对断言，墙前最终动作结果另记。
- 完整 UI 含展开版 666 项两遍均在转义回归失败：
  原断言要求整表无 img，但新固定查询开关本身有合法列表图标。
  修正为候选/query/完整身份通道子树无注入 img，同时整表仅允许
  `motion-query-toggle` 下 `icons/list.png` 这一张固定图标；
  保留并扩大 DB/payload/component/channel 的恶意 markup 注入检查。
- 地面受阻修正版 C# ON 完整首遍 347 项出现 2 项失败，
  原始报告 `artifacts/gasp-p5-blocked-final-script-tests-iteration1.xml` 保留。
  一项为暂停 summary 使用推进前 view 比较：暂停前实际已有 interval 更新，
  blend 从 0.833 推进到 0.926；raw/root/future/searchCount 的冻结断言均通过。
  主线程独立核对后认可修正比较锚点为真正暂停前快照，不降低冻结要求。
  另一项为正式 120 Hz 反向未出现 Turn，各稳态和墙前阶段通过，属于实际缺陷而非夹具误报。
  独立 trace 显示反向第 0 帧无物理子步，朝向更新却将实际速度清零，
  C# Idle 筛选及受阻查询因此误触发；下一帧恢复 +200 cm/s 后，
  0.15 秒重选窗口继续保持 Idle，约 0.208 秒才选 Start，错过早期 Turn。
  30 Hz 在首帧选 Turn，60 Hz 在第 5 帧选 Turn；不是仅搜索间隔拍频即可解释的失败。
  具体源修正及新版本重跑另记，不把首遍报告登记为最终通过。
- 经主线程批准，物理同步拆分位置/朝向变化：位置传送继续清空实际速度和刷新支撑，
  合法胶囊纯 yaw 只更新旋转并保留最近完成子步的反馈。
  查询速度搬运继续从同一窗口的关节/根差分保留局部动画速度，
  但当前根速度改用权威实际子步值，避免 120 Hz 相邻渲染位移产生零/双倍速度。
  新增真实 Jolt 的无子步 yaw 保速/传送清速两例，以及独立手算的零/双倍渲染位移查询回归；
  原有 yaw 搬运与非零局部动画速度回归保留。修正版完整结果另记。

## 主线程验收登记

2026-10-18 的各轮证据如下。物理反馈修正版为最终源码；
其余行保留中间阶段及失败证据，不将跳过算作通过。

| 验证层 | 已有证据 | 结果与边界 |
| --- | --- | --- |
| 尾窗/展开版构建 | `artifacts/gasp-p5-debug-build-final.log`、`gasp-p5-development-build-final.log`、`gasp-p5-csharp-off-build-final.log` | 当轮冻结后 Debug 全目标、Development main、C# OFF Release main/script_tests 均构建退出码 0；不替代后加地面受阻查询修正的增量构建 |
| 尾窗/展开版 C# OFF Release | `artifacts/gasp-p5-csharp-off-tests-verified.log/xml` | 该阶段 79 项连续两遍全部通过，不替代后续生产修正或 C# ON 真宿主路径 |
| UI 中间布局 | `artifacts/gasp-p5-ui-tests-01.log/xml` 及主线程第二遍记录 | 压缩布局前完整 664 项连续两遍，均 659 通过、5 项符号链接权限跳过；不能替代最终两行布局及新增摘要回归 |
| 正式控制器类别 | `artifacts/gasp-p5-formal-actions-02.log/xml` | 真实 CLR/Jolt 1 项通过，出现 Start/Stop/Turn/Jump/Fall/Land；不是独立截图夹具，也不是每阶段完整稳态效果通过 |
| 动画中间修正版 | `artifacts/gasp-p5-animation-tests-03.log/xml` | 102 项连续两遍通过，已含修正的手算查询与缓存绑定；不包含后加的六项尾窗回跳回归 |
| 紧凑 UI 中间版 | `artifacts/gasp-p5-ui-compact-tests.log/xml` | 6 项连续两遍通过；不包含后加的真实查询展开/hover 回归 |
| 尾窗版动画模块 | `artifacts/gasp-p5-animation-tests-verified.log/xml` | 108 项、14 套件连续两遍全部通过；含 32 项搜索、11 项局部混合、手算查询及六项尾窗策略回归，零失败/跳过 |
| 展开版完整 UI 初验 | `artifacts/gasp-p5-ui-tests-verified.log/xml` | 完整 666 项两遍均有 1 项旧转义断言失败，已明确修订其合法固定图标边界；不能登记为通过 |
| 地面受阻修正版 Debug 构建 | `artifacts/gasp-p5-blocked-final-build-recovered.log`，失败保留 `gasp-p5-blocked-final-build.log`、`gasp-p5-blocked-final-build-low-memory.log` | 首次无界 `-j` OOM；低并发链接时扫描 1938 对象，仅确认 main 的 `engine/modelPreviewCamera.cpp.obj` 和 script_tests 的 `scriptHudTests.cpp.obj` 为 0 字节，定向移除生成对象/对应 objects.a 后 `-j2` 全目标退出码 0；属构建环境恢复，不作为源码回归 |
| 地面受阻版动画模块 | `artifacts/gasp-p5-animation-tests-final-blocked.log/xml` | 111 项/14 套件连续两遍全部通过、零失败/跳过、退出码 0；含 32 搜索、11 局部混合和 6 运行时查询（含地面受阻/自由移动/空中预测）；不替代后加 120 Hz 物理反馈修正 |
| 物理反馈修正版 Development/C# OFF | `artifacts/gasp-p5-development-substep-final-build.log`、`gasp-p5-csharp-off-substep-final-build.log`、`gasp-p5-csharp-off-substep-final-tests.log/xml` | 两处物理反馈修正后 Development main 与 C# OFF Release main/script_tests 构建退出码 0；OFF 79 项连续两遍全部通过、退出码 0，主线程独立核对 XML 79、零失败/错误 |
| 地面受阻版 UI 布局 | `artifacts/gasp-p5-ui-tests-final-blocked.log/xml` | 完整 666 项/37 套件连续两遍均 661 通过、5 符号链接环境跳过、零失败，退出码 0；7 项 MM 含 960×540/1280×720/1920×1080 布局、固定槽位、实际 pointer 展开/hover、42 维末行、转义和 blocked 冻结/即时切换；不替代后加 120 Hz 物理反馈修正的共享库重跑，实窗图范围另记 |
| 物理反馈修正版 Debug/动画 | `artifacts/gasp-p5-physical-velocity-final-build.log`、`gasp-p5-physical-velocity-animation-final.log/xml` | Debug 全目标重建退出码 0；动画 112 项/14 套件连续两遍全部通过、零失败/跳过、退出码 0；32 搜索、11 混合、7 查询，含固定 actual 下零/双倍渲染位移与原 yaw 局部速度搬运 |
| 物理反馈修正版 UI | `artifacts/gasp-p5-physical-velocity-ui-final.log/xml` | 共享库重链后完整 666 项/37 套件连续两遍均 661 通过、5 符号链接环境跳过、零失败，退出码 0；完整查询/hover/转义/blocked/多尺寸覆盖仍通过 |
| 正式长阶段中间版 | `artifacts/gasp-p5-formal-steady-01.log/xml` | 两项中正式动作长阶段通过、墙前失败；五个末秒窗口各 60 帧符合目标，碰墙 Idle 为 0；该报告仍为失败，修正后结果由后续新报告承载 |
| 地面受阻修正版墙专项 | `artifacts/gasp-p5-blocked-final-wall.log/xml` 及同前缀 trace | 60 Hz 专项 1/1 通过，退出码 0；主线程独立核对末 60 行均 Idle、终点 z=-199.800003 cm、实际速度为零，blocked 查询为真且四个未来点均原地、trajectoryCost=0；不替代 30/60/120 Hz 或完整 CLR 重跑 |
| 地面受阻版 CLR 首遍 | `artifacts/gasp-p5-blocked-final-script-tests-iteration1.xml` | 347 项、2 失败：暂停 summary 锚点夹具错误和真实 120 Hz 反向无 Turn；仍为失败，保留报告，修正后结果由后续新报告承载 |
| 物理反馈修正版专项 | `artifacts/gasp-p5-physical-velocity-script-focused.log/xml`、`gasp-p5-physical-velocity-query-focused.log/xml` | 脚本选择集 29/29（真实 Jolt 22、Pause 1、正式动作/墙前六例）及查询 7/7 全通过，退出码 0；主线程独立解析 30/60/120 Hz 反向 Turn 首帧分别为 0/5/9，末段均 WalkLoop；120 Hz 首帧 actual=-200、blocked=false，次帧 +200，不再误 Idle；不替代完整两遍 |
| 物理反馈修正版完整 CLR/正式效果 | `artifacts/gasp-p5-physical-velocity-final-script-tests.log/xml` | 完整 349 项/34 套件连续两遍全部通过、零失败/跳过、退出码 0（278.374/270.638 秒）；六项正式控制器 30/60/120 Hz 动作/墙前均通过，包含起停/转身/跳落出现、各阶段末一秒完整循环/Idle、碰墙实际速度归零及冻结查询；不等同真人键鼠全矩阵 |
| 最终像素与生命周期 | `artifacts/gasp-p5-substep-reverse-blocked-final-20261018/capture.json`、`gasp-p5-physical-velocity-world-gate.stdout.log/.stderr.log` | 最后源码反向/受阻单/双线程 4 项全部截图/阶段/近图反馈通过，四图均由主线程核对，具体范围见下；World/Graph gate 退出码 0、stderr 为空，完成事务/退休排空并正常 teardown。P5 未改 shader，不机械重跑 shader gate；截图强制终止独立记账 |
| 源码边界与场景 schema | `artifacts/gasp-p5-final-boundary-check.log`、`gasp-p5-final-scene-schema.log`、`gasp-p5-final-diff-check.log` | 本轮 65 条架构边界通过；40 场景、0 失败、0 豁免；diff 空白检查通过（保留既有 LF/CRLF 提示） |
| 最终文档/边界/schema/空白 | 收尾四项检查见 `artifacts/gasp-p5-physical-velocity-final-*-check.log` | 全部退出码 0：文档 63 路由/65 规则/353 相对链接；65 条架构边界通过；40 场景零失败/豁免；diff 空白检查通过，保留已有 LF/CRLF 提示 |

### 实窗截图的证据范围

`artifacts/gasp-p5-capture-development-20261018-d/capture.json` 的 20 项阶段检查
全部通过，20 项截图前最近原生反馈也符合目标类别；它使用独立确定性 C# 输入夹具，
不是正式控制器或真人键盘操作。主线程实际看过其中 reverse-t2，确认 Selected 为 Turn180L。

`artifacts/gasp-p5-capture-development-20261018-e/capture.json` 的 6 项 Walk 阶段
检查通过，但截图前最近反馈仅 5 项符合 WalkLoop；walk-t1-900x700 在截图附近仍为 Start。
主线程核对 walk-t2-1920 的 WalkLoop/完整候选，以及 walk-t2-900 的角色/面板不重叠；
窄窗片段名可省略显示，全文由真实 hover/查询展开入口承载，不能声称六张图均已视觉确认 WalkLoop。
e 轮扩大地面并调整镜头；d 轮 Walk1920 地面暗区来自旧夹具尺寸不足，不作为渲染器缺陷。

`artifacts/gasp-p5-formal-playground-20261018/` 与
`artifacts/gasp-p5-formal-playground-clean-20261018/` 保存正式关卡截图及报告。
主线程已核对正式 t1 1920 图中的关卡贴图、角色与 Idle 候选；这只构成正式 Idle 冒烟，
不是完整动作矩阵。以上截图进程的强制终止均不计正常 teardown。

地面受阻截图 `artifacts/gasp-p5-blocked-development-20261018/capture.json` 中，
t2 完成并通过动作检查；t1 首次因启动/冷编译超过阶段期限，报告仍为行为失败，
虽已完成截图且临近反馈为 Idle，也不能将该次报告改成通过。
单线程成功复验单独保存在
`artifacts/gasp-p5-blocked-development-t1-retry-20261018/capture.json`。
主线程已查看 t2 与 t1 成功重试的图：ready blocked、Idle、trajectoryCost=0，
琥珀 desired 穿墙、脚下红色查询点保持原地；原地轨迹退化为 marker 属预期显示。
该夹具的截图证明受阻显示分离，不代替正式 30/60/120 Hz 控制器动作矩阵。

物理反馈最终修正版的定向截图位于
`artifacts/gasp-p5-substep-reverse-blocked-final-20261018/capture.json`，
反向/受阻各单/双线程共 4 项均截图完成、阶段通过、临近原生反馈符合目标。
主线程已实际核对四张 1280×720 图：reverse t1/t2 的 Selected 均 Turn180L，
混合比例约 0.618/0.615；blocked t1/t2 均 ready blocked、Idle、trajectoryCost≈0，
琥珀 desired 穿墙而脚下红色查询 marker 原地。
这是最后两处物理反馈修正后的定向复验；先前 d 轮 20 项矩阵与 e 轮布局图仍属于
各自版本证据，没有将它们全部重新标为最新版本截图。

### 用户复验入口

从仓库根目录启动 Development：

```powershell
.\build-vscode\development\bin\main.exe --dev-ui --initial-scene Maps/SC_gasp/SC_gasp_playground.json
```

程序绝对路径为
`D:\YYBWorkSpace\GitHub\VulkanLearn\build-vscode\development\bin\main.exe`；
正式场景位于资源目录 `Maps/SC_gasp/SC_gasp_playground.json`。
该参数只覆盖本次启动，不修改 config 默认 initScene。
点击 Play 后以 WASD 移动、Left Shift 跑步、Space 跳跃，Motion 开关显示轨迹与候选；
右侧列表图标切换完整查询/候选，悬停候选查看全文。
复验目标为原生自动选起步、循环、停止及反向转身，碰墙后停止并回 Idle；
红色为上次搜索的查询未来、琥珀色为实时 desired/no-collision，受阻查询可显示 blocked。
自动化断言与实窗截图尚未等同本轮真人键鼠全矩阵验收。

正式关卡 description 已通过标准资产修改接口写明当前 MM/局部混合范围和未实现项；
`tool/animation/build_gasp_level.py` 的生成描述同步，避免再次生成时写回 P3 的旧说明。
此次只同步生成器说明，没有重新生成正式关卡。

## 结论与下一阶段

P5 技术实现、相关完整模块连续两遍、正式控制器 30/60/120 Hz 动作/墙前、
最后修正版定向实窗截图及 World/Graph 生命周期已完成本轮验收；
本轮未完整重跑根 CTest，5 项符号链接环境跳过仍为未覆盖。
P0 内容使用范围确认、UE 原版行为对照及本轮真人键鼠全矩阵不在上述完成范围内；
P5 不等于完整 GASP 或 M1 完成。

下一阶段 P6 聚焦低速、急停、反向、斜坡/台阶的连续性与足部质量，
以修正开关前后的滑步、跳变/穿地实图，以及接地/跳跃解锁、偏移界限和传送重置验收。
惯性化、根偏移/朝向、步幅修正和足部 IK 尚未实现。
