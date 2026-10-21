# GASP P6 动态质量执行记录

状态：执行记录，非当前契约。接续静态接地分项，用户授权完成 P6 剩余工作。
原始 UE 项目保持只读；日志和中间结果保留在引擎 `artifacts/`，
临时运行资源仅进入资源根 `Generated/Validation/`。

本记录包含三次口径修正和多项求解器缺陷修正。首轮失败数据全部保留，
不把失败尝试改写为通过；最终结果只对应文末列出的冻结源码与日志。

## 实施前固定的范围

- 正式 C# 控制器的走跑全周期支撑与释放，足部双参考点及台阶边缘支撑。
- grounded 无锁足时骨盆平滑回位。
- 可关闭的有限时惯性化与有界视觉根位置/朝向修正。
- 正式关卡组合出图与已有跳跃、落地、受阻、暂停和重置路径验收。
- 移动平台不是本轮必需项，不把静态支撑结果推广到移动平台。

骨架没有 heel 骨。踝下投影和 ball 投影必须明确称为骨架参考点；
它们不能独自证明真实鞋底网格没有穿透。真实网格检查与近似偏差单独记录。
UE 原 `contact_l/r` 经过蓝图重映射，不当作二值支撑标签。
旧曲线筛选踝漂移失败仍保留于步频记录。

## 先冻结的质量口径

动态支撑标签由正式源片段的密集 raw-root 轨迹及双参考点运动/高度派生，
在读取修正输出之前固定；不按 solver 的 planted 状态筛掉误差样本。
固定 60 Hz，不扩无关 FPS 矩阵。

走跑各进行相同输入的关闭/开启对照，MatchSpeed 保持相同。
稳态至少覆盖一个完整源周期，固定支撑相位覆盖至少 95%。
初始目标为每足累计滑移/平均切向速度至少降低 50%，
固定支撑窗最大漂移不超过 3 cm，支撑参考点穿入不超过 1 cm。
源动作存在滚动时分别测量实际支点与另一点，不强迫摆腿贴地。
任何无法满足或不适用的目标都须保留失败数据并解释，不能静默放宽。

台阶采用一直前行的六段踏面，深度各 80 cm、宽 300 cm，
顶高依次为 10、20、30、20、10、0 cm。
高度真值从作者矩形平面解析，不复用求解器的 Jolt 命中作为 oracle。
边缘样本保留单列，检查实际跨越、支撑穿入/悬空、摆腿释放、
骨盆单帧变化及胶囊连续性；允许边缘单支点，不把两踏面连接成虚构斜坡。

足部开关对照要求 Actor、MM 查询/结果、唯一片段时钟和全部 pre-IK 姿态一致。
视觉根修正必须另外保留不含根修正的查询姿态，避免反馈污染。
惯性化改变基础姿态与后续查询是其正常行为，不能声称其开关下查询完全相同。
所有 section 仍在候选成功后一起发布，旧快照不可变。

## 实现摘要

### 动态足部支撑

`FootPlacementSolver` 从单一 ball 投影扩展为 ball 与“踝下 heel 代理”双参考点。
两点分别从 pre-IK 参考点上方 30 cm 向下探测 70 cm；只接受法线 Y 至少
`cos(45°)`、非 trigger、同 owner 掩码的最近静态支撑。

- 进入锁定：历史有效、该点世界水平速度不超过 40 cm/s、到平面距离不超过 10 cm。
- 释放：速度超过 80 cm/s、距离超过 16 cm、锚点偏差超过 20 cm 或支撑身份变化。
- 支点：两点共面且都可锁时取更贴近地面的参考点，1 cm 内保持原支点；
  另一参考点低 1 cm 以上时切换支点，跟随 heel→ball 滚动。
- 非共面踏面取更高/可锁的踏面作为单支点，不把台阶边缘连成虚构斜坡。
- 另一参考点只在同一支撑面内做非穿透修正；跨踏面时允许其悬在下一级踏面，
  不把支撑点顶离锚点。
- 骨盆下移按当前锚点和当帧重锚候选的腿长球面截面计算，最多 15 cm、
  每秒 60 cm；未锁足参考点低于其地面时限制下移量，避免摆动脚被压入地面。
