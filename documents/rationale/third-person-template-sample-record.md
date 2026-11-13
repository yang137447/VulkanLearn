# UE Third Person Template 复刻记录

> 证据记录，不是契约。日期：2026-11-13。
> 来源：本机 `D:\YYBWorkSpace\UnrealProject\ParallaxTest` 的 UE 5.7 Third Person
> Template；只读导出使用 UE 5.8.4 `UnrealEditor-Cmd.exe`。

## 1. 官方 ABP 结构

`ABP_Manny` 有两个嵌套状态机：

- `Locomotion`：`Idle` 与 `Walk / Run`；`Walk / Run` 使用
  `BS_MM_WalkRun`（Walk 230 cm/s、Run 500 cm/s）。
- `Main States`：`Locomotion`、`Jump`、`Fall Loop`、`Land`；使用
  `To Falling`（Locomotion/Land）和 `To Land`（Jump/Fall Loop）两个 State Alias。

T3D 审计还记录到 `MaxTransitionsPerFrame`、`SkipFirstUpdateTransition`、
`ReinitializeOnBecomingRelevant` 与 `Allow Conduit Entry States` 等状态机设置。

## 2. VL 资产

资源工作树新增：

```text
Maps/SC_third_person/
  Skeletons/SK_ThirdPersonManny.json
  Meshes/SKM_ThirdPersonManny.json
  Source/SKM_Manny.fbx
  Source/MM_Idle.fbx + .anim.json
  Source/MM_Walk_InPlace.fbx + .anim.json
  Source/MM_Walk_Fwd.fbx + .anim.json
  Source/MM_Run_Fwd.fbx + .anim.json
  Source/MM_Jump.fbx + .anim.json
  Source/MM_Fall_Loop.fbx + .anim.json
  Source/MM_Land.fbx + .anim.json
  Blueprints/BP_third_person_character.json
  Blueprints/ThirdPersonCharacterBlueprint.blueprint.cs
  Blueprints/ThirdPersonAnimationBlueprint.cs
  Blueprints/ThirdPersonLocomotionGraph.cs
  SC_third_person_playground.json
```

导出结果为 161 骨、7 段动画、LOD0 两个 section。`MM_Idle` / `MM_Walk_InPlace` /
`MM_Walk_Fwd` / `MM_Run_Fwd` 的 UE 片段只写 89 根实际变化骨骼，`MM_Jump` /
`MM_Fall_Loop` / `MM_Land` 写 159 根；VL 现在允许片段骨骼层级是网格骨架的子集，
缺失关节回退绑定姿态。`MM_Walk_InPlace` 与 `MM_Walk_Fwd` 时长同为 2.7333 s。

## 3. VL 实现

- `AnimationGraphBlueprint`：通用数据驱动图宿主，只负责 `AnimationGraphRuntime`
  求值与 `Play`/`PlayTransition` 提交。
- `AnimationGraphStateAlias`：UE State Alias 的共享转移来源集合；
  `AnimationGraphRuntime.SelectTransition` 按 alias 展开。
- `VerticalSpeed`：状态机条件叶节点，用于区分起跳（`> 0`）与自由落体（`<= 0`）。
- `ThirdPersonLocomotionGraph`：两层状态机；`Idle` 与 `Walk / Run`
  BlendSpace1D（Speed 轴 0 = Walk_InPlace、230 = Walk、500 = Run），以及
  `Jump`/`Fall Loop`/`Land`，共享 `To Falling` / `To Land` alias。
- `ThirdPersonCharacterBlueprint`：Jolt 胶囊、SpringArm、视角、加速/刹车和跳跃；
  `AutoWalkTest` 是默认关闭的自动化探针。

## 4. 验证证据

Release `main_game` 从 `artifacts/third-person-runtime`（主工作区内的临时 runtime
sandbox）启动，加载 `Maps/SC_third_person/SC_third_person_playground.json`：

```text
script play
injectinput Jump down
TPGraph state=Jump clip=MM_Jump ...
TPGraph state=Fall Loop clip=MM_Fall_Loop ...
TPGraph state=Land clip=MM_Land ...
TPGraph state=Locomotion/Idle clip=MM_Idle ...
```

自动左移探针（`AutoWalkTest`，临时把 `WalkSpeed` 设为 115 / 365 / 499）得到：

