# 动画运行时正式版补齐路线

> 规划文档，不是契约。基线为 2026-11-04 的 `codex/valley-echo-locomotion`：
> 已具备单片段播放、交叉淡入/惯性化、归一化 SyncGroup 相位、Sequence/Montage
> notify 和手动曲线采样。本文只讨论正式版还缺什么、按什么顺序补、如何验收。

## 0. 当前边界

先把“已有”与“看起来有”分开：

| 能力 | 当前状态 | 正式版差距 |
| --- | --- | --- |
| Sequence 播放 | 单 `SkeletalPlayback` 单目标片段 | 无 BlendSpace、无 layered pose、无 additive |
| 过渡 | `TransitionTo` + 线性交叉/有限惯性化 | 无 UE transition rule graph、无 exact inertialization 曲线 |
| SyncGroup | R1 已落地：`AnimationSyncGroupRegistry` leader election + follower 每 tick 校正 + Local/Component scope | 多 linked AnimBP 实例仍以 Actor/组件近似 |
| SyncMarker | R1 已落地：`MarkerSync` 同名 marker 区间 alpha 对齐，缺 marker 回退诊断 | 更复杂的 marker 分段/事件图未执行 |
| 曲线 | Float 曲线导入、原生 `SampleCurve`、C# 查询/显式快照与 Echo locomotion 条件 | 无逐帧自动求值、无曲线驱动 modifier/条件图 |
| 状态机 | R3 已落地 C# `AnimationGraphRuntime` + `LocomotionAnimationBlueprint` 图宿主；资源侧 `EchoLocomotionGraph.cs` 持有 Echo 数据 | BlendSpace/分层/动作层仍未执行 |
| 分层动画 | Echo 无骨骼 mask 分层；`FullBodyLayer` 是整身直通 | 通用 layer interface/PostProcess/ControlRig 未执行 |
| Montage | sections/notifies/warp + 多 `segments[]` + C# `PlayAction` 动作层已支持 | Montage blend-in/out、非 Slerp 旋转与多段 modifier 合并未执行 |
| 曲线驱动骨骼 | R1/R2 曲线采样；R5 已落地原生 PoseDriver（`vl.anim-pose-driver.v1`、UE RBF 求值、只读曲线发布）、SlopeWarping 与 Twist PostProcess（`vl.anim-post-process.v1`） | Helpers/Constraint/RigidBody 与 ModifyCurve 图节点仍未执行；无表情 RigLogic |

## 1. 正式版分层

正式版按三层收口，避免继续把逻辑堆进一个角色 C# 类：

1. **原生求值层**：`SkeletalPlayback` / 新的 pose evaluator，拥有单段、同步组、
   混合层、blend space、曲线求值和姿态缓存。C# 不持有骨骼姿态。
2. **图执行层**：数据驱动 AnimGraph（state machine / sequence / blend / layer /
   slot / curve modifier），由 C# `AnimationGraphRuntime` 求值，只向原生提交
   目标姿态/权重/参数。
3. **作者层**：C# 动画蓝图仍是正式入口，但角色差异通过资源资产和少量
   `[Expose]` 参数表达；Echo 不再要求一个专门的状态机子类。

## 2. R1：补齐 SyncGroup 语义

实现状态（2026-11-04）：已完成。`source/animation/animationSyncGroup.*`、
`SkeletalPlayback` 集成、World tick 协调、C# feedback 与 16 项模块测试均已落地；
Echo 24 个 sidecar 显式写 `syncGroupScope=Local`。下面是原始目标，保留作为验收清单。

- 新增 `AnimationSyncGroupRegistry`（World/Component 级）：
  - 角色：`CanBeLeader`、`AlwaysFollower`、`AlwaysLeader`、`TransitionLeader`、
    `TransitionFollower`、`ExclusiveAlwaysLeader`；
  - 每个 group 只选一个 leader；`CanBeLeader` 按 blend weight/播放中状态选举；
  - `TransitionLeader` 只在混合进入期间领导，混合结束后转 follower；
  - `TransitionFollower` 混合期间不参与组，结束后跟随；
  - `ExclusiveAlwaysLeader` 若不能成为 leader，按 UE 退化为 `DoNotSync`。
- `EAnimSyncGroupScope::Local/Component`：
  - Local 只在一个 mesh 的 player 内同步；
  - Component 在同一 SkeletalMeshComponent 的 main/linked instances 内同步；
  - 需要为 linked AnimBP/分层实例预留 group 注册接口。
- 两种方法：
  - `SyncGroup`：follower 每 tick 把归一化时间对齐 leader；
  - `MarkerSync`：按最近两个同名 marker 的区间 alpha 对齐；缺 marker 时回退
    `SyncGroup` 并记录诊断，不静默漂移。
- 速率：follower 的 `rate` 由 leader rate × 时长比推导，限制在安全范围；
  过渡第一帧允许相位跳变，后续持续校正。
- 资产生成：从 AnimBP T3D 补抽 `SyncGroupScope`（若该版本有该字段）；
  Echo 当前默认 `Local`，需要在 asset 里显式写出默认值而不是靠运行时猜。
