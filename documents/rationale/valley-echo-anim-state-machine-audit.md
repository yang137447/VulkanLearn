# Echo AnimBP 状态机审计

> 证据记录，不是契约。来源是 UE 5.8.4 只读导出的 `Echo_AnimBP.uasset` T3D
> （UE `ObjectExporterT3D`）与 `Enum_LocomotionState.uasset`。
> T3D 由 `tool/animation/audit_echo_anim.py`
> 生成，输出在 `artifacts/ue-echo-export/anim-audit/`（gitignored）。

## 外层 Locomotion

`AnimGraphNode_StateMachine_1.Locomotion` 是状态机入口，状态如下：

| 状态 | 含义 |
| --- | --- |
| `Idle` | 待机；内部还有 Idle States 子状态机 |
| `Walk` | 步行；内部是 Walk States 子状态机 |
| `Jog` | 慢跑；内部是 Jog States 子状态机 |
| `Wide Idle` | 远距离待机/宽站姿；当前 VL 未实现 |

外层转移（`AnimStateTransitionNode`）：

| From → To | Crossfade | Logic | 条件 |
| --- | ---: | --- | --- |
| Idle → Walk | 默认 0.2s | Standard Blend | `LocomotionState == Walk` |
| Idle → Jog | 默认 0.2s | Standard Blend | `LocomotionState == Jog` |
| Walk → Jog | 默认 0.2s | Inertialization | `LocomotionState == Jog` |
| Walk → Idle | 0.3s | Inertialization | `LocomotionState == Idle` |
| Jog → Walk | 0.3s | Standard Blend | `LocomotionState == Walk` |
| Jog → Idle | 0.3s | Inertialization | `LocomotionState == Idle` |
| Wide Idle → Walk/Jog | 默认 0.2s | Standard Blend | 对应枚举值 |

`Enum_LocomotionState` 的枚举名是 `NewEnumerator0=Idle`、`NewEnumerator1=Walk`、
`NewEnumerator2=Jog`。外层状态机不直接读速度，只读这个离散枚举。

## Idle 内层

`Idle States` 子状态机负责“进入 Idle 时先播停步，再回 Idle Loop”：

| From → To | Crossfade | 条件/规则 |
| --- | ---: | --- |
| Entry → Jog Stop | 0 | `Speed > 200`，PriorityOrder 0（最高） |
| Entry → Walk Stop | 0 | `Speed > 1`，PriorityOrder 2 |
| Entry → Idle Loop | 0 | `true`，PriorityOrder 3（最低，兜底） |
| Jog Stop → Idle Loop | 0.5s | `bAutomaticRuleBasedOnSequencePlayerInState` |
| Walk Stop → Idle Loop | 0.5s | `bAutomaticRuleBasedOnSequencePlayerInState` |

UE 的 PriorityOrder 是升序检查，数值越小越先检查；因此 Jog Stop 先于 Walk Stop，
Idle Loop 是最后兜底。

## Walk / Jog 内层

`Walk States` 与 `Jog States` 结构相同，只有角度变量名不同：

| From → To | Crossfade | 条件 |
| --- | ---: | --- |
| Entry → Start Left 180 | 0 | `135 <= WalkStartAngle/JogStartAngle <= 180` |
| Entry → Start Left 90 | 0 | `45 <= angle <= 135` |
| Entry → Start Right 90 | 0 | `-135 <= angle <= -45` |
| Entry → Start Right 180 | 0 | `-180 <= angle <= -135` |
| Entry → Start F | 0 | `true`，PriorityOrder 2（方向区间之后的兜底） |
| Entry → Cycles | 默认 0.2s | `!PlayWalkStart` / `!PlayJogStart`，PriorityOrder 0 |
| Start * → Cycles | 0.5s | 序列播放完自动规则；Jog 另有一条 `GetRelevantAnimTimeRemaining() < 1.1` 的提前退出（0.5s） |