```text
TPGraph state=Locomotion/Walk / Run clip=MM_Walk_Fwd blend=MM_Walk_InPlace:0.500 speed=115.0 grounded=True
TPGraph state=Locomotion/Walk / Run clip=MM_Walk_Fwd blend=MM_Run_Fwd:0.500      speed=365.0 grounded=True
TPGraph state=Locomotion/Walk / Run clip=MM_Walk_Fwd blend=MM_Run_Fwd:1.000      speed=499.0 grounded=True
```

三段都以 `MM_Walk_Fwd`（2.7333 s）作为固定 A，因此跨 230 内部边界不会切换
播放时钟；B 的权重按片段分别缓存，230 两侧都从 0 起步。截图保存在
`artifacts/third-person-shots/tp_blend_low_fixed.png`（低段 0.5）、
`tp_blend_mid365.png`（中段 0.5）与 `tp_blend_run.png`（≈纯 Run）。

UE 基准来自只读 T3D 导出
`...\third-person-template\ue-third-person-audit\BS_MM_WalkRun.t3d`：
`InterpolationParam(0)=(InterpolationType=BSIT_EaseInOut)`、
`TargetWeightInterpolationSpeedPerSec=5.000000`、`AnimLength=2.733333`、
`BlendParameters(0)=(DisplayName="Speed",Max=500)`，sample 为
`MM_Run_Fwd @ 500`、`MM_Walk_Fwd @ 230`、`MM_Walk_InPlace @ 0`。

回归证据（Release，`artifacts/third-person-test-build`）：

- `animation_preflight_tests`：360/360 通过，覆盖片段骨骼子集导入与
  `SkeletalBlendSpace` 相位/端点/夹紧。
- `script_tests`：509 通过；20 个 `ScriptBuildTest` 在工程根创建
  `vl-build-*` 目录时被本机沙箱拒绝，与本改动无关。
  `FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows` 本轮通过，
  但曾在同一资源上出现约 11 cm 支撑误差；2026-10-24 的主干二进制通过同一用例，
  指向 2026-11-11 的 `382cc46 valley echo: ... bone physics WIP`
  （`skeletalPlayback.cpp` +999 行）带来的主干不稳定项，不是本次 Third Person 改动。
- `ScriptLocomotionTest.*`（含 alias/VerticalSpeed/BlendSpace 用例）30/30 通过；
  `py -3 tool\validate-docs.py` 通过。
- 测试写盘注意：本机沙箱只允许原生产物写入主工作区，因此整套 `script_tests`
  需把 `config.json -> resourcePath` 临时指向主工作区内的资源副本，并把
  `TEMP`/`TMP` 指到同一工作区；资源工作树本身对原生进程只读。

BlendSpace1D 升级后的证据（同一 Release 构建）：

- `animation_preflight_tests`：360/360 通过，其中 `SkeletalBlendSpace.*` 3 个用例
  覆盖相位对齐（A 4 s / B 2 s 时 B 采样 `phase*duration`）、端点权重 0/1 与
  单片段逐元素一致、权重夹紧、A 不匹配拒绝、`TransitionTo` 清除混合源。
- `script_tests --gtest_filter=ScriptLocomotionTest.*`：30/30 通过，新增
  `BlendSpaceSelectsClipsAndWeightsOnSpeedAxis` 覆盖 230/365/500 的片段与权重、
  端点、三 sample 区间选择与 0.1 s 权重重插值；校验 mask 扩展到 4095。
- 整套 `script_tests`：509 通过；失败集合与上一节相同（20 个沙箱写盘 + 1 个
  主干 WIP 回归），没有新增失败。
- 运行时输入注入曾暴露一个既有崩溃：Play 尚未建立 action 表时
  `InputSubsystem::UpdateActionState` 对 `operator[]` 产生的 null 调用 `value()`；
  现在缺失节点会补成 `{held,pressed,released}` 完整形状，注入不再终止进程。

## 5. 明确差距

- `Walk / Run` 已改为原生 BlendSpace1D（Walk 230 ↔ Run 500，按平面速度轴），
  相位同步、权重夹紧、端点稳定与权重 5/s 插值均已实现；1D 之外仍不支持
  BlendSpace2D、每 sample 播放速率或 per-sample 同步组。
- 未实现 Conduit、上半身骨骼 mask 分层、AimOffset、TurnInPlace、Pivot、
  完整 camera mode stack、ControlRig 逐骨/双足 IK。
- 当前 Manny 材质为中性 PBR，不宣称与 UE 成片材质等价。