- 数值噪声小于 `1e-4 cm` 的骨盆偏移归零，避免发布虚假的 active 状态。

`PreIkModels` 仍是 post-blend/pre-IK；`Models`、palette 和 bounds 使用 IK 后结果。
Actor/Jolt 继续独占胶囊位移。反馈新增每足 pre/post ball/heel、支撑身份、
锚点、目标误差、contactMode（`ball`/`heel`/`none`），以及只读诊断
`soleSpeed/heelSpeed/soleGap/heelGap/canPlantBall/canPlantHeel`。

### 惯性化

`FiniteInertialization` 接收过渡边界两侧的局部运动和 1 ms 邻域速度，
用五次 Hermite 输出有限时偏移：起点匹配源位移/速度、终点值与速度精确归零。
四元数使用 SO(3) 指数/对数及左雅可比搬运目标角速度；scale 在 log 空间处理。
中断时以当前显示姿态重新取源/目标差值，不累积旧层。
只作用于后续过渡；`Play` 和硬切关闭，暂停/零 delta 冻结，目标时钟仍唯一。

### 视觉根偏移

`RootOffsetSolver` 只补偿实际水平速度与 yaw 速度的跳变，输出有界世界
XZ 平移（≤20 cm）与 yaw（≤45°）；匀速不持续注入误差。
临界阻尼半衰期 0.08 s，使用解析指数而非分步近似。
每帧从 `displayedLocals` 重新生成视觉根姿态，`PreIkModels` 单独保留
不含根修正的查询姿态；硬切、Seek、传送、root-motion、暂停和关闭清理。
它不写 Actor/Jolt，也不改变 Motion Matching 的下一帧输入。

### C# 与关卡

`Animation.Match` 的 `footPlacement/inertialization/rootOffset` 开关默认关闭，
正式 `GaspCharacter.blueprint.cs` 提供三个实例属性；改变属性会重新提交 Match。
HUD 显示接地原因/骨盆偏移与根偏移状态。

## 验证预算

使用相邻 GoogleTest 套件和正式 CLR/Jolt 控制器质量夹具，
生命周期受影响选择集连续两遍。所有共享布局完成并冻结后统一构建，
避免不同翻译单元读取不同对象布局。
Debug 和 Development 构建最多各 `-j2`，不并发两个截图采集器。
不增加 runtime test 标志，不把强制终止的截图进程计作正常 teardown。

## 失败与口径修正

### 1. Run 源支撑窗为零

首次固定口径要求 `|垂直速度| <= 20 cm/s`。240 Hz 的垂直差分在触地瞬间抖动，
把 Run 的连续支撑切成 2–4 个 bin，`>= 0.05 s` 窗口数为 0，
Walk 也被切成 10–13 段。失败报告为
`artifacts/gasp-p6-dynamic-quality-01.xml` 与 `-02`。

修正只改测量：支撑判定用水平速度不超过 40 cm/s 且高度不超过周期最低点 +3 cm；
垂直速度仍写入每样本证据，但不再作为硬门。阈值 50%/3 cm/1 cm/95% 不变。

### 2. 支撑窗被支点切换拆碎

按逐样本“更慢参考点”切窗时，Run 的 ball/heel 速度接近，窗口只剩 1–4 帧。
修正为：窗口按“任一参考点连续支撑”切分；支点逐样本取更低的参考点，
1 cm 内保持，另一点低 1 cm 以上才切换。这样 heel→ball 滚动属于同一支撑相位。

### 3. 触地/离地过渡被算作滑移

求解器在支撑窗首帧才落点、末帧释放；把这两帧算入滑移会同时惩罚落点修正和
释放回弹。修正为每端最多排除两个 60 Hz 渲染帧（240 Hz 的 8 个 bin），
仍计入 stanceBins 与覆盖统计；不按 solver planted 状态筛样本。
片段切换后的前 8 个稳态帧也不计入，因为脚速历史来自旧片段。

上述排除只作用于支撑窗边缘和片段切换，不改变支撑窗内部的样本与阈值。

### 4. 求解器缺陷

