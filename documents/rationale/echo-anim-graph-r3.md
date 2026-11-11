# Echo AnimationGraph R3 记录

> 证据记录，不是契约。当前行为以
> `documents/contracts/animation-runtime-skinning.md` 与源码为准。

## 目标

R3 把 Echo locomotion 从硬编码 `LocomotionStateMachine` + `LocomotionClipSet`
迁到正式的 C# 数据驱动 AnimationGraph。Echo 只使用 StateMachine、SequencePlayer、
SyncGroup/MarkerSync 数据、MoveData 曲线条件、Inertialization 标记和
Fall/Land 一次性转移；BlendSpace、mask 层、AimOffset、TurnInPlace、Pivot、
FootPlacement、additive、SlopeWarping 不在本阶段。

## 实现落点

- `script/VulkanLearn.Runtime/AnimationGraphCondition.cs`
  - 叶条件与 `All`/`Any`/`Not` 组合；深度 8、节点 128 的独立预算。
  - 曲线条件缺键为 false；方向曲线缺失时使用未缩放相对 yaw。
- `script/VulkanLearn.Runtime/AnimationGraphDefinition.cs`
  - `AnimationGraphDefinition`、`StateMachine`、`State`、`Transition`、
    `AnimationGraphDirectionBranch` 与校验。
  - 状态迁移环合法；嵌套状态机引用环被拒绝。Entry 可达性单独校验。
- `script/VulkanLearn.Runtime/AnimationGraphRuntime.cs`
  - 每帧冻结 `AnimationGraphInput`，输出 `AnimationGraphDecision`。
  - 外层状态机先求值；外层迁移后重建内层 entry 栈，同帧继续检查内层 entry。
  - 外层 transition 拥有内层状态机的整体交叉淡入；内层 transition 只在无外层迁移时
    接管 blend/sync/inertialization。
- `script/VulkanLearn.Runtime/LocomotionAnimationGraph.cs`
  - 通用 locomotion 图模板：片段、曲线名、过渡时间和阈值全部由
    `LocomotionGraphConfig` 提供，模板本身不含 Echo 字符串。
- `Maps/SC_valley_echo/Blueprints/EchoLocomotionGraph.cs`
  - Echo 的唯一片段/时间/曲线名来源；24 个动画里本阶段使用的 Idle/Walk/Jog、
    方向起步、Stop、Fall、Land 与 `Locomotion` 同步组都在这里。
- `LocomotionAnimationBlueprint`
  - 保留 `[Expose]` 参数与 `SetLocomotionInput`；内部只构造图输入、调用
    `AnimationGraphRuntime.Update`，并按 decision 提交 `Play`/`PlayTransition`。
  - `LocomotionStateMachine` 仅保留为旧行为对照与 `LocomotionDecision` 兼容投影，
    不再参与片段选择。

## Echo 图语义

- 外层：Idle/Walk/Jog/Airborne/Land 五个状态；Walk/Jog 各有内层状态机。
- Idle 内层：Entry 仅在从 Walk/Jog 进入时按 `Speed > 200` 选 Jog Stop、
  `Speed > 1` 选 Walk Stop，否则直接 Idle Loop；Stop 完成以 0.5s 回 Idle。
- Walk/Jog 内层：Entry 仅从 Idle 进入时按方向分支选 F/Left90/Left180/Right90/
  Right180；从 Walk↔Jog 或 Land 进入直接 Cycles；Start 完成以 0.5s 同步相位进
  Cycles。
- 外层时间：Idle→Walk/Jog 0.2s；Walk→Idle/Jog→Idle 0.3s 且 Inertialization；
  Walk→Jog 0.2s 且 Inertialization；Jog→Walk 0.3s；Fall/Land 0.2s。
- 同步：Walk↔Jog、Start→Cycles 输出 `Locomotion` 组与候选 syncPhase；
  宿主仍先检查目标 clip 是否真的声明该组。
- 落地：Airborne 循环 Fall；接地后进 Land，Land 完成后再按输入回到
  Idle/Walk/Jog。因此 Land 一次性片段不会被地面状态提前打断。

## 对齐与测试

- `ScriptLocomotionTest` 原 20 项继续通过，说明起停、方向、曲线、SyncGroup、
  Fall/Land 的既有验收没有回归。
- 新增 4 项：
  - `AnimationGraphFakeCharacterChangesOnlyData`：伪造 `Fake_Rest`/`Fake_Move`
    图，证明换图不需要改 runtime。
  - `AnimationGraphValidationRejectsInvalidDefinitions`：重复状态、未知目标、
    嵌套环、不可达、空 clip、非法阈值、预算上限七类拒绝。
  - `EchoResourceGraphDefinesClipsWithoutCharacterCode`：直接构建资源侧 Echo 图，
    选中 `Idle_to_Walk_Fwd`、0.2s、`StartWalk`、Forward。
  - `AnimationGraphMatchesLegacyLocomotionDecisions`：旧状态机与新图在固定输入序列
    上逐帧比较 state/mode/transition/direction，零不一致。
- 新增 `FormalFrameRates/ScriptAnimationGraphRateTest` 3 项：30/60/120 Hz 跑
  完整起停 + 空中/落地序列，9 秒后都收敛到 Idle/None。
- `animation_preflight_tests` 314 项通过。
- `build/bin/script_tests.exe --gtest_filter=ScriptLocomotionTest.*` 24 项通过；
  图帧率参数化 3 项也通过。

## 真实场景

```powershell
build\bin\main.exe --initial-scene "Maps/SC_valley_echo/SC_valley_echo_playground.json" `
  --console-command "script play" --console-command "screenshot echo-graph-r3.bmp" `
  --console-command-delay 180
```

- World/graph transaction generation=1 提交，场景为真实
  `SC_valley_echo_playground.json`。
- `script play` 返回 state 1；GPU pre-skin 为 20 个对象、506,847 顶点、7,928 组。
- 截图输出到 `Generated/Screenshots/echo-graph-r3.bmp`，角色处于 Idle 姿态。
- 额外尝试 `injectinput MoveForward down` 时，控制台在记录 “Input injected” 后抛出
  `nlohmann::json type_error: cannot use value() with null`；这是既有输入注入路径的
  问题，未被 R3 图逻辑使用。R3 的验证不依赖该命令。

## 已知边界

- Slot/Montage 动作层未接入图，R4 处理。
- BlendSpace、mask/layered blend、AimOffset、TurnInPlace、Pivot、FootPlacement、
  additive、SlopeWarping 未实现；Echo 当前不需要。
- SyncMarker 只作为条件输入与原生反馈存在；R3 图没有新增 marker 分段求值。
- `LocomotionClipSet` 的 Echo 默认名保留用于旧测试/兼容宿主；Echo 正式路径由
  `EchoLocomotionGraph.cs` 提供。