`PlayWalkStart` / `PlayJogStart` 由 `UpdateOnWalkEntry` / `UpdateOnJogEntry`
函数图设置：从 Idle 进入步态时播方向起步；Walk↔Jog 切换时直接进 Cycles。
VL 的 `LocomotionStateMachine` 用“Idle→Walk/Jog 才发 Start，Walk↔Jog 只换循环”
表达同一条件。

## DetermineLocomotionState

UE 函数图的实际条件：

```text
if (!IsFalling && !IsSitting):
    if dot(normalize(Velocity), normalize(Acceleration)) < -0.5:
        LocomotionState = Idle
    else if IsMovementWithinThresholds(1, 300, 0.5):
        LocomotionState = Jog
    else if IsMovementWithinThresholds(1, 0, 0.01):
        LocomotionState = Walk
    else:
        LocomotionState = Idle
```

`IsMovementWithinThresholds(MinCurrentSpeed, MinMaxSpeed, MinInputAcceleration)`：

```text
return MinCurrentSpeed <= Speed
    && MinMaxSpeed <= MaxSpeed
    && MinInputAcceleration <= |InputVector|
```

其中 `Speed` 是实际移动速度，`MaxSpeed` 是角色当前最大速度（Sprint 时会到 Jog 上限），
`InputVector` 是原始输入向量。Falling/Sitting 分支在 T3D 里没有连线到新的
`LocomotionState` 赋值，保持上一状态；VL 用 `Grounded` 进入独立的 Airborne
状态，`Jump_Idle_Fall` 按 UE 的 `bLoopAnimation=False` 播一次并保持末帧。

## VL 映射

- `script/VulkanLearn.Runtime/LocomotionStateMachine.cs`
  - `UseUeStateConditions = true` 时按上面的 `Speed / MaxSpeed / InputMagnitude /
    VelocityDirection` 选择 Idle/Walk/Jog；
  - Idle 停步阈值改为 `Speed > 200 → StopJog`、`Speed > 1 → StopWalk`；
  - 方向桶使用 45°/135° 边界，与 `WalkStartAngle`/`JogStartAngle` 一致。
- `script/VulkanLearn.Runtime/LocomotionAnimationBlueprint.cs`
  - Idle→Walk/Jog 0.2s；Walk/Jog→Idle 与 Walk↔Jog 0.3s；
  - Start→Cycles、Stop→Idle 0.5s；
  - Walk→Jog 与 Stop→Idle 默认启用 Inertialization；
  - `StopTransitionMaxSeconds` 默认 0（完整播放停步片段，符合 UE 自动规则）。
- `source/animation/skeletalPlayback.*` 原本已有 `TransitionTo(clip, seconds, loop,
  blendSeconds, inPlace)`；新增脚本命令 `mode=transitionTo` 与
  `AnimationPlayback.PlayTransition(...)` 把它暴露给动画蓝图。
- `Maps/SC_valley_echo/Blueprints/EchoCharacterBlueprint.blueprint.cs`
  - 增加 2048/3000/512 cm/s² 的地面加速/刹车/空中加速近似，让实际 `Speed`
    能进入 `>1` 和 `>200` 的停步阈值；否则瞬间 `Move(0)` 会跳过整个 Idle Stop 分支。
- `tool/animation/extract_echo_sync.py` + `build_echo_assets.py`
  - 从 AnimBP T3D 的 Sequence Player 节点抽取
    `SyncGroup/Role/Method/SyncGroupScope/StartPosition/Loop`，写入每个 `.anim.json`
    的 `players`；T3D 没有 Scope 字段时显式按 `Local` 写入，`syncMarkers` 由原始 JSON
    升级为结构化数据。
  - 当前 Echo 共 23 个 player 绑定，全部属于 `Locomotion` 或 `DoNotSync`。
