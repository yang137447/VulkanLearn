# Echo 动画分层与 Montage 审计

> 状态：rationale（非契约）。记录 2026-11-04 对 UE 5.8.4 Valley Echo 只读导出副本的
> Montage / AnimLayer 证据、VL 可消费范围，以及 `ReachOut_Montage_RM` 从旧 schema
> 拒绝到 R4 多段落地的决策。
> 当前行为仍以 [`animation-montage.md`](../contracts/animation-montage.md) 和源码为准；
> 本文的技术描述可能随源码变化。

## 证据来源与方法

- UE 只读副本：`artifacts/ue-echo-export/VLEchoExport`（只含 Echo 内容，未保存任何包）。
- 导出脚本：[`tool/animation/audit_echo_montages.py`](../../tool/animation/audit_echo_montages.py)，
  用 `ObjectExporterT3D` 导出 Montage T3D，用 UE 5.8 的
  `AnimationLibrary.get_animation_notify_events_for_track` 读取 notify 结构，并可选导出被引用
  AnimSequence 的 FBX。
- 本批资产：`VaultOver_Montage`、`ReachOut_Montage_RM`、`ReachOut_End_Montage`。
- 转换脚本：[`tool/animation/import_echo_montages.py`](../../tool/animation/import_echo_montages.py)，
  复用 GASP 的 T3D 规则生成 `vl.anim-montage.v1`。

导出时必须同时启用 MotionWarping 插件（`-EnablePlugins=...,MotionWarping`）。否则 T3D 会丢掉
`NotifyStateClass` 和 modifier 对象，MotionWarping 窗口会退化成普通 notify。
`VaultOver_Montage` 还引用 `/Game/AncientContent/Blueprints/Character/BP_NotifyState_ExitToLoco`；
该 Blueprint 不在 Echo-only 副本里时，T3D 与 Python API 都会把它报告为
`NotifyStateClass=None`，转换器因此拒绝猜测它的 kind。本批从只读源工程把该 BP 复制进一次性
副本后重新导出，才得到完整结构；原始 UE 工程未被修改或保存。

## Montage 结果

| UE 资产 | VL 结果 | length | sections | notifies | warp 窗口 |
| --- | --- | ---: | --- | --- | --- |
| `VaultOver_Montage` | `AM_VaultOver_Montage.json` | 2.03 | 1 `Default` | 4（3 × MotionWarping + 1 × `NotifyState_ExitToLoco_C`） | 3 |
| `ReachOut_End_Montage` | `AM_ReachOut_End_Montage.json` | 1.966667 | 1 `Default` | 0 | 0 |
| `ReachOut_Montage_RM` | `AM_ReachOut_Montage_RM.json` | 8.0 | 2（`Default` → `Loop`，`Loop` 自连接） | 3 | 2 |

`VaultOver_Montage` 的窗口（Montage-local 秒）：

| target | provider | warpRotation | 区间 |
| --- | --- | --- | --- |
| `FrontEdge` | `None` | true | `[0.107012, 0.489629]` |
| `BackEdge` | `None` | false | `[0.834159, 1.236030]` |
| `BackFloor` | `None` | false | `[1.188985, 1.538663]` |

三个窗口的 easing 都是 `Linear`，`rotationTimeMultiplier` 都是 1，`ignoreZAxis` 都是 false。
`provider=None` 表示 UE 不提供骨骼/静态点，warp 目标位置由调用方提交；VL 对应
`PlayMontage` 的 `targets` 键集合，缺一个或多一个都会被拒绝。

`ReachOut_Montage_RM` 的窗口：

| target | provider | warpRotation | 区间 |
| --- | --- | --- | --- |
| `FacingRotation` | `None` | true（`bWarpTranslation=False`，只转不位移） | `[0.0001, 1.333433]` |
| `CharacterLocation` | `None` | false | `[0.053628, 0.594669]` |

它还有一个 `PlayMontageNotify`（实例名 `Touched`）在 `1.333333`，正好落在
`Default` → `Loop` 的分界点。

## 分层 AnimBP 结构

### `Echo_AnimLayerInterface`

- `ParentClass` 是 `/Script/Engine.AnimLayerInterface`。
- 只有一个 layer function graph：`FullBodyLayer`。
- `FullBodyLayer` 内部只有 `AnimGraphNode_Root` 和
  `AnimGraphNode_LinkedInputPose`（`Node=(Name="FullBody")`），即把输入姿势原样输出。
- 没有 per-bone 分层、没有 Slot 节点、没有 Montage 节点、没有额外曲线修改。

### `Echo_AnimBP`

- 主 `AnimGraph` 里有一个 `AnimGraphNode_LinkedAnimLayer`：
  `Interface=Echo_AnimLayerInterface_C`、`Layer=FullBodyLayer`、
  `FunctionReference=FullBodyLayer`。这是对上述唯一 layer 的消费点。
