# Echo R5 UE 动画特征审计

> 状态：rationale（非契约）。本文只回答“Echo 实际消费了哪些 UE 动画特征、
> 哪些属于正式版必需子集”，不声明任何运行时实现状态。
>
> 证据日期：2026-11-04。只读取已有 T3D、导入 sidecar 与 Montage 审计结果；
> 没有运行 UE 工程命令，没有修改资源或代码。

## 1. 结论先行

Echo locomotion 的核心不是 BlendSpace 图，而是三层离散状态机：

```text
Locomotion StateMachine
  Idle -> Idle States  (Idle_Long / Walk_Fwd_to_Idle / Jog_Fwd_to_Idle)
  Walk -> Walk States  (5 个方向 Start + Walk_Fwd cycle)
  Jog  -> Jog States   (5 个方向 Start + Jog_Fwd cycle)
        -> SaveCachedPose("Locomotion")

Main States StateMachine
  On Ground -> UseCachedPose("Locomotion")
  InAir / Land / Sitting / StandUp / BeginTouching / HoldTouching / EndTouching
        -> LinkedAnimLayer("FullBodyLayer")
        -> Slot(DefaultSlot)
        -> Inertialization
        -> ControlRig(Echo_SlopeWarping_CtrlRig)
        -> Root
```

因此：

- **必须正式版支持**：状态机/SequencePlayer、SyncGroup/MarkerSync、
  `MoveData_*RotationDelta` 的曲线写回、Inertialization、Slot/Montage、
  斜坡 ControlRig（或等价的正式节点）。
- **必须（R5 追加）**：PostProcess 的 Pose Space Deformation 输入阶段。
  9 个 PoseDriver 已按 T3D 导出为 `vl.anim-pose-driver.v1`，原生 RBF 求值与
  只读曲线发布已落地（见 `contracts/animation-pose-driver.md`）。
- **已落地（R5 追加）**：`Echo_Twist_CtrlRig` + `Echo_Helpers_CtrlRig` 的原生替代
  （`vl.anim-post-process.v1`，8 组驱动 / 16 根 twist 骨 + 7 项 helper 重挂父骨，
  见 `contracts/animation-post-process.md`）。
- **仍缺（下游消费者）**：刚体/约束附件模拟、Montage 的 blend-in/out、
  ReachOut/Sitting/Touching 等非纯 locomotion 状态。
- **Echo locomotion 不用**：BlendSpace 1D/2D、AimOffset、TurnInPlace、Pivot、
  LayeredBoneBlend、OptionalLayer、FootPlacement 节点、additive layer。
- **现有数据不足以支持“上半身分层”结论**：`FullBodyLayer` 是整身直通，
  `DefaultSlot` 也是整身 Montage 注入点；没有骨骼 mask。

## 2. 证据与计数口径

| 证据 | 规模 | 用途 |
| --- | ---: | --- |
| `artifacts/ue-echo-export/anim-audit/Echo_AnimBP.t3d` | 2,138,395 B | 主 locomotion、外层状态机、layer/slot/ControlRig |
| `artifacts/ue-echo-export/anim-audit/Echo_AnimLayerInterface.t3d` | 40,466 B | `FullBodyLayer` 的真实内容 |
| `artifacts/ue-echo-export/anim-audit/Echo_PostProcess_AnimBP.t3d` | 1,355,716 B | 骨骼后处理、PSD、Twist/Helpers、附件物理 |
| `artifacts/ue-echo-export/montage-audit2/ue-montage-audit.json` | 3 个 Montage | slot、section、AnimSegment、notify/warp |
| `..\VukanLearnResources\Maps\SC_valley_echo\Source\*.anim.json` | 24 个 sequence sidecar | SyncGroup 参数与 SyncMarker 数据 |

计数按唯一 `ExportPath` 的 `Begin Object Class=...` 统计。PostProcess T3D 会在
`ExecuteUbergraph_Echo_PostProcess_AnimBP` 下重复序列化同一批节点，直接搜索类名会把
数量翻倍；本文的 “PostProcess active” 只计 `:AnimGraph.` 下的真实图节点。