- 支点原先固定偏好 ball，随后又长期锁 heel；改为双点更低者 + 1 cm 滞回 + 滚动切换。
- 骨盆下移会同时压入未锁足的摆动脚；新增摆动参考点地面间隙约束。
- 跨踏面时另一参考点被强行抬到当前平面，导致边缘支撑点悬浮 1.9 cm；
  改为只在同一支撑面内做非穿透修正。
- 极小正间隙会把骨盆抬升 `8.6e-4 cm` 并让状态保持 active；增加间隙方向约束与
  数值归零。

## 最终结果

### 构建与模块测试

- Debug `animation_preflight_tests` 与 `script_tests` 最终增量构建退出 0，
  日志 `artifacts/gasp-p6-dynamic-final-build.log`；此前全量构建为
  `gasp-p6-dynamic-build-01.log`、`gasp-p6-dynamic-script-build-02.log`。
- `animation_preflight_tests` 129 项连续两遍全部通过、零失败/跳过，
  日志 `artifacts/gasp-p6-dynamic-cpu-02.log/xml`、`gasp-foot-lock-cpu-full.log/xml`。
- 受影响生命周期与静态坡面选择集 8 项连续两遍全部通过，
  日志 `artifacts/gasp-p6-dynamic-lifecycle.log/xml`（71.848 s/遍）。
- C# OFF Release `script_tests` 79 项连续两遍全部通过，
  日志 `artifacts/gasp-p6-dynamic-csharp-off-build-01.log`、
  `gasp-p6-dynamic-csharp-off-tests.log/xml`。
- C# ON Development `main.exe` 构建退出 0，
  日志 `artifacts/gasp-p6-dynamic-development-build.log`。

### 正式控制器动态质量夹具

`ScriptAnimationTest.FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows`
固定 60 Hz、MatchSpeed 两组相同、走/跑各 10 s、台阶 6 s。
首次通过为 `artifacts/gasp-p6-dynamic-quality-07.xml`，第二遍为 `-08.xml`，
两遍各 189 s，均 1/1 通过。下表为 `-07` 的数值（`-08` 同量级且通过）：

| 组 | 足 | 关闭路径/平均速度 (cm) | 开启路径/平均速度 (cm) | 路径降幅 | 支撑窗漂移 (cm) | 支撑穿入 (cm) | 支撑间隙 (cm) |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Walk | L | 17.839 / 6.047 | 5.020 / 1.702 | 71.9% | 0.808 | 0.256 | 0.546 |
| Walk | R | 12.203 / 4.282 | 3.952 / 1.387 | 67.6% | 0.514 | 0.265 | 0.291 |
| Run | L | 6.671 / 9.762 | 1.329 / 1.946 | 80.1% | 0.249 | 0.128 | 0.956 |
| Run | R | 7.901 / 12.156 | 1.141 / 1.756 | 85.6% | 0.163 | 0.088 | 0.677 |
| Stairs | L | 2.785 / 6.685 | 0.160 / 0.385 | 94.3% | 0.059 | 0.139 | 0.139 |
| Stairs | R | 3.282 / 4.689 | 0.559 / 0.799 | 83.0% | 0.117 | 0.204 | 0.292 |

- Walk/Run 的支撑相位覆盖均为 1.00；源秒数分别为 6.983 s（一个 4 s 周期以上）
  和 6.285 s（2.5 s 周期以上）。
- Stairs 的支撑样本只有 25/42 帧、覆盖 0.264/0.414。该组只作单参考点台阶烟测，
  不宣称整段踏面或完整周期覆盖。
- 台阶最高踏面 30 cm；六段踏面全部跨越（`treadsCrossed` 全 true）。
- 足部开关下 Actor、MM 查询/结果、片段、时钟和全部 pre-IK 姿态逐帧一致：
  `offOnStateMismatches=0`、`maximumPreIkDifference=0`、`failedFrames=0`。
- 代理点穿入全部不超过 1 cm；漂移全部不超过 3 cm；步频平均降幅 67.6%–85.6%，
  超过 50% 目标。

### 实景出图

三个独立采集组，均为 1280×720、单线程、`MatchSpeed=true`：

