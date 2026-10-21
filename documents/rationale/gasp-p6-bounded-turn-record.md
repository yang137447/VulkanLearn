# GASP P6 有界转向记录

> 执行记录，非当前契约。2026-10-18 开始记录 P6 首个用户反馈缺陷；
> 本条不代表 P6、M1 或完整 GASP 验收完成。

## 问题与归因

用户操作正式角色时反馈转向像硬切。资源侧
`Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs` 原先在非零移动输入时
立即将 Actor yaw 设置为移动目标角。已有原生 `0.18 s` 局部 TRS
交叉混合只能平滑骨架相对 Actor 的姿态，无法抵消整个 Actor 世界矩阵的瞬间旋转。
因此本次修正先限制 C# Actor 转向，保留原生搜索与混合的所有权。

## 本次实现

`TurnSpeed` 默认 `540°/s`，初始化时要求有限且大于零。
每次 Tick 从实时 `Transform.Rotation.Y` 求目标差，
将角差归一到 `(-180°, 180°]`，再限制为 `±TurnSpeed * scaledDelta`。
恰好正反 `180°` 都固定为正向，避免该退化目标的转向符号随数值表示改变。
没有输入时保持朝向；中途改变方向或位置传送后都从当前 Transform 重新求差，
没有独立累计 yaw、角速度或缓存目标需要清除。

`Character.Move` 仍立即提交相机空间移动意图。相机的局部 yaw
继续取观察 yaw 与 Actor yaw 的差，无鼠标输入时不随身体转向改变世界观察方向；
相机位置仍可随角色位移或扫球缩臂改变。
`TimeScale=0` 的转向预算为零，不能据此宣称 Tick 内既有鼠标观察也冻结；
Pause、Stop 和重新 Play 继续使用宿主的既有生命周期。
本轮没有扩展资产编辑范围：正式 BP 的 `instanceEditable` 清单不包含 TurnSpeed，
该属性不作为正式 BP 实例 Details 的可调项。

## 验收口径

下表保留开始验收前设定的口径，不作为实测结果；
已经完成的自动化、实景启动与主线程核对结果见后文，未覆盖项仍分别记账。

| 项目 | 预期与边界 |
| --- | --- |
| 30/60/120 Hz | 默认每帧 yaw 上限分别为 `18°/9°/4.5°` |
| 90°/180° | scaled 时间约 `1/6 s`、`1/3 s` 到达，允许一帧离散舍入，不越过目标 |
| 角度环绕 | 检查 `179° ↔ -179°`，用包裹角差比较，避免把等价 Euler 表示当整圈旋转 |
| 反向与中断 | 检查正负 90°、正负 180°、固定正向 180°及转向中改变目标 |
| 可见角色 | 验证真实 World 对象矩阵和 pelvis；冻结局部姿态时可隔离 Actor 转向，正常动画 pelvis 不直接套用 Actor 角速度上限 |
| 相机 | 无鼠标输入时比较世界观察基向量，允许跟随位移和碰撞缩臂 |
| 生命周期 | 检查 Pause、TimeScale=0、Stop/Play、位置传送；保留既有动作候选和 Motion Matching 回归 |

pelvis 世界矩阵应取对象模型矩阵乘播放器的模型关节矩阵；
蒙皮 palette 已乘 inverseBind，不能直接作为模型关节矩阵测量。
片段选中类别、Actor yaw 平滑和实际可见角色连续性分别记录，互相不能替代。

## 保留限制

移动意图立即转向，身体需要时间跟进，因此短时侧向或向后滑动仍可能出现。
原生未来轨迹的朝向帧仍使用当前朝向，不做未来角速度预测。
这次修正没有引入惯性化、根偏移补偿、步幅修正或足部 IK。
后续质量基线仍需单独测量脚滑、穿地、脚离地与根/胶囊偏差，
不能把 Actor 有界转向作为这些问题已经解决的证据。