## 3. 节点清单

### 3.1 三份 T3D 的横向统计

| 节点/特征 | `Echo_AnimBP` | `Echo_AnimLayerInterface` | `Echo_PostProcess_AnimBP` | 所在图/说明 |
| --- | ---: | ---: | ---: | --- |
| `StateMachine` | 5 | 0 | 0 | 主图 2；Locomotion 内嵌 3 |
| `SequencePlayer` | 23 | 0 | 0 | Main 7 / Idle 3 / Walk 6 / Jog 6 / Locomotion 1 |
| `ModifyCurve` | 20 | 0 | 1 | 主图 Walk 10 + Jog 10；PostProcess 1 |
| `Slot` | 1 | 0 | 0 | 主图 `DefaultSlot` 注入点 |
| `Montage` | 0 个图节点 | 0 | 0 | 3 个外部 Montage 资产通过 Slot 进入 |
| `LinkedAnimLayer` | 1 | 0 | 0 | 主图调用 `FullBodyLayer` |
| `LinkedInputPose` | 1 | 1 | 1 | 主图/接口的 `FullBody`；PostProcess 的输入 |
| `SaveCachedPose` / `UseCachedPose` | 1 / 1 | 0 / 0 | 0 / 0 | 缓存名均为 `Locomotion` |
| `Inertialization` | 1 | 0 | 0 | Slot 之后、SlopeWarping 之前 |
| `ControlRig` | 1 | 0 | 2 | 主图 `Echo_SlopeWarping_CtrlRig`；PostProcess `Twist` / `Helpers` |
| `PoseDriver` | 0 | 0 | 9 | PostProcess 的 pose-space deformation |
| `RigidBody` | 0 | 0 | 4 | Canister / RShoulderpad / Skirt / Pouch |
| `Constraint` | 0 | 0 | 3 | Scarf / Canister / LargePouch |
| `LocalToComponentSpace` / `ComponentToLocalSpace` | 0 / 0 | 0 / 0 | 2 / 2 | PostProcess 刚体/约束前后 |
| SyncGroup | 12 个 player 绑定 | 0 | 0 | 全部在 Walk/Jog 内层 |
| SyncMarker 图节点 | 0 | 0 | 0 | UE marker 属于 Sequence，不是 AnimGraph 节点 |
| SyncMarker 数据 | 56 条 | 0 | 0 | 12 个 sidecar，`Foot_L` 28 + `Foot_R` 28 |
| BlendSpace 1D/2D | 0 | 0 | 0 | 无节点 |
| AimOffset | 0 | 0 | 0 | 无节点 |
| TurnInPlace / Pivot | 0 / 0 | 0 / 0 | 0 / 0 | 无节点 |
| LayeredBoneBlend | 0 | 0 | 0 | 无节点 |
| OptionalLayer | 0 | 0 | 0 | 图、接口和实现接口声明中均无 |
| FootPlacement | 0 | 0 | 0 | 无节点 |
| Additive player/layer | 0 | 0 | 0 | 无节点 |

补充：

- `Echo_AnimBP` 的 23 个 `SequencePlayer` 与 24 个导入 sequence 不是一一对应：
  `VaultOver` 只通过 Montage 进入，另外 23 个是 AnimBP player 绑定。
- 12 个 SyncGroup player 中：`TransitionLeader` 5、`AlwaysLeader` 5、
  `CanBeLeader` 1（`Jog_Fwd`）、`AlwaysFollower` 1（`Walk_Fwd`）。
- 11 个非 Walk/Jog player（Main States 7 + Idle States 3 + Locomotion 1）不进入
  SyncGroup。Sidecar 中的 `DoNotSync` 共 12 个，多出的一个是 Montage-only
  `VaultOver`，不是 AnimBP 图节点。

### 3.2 按图统计