| 组 | 阶段 | 结果 | 报告 |
| --- | --- | --- | --- |
| 对照（全部关闭） | Walk/Run/Stairs | 3/3 截图与阶段行为通过 | `artifacts/gasp-p6-dynamic-off-20261019b/capture.json` |
| 足部开启 | Walk/Run/Stairs | 3/3 通过；开启组反馈 `Planted`、pelvis 偏移非零 | `artifacts/gasp-p6-dynamic-foot-on-20261019/capture.json` |
| 组合开启 | Walk/Reverse/Jump/Land | 4/4 通过；暂停后取图 | `artifacts/gasp-p6-dynamic-combined-on-20261019b/capture.json` |
| 组合开启（停步） | Stop/Blocked | 2/2 通过；Stop 根偏移 `smoothing`、位移 -2.334 cm；Blocked 足部 `planted`、pelvis -0.752 cm | `artifacts/gasp-p6-dynamic-combined-stop-20261019/capture.json` |

组合组同时开启足部、惯性化和根偏移，覆盖 Walk、180° 反向、Jump、Land；
Stop 组在停步瞬间取到根偏移 `smoothing`，Blocked 组取到足部 `planted` 与骨盆偏移。
行为、查询与截图均通过。截图前托管反馈与 RT 图仍不是精确同帧，
具体见各组 `capture.json` 的 `screenshotBehaviorPairing` 字段。

这些图是组合行为与画面完整性证据，不是足底像素验收：角色在 1280×720 画面中较小，
地面材质对比低，视觉模型无法据此可靠判断脚底接触；台阶组在六段全部跨越后取图，
踏面已落在角色身后、不在画面内。足底接触结论仍只取质量夹具的代理点数值，
真实鞋底网格的穿透另见下节。

### 文档与静态检查

`py -3 tool/validate-docs.py` 通过：63 路由、65 规则、378 相对链接；
架构边界审计通过；40 个场景 schema 零失败/零豁免；`git diff --check` 退出 0，
只保留既有 LF/CRLF 提示。日志为 `artifacts/gasp-p6-dynamic-final-docs.log`、
`gasp-p6-dynamic-boundary.log`、`gasp-p6-dynamic-scene-schema.log`、
`gasp-p6-dynamic-diff-check.log`。

## UE LockType 对照

2026-10-19：按 UE 5.8 `FAnimNode_FootPlacement` 的 `PivotAroundBall` /
`PivotAroundAnkle` 把求解器支点改为显式模式，并在同一 P6 夹具上各跑一遍。
UE 源码结论：IK 只写 `FootBone`（heel/ankle joint），`BallBone` 是 secondary
plant bone，参与 plant-plane 距离和 pivot；默认 `PivotAroundBall`，自动判断
ball/ankle 仍是 TODO；`FPlantResult` 不输出 ball，toe rolling 是 TODO。
因此本实验不新增 toe IK，只比较锚点参考。
质量夹具用 `VL_FOOT_LOCK` 选择 `Auto`/`Ball`/`Ankle`，默认 `Auto`；
开关只选择 production Match 的模式，不改变任何通过阈值。

| 模式 | Walk L/R 路径 | Run L/R 路径 | Stairs L/R 路径 | Run 穿入 L/R | 结论 |
| --- | --- | --- | --- | --- | --- |
| 关闭 | 17.839 / 12.203 | 6.671 / 7.901 | 2.785 / 3.282 | 1.502 / 1.150 | 对照 |
| `Auto`（默认） | 5.020 / 3.952 | 1.329 / 1.141 | 0.160 / 0.559 | 0.128 / 0.088 | 全部阈值通过 |
| `Ball` | 5.063 / 4.917 | 1.337 / 1.129 | 0.687 / 1.264 | **1.333** / 0.908 | 仅 Run 单帧 1.33 cm 超 1 cm |
| `Ankle` | 16.606 / 15.090 | 8.418 / 8.062 | 2.114 / 1.887 | 1.036 / 0.718 | 滑移几乎不降，Walk 穿入 3.3 cm |

