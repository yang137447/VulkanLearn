# GASP P6 足部接地记录

> 执行记录，非当前契约。接续 P6 步频分项；本条不代表 P6 或完整 GASP 完成。

## 范围

本次提供默认关闭的 `FootPlacement` 对照开关，C# 只提交开关，
原生在交叉混合后执行静态地面探测、足部锁定、受限视觉骨盆偏移及两骨 IK。
复用 ozz `IKTwoBoneJob` 和已有 Jolt 查询过滤，不另建数学引擎或渲染调试系统。
UE 原工程只读，正式关卡对象、贴图及光照保持原有数据。

`PreIkModels()` 保留 post-blend/pre-IK 结果，供实际姿态历史和 MM 查询读取；
`Models()` 与不可变 `SkinPose` 为修正后的渲染结果。
冻结混合源不含 IK，Jolt 继续独占胶囊与 Actor 位移。
足部反馈与全部 section 的 palette/bounds 在候选求值完成后一起发布。

每只脚使用绑定姿态 ball 在绑定根平面的投影，转换成 foot 局部参考点。
这是单参考点近似，不是 heel/ball 双点支撑，也不是鞋底网格真值。
接触门控读取修正前的世界足速与高度，含进入/退出迟滞；
不把 UE 导出 `contact_l/r` 原曲线当作二值支撑标签。
高抬脚、快速摆腿没有通过接触门控时保留原动画。

首版只支持静态支撑和规定的直接双腿骨链。
移动平台、完整 UE FootPlacement、鞋底滚动、台阶遍历质量、
根偏移和惯性化不属于本轮通过声明。

## 精简验收口径

沿用用户要求，新增少量有判别力的 CPU 边界断言，
真实 CLR/Jolt 只做固定 60 Hz 的 18° 横坡开关对照，
每组两秒预热、两秒稳态采样，不扩 FPS 矩阵。
从绑定骨架独立重建参考点与足部 up，测量到已知坡面的有符号距离与法线夹角，
不用求解器自己的 `targetError` 作为质量真值。
目标为开启后稳态参考点误差不超过 1 cm、法线夹角不超过 5°，
并检查关闭到开启的改善及 Actor、pre-IK、MM 查询与时钟一致。
若失败，保留报告，不能调松阈值后称为原目标通过。

暂停/零时间、关闭、Seek/硬切、空中、传送、失去支撑及非法请求
复用最近的模块/生命周期选择集；生命周期受影响项连续运行两遍。
外部截图只做同一坡面开关各一项，反馈不保证与 RT 精确同帧，
强制结束采集进程不作为正常 teardown 证据。

## 验收状态

本条创建时实现与验证正在进行，尚无最终通过结论。
后续追加实际构建、测量、失败与图片结果，不覆盖历史证据。

前一分项未通过的高 contact 踝漂移结果继续保留在
[稳态步频记录](gasp-p6-speed-matching-record.md)。
本次静态坡面接地验收不追溯改变该结果，也不能证明走跑全周期脚滑已解决。

## 实现审查与 CPU 结果

首版“探测命中就校正”的实现会压住摆腿，审查后改为只有通过 pre-IK 足速/
高度迟滞的 planted 足进入骨盆、腿链与法线求解。未锁定时保留原动画。
另修复一次性动画端点不再生成新基础姿态时的锁足冻结；
端点正时间仍可处理离地/支撑退出，显式暂停则保持既有姿态与诊断。

Debug 首次构建因 MinGW COFF section 数量上限失败，日志为
`artifacts/gasp-p6-foot-debug-build.log`。沿用仓库现有模式，
仅对 `footPlacement.cpp` 添加 `-Wa,-mbig-obj`，不修改优化或调试配置。
随后一次构建暴露测试宏对 Eigen 模板逗号的拆参，修为具名值后保留原断言，
失败日志为 `artifacts/gasp-p6-foot-debug-retry-build.log`。

`SkeletalBlend` 的 16 项连续两遍通过，其中新增四项接地边界。
覆盖世界锚点、绑定参考点/法线、旧 SkinPose 不可变、pre-IK/冻结混合源隔离，
以及摆腿、空中、无支撑、传送、大 delta、关闭、Seek、端点和显式暂停。
日志与末遍 XML 为 `artifacts/gasp-p6-foot-cpu.log/xml`。
这是 CPU 夹具结果，不代替正式骨架、Jolt 或实景验收。

## 首轮真实链路失败

六项桥接/生命周期选择集连续两遍均有两项失败：
`MotionMatchingBindsFormalStatisticsAndFreezesPublishedSearchValues` 与
`TeleportClearsMotionQueryHistoryAndOldTransition` 在预热后 query 未就绪；
同步 ProbeGround 过滤和其余四项通过。日志为
`artifacts/gasp-p6-foot-lifecycle.log/xml`，不以部分通过代替整组通过。