| 图 | StateMachine | SequencePlayer | StateResult | TransitionResult | ModifyCurve | 其他 |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| `AnimGraph` 顶层 | 2 | 0 | 0 | 0 | 0 | Slot 1、LinkedAnimLayer 1、SaveCachedPose 1、Inertialization 1、ControlRig 1 |
| `Main States` | 0 | 7 | 8 | 11 | 0 | UseCachedPose 1 |
| `Locomotion` | 3 | 1 | 4 | 8 | 0 | 3 个内嵌状态机 |
| `Idle States` | 0 | 3 | 4 | 5 | 0 |  |
| `Walk States` | 0 | 6 | 7 | 11 | 10 |  |
| `Jog States` | 0 | 6 | 7 | 11 | 10 |  |
| `FullBodyLayer` | 0 | 0 | 0 | 0 | 0 | LinkedInputPose 1、Root 1 |

总计正好是 5 个 StateMachine、23 个 SequencePlayer、30 个 StateResult、
46 个 TransitionResult、20 个主图 ModifyCurve。

各内层图的 sequence：

| 图 | SequencePlayer 的 sequence |
| --- | --- |
| `Main States` | `ReachOut_Start`、`ReachOut_End`、`Jump_Idle_Fall`、`Jump_Idle_Land`、`Sitting_Idle`、`StandFromSitting`、`ReachOut_Loop` |
| `Idle States` | `Idle_Long`、`Jog_Fwd_to_Idle`、`Walk_Fwd_to_Idle` |
| `Walk States` | `Idle_to_Walk_Fwd_Left_180`、`Walk_Fwd`、`Idle_to_Walk_Fwd`、`Idle_to_Walk_Fwd_Left_90`、`Idle_to_Walk_Fwd_Right_90`、`Idle_to_Walk_Fwd_Right_180` |
| `Jog States` | `Idle_to_Jog_Fwd_Left_180`、`Jog_Fwd`、`Idle_To_Jog_Fwd`、`Idle_to_Jog_Fwd_Left_90`、`Idle_to_Jog_Fwd_Right_90`、`Idle_to_Jog_Fwd_Right_180` |
| `Locomotion` | `Idle` |

### 3.3 PostProcess active 图

`Echo_PostProcess_AnimBP` 的真实 `AnimGraph` 功能节点为 25 个，另有 9 个注释框和
25 个 node binding：

| 分组 | 节点 | 具体资产/骨骼 |
| --- | ---: | --- |
| Pose Space Deformation | 9 `PoseDriver` | `neck_02`、`lowerarm_l/r`、`calf_l/r`、`upperarm_l/r`、`hand_l/r` |
| Remove PSD Curves | 1 `ModifyCurve` | 95 条 pose-driver 曲线，`ApplyMode=Scale`，值全 0 |
| Pin Constraints | 3 `Constraint` | `C_Scarf_A_Root_Jnt`、`R_Canister_A_Joint1_Jnt`、`R_LargePouch_A_Joint1_Jnt` |
| Twist Setup | 1 `ControlRig` | `Echo_Twist_CtrlRig` |
| Helpers | 1 `ControlRig` | `Echo_Helpers_CtrlRig` |
| 附件/布料物理 | 4 `RigidBody` | Canister、RShoulderpad、Skirt、Pouch |
| 空间转换 | 2 + 2 | `LocalToComponentSpace` / `ComponentToLocalSpace` |

注释框名称直接对应作者意图：`Pose Space Deformation`、`Remove PSD Curves`、
`Pin Constraints`、`Canister Setup`、`RShoulderpad Setup`、`Skirt Setup`、
`Pouch Setup`、`Twist Setup`、`Helpers`。

## 4. 特别问题

### 4.1 Echo 是否有 BlendSpace/AimOffset/TurnInPlace/Pivot？

**没有。**

- 三份 T3D 中 `BlendSpace` 的节点/类型命中都是 0。
- `AimOffset`、`TurnInPlace`、`Pivot` 的节点/类型命中也都是 0。
- Echo 的方向起步不是 BlendSpace，而是 `Walk States` / `Jog States` 内 5 个离散
  Start 状态，由 `WalkStartAngle` / `JogStartAngle` 的 45°/135° 分桶选择。