- 验收：
  - role 矩阵和 leader election 模块测试；
  - marker 缺失/重复/越界、循环回绕、30/60/120 Hz 下相位误差 ≤1 帧；
  - Walk↔Jog 脚步在真实场景中无可见滑步；
  - 每帧同步组协调零堆分配（profiler 断言）。

## 3. R2：曲线进入运行时

实现状态（2026-11-04）：R2 的只读运行时入口已完成。原生
`SkeletalPlayback::SampleCurve(name, time)`/`SampleCurve(name)`、C#
`CurveNames/HasCurve/HasCurrentCurve/CurveSnapshot`、Echo 的
`MoveData_Speed`/`MoveData_*RotationDelta` 条件与 30/60/120 Hz 回归均已落地。
曲线值不进入逐帧 feedback，也不自动执行 AnimBP 图。`FootPlacement`、
`SlopeWarping`、`AimOffset`、面部 RigLogic 与 `ModifyCurve`/additive 写回仍在
R5；本阶段只提供其中的只读采样基础。

- 原生 `SkeletalPlayback::SampleCurve(name, time)` 与当前播放时间快捷入口；
  只对显式请求的曲线求值，不把所有曲线塞进每帧 feedback。
- `AnimationPlayback` 增加当前曲线快照/条件 API；`CurrentCurve` 保持手动采样。
- 曲线参与 `DetermineLocomotionState`：
  - `MoveData_Speed`、`MoveData_WalkRotationDelta`、`MoveData_JogRotationDelta`
    作为状态/转向条件的一部分，而不是只由 input 推断；
  - 曲线缺失是显式诊断，不默认 0。
- 原生曲线消费者：FootPlacement、SlopeWarping、AimOffset、Facial/RigLogic
  推迟到 R5；R2 不把“可采样”写成“已驱动骨骼/IK”。
- 验收：曲线端点/中间值/缺失/NaN 测试；状态条件场景矩阵；曲线驱动 IK 的
  位置误差阈值需在实现前写进测试。

## 4. R3：数据驱动 AnimGraph

实现状态（2026-11-04）：已完成 Echo 所需子集。`AnimationGraphDefinition` /
`AnimationGraphRuntime` / `AnimationGraphCondition` 与 `LocomotionAnimationGraph`
模板已落地；`LocomotionAnimationBlueprint` 只保留 Expose 参数、输入冻结和
`AnimationPlayback` 提交边界。资源侧 `EchoLocomotionGraph.cs` 定义 Echo 片段、
方向起步、同步组、曲线名和 0.2/0.3/0.5s 过渡时间；`EchoAnimBlueprint` 只转交
Expose 参数。新增 managed 夹具覆盖图校验拒绝矩阵、假角色换图、Echo 图探针、
旧状态机逐帧对照和 30/60/120 Hz 终态；`animation_preflight_tests` 320 项与
`ScriptLocomotionTest` 27 项、图帧率 3 项全部通过。Slot/Montage 动作层已在 R4 落地，
BlendSpace/AimOffset/TurnInPlace/Pivot/LayeredBoneBlend/FootPlacement/additive/
SlopeWarping 仍在 R5。

把 `LocomotionAnimationBlueprint` 的硬编码决策迁到资源图，C# 只做宿主和参数：

- 资源格式 `vl.anim-graph.v1`：
  - `stateMachines[]`（nested、entry、states、transitions、priority、automatic rule）；
  - `sequencePlayers[]`（clip、loop、rate、syncGroup/role、startPosition）；
  - `blendSpaces[]`（1D/2D、轴、样本、同步组）；
  - `layers[]`（bone mask、weight、blend mode）；
  - `slots[]`（Montage 动作层）；
  - `curveModifiers[]`（ModifyCurve、additive）；
  - `conditions[]`（参数/曲线/时间/同步 marker/枚举）。
- C# `AnimationGraphRuntime` 负责图求值，原生只接收最终 pose/clip/weight；
  图求值不直接触碰 Vulkan/Jolt。
- `LocomotionAnimationBlueprint` 保留为 Echo 的薄宿主；新角色默认用图资产，
  不再复制一份状态机子类。
- 验收：Echo 图资产与当前 C# 行为逐项对齐；新增一个测试角色只改资产；
  图校验拒绝循环、未知节点、超预算和非法同步组。

## 5. R4：分层动画与 Montage 动作层

实现状态（2026-11-04）：Echo 所需动作层已完成。`vl.anim-montage.v1` 支持可选
`segments[]`（不同 sidecar、段源码区间、段 rateScale/loop、连续时间轴）与 Montage
顶层 `rateScale`；原生 `SkeletalPlayback` 在段边界切换 clip 而不重启 Montage 时钟，
section 可跨段链接/自循环，sidecar notify 与 warp 窗口按段映射。C# 正式入口是
`LocomotionAnimationBlueprint.PlayAction(AssetId, targets?)` / `CancelAction()`；
动作激活期间不提交 base clip，原生 `State.Montage` 完成后恢复。Echo 的
`ReachOut_Montage_RM` 已按 2 段原结构导入，`ReachOut_End_Montage` 接到 Interact。
**未完成**：Echo 没有骨骼 mask 分层，`FullBodyLayer` 仍是整身直通；Montage
blend-in/out 与非 Slerp 旋转仍不在执行范围。

