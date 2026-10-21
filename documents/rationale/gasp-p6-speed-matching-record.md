# GASP P6 稳态步频记录

> 执行记录，非当前契约。2026-10-18 开始。
> 本次是 P6 的稳态步频分项，不代表 P6、足部 IK 或完整 GASP 完成。

## 范围与基线

在有界 Actor 转向之后，先处理可单独测量的稳态速度失配。
P1 导入报告 `artifacts/gasp-p1/cpu-import-03.json` 的原始根轨道给出：

| 片段 | 时长 | 周期水平位移 | 源速度 | 正式角色速度 | 所需速率 |
| --- | --- | --- | --- | --- | --- |
| Walk Loop | 4 s | 800.0072 cm | 约 200 cm/s | 200 cm/s | 约 1 |
| Run Loop | 2.5 s | 1250 cm | 500 cm/s | 450 cm/s | 0.9 |
| Sprint Loop | 2 s | 1400 cm | 700 cm/s | 450 cm/s | 约 0.6429 |

主线程重新读取报告端点核对上述数字。它们只证明源根速度与实际移动速度存在差异，
不直接等于支撑脚的滑移速度；后者需要独立接触标注和实际世界关节轨迹。
本轮不引入足底探测、脚锁、骨盆偏移、根偏移、惯性化或骨骼步幅变形。

## 实现约束

C# 提交是否允许匹配；原生从同代原始片段根轨道派生周期平均水平速度，
使用 Jolt 已完成子步的实际水平速度决定播放速率，保留唯一动画时钟。
仅处理地面、方向一致、混合完成且没有动作锁的移动循环。
绑定时拒绝把有显著周期旋转的循环作为此近似的输入。
原地循环、起停、转身、跳跃与落地保持原速。

速率限制为 `[0.8, 1.2]`，每 scaled 秒最多变化 2；
条件不成立或关闭时回到 1。Sprint 的理论速率低于下限，
必须记录饱和后的剩余差值，不能宣称它完全消除了失配。
周期平均只适用于当前已核对的直线匀速片段，不承诺任意弯曲或非匀速循环的瞬时匹配。

变速不调用 Play/Seek，不改变已发布姿态、播放身份或混合源；
目标时钟按 rate 推进，混合进度仍按 scaled delta 推进。
继续候选按实际速率重参数化采样窗口和关节速度；
新数据库候选仍为源速率 1，统计与 42D 资产保持原定义。
这是选帧后的稳态时间缩放，不是多速率候选搜索或完整 stride warping。

## 验收口径

本轮只增加有判别力的模块用例和一条正式控制器质量对照，
不复制 30/60/120 Hz 矩阵，不重复全量 377 项脚本或 112 项动画测试。
复用已有 60 Hz 动作、受阻与暂停检查；生命周期相关的受影响选择集按仓库规则执行两遍。

实际脚滑对照使用相同 60 Hz 输入与 Jolt 控制器。
仅在连续同片段、非混合的稳态区间，用源 sidecar 的 `contact_l/r`
独立标注支撑相，再测量世界足部的水平移动距离和平均速度。
曲线时间/值在线性重构下仅作外部测量标签，不作为 UE RichCurve 等价或运行时曲线执行。
脚踝位置也不作为足底穿地测量。

实景复用既有采集工具，只做 Run 的开/关各一项。
反馈和 RT 截图不能保证同帧，截图进程强制结束不作正常 teardown 证据。
正式角色开放 `MatchSpeed` 实例属性并显示原生速率，供同场景操作对照。

## 当前进度

本条创建时处于实现与验收阶段，尚无最终通过结论。
构建、测试、真实脚漂移和图片结果在后续条目追加；失败证据也保留。