- 角色转向由函数图更新 actor rotation；没有独立的 TurnInPlace 或 Pivot 动画资源。

所以 R5 的 “BlendSpace 1D/2D + AimOffset/TurnInPlace/Pivot” 对 Echo 不是必需项，
不能拿 Echo 作为这些节点的验收资产。

### 4.2 分层动画的实际结构

实际结构是 **整身直通 + 一个 Montage Slot**，不是上下身分层：

1. `Echo_AnimLayerInterface` 只有 `FullBodyLayer` 一个函数图。
2. `FullBodyLayer` 只有 `LinkedInputPose(Name="FullBody") -> Root`，没有骨骼 mask、
   没有 Slot、没有第二层、没有 `OptionalLayer`。
3. `Echo_AnimBP` 的 `LinkedAnimLayer` 把 `Main States` 的输出接到 `FullBodyLayer`。
4. 回传后进入 `Slot`，再进入 `Inertialization`、斜坡 ControlRig、Root。
5. `Slot` 没有非默认 `SlotName`；骨骼 `SK_Echo.json` 只登记了 `DefaultSlot`，
   因此该节点就是 `DefaultSlot`。

`FullBodyLayer` 和 `DefaultSlot` 都不能证明存在上半身覆盖层。R4 若要求
“Walk/Jog + ReachOut 上半身叠加”，需要新增骨骼 mask/BlendLayer 数据和新的
AnimBP 结构；这不是从 Echo 当前 T3D 直接导出的能力。

### 4.3 PostProcess 的 ControlRig/PoseDriver/ModifyCurve 影响什么？

它们执行在基础 locomotion pose 之后，会改变最终可见骨骼姿态，但不参与 locomotion
状态、clip 选择、同步组或根运动：

- **PoseDriver/PSD**：在颈部、肘、膝、腕、上臂等身体骨上做 pose-space deformation。
  不是表情/RigLogic。
- **ControlRig**：`Echo_Twist_CtrlRig` 和 `Echo_Helpers_CtrlRig`，属于扭转与辅助骨修正。
  不是斜坡修正。
- **ModifyCurve**：把 95 条 PSD 输出曲线按 0 缩放，注释为 `Remove PSD Curves`；
  不碰 `MoveData_*` locomotion 曲线。
- **RigidBody/Constraint**：围巾、罐子、肩甲、裙摆、腰包等附件/次级运动。

真正影响斜坡 locomotion 的是主 `Echo_AnimBP` 的
`Echo_SlopeWarping_CtrlRig`，不是 PostProcess。该节点：

- 位于 `Inertialization` 之后、Root 之前；
- `AlphaInputType=Curve`，曲线名默认 `DisableIK`；
- `AlphaScaleBiasClamp` 把 `[0,1]` 映射到 `[1,0]`，即 `DisableIK=0` 时启用；
- 声明 `Speed`、`EnableFBIK` 两个 custom pin。

因此对“基础 locomotion 姿态”的准确回答是：

- 斜坡 ControlRig 会影响基础 locomotion，属于必需项；
- PostProcess 的 PSD/Twist/Helpers/附件会改变最终身体外观，但不改变 locomotion 决策；
- 它既不是“只做表情”，也不是“只做斜坡”；附件物理与身体 PSD 同时存在。

### 4.4 三个 Montage 对 VL schema 的真实要求

| Montage | Slot | SequenceLength | Section | AnimSegment | Notify/warp |
| --- | --- | ---: | --- | --- | --- |
| `VaultOver_Montage` | `DefaultSlot` | 2.03 | 1 个 `Default` | 1 段 `VaultOver`，0–2.03 | 4 notify（3 warp + `NotifyState_ExitToLoco_C`），warp 目标 `FrontEdge`/`BackEdge`/`BackFloor` |
| `ReachOut_Montage_RM` | `DefaultSlot` | 8.0 | `Default -> Loop`，`Loop -> Loop` | 2 段：`ReachOut_Start` 到 1.333333；`ReachOut_Loop` 从 1.333333 到 6.666667 | 3 notify（2 warp + 边界 `PlayMontageNotify`），warp 目标 `FacingRotation`/`CharacterLocation` |
| `ReachOut_End_Montage` | `DefaultSlot` | 1.966667 | 1 个 `Default` | 1 段 `ReachOut_End`，0–1.966667 | 0 |