- `SkeletalPlayback::TransitionToSynced` / `AnimationPlayback.PlayTransition`
  - `EAnimSyncMethod::SyncGroup` 以 `syncPhase * duration` 对齐目标循环；
    入口对齐只是第一帧，后续由 `AnimationSyncGroupRegistry` 每 tick 选举 leader 并
    持续校正 follower。
  - `MarkerSync` 按最近两个同名 marker 的区间 alpha 对齐；缺同名 marker 回退
    `SyncGroup` 并发布诊断。`Graph` 保留数据、执行层明确不执行。
  - 动画曲线由显式 `SkeletalPlayback::SampleCurve`/C# `CurveValue`、
    `CurrentCurve`、`CurveSnapshot` 读取；Echo 角色蓝图把
    `MoveData_Speed`、`MoveData_WalkRotationDelta`、`MoveData_JogRotationDelta`
    提交到 `LocomotionInput`。状态机把这些曲线作为现有速度/朝向输入的补充条件，
    缺失时回退旧路径，不自动执行曲线图。

## 尚未复刻

- UE 的 `Wide Idle` 状态、分层动画 `Echo_AnimLayerInterface`、
  `Echo_PostProcess_AnimBP`、ControlRig 斜披修正和 Montage 动作层。
- UE 的 Inertialization 曲线与 VL 的有限惯性化实现不保证逐帧一致；当前使用
  VL 原生 `TransitionTo` 的同名开关，时间参数一致。
- UE 的 `IsFalling/IsSitting` 分支保持上一 LocomotionState；VL 用独立的
  Airborne/Fall/Land 路径替代。
- `EAnimSyncMethod::Graph` 只保留数据，执行层返回 `sync-graph-not-executed`；
  UE AnimBP 的曲线图/Modify Curve 节点也未实现。R2 只完成显式只读采样与
  locomotion 条件，曲线不写回姿态，也不驱动 FootPlacement、SlopeWarping、
  AimOffset 或面部 RigLogic；这些消费归 R5。

## 2026-11-05 独立复核与修正

按 `artifacts/ue-echo-export/anim-audit/Echo_AnimBP.t3d` 重新逐节点核对后，补齐或纠正：

| 项 | UE 事实 | 修正 |
| --- | --- | --- |
| Land 提前退出 | `AnimStateTransitionNode_11`：`GetRelevantAnimTime(0) > 0.05` AND `Speed > 100` → On Ground，Crossfade 0 | `LocomotionGraphConfig.LandEarlyExitSeconds/Speed`（0.05/100），Land→步态新增优先级 10、0 秒混合的出口 |
| 片段入口时间 | `Walk_Fwd_to_Idle=0.10`、`Jog_Fwd_to_Idle=0.15`、`Idle_to_Walk_Fwd_Left_180=0.20`、`Right_180=0.25` | 原 `Play` 不接受 startTime；`ApplyDecision` 在 `StartPosition > 0` 时改走 `transitionTo` + 0 秒混合（等价硬切） |
| Idle 循环片段 | `Idle Loop` 播 `Idle_Long`（20.17 s）；`Idle` 只被不可达的 Wide Idle 使用 | `EchoLocomotionGraph.Idle = "Idle_Long"` |
| 过渡时间 | OnGround→InAir 0.4、InAir→Land 0.1、Land→步态 0、Idle→Walk/Jog 0 | Echo 资源图逐项对齐；runtime 默认值保持通用 |
| Any State → InAir | 两个根状态机共用 Any State 过渡 | 恢复 `"*" → Airborne`；runtime 的 `To == 当前状态` 守卫已能阻止自过渡 |
| 并行状态机 | `DetermineLocomotionState` 是唯一状态源 | 删除 `LocomotionAnimationBlueprint` 里的 `legacyProjection`；Airborne/Land 沿用 `lastGroundedState`，`TargetSpeed` 不再在落地期间输出步态名义速度 |
| 接地去抖 | UE 直接读 `IsFalling` | 抽出 `GroundStateFilter`（`script/VulkanLearn.Runtime/GroundStateFilter.cs`），每帧只在物理步后采样一次；原实现每帧调用两次，"2 帧"实际约等于 1 帧 |