- 原生增加 bone mask/layer blending；先支持 2 层（base + overlay），再扩展。
- 导出并执行 `Echo_AnimLayerInterface`、`Echo_PostProcess_AnimBP` 的
  layer/slot 结构；ControlRig/SlopeWarping 作为后续独立节点，不混入状态机。
- 导出 Echo 3 个 Montage：`VaultOver_Montage`、`ReachOut_Montage_RM`、
  `ReachOut_End_Montage`，包括 section、notify、warp target、slot。
- `PlayMontage` 走 Slot 层：上半身动作叠加下半身 locomotion；Montage 结束
  按 asset 的 blend-out 规则回 base，不用角色 C# 手写收口。
- 验收：Walk/Jog + ReachOut 上半身叠加；VaultOver 的 root/warp 不与胶囊
  双消费；Montage cancel/interrupt 的 section/notify 测试。

## 6. R5：UE 特征对齐

实现状态（2026-11-04）：按 `documents/rationale/echo-r5-feature-audit.md`
收缩到 Echo 实际消费的 **SlopeWarping**。原生 `SlopeWarpingSolver`
（`source/animation/slopeWarping.*`）与 `SkeletalPlayback`/C# `AnimationPlayback`
已接线；Echo 资源蓝图默认启用，探针场景
`SC_valley_echo_slope_probe.json` 使用 25° 静态斜坡。该实现是整身 root 对齐 +
脚部平面高度补偿，不执行 UE ControlRig 的逐骨/双足 IK，也未接 `DisableIK` 曲线。
Echo **不需要** BlendSpace、AimOffset、TurnInPlace、Pivot、LayeredBoneBlend、
OptionalLayer、FootPlacement 与 additive；这些不再作为 R5 的 Echo 验收入口。

按“能被 Echo 实际使用”排序，不追求一次覆盖整个 AnimGraph：

1. SlopeWarping（ControlRig 的正式替代节点）——已完成正式原生子集；
2. Facial RigLogic 曲线和 Hair AnimBP——可选；
3. Additive layer、Montage blend-in/out、section loop——Echo locomotion 不用；
4. Root motion 模式——当前 Echo 全为 in-place。

每项必须有 UE 对照资产/截图/曲线采样证据，不能用“看起来差不多”验收。

## 7. R6：生产化

- 性能：每角色 pose/curve/sync 成本、多角色同步组协调、零逐帧分配；
  预算在 profiler 数据出来后写死，不提前承诺数字。
- 稳定性：动画资产热重载、图校验、错误隔离、World/Play 代际清理。
- 调试：HUD/console 输出当前 state、clip、syncGroup、leader、phase、marker、
  曲线值；与现有 `gpustats/perf` 同一边界。
- 测试：模块 GoogleTest 为主；真实场景只做端到端探针；不新增 runtime test
  flag（遵守 `documents/contracts/testing.md`）。
- 文档：每个节点的执行语义、拒绝矩阵、曲线/同步组边界进入
  `documents/contracts/`，实现历史进入 `documents/rationale/`。

## 8. 建议顺序与门槛

| 阶段 | 先决条件 | 完成门槛 |
| --- | --- | --- |
| R1 SyncGroup | 当前 `players`/`TransitionToSynced` | role/leader/marker 测试；Walk↔Jog 视觉验收 |
| R2 曲线 | R1 稳定 | 已完成只读采样、曲线条件、缺失诊断与帧率回归；骨骼消费者归 R5 |
| R3 AnimGraph | R1/R2 接口冻结 | Echo 行为图资产化；新角色零 C# 状态机 |
| R4 分层/Montage | R3 图节点 | 上半身动作 + locomotion；3 个 Echo Montage |
| R5 UE 特征 | R3/R4 | Echo 只验 SlopeWarping；整身对齐/高度补偿模块测试 + 25° 斜坡场景截图 |
| R6 生产化 | R1–R5 持续 | 预算、热重载、调试、文档全绿 |

## 9. 明确不做/风险

- 不把整个 UE AnimGraph/ControlRig 解释器搬进 VL；只实现有资产证据、
  有消费方的节点子集。
- 不在角色 C# 蓝图里继续堆 clip 字符串或同步组特判；角色只提交
  `LocomotionInput`/动作请求。
- SyncGroup 的 leader 选举与 linked instance 顺序是高风险区，必须先写
  role 矩阵测试再接真实资产。
- 曲线逐帧求值和分层 blending 都可能改变现有姿态/根运动语义，必须在
  R2/R4 前补模块级回归，不靠 runtime test flag。
- Echo 的 3 个 Montage 和 layered AnimBP 依赖额外 UE 只读导出；导出失败
  时保留当前单层 locomotion，不伪造“已支持”。