`VaultOver_Montage` 还有显式 `BlendIn=(BlendTime=0.150000)`；三个 Montage 的
`BlendOut` 都只有 `BlendOption=Linear`，没有显式时间。`ReachOut_Montage_RM` 还有
`RateScale=0.500000`。

对 `vl.anim-montage.v1` 的真实要求：

1. `slot` 必须按 `DefaultSlot` 校验；三个资产一致，不需要多 slot。
2. 单段 + 单 section 的 `VaultOver_Montage`、`ReachOut_End_Montage` 可由现有
   `sequence + sections + notifies` 表达；warp 目标与普通 notify 语义也可表达。
3. `ReachOut_Montage_RM` 不能由“一个 `sequence` 路径 + 同 clip 时间切段”无损表达。
   它需要至少支持：
   - slot track 内有序 `AnimSegments`，每段带 `sequence`、`startPos`、`animEndTime`；
   - section 带 `segmentIndex`、`segmentBeginTime`、`segmentLength`；
   - `nextSection` 支持 `Default -> Loop` 和 `Loop -> Loop`；
   - Montage 级 `rateScale`。
4. 如果只允许一个 sequence 字段，则只能选择“资源侧烘焙一个与 UE 等价的复合 clip”。
   这会是有损的派生资产，不能同时声称与 UE 的 AnimSegment/section 结构一致。
   若 R4 的验收要求“3 个 Montage 按 UE 原结构可播放”，上述多段 schema 是硬要求；
   若接受烘焙，则应把它记录为资源侧口径，而不是运行时多段能力。
5. `BlendIn=0.15` 与 section 自循环可以分别延后到 Montage 混合/循环增强；它们不阻塞
   单段 VaultOver/ReachOut_End 的结构导入，但会阻塞 UE 级视觉对齐。

## 5. Echo 必需子集

| 能力 | 证据 | 结论 |
| --- | --- | --- |
| StateMachine + SequencePlayer | 5 个状态机、23 个 player、30 state、46 transition | 必须；Echo 的步态选择完全靠图，不是 BlendSpace |
| 离散方向 Start 状态 | Walk/Jog 各 5 个方向片段 | 必须；不能用 BlendSpace 替代而不改变资产结构 |
| SyncGroup / MarkerSync | 12 个同步 player；12 个 sidecar 共 56 个脚部 marker | 必须；Walk↔Jog 与 cycle 相位依赖 |
| `MoveData_*RotationDelta` 曲线写回 | 20 个主图 ModifyCurve，全部位于 5 个方向 Start 状态 | 必须；只读采样不足以复刻 |
| Inertialization | 主图 1 个节点，过滤 `MoveData_Speed/WalkRotationDelta/JogRotationDelta` | 必须；多条 Walk/Jog/Idle 过渡使用 |
| Slot + Montage | 1 个 `DefaultSlot`，3 个 Montage | 必须；动作注入点 |
| Montage section/AnimSegment | `ReachOut_Montage_RM` 的 2 段结构与自循环 | 若要求 3 个资产原样可播则必须；否则只能资源侧烘焙 |
| SlopeWarping ControlRig | 主图 `Echo_SlopeWarping_CtrlRig`，`DisableIK` 曲线控制 | 必须；影响最终 locomotion 骨骼姿态 |
| PostProcess PSD/附件物理 | 9 PoseDriver、2 ControlRig、4 RigidBody、3 Constraint | PoseDriver 求值 + Twist/Helpers ControlRig 已落地；Constraint/RigidBody 仍未执行 |

## 6. 可延后 / Echo 不用

### 可延后

- `FullBodyLayer` / `LinkedAnimLayer` 的通用运行时：Echo 只有一个直通 layer，
  可由 R3 图编译阶段内联，或作为单层 pass-through 处理。