- 一个 `AnimGraphNode_Slot`，T3D 没有导出任何非默认属性，因此是 UE 默认
  `DefaultSlot`；`SK_Echo.json` 的 `slots` 也是 `["DefaultSlot"]`。
- 一个 `AnimGraphNode_SaveCachedPose` 和一个 `UseCachedPose`，缓存名都是 `Locomotion`。
- 5 个 StateMachine 节点、46 个 TransitionResult、30 个 StateResult、23 个
  SequencePlayer、20 个 ModifyCurve、1 个 Inertialization、1 个 ControlRig。
- 没有 `AnimGraphNode_LayeredBoneBlend`；没有第二个 layer interface 或第二个
  `LinkedAnimLayer`。

因此 Echo 的“分层”并不是上下身分槽：`FullBodyLayer` 是整身直通，`DefaultSlot` 是 Montage
注入点，真正的步态选择发生在 `Echo_AnimBP` 的 StateMachine 和 `Locomotion` cached pose 上。

### `Echo_PostProcess_AnimBP`

- 没有 `SlotName`、没有 `Layer`、没有 `Interface`；它不是 AnimLayerInterface 的 layer。
- 图里是后处理/物理链：`ControlRig`、`PoseDriver`、`RigidBody`、`Constraint`、
  `ModifyCurve`、`LocalToComponentSpace` / `ComponentToLocalSpace`、`Root`。
- 它依赖骨骼网格的 Post Process Anim Blueprint 槽，而不是 Montage slot；VL 当前没有这条
  后处理图执行路径。

## VL 可消费 / 不可消费

| UE 结构 | VL 现状 | 证据 |
| --- | --- | --- |
| Montage 的 `slot` | 只作为 SK `slots` 的注册校验值；播放时整身替换 clip，不做按槽混合 | `animationMontage.cpp` 的 `BindMontage`、`animationScriptData.cpp` 的 `PlayMontage` |
| section / `nextSection` / `nextTime` / `linkMethod` | 可消费；同一 Montage 内链式推进 | `skeletalPlaybackMovement.cpp` 的 `TickMontageSections` |
| 普通 notify / notifyState | 可消费；BP notify 的**数据**保留为 notifyState，但 BP 逻辑不执行 | `notifyStateTests.cpp`、`animationScriptData.cpp` |
| MotionWarping 窗口（provider `None`/`Bone`/`Static`） | 可消费；`None` 的目标位置由 `PlayMontage(targets)` 提交 | `animation-montage.md`、`motionWarpingTests.cpp` |
| `LinkedAnimLayer` / `FullBodyLayer` | **不可消费**；VL 没有 linked anim layer 运行时，也没有 layer interface 资产 | 本文的 T3D 结构；`source/animation` 无 layer 类型 |
| `SaveCachedPose` / `UseCachedPose` | **不可消费**；VL 没有 cached pose 图执行 | 同上 |
| `Echo_AnimBP` StateMachine / Transition 规则 | **不由 Montage 运行时消费**；本分支用 C# `LocomotionStateMachine` / `LocomotionAnimationBlueprint` 单独复刻 | `script/VulkanLearn.Runtime/Locomotion*.cs` |
| `Echo_PostProcess_AnimBP` 的 ControlRig/PoseDriver/RigidBody/Constraint/ModifyCurve | **不可消费**；VL 没有 Post Process AnimBP 图执行 | 本文的 T3D 结构 |
| Montage `BlendIn`/`BlendOut` | **不可消费**；`vl.anim-montage.v1` 没有 blend 时间字段 | `VaultOver_Montage` T3D 的 `BlendIn=(BlendTime=0.15)` |
| 多 `AnimSegment` / 多 Sequence 的 Montage | R4 已支持：可选 `segments[]`，段边界切 clip 且不重启 Montage 时钟 | `animation-montage.md`、`SkeletalPlayback::TickSegmentedMontage` |
| Montage `RateScale` | R4 已支持顶层 `rateScale`，与调用方 rate 相乘 | `ReachOut_Montage_RM` 的 `RateScale=0.5` |

## 三个 Montage 的落地决定

### `AM_VaultOver_Montage`

- 写入 `Maps/SC_valley_echo/Animations/AM_VaultOver_Montage.json`，并登记到
  `SKM_Echo.json` 的 `skinning.montages`。
- 播放 `VaultOver`（sidecar `Maps/SC_valley_echo/Source/VaultOver.anim.json`）。
- `NotifyState_ExitToLoco_C` 作为 notifyState 保留；VL 只发 begin/end feedback，
  不执行 Blueprint 的 ExitToLoco 逻辑。调用方若要复刻该行为，需要在 C#/场景逻辑里消费
  `State.Notifies`。
- 三个 warp target 必须由调用方一次性提交；VL 不会从 Montage 反推世界位置。

### `AM_ReachOut_End_Montage`