当前事实入口为 [动画运行时](../contracts/animation-runtime-skinning.md)、
[轨迹与特征](../contracts/animation-motion-features.md) 与
[C# 宿主](../contracts/csharp-blueprint-runtime.md)。
阶段目标见 [GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。

## 首轮质量结果与口径修正

2026-10-18：首轮正式控制器四模式对照在 60 Hz 各执行 12 秒，固定排除前三秒，
不扩大 FPS 矩阵。首轮质量测试退出 1，耗时 130.006 秒。
原始证据为 `artifacts/gasp-p6-speed-quality.log/xml`，失败数据完整保留：

| 模式 | 平均倍率 | 高曲线踝部样本/秒数 | 世界水平踝漂移均速 |
| --- | --- | --- | --- |
| Walk off | 1 | 426 / 7.10 s | 18.08246 cm/s |
| Walk on | 0.99998986 | 426 / 7.10 s | 18.06704 cm/s |
| Run off | 1 | 27 / 0.45 s | 36.33184 cm/s |
| Run on | 0.90000847 | 28 / 0.4667 s | 33.50802 cm/s |

四组源相位覆盖均为 100%，稳态没有重选或混合，反馈开关一致且控制器无故障。
Run 未达到预设的 10% 且 5 cm/s 改善线，也未达到 30 样本与 0.5 秒的覆盖要求。
不能由 Run 反馈残差接近零推导脚滑已消除。

主线程与 subagent 只读复核原 UE 动画蓝图导出：
`artifacts/gasp-p0/ue-audit-06/SandboxCharacter_CMC_ABP.copy` 的 FootPlacement
将 `foot_l/r` 配为 FKFootBone，`ball_l/r` 配为 BallBone，
将 `contact_l/r` 配为 SpeedCurveName。原注释另外明确 ground alignment、
foot locking、地面 trace、pelvis 调整以及 Leg IK。
相邻 RemapCurves 节点还显式执行 `contact_l/r=(1-contact_l/r)*100`，
所以不能把导出的原曲线数值直接解释为 UE 接收的速度值。
源曲线是窄峰，未提供已验证的二值足底支撑标签；
测得的踝骨平移也不能区分足部滚动与足底滑动。
因此首轮数字只称为“高 contact 曲线区间的踝漂移”，不作为足底支撑真值。

只读敏感性分析未用于调整阈值：阈值 0.8 的 Run 开启组反而更差；
同一关闭组 27 个区间作同相位反事实重定时约为 31.77 cm/s，
仍未满足原要求的 5 cm/s 改善。相位和左右脚权重确有影响，但不能抹去首轮失败。

据此收窄本分项的验收对象为“原根周期行程与 Jolt 实际行程一致性”。
通过播放器实际时钟采样未经 in-place 的原根轨道，跨周期累积行程，
与 Actor 实际位置差独立比较；不以反馈 `residualSpeed` 作为自己的验证真值。
曲线筛选踝漂移保留为诊断，不放宽原脚滑断言后宣称通过。
正式 `MatchSpeed` 默认改为 false，可在实例属性中开启作同场景对照。
这不完成 P6 的接地质量验收。

首轮 Development Run 图的关闭组仍在混合、开启组已稳态，不能用作同相位脚滑对照。
既有采集工具现统一两组 Run 的稳态等待条件；首轮报告仍保留在
`artifacts/gasp-p6-speed-run-off/` 与 `artifacts/gasp-p6-speed-run-on/`。

## 后续接地分项边界

足底修正须先定义 ball/heel 或明确足底锚点与地面支撑测量，
再接入现成 ozz TwoBone IK 和 Jolt 地面查询。
原生播放器必须分开保留 post-blend/pre-IK 姿态历史和 IK 后渲染姿态，
避免修正后的脚位置污染 Motion Matching 查询；胶囊位移仍由 Jolt 拥有。
平地、坡面与台阶应按明确输入分别验收，不把本次时间缩放当作这些行为已完成。

## 本分项最终验收

Debug 动画定向 16 项通过；其中新增四项覆盖原根周期均速、稳态倍率门控、
继续候选重定时和播放器改速不重启。脚本桥接/时钟/暂停/Stop/传送七项连续两遍通过，
日志为 `artifacts/gasp-p6-speed-targeted-lifecycle.log/xml`。
只新增一项正式控制器行程测试，复用动作与受阻 `/60` 两项，
三项全部通过，耗时 129.214 秒，其中行程测试为 66.714 秒。
该组日志为 `artifacts/gasp-p6-speed-travel-and-actions.log/xml`。
未完整重跑 378 个脚本、116 个动画或 UI/三项 Vulkan runtime gate。

四组各执行八秒，固定排除前三秒，实际稳态积分为 4.9833 秒：

| 模式 | 平均倍率 | 原根行程 | Jolt Actor 行程 | 行程差/稳态秒 |
| --- | --- | --- | --- | --- |
| Walk off | 1 | 996.674886 cm | 996.677002 cm | 0.000425 cm/s |
| Walk on | 1.000001 | 996.674145 cm | 996.677002 cm | 0.000573 cm/s |
| Run off | 1 | 2491.665038 cm | 2242.519775 cm | 49.995702 cm/s |
| Run on | 0.900008 | 2242.516207 cm | 2242.519775 cm | 0.000716 cm/s |

四组源相位覆盖 100%、至少完整一个源周期，反馈一致且控制器无故障。
原根通过独立 `SampleModels` 批采实际相位并补跨周期位移，
没有读取步频残差作为验收真值。这验收的是当前直线循环的根行程匹配，
不取代上文未通过的足部质量结果。

Debug `script_tests` 与动画目标、Development `main`、C# OFF Release
`main/script_tests` 构建通过，均使用 `-j2`。
OFF 受影响批请求用例连续两遍通过，没有重跑全套 79 项。
构建与 OFF 日志分别位于 `artifacts/gasp-p6-speed-debug-final-build.log`、
`artifacts/gasp-p6-speed-development-final-build.log`、
`artifacts/gasp-p6-speed-csharp-off-build.log` 和
`artifacts/gasp-p6-speed-csharp-off-targeted.log/xml`。

关闭组补采为 `artifacts/gasp-p6-speed-run-off-steady/capture.json`，
保留原先开启组的 `artifacts/gasp-p6-speed-run-on/capture.json`。
主线程目视两张 1280×720 图均 Selected Run Loop、blend=1；
开关各自反馈为 rate=1 与约 0.9。采集的输入夹具和正式控制器模块分别记账，
不把两张不同相位图片当作脚滑测量或反馈/RT 同帧证据。
采集通过、stderr 为空，config 精确恢复，全部自有进程已等待结束；
强制终止不作正常 teardown 验收。

65 条边界、文档分层与两仓 diff 空白检查通过。
当前分项以默认关闭的对照功能交付，**P6 整阶段和足底质量仍未完成**。