- `SaveCachedPose` / `UseCachedPose` 的通用图语义：Echo 只有一个缓存
  `Locomotion`、一个消费点，可内联。
- PostProcess 的 Pose Space Deformation、Twist/Helpers、附件刚体与约束。
- ReachOut、Sitting、StandUp、Touching 等非纯 locomotion 状态。
- Montage blend-in/out 曲线与更复杂的 section 混合。
- 独立 `Echo_Hair_AnimBP` 的参数桥接：主图 EventGraph 有 `HairMesh`、
  `Echo_Hair_AnimBP` cast 和 `bIsWindy` 写入，但本次没有提供 Hair AnimBP T3D；
  它不影响 locomotion 图。

### Echo locomotion 不用

- BlendSpace 1D/2D；
- AimOffset；
- TurnInPlace / Pivot；
- LayeredBoneBlend / OptionalLayer；
- FootPlacement；
- additive animation player/layer；
- 本次证据中的 Facial RigLogic / 表情图；
- `SyncMarker` 作为 AnimGraph 节点：Echo 使用 marker 数据，但没有 marker 节点。

“本次证据中”是必要限定：`Echo_Hair_AnimBP` 已被主图引用，但不在三份 T3D 内；
不能据此外推发丝图的节点清单。

## 7. 与 R1-R4 roadmap 的对应

| roadmap 范围 | Echo 证据 | 审计结论 |
| --- | --- | --- |
| R1 SyncGroup/MarkerSync | 12 个 `Locomotion` player；56 个 `Foot_L/Foot_R` marker | Echo 的同步验收应在 Walk/Jog cycle 与 5 个方向 Start 上做，不涉及 linked instance |
| R2 曲线 | 20 个主图 ModifyCurve；Inertialization 过滤 3 条 MoveData 曲线；SlopeWarping 读 `DisableIK` | 曲线写回是 Echo 必需，不是“以后再做”的增强；PostProcess 95 条 PSD 曲线另属可选 |
| R3 数据驱动图 | 5 SM / 30 state / 46 transition / 23 player；0 BlendSpace/AimOffset | R3 的图格式要覆盖 nested state、sequence、sync、curve modifier；不应以 Echo 驱动 BlendSpace/AimOffset 节点 |
| R4 分层/Montage | `FullBodyLayer` 只有 linked input -> root；`DefaultSlot`；3 Montage | R4 的“分层”在 Echo 上只能验证 pass-through，不能验证上下身 masking；Montage 的硬缺口是 ReachOut 的多 AnimSegment |
| R5 UE 特征 | 主图 SlopeWarping；PostProcess PSD/附件；0 BlendSpace/Aim/Pivot/FootPlacement | R5 对 Echo 应收缩为 SlopeWarping + 可选 PostProcess；其余 UE 特征留给其他资产或后续角色 |

## 8. 风险与未决项

- `ReachOut_Montage_RM` 的多段表达已在 R4 决策为扩展 schema：`segments[]` 保留两条
  AnimSegment、section 自循环与 Montage `RateScale=0.5`，没有烘焙单段复合 clip。
- `ModifyCurve` 的精确写回顺序依赖 UE 的 `Scale`/默认 `Set` 语义；结构已确认，
  但逐帧数值仍需参考曲线采样验证。
- SlopeWarping ControlRig 的最终骨骼误差需要 UE 对照，不应只按节点类型判定。
- PostProcess 的 PSD 与附件物理若不做，基础 locomotion 状态仍可运行，但最终外观
  会和 UE 有可见差异。
- Hair AnimBP、面部资源未纳入本次 T3D 证据，不能从本审计推导它们的节点需求。

## 9. 复现与校验

- 主图/接口/PostProcess：`tool/animation/audit_echo_anim.py` 的只读导出路径。
- Montage：`tool/animation/audit_echo_montages.py` 的 T3D + notify 审计。
- 文档校验：

```powershell
py -3 tool\validate-docs.py
```