首轮 18° 对照同样失败，日志为 `artifacts/gasp-p6-foot-slope-first.log/xml`。
开/关各 240 个稳态双足样本，最大参考点平面误差均为 7.150176 cm，
最大法线夹角均为 18.471606°，开启组活动帧为 0/120；
未达到 1 cm/5° 目标，不调整阈值。
记录显示原生原因始终 `root-motion`、searchCount=0，
与 MM 入口应先转为 in-place 的代码不一致。

主线程发现暂停标记在先前构建进行中插入了播放器字段布局，
存在部分编译单元按旧 bool 偏移读取 `InPlace` 的混编证据。
源码冻结后刷新共享头的依赖时间，统一重编全部受影响编译单元；
这是构建一致性修复，不是修改动作许可、搜索策略或质量门槛。
后续结果另行追加，原失败报告保留。

## 统一重编后的定向结果

统一重编后 `InPlace`、query、传送回归恢复正常。
六项生命周期选择集连续两遍均为五通过、一失败，
日志为 `artifacts/gasp-p6-foot-lifecycle-coherent.log/xml`。
唯一失败是持续 200 cm/s 移动 45 帧后要求 `plantedDuringPlay`，
该夹具不满足必然进入接触门控的前提。
修正仅增加真实停止、Idle 混合完成后的接地阶段，再验收 Pause/零时间/Stop；
原移动搜索、节流与冻结查询断言保留，不放宽生产接触门槛。

统一重编后的首个 18° 坡面结果仍未通过，报告为
`artifacts/gasp-p6-foot-slope-coherent.log/xml`。
开关各 240 个稳态双足样本，开启活动帧为 120/120。
关闭时最大/平均参考点误差为 7.193770/6.331438 cm，
开启后为 3.212400/0.154258 cm；
最大法线夹角由 18.331475° 降到 0.019783°。
Actor、完整 MM 反馈/查询、片段时钟逐帧一致，全部 pre-IK 骨矩阵最大差为零。
平均改善不代替最大 1 cm 目标；这轮仍记为质量失败。

最大误差出现在第 224 帧：Jolt 角色沿坡缓慢位移，旧锁足达到 20 cm
释放距离后在当帧重建更低的锚点，骨盆仍只有 -3.3689 cm 偏移。
后续每帧 1 cm 的下移使误差依次降为 2.333、1.487、0.716 cm。
修正方向是用垂直几何可达区间，同时为当帧地面投影的潜在重锚点预留骨盆高度；
继续保留 15 cm 上限和双向 60 cm/s 变化限制，不用骨盆瞬移掩盖足部误差。

Development C# ON 与 Release C# OFF 的统一重编均通过。
OFF 仅复跑 `CandidateBatchFailureDoesNotPublishEarlierMotionBinding`，
连续两遍通过，日志为 `artifacts/gasp-p6-foot-csharp-off-coherent-tests.log/xml`。
这不代表重新执行 OFF 的全部已注册测试。

正式 playground 的说明通过同一个 `vl_asset_edit` host 会话执行
get_document/get_capabilities/modify/validate/save，revision 为 1→2→3，
独立深比较确认除 description 外语义完全一致。
证据为 `artifacts/gasp-p6-foot-playground-description-api.json`。

## 可达性修正与正式质量复验

实现上述精确垂直可达上界与潜在重锚点预留后，保留全部接触门槛、
`soften=0.98`、15 cm 上限及上下 60 cm/s 变化限制。
既有 CPU 坡面用例扩展 24 cm/120 帧的缓慢位移，确实触发 20 cm
重锚，逐帧检查参考点/锚点误差小于 1 cm，骨盆单帧变化不超过 1.0001 cm。
没有新增测试目标或扩大帧率矩阵。
最终 `SkeletalBlend` 16 项连续两遍通过，
日志为 `artifacts/gasp-p6-foot-final-cpu.log/xml`。

只读审查另发现自然 Completed 与显式暂停的组合漏洞：
一次性动画完成后再暂停，MM 修改许可可能恢复播放。
给播放器增加只读暂停 getter（不改变对象布局），MM 入口优先尊重显式暂停；
扩现有 Console Pause 用例覆盖完成、暂停、改许可、主动恢复。
原有自然完成后撤销动作锁用例继续单独覆盖自动完成恢复。

最终固定 60 Hz 的真实 CLR/Jolt/正式 GASP 18° 横坡结果：

| 指标 | 关闭 | 开启 |
| --- | --- | --- |
| 稳态双足样本 | 240 | 240 |
| 接地帧 / 活动帧 | 120 / 0 | 120 / 120 |
| 最大绝对参考点误差 | 7.193770 cm | 0.272459 cm |
| 平均绝对参考点误差 | 6.331438 cm | 0.233384 cm |
| 最大法线夹角 | 18.331475° | 0°（当前浮点测量精度） |
| 平均法线夹角 | 18.103976° | 0°（当前浮点测量精度） |

