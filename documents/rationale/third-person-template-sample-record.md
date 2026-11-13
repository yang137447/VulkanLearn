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
- `Conduit`：UE `bIsAConduit` 的无姿态过路状态，带 `EntryRule`；目标为 conduit 时
  同一帧递归它的出边，visited 防环，最终采用“conduit → 内容状态”那条 transition
  的混合参数。状态机新增 `AllowConduitEntryStates`。实现依据 UE 5.8
  `AnimNode_StateMachine.cpp` 的 `FindValidTransition`（入口规则、visited 状态缓存、
  同帧递归与 `SetStateInternal` 的 conduit 入口约束）。
- `MaxTransitionsPerFrame` / `SkipFirstUpdateTransition`：按 UE
  `FAnimNode_StateMachine` 的循环预算与首帧丢混合语义实现；Third Person 图显式
  使用 UE 类默认值 3 / true，VL 自身的默认值保持 1 / false 以保留原有预算与混合
  行为；entry 帧开始检查常规转移是与 UE 对齐的新行为。
- `LayeredBoneBlend`：原生 base → additive → layer 顺序，按骨骼 mask 逐关节
  混合 overlay（空 mask 全身、未知骨骼拒绝）；官方模板未使用分层，本轮只补
  通用能力与测试。
- Pose Graph 首组：`SaveCachedPose`/`UseCachedPose` 用实例级局部姿态快照，
  `AdditiveClip` 按 UE `AAT_LocalSpaceBase` 的 `base * inverse(reference)` 复合，
  `Slot` 先作为图级元数据。Third Person 的 `Land` 改为
  `UseCachedPose = "Locomotion"` + `AdditiveClip = "MM_Land"`，与官方 ABP 的
  `UseCachedPose + ApplyAdditive(Alpha=1)` 一致。
- Additive 扩展：`RotationOffsetMeshSpace` 按 UE 的“局部→mesh 空间→累加→回局部”
  只处理旋转增量；`FrameCurves()` 汇总基础片段（BlendSpace 加权）与 additive
  片段曲线并由 feedback 发布。
- BlendSpace 资产：`BS_MM_WalkRun.json` 由 `skinning.blendSpaces` 引用，原生
  loader 校验后经 animation catalog 下发；图只写 `BlendSpace = "BS_MM_WalkRun"`，
  宿主在 catalog 可用时解析成运行期 samples（prepare 阶段创建组件，故解析延迟到
  start 后的首次求值）。运行时 365 cm/s 仍得到 `MM_Walk_Fwd ↔ MM_Run_Fwd 0.500`。
  片段（×alpha）的同名曲线，并随反馈的 `curves` 发布（≤64 键、跳过 0 值）。
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

Pose Graph 的 UE 依据来自 `tool/animation/inspect_third_person_additive.py`
（输出 `artifacts/third-person-additive-audit/additive-audit.json`）：
根图 `SaveCachedPose` 的 `CachePoseName=""`、`Slot` 的 `SlotName="DefaultSlot"`、
`Land` 的 `ApplyAdditive` 为 `Alpha=1.000000`；同一工程导出的
`MM_Land.anim.json` 带 `rootMotion.additiveAnimType = "LocalSpaceBase"`。

回归证据（Release，`artifacts/third-person-test-build`）：

- `animation_preflight_tests`：375/375 通过，覆盖片段骨骼子集导入、
  `SkeletalBlendSpace` 相位/端点/夹紧、`SkeletalAdditive`/`SkeletalCachedPose`，
  `SkeletalLayered` 的 mask/alpha/顺序，以及 `SkeletalSlotMontage` 的
  masked overlay/完成语义。
- `script_tests --gtest_filter=ScriptLocomotionTest.*`：35/35 通过，覆盖
  alias/VerticalSpeed/BlendSpace（230/365/500、端点、三 sample 区间、0.1 s 重插值）
  与 Conduit（同帧落地、入口规则回落、链、环、入口 conduit）、以及
  `MaxTransitionsPerFrame`/`SkipFirstUpdateTransition`、Pose Graph 决策输出与
  LayeredBoneBlend 字段；校验 mask 扩展到 7864319。
- 整套 `script_tests`：514 通过；失败为 19 个 `ScriptBuildTest`（在工程根创建
  `vl-build-*` 时被本机沙箱拒绝）加 1 个偶发的
  `FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows`。
  后者曾在同一资源上出现约 11 cm 支撑误差；2026-10-24 的主干二进制通过同一用例，
  指向 2026-11-11 的 `382cc46 valley echo: ... bone physics WIP`
  （`skeletalPlayback.cpp` +999 行）带来的主干不稳定项，不是本次 Third Person 改动。
- `py -3 tool\validate-docs.py` 通过。
- 测试写盘注意：本机沙箱只允许原生产物写入主工作区，因此整套 `script_tests`
  需把 `config.json -> resourcePath` 临时指向主工作区内的资源副本，并把
  `TEMP`/`TMP` 指到同一工作区；资源工作树本身对原生进程只读。
  `FormalGasp*` 用例还会通过编译期脚本资源根读取
  `Generated/MotionMatching` 缓存，缓存缺失时会因写入被拒而整组失败，
  验收前需先把缓存复制到工作树的对应目录。
- 运行时输入注入曾暴露一个既有崩溃：Play 尚未建立 action 表时
  `InputSubsystem::UpdateActionState` 对 `operator[]` 产生的 null 调用 `value()`；
  现在缺失节点会补成 `{held,pressed,released}` 完整形状，注入不再终止进程。

## 5. 明确差距

- `Walk / Run` 已改为原生 BlendSpace1D（Walk 230 ↔ Run 500，按平面速度轴），
  相位同步、权重夹紧、端点稳定与权重 5/s 插值均已实现；1D 之外仍不支持
  BlendSpace2D、每 sample 播放速率或 per-sample 同步组。
- 未实现上半身骨骼 mask 分层、AimOffset、TurnInPlace、Pivot、
  完整 camera mode stack、ControlRig 逐骨/双足 IK。
- 当前 Manny 材质为中性 PBR，不宣称与 UE 成片材质等价。