- 写入 `Maps/SC_valley_echo/Animations/AM_ReachOut_End_Montage.json`，并登记到
  `skinning.montages`。
- 播放 `ReachOut_End`；没有 notify、没有 warp、没有 section 链，是纯收口片段。

### `ReachOut_Montage_RM`：多段资产（R4 已写入）

该资产最初被旧单 sequence schema 拒绝。R4 扩展后按原结构无损表示：

1. `SlotAnimTracks(0).AnimSegments` 有两段：`ReachOut_Start`（0–1.333333）和
   `ReachOut_Loop`（1.333333–6.666667）。schema 的 `sequence` 只接受一个 sidecar 路径，
   Montage sections 也只能在同一 clip 内切时间，不能跨 clip 切换。
2. `CompositeSections` 是 `Default` → `Loop`，且 `Loop` 自连接；`SequenceLength=8.0`，
   但 `ReachOut_Start` 只有 1.333333 秒，`ReachOut_Loop` 只有 6.666667 秒。
3. `RateScale=0.5`，schema 没有资产级播放速率。

最终选择扩展 VL schema，而不是烘焙单段复合 clip：

- 顶层 `rateScale=0.5` 保留 UE `AnimMontage.RateScale`；
- `segments[]` 记录 `ReachOut_Start` 0–1.333333 与 `ReachOut_Loop`
  1.333333–8.0 的 Montage 时间轴，`clipStartTime/clipEndTime` 指向各 sidecar；
- section `Default→Loop` 与 `Loop→Loop` 用显式 Absolute link 写到目标段起点；
- Montage notify 保持 Montage-local；sidecar Footstep/Foley notify 在运行期映射到段；
- 段边界切换 clip 不重启 Montage 时钟；section 可跨段链接和自循环。

该资产已登记到 `SKM_Echo.json`。`EchoCharacterBlueprint` 的 Interact 示例播放无 warp 的
`AM_ReachOut_End_Montage`；`ReachOut_Montage_RM` 需要 `CharacterLocation` 与
`FacingRotation` targets，保留正式资产/`PlayAction` 入口，不在场景里硬编码。

## 验证

- UE 侧：T3D + `get_animation_notify_events_for_track` 的 notify 名称/时间/时长/class 与转换结果
  逐项交叉检查通过；`VaultOver_Montage` 的 4 条 notify、3 个窗口与 T3D 一致。
- 资源侧：3 个 `AM_*.json` 随 `SKM_Echo.json` 进入真实 Scene 加载；多段资产的
  `segments[]`、section link 与源码区间通过 `ParseMontage`/`BindMontageSegments`。
- 规划侧：使用 `vl_asset_edit.exe` 对生成的 `SM_` 验证副本执行 `mesh.get_document` +
  `mesh.validate`，`skinning.montages` 清单与引用路径通过
  `MeshAssetValidator::BuildLoadPlan`。
- `animation_preflight_tests` 全量 320 项通过；`AnimationMontage.*` 27/27。
- `audit_echo_montages.py` 导出的 FBX 与已提交的 `Source/*.fbx` **不是逐字节相同**：
  UE 的 FBX 导出会写入导出元数据/GUID，即使文件大小相同摘要也不同。本批没有替换
  `Source/*.fbx`；运行时仍以已提交的 FBX 和它的 BLAKE3 为输入，artifacts 里的 FBX
  只作为本次审计证据。
- 整场景验证：`SC_valley_echo_playground.json` 真实加载通过；`script play` 后
  `injectinput Interact down` 触发 `EchoActionTrigger`，ReachOut 姿态出现，约 2 s 后
  `EchoActionReturn clip=Idle`。截图保存在 `Generated/Screenshots/echo-action-live.bmp`
  与 `echo-action-return.bmp`。

## 未解决风险

- **分层运行时**：`FullBodyLayer` 当前是整身直通；如果将来要做真正的上下身分层，VL 需要新的
  layer interface / slot blend 数据模型与执行层，不能把 `slot` 字段当成已实现的分层。
- **Post Process AnimBP**：Echo 的 Rig/物理后处理完全不在 VL 数据层；角色外观与 UE 的差异
  包含这部分。
- **BP notify 逻辑**：`NotifyState_ExitToLoco_C` 的数据在，但行为不在；需要 C# 侧消费 feedback。
- **`ReachOut_Montage_RM` 的 warp 目标**：资产可用，但 `CharacterLocation`/`FacingRotation`
  需要场景调用方显式提供；当前 Interact 示例只播放无 warp 的 `ReachOut_End_Montage`。
- **多段 warp 合流**：跨段窗口在每段分别裁剪；Echo 的 ReachOut 只跨边界 1e-4 s，
  未验证 UE 多 modifier 在更长跨段窗口上的逐 tick 合并等价性。
- **BlendIn/BlendOut**：VaultOver 的 0.15 s blend-in 未被表达；VL 播放是硬切/调用方混合。