所有稳态样本均计入，没有按 planted 筛除误差帧。
达到最大 1 cm / 5° 及平均改善 70% 目标；
开启/关闭的 Actor、完整 MM 反馈/查询与片段时钟逐帧一致，
全部 pre-IK 骨矩阵最大差为零。
实际胶囊仍沿坡缓慢位移，IK 没有改变这一物理行为；
锁定超出半径会重建锚点，不宣称角色或鞋底在世界空间永久不动。

与既有动作、撞墙 `/60` 合计三项通过，耗时 143.732 秒；
动作与受阻均启用 MatchSpeed 和 FootPlacement，空中帧检查 IK 退出，
没有扩展 30/120 Hz 或重跑完整脚本集。
日志为 `artifacts/gasp-p6-foot-final-quality-actions.log/xml`。

Development 双线程、1280×720 的同坡面开关各一张实景截图完成，
报告为 `artifacts/gasp-p6-foot-slope-visual-off/capture.json` 与
`artifacts/gasp-p6-foot-slope-visual-on/capture.json`。
主线程目视确认画面、角色及修正后的腿部姿态可见，开启反馈为双脚 planted；
图中辅助线和小尺度脚部不能替代上述独立数值真值。
两组均为 Idle、grounded 的稳态采集，托管反馈与 RT 图片不保证精确同帧。
采集进程被强制结束并等待退出，退出码 1，不作为正常 teardown 通过证据。
工具报告 config 已按原字节恢复。

最终七项桥接/生命周期选择集连续两遍全部通过，无失败或跳过；
日志为 `artifacts/gasp-p6-foot-final-lifecycle.log/xml`。
其中真实静止接地后验证 Pause/零时间冻结与 Stop 清锁；
Console 用例验证循环暂停，以及自然 Completed 后显式暂停、改许可、再主动恢复。
独立的自然完成用例仍通过，没有用“所有 Completed 都停止搜索”规避问题。
Debug 脚本/动画目标和 Development main 的最终构建通过，
日志为 `artifacts/gasp-p6-foot-final-debug-build.log` 与
`artifacts/gasp-p6-foot-final-development-build.log`。

Release C# OFF 的最终 main/script_tests 构建通过，
仅复跑受影响的候选批次用例两遍，均通过；
日志为 `artifacts/gasp-p6-foot-final-csharp-off-build.log` 与
`artifacts/gasp-p6-foot-final-csharp-off-tests.log/xml`。
没有将既有 OFF 全量结果记作本轮实跑。

## 分项结论

静态坡面单参考点接地初版通过本条规定的定向技术验收，
正式角色 `FootPlacement=false` 的默认值保持，开启后由原生执行并发布诊断。
本轮最终累计覆盖 16 个动画 CPU 用例、10 个不同脚本用例，
生命周期受影响项连续两遍；同名候选批次另在 C# OFF 环境复验两遍。
65 条架构边界审计、40 场景 schema 校验通过，采集后 config 已恢复，
自有采集进程均已结束。文档和空白检查另以最终门禁日志为准。

未重新执行完整脚本/UI 集、全量 CTest 或三项 Vulkan runtime gate。
不修改前一分项脚滑失败的结论，不宣称完整鞋底、移动平台、台阶遍历、
走跑全周期消滑、根偏移、步幅变形或惯性化通过。
P6 整阶段仍未完成。

## 调试可视化补充

`World::CaptureDebugDraw` 的足部修正线由单色 green 改为 8 段红→绿插值，
pre/post ball 与 heel 各一条 3 px 梯度线；红点表示 pre-IK、绿点表示 post-IK，
青色为 probe/法线，黄色球为锁定锚点。该组线不再依赖 Motion 轨迹开关，
只要 `FootPlacement.active` 就通过 `Engine.Motion` publisher 提交。

验收场景 `SC_gasp_foot_debug.json`：正式 CharacterBlueprint 开启
`FootPlacement=true`、`MatchSpeed=true`，以 `debugdraw on` 和
`debugdraw category Motion on` 启动；`debugdraw status` 读到
`78 lines / 8 points`。1280×720 截图 `artifacts/gasp-foot-debug.bmp`
的脚部区域检测到红色 34 px、绿色 200 px（bbox 约 x=590–672, y=533–598），
与红→绿辅助线一致；这是像素计数，不是视觉模型判读。
`artifacts/gasp-foot-debug.log/.err` 记录状态与退出，err 为空。

限制：显示的是 ball/踝下代理点，不是鞋底网格；只验证调试几何与开关独立，
不改变 P6 的滑移/穿地结论。