仍保留的已知差异：

- `Walk → Jog` 在 UE 是 Inertialization + Crossfade 0。VL 原生 `TransitionTo` 在
  `blendSeconds == 0` 时会清空 inertialization，因此暂时保留 0.2 秒混合窗口；
  要对齐需要先让原生 inertialization 脱离 blend 时长。
- UE 的 `Sitting` / `StandUp` / `Wide Idle` 仍未实现（`Wide Idle` 在 UE 不可达）。
- `PlayWalkStart` / `PlayJogStart` 的赋值在 Blueprint 函数图，T3D 只有空模板；
  当前用 `EntryFromState(Idle)` 近似。

### 2026-11-05 追加：循环片段播放速率

`Walk_Fwd` 的 `MoveData_Speed` 曲线为 **159.27 cm/s**，而角色控制层按
`WalkSpeed = 200` 移动；速率差 25%，支撑脚相对地面持续打滑（观感像“脚被地面黏住”）。
`Jog_Fwd` 为 433.32 cm/s，与 `JogSpeed = 450` 只差 4%。

一开始试过对循环片段按 `TargetSpeed / MoveData_Speed` 缩放播放速率，但这样
`Walk_Fwd` 会跑到 1.256 倍，步频从 0.51s/步变成 0.41s/步，和起步片段明显不一致。
现在改为**片段一律 1 倍速**（与 UE AnimBP 一致），由位移层跟随曲线：
运行时 `Walk_Fwd` 为 `rate=1.000`、`speed=animSpeed=159.3`，起步与循环步频一致。

起步片段还要反向驱动位移：`Idle_to_Walk_Fwd_Left_90` 的曲线在 2 秒内从 2 爬到 198，
而角色原先 0.1 秒就加速到 200，切换进走路的前两秒脚速只有位移的一半。现在角色
`Tick` 用 `LocomotionAnimationBlueprint.AnimationGroundSpeed()`（曲线 × 播放速率）
作为目标速度，缺曲线时回退到 `WalkSpeed/JogSpeed`。运行时 trace 显示实际
`speed` 与 `animSpeed` 全程一致（33 → 64 → 93 → 120 → 145 → 169 → 190，进入循环后
两者都是 200）。

### 2026-11-05 追加：进入循环的相位

`Start → Cycles` 原先用归一化相位映射：源片段相位 1.0 → 目标 `Walk_Fwd` 时间 0。
按 sidecar 的 `syncMarkers` 对照，源片段 `Idle_to_Walk_Fwd_Left_90` 结束在
`Foot_L` 落地后 0.257s，而 `Walk_Fwd` 的第 0 帧在 `Foot_R` 落地后约 0.18s——
左右脚正好对反，0.5s 交叉淡入期间两只脚互相抢位，表现为“进 walk loop 卡脚”。

现在 `LocomotionAnimationBlueprint.TryResolveMarkerEntry` 用源/目标共有的同名
marker，把“距上次落脚的时间”搬到目标片段（`Walk_Fwd` 入口从 0 改成 ~0.59s），
并在同一次过渡里传 `syncPhase = -1`，不再做归一化相位对齐。缺 marker 时回退旧行为。

`Start * → Cycles` 的混合改为 **Inertialization**（`LocomotionGraphConfig.
InertializationOnStartToCycle`，默认开）：0.5 秒窗口内由惯性化接管速度连续性，
不再做姿态线性交叉淡入；关闭时退化为原来的标准交叉淡入。运行时 trace 的
`EchoSwitch` 显示该过渡 `blend=0.50 cross=True inert=True`，目标片段从 marker
入口时间 ~0.59s 开始推进。

## 复现

```powershell
# 在 VLEchoExport 只读副本上运行；输出目录必须不存在且在项目外
artifacts\ue-echo-export\run_anim_audit.bat
```

原始 T3D：`artifacts/ue-echo-export/anim-audit/Echo_AnimBP.t3d`。