当前事实入口为 [动画运行时](../contracts/animation-runtime-skinning.md)、
[C# 宿主](../contracts/csharp-blueprint-runtime.md)；
阶段边界见 [GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)。

## 首轮实测与夹具修正

Debug 首轮选择集执行 34 项，31 通过、3 失败，耗时 `405.102 s`。
三个失败均为新增 `FormalGaspStopAndPlayRestartFromAuthoredHeading`
在 30/60/120 Hz 的测试：Stop 按既有宿主契约清空 runtime states，
夹具却继续通过 `State()` 寻找 owner，因此抛出 `Missing actor`。
修正只改测试断言：Stop 后确认 runtime states 为空、mesh 恢复作者矩阵，
重新 Play 后再检查作者 yaw 和第一步转向；资源侧业务 C# 没有因此改动。

其余首轮通过项包含 30/60/120 Hz 的 D 输入 `-90°`、S 输入固定正向 `180°`、
冻结局部姿态的 pelvis/左右脚世界连续性、转向中断、`179° → -179°` 短弧、
Pause、TimeScale=0、传送，以及 TurnSpeed=0 的初始化拒绝。
既有正式控制器动作和墙前专项共六项也全部通过，保留起停/转身/跳落和稳态断言。
本轮没有分别穷举左右 `±90°`、正负 `180°` 或反向跨界的全部组合。

首轮日志为 `artifacts/gasp-p5-turn-smoothing-first-tests.log/xml`，
失败报告保留；文件名沿用此前任务前缀，不改变本记录属于 P6 首项缺陷的范围。
随后新增真实 `GetRenderCamera()->GetForwardVector()` 不随身体旋转的断言，
与 Stop 夹具修正一同重编；受影响的相机六项与 Stop 三项共九项单独复跑。
其余 25 项沿用首轮业务源码未变时的结果，不把这次安排写成完整 34 项再次运行。

主线程独立解析首轮 XML 的六份 `turnTrace`，得到下列实际 Actor 转向数据。
到达时间按固定 dt 与首次到达帧计算，包含实际 CLR/Jolt/World 更新；
该数据不是仅由 `degrees / TurnSpeed` 公式填表。

| 输入与目标 | Hz | 最大单帧角度 | 首次到达 scaled 秒 |
| --- | --- | --- | --- |
| D，`-90°` | 30 | `18°` | `0.1666667` |
| D，`-90°` | 60 | `9°` | `0.1666667` |
| D，`-90°` | 120 | `4.5°` | `0.1666667` |
| S，`+180°` | 30 | `18°` | `0.3333333` |
| S，`+180°` | 60 | `9°` | `0.3333333` |
| S，`+180°` | 120 | `4.5°` | `0.3333333` |

## 最终定向复验

Debug 最终重编退出 0，受影响的九项全部通过、零失败，
耗时 `60.399 s`，测试退出 0。结合首轮未受影响的 25 项，
本次覆盖 34 个不同用例，不声称完整 34 项最终单遍或连续两遍通过。
`--gtest_list_tests` 退出 0，实际注册总数为 377；
P5 完整 349 项的历史记录继续保留，本轮没有运行完整 377 项。

日志为 `artifacts/gasp-p5-turn-smoothing-final-build.log`、
`artifacts/gasp-p5-turn-smoothing-final-tests.log/xml` 和
`artifacts/gasp-p5-turn-smoothing-test-list.log`。
正式动作回归的三帧率均曾选中 Turn180L，
但首次入选帧和当时 Actor yaw 不同，不宣称跨帧率逐帧选择一致。

Development 正式 playground 加载成功，主线程实际进入 Play/Immersive 并核对出图，
双线程 World/Graph 提交正常，`artifacts/gasp-p6-turn-user.stderr.log` 为空；
对应 stdout 为 `artifacts/gasp-p6-turn-user.stdout.log`。
单次键事件未形成可确认的持续移动输入，因此这次实窗启动不计动态转向观感验收。
程序按用户实操用途保留运行，不把仍在运行写成正常 teardown。

该缺陷的自动化检查完成；动态观感仍由用户操作确认。
完整 P6 的脚滑/接地质量与根、步幅、足部修正没有因此完成，
本轮也没有重跑 UI/动画全量、C# OFF 或三项 Vulkan runtime gate。

文档分层校验通过：63 条路由、65 条边界规则索引和 360 个相对链接；
65 条架构边界审计与 `git diff --check` 退出 0，后者仅有既有 LF/CRLF 提示。
日志为 `artifacts/gasp-p6-turn-final-doc-check.log`、
`artifacts/gasp-p6-turn-boundary-check.log` 和
`artifacts/gasp-p6-turn-final-diff-check.log`。