- `Ball` 与 `Auto` 在走跑滑移上同量级，说明 UE 默认 ball 支点在我们骨架上基本可用；
  失败点是一帧 ball 不可锁而 heel 可锁：求解器不落点，代理点保留源动画的 1.33 cm
  穿入。尝试过的两类 ankle 兜底都会让 Walk/Stairs 的支点被错误固定
  （`gasp-foot-lock-ball-quality-02/-03`），因此没有保留。
- `Ankle` 直接固定“踝下投影”而不是真实 heel 骨，等于把脚掌前部当摆腿，
  滑移接近关闭组，不能作为可用默认。UE 的 `PivotAroundAnkle` 语义依赖其
  foot 骨/骨架，不可照搬。
- 结论：保留 **`Auto` 为默认**（P6 通过），`Ball`/`Ankle` 作为 UE 对照的显式模式，
  通过 C# `Match(..., footLock)` / BP `FootLock` 选择；不增加 toe IK 目标。
  若要进一步逼近 UE 默认，应先修“ball 不可锁而 heel 可锁”的单帧兜底或 UE 的
  plant-plane 重建，而不是再加一个脚尖 IK。

## 限制与未验收

- **真实鞋底网格**：验收只覆盖 ball 与踝下 heel 两个绑定代理。
  同一批 10 s 夹具中网格鞋底顶点最大穿入为 Walk 5.522 cm、Run 4.498 cm、
  Stairs 9.037 cm（关闭组 2.614/3.102/10.011 cm）。这不是通过项，
  也不能由代理点误差推出；完整鞋底、鞋跟形状和脚趾滚动仍未验收。
- **台阶质量**：仅单参考点、固定 60 Hz 烟测；覆盖不足一个完整源周期，
  移动平台、连续上下坡、台阶跑步和胶囊边缘连续性未验收。
- **惯性化/根偏移**：模块覆盖 C1、终点、重复中断、暂停、边界与视觉/查询隔离；
  实景只验证组合开关下的动作与截图，没有逐项像素差异或主观观感评分。
- **FPS 矩阵**：未扩 30/120 Hz；动态质量数值只对应固定 60 Hz。
- **移动平台**：地面探针明确拒绝非静态支撑，移动平台保持未实现。

P6 的自动化分项到此完成；真人键鼠全矩阵、真实鞋底网格与完整 UE FootPlacement
等价性仍不属于本次通过范围。

## 证据索引

- 失败迭代：`artifacts/gasp-p6-dynamic-quality-01.xml` … `-06.xml` 及同名 `.log`。
- 最终两遍：`artifacts/gasp-p6-dynamic-quality-07.xml`、`-08.xml`。
- 动态质量单测：`artifacts/gasp-p6-dynamic-cpu-02.log/xml`。
- 生命周期：`artifacts/gasp-p6-dynamic-lifecycle.log/xml`。
- C# OFF：`artifacts/gasp-p6-dynamic-csharp-off-build-01.log`、
  `gasp-p6-dynamic-csharp-off-tests.log/xml`。
- 构建：`artifacts/gasp-p6-dynamic-build-01.log`、
  `gasp-p6-dynamic-script-build-02.log`、`gasp-p6-dynamic-final-build.log`、
  `gasp-p6-dynamic-development-build.log`。
- LockType：`artifacts/gasp-foot-lock-auto-quality-01.xml`、
  `gasp-foot-lock-ball-quality-01.xml`、`gasp-foot-lock-ankle-quality-01.xml`；
  兜底失败保留 `gasp-foot-lock-ball-quality-02.xml`、`-03.xml`；
  模块/脚本为 `gasp-foot-lock-unit-tests-01.log`、
  `gasp-foot-lock-script-tests-01.log`。
- 截图：`artifacts/gasp-p6-dynamic-off-20261019b/`、
  `gasp-p6-dynamic-foot-on-20261019/`、`gasp-p6-dynamic-combined-on-20261019b/`、
  `gasp-p6-dynamic-combined-stop-20261019/`。

阶段目标见 [GASP 计划](../plan/architecture/gasp-csharp-animation-plan.md)；
当前实现边界见 [动画运行时契约](../contracts/animation-runtime-skinning.md)。
