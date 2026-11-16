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
- `AnimationPoseGraph`：纯代码节点图（无编辑器），把 Player/Blend/Additive/
  Layer/Slot/CachedPose 求值后扁平化到现有原生决策；尚未替换状态机输出，
  先用于验证节点语义与后续迁移。
- `AutomaticRule` / `MinTimeBeforeReentry` / `TransitionRequest`：按剩余时间触发
  （开启后忽略普通条件）、按状态机时钟冷却、事件请求在下一个 `Update` 内消费一次；
  automatic 只在源状态就是当前播放片段时有效，loop 按当前循环剩余时间计算。
- `CachedPoseBranches`：每帧先于主状态求值的命名分支（clip 或 1D blend），
  `UseCachedPose` 优先读同帧结果；Third Person 的 `Locomotion` 分支引用
  `BS_MM_WalkRun`，Land 读取本帧 Walk/Run 姿态再叠加 `MM_Land` additive。
- `LayeredBoneBlend`：原生 base → additive → layer 顺序，按骨骼 mask 逐关节
  混合 overlay（空 mask 全身、未知骨骼拒绝）；官方模板未使用分层，本轮只补
  通用能力与测试。
- Pose Graph 首组：`SaveCachedPose`/`UseCachedPose` 用实例级局部姿态快照，
  `AdditiveClip` 按 UE `AAT_LocalSpaceBase` 的逐通道局部差值复合，
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

- `animation_preflight_tests`：382/382 通过，覆盖片段骨骼子集导入、
  `SkeletalBlendSpace` 相位/端点/夹紧、`SkeletalAdditive`/`SkeletalCachedPose`，
  `SkeletalLayered` 的 mask/alpha/顺序，以及 `SkeletalSlotMontage` 的
  masked overlay/完成语义，以及 `SkeletalSlotMontage` 的 BlendProfile 权重。
- `script_tests --gtest_filter=ScriptLocomotionTest.*`：37/37 通过，覆盖
  alias/VerticalSpeed/BlendSpace（230/365/500、端点、三 sample 区间、0.1 s 重插值）
  与 Conduit（同帧落地、入口规则回落、链、环、入口 conduit）、以及
  `MaxTransitionsPerFrame`/`SkipFirstUpdateTransition`/Automatic Rule/
  MinTimeBeforeReentry/Transition Request、Pose Graph 决策输出与
  LayeredBoneBlend/BlendProfile 字段；校验 mask 扩展到 7864319。
- 整套 `script_tests`：516 通过；失败为 19 个 `ScriptBuildTest`（在工程根创建
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

## 5. 明确差距（2026-11-13 当时）

- 当时 `Walk / Run` 已改为原生 BlendSpace1D（Walk 230 ↔ Run 500，按平面速度轴），
  相位同步、权重夹紧、端点稳定与权重 5/s 插值均已实现；1D 之外尚不支持
  BlendSpace2D、每 sample 播放速率或 per-sample 同步组。
- 当时未实现上半身骨骼 mask 分层、AimOffset、TurnInPlace、Pivot、
  完整 camera mode stack、ControlRig 逐骨/双足 IK。
- 当前 Manny 材质为中性 PBR，不宣称与 UE 成片材质等价。

后续状态见第 6、7 节；本节保留当时判断，不覆盖后续实现。

## 6. 2026-11-15：BlendSpace2D / MontagePlayer / Slot 补全

本节更新第 5 节的“明确差距”：BlendSpace2D、per-sample rate/syncGroup、
图内 MontagePlayer 与双输入 Slot 已实现；AimOffset / TurnInPlace / Pivot 仍需
完整 SK_Mannequin 动画资产，未在本次范围内。

- `animation_preflight_tests`：当时 399/399 通过（加入落地参考帧回归后为
  401/401，见第 7 节）。新增
  `SkeletalBlendSpace2D` 四角/中心双线性权重、per-sample rate 相位和非法网格拒绝，
  以及 `SkeletalPoseGraph.MontageSlot*` 的 section 推进、notify begin/end、
  Slot 白名单遮罩、未激活 pass-through、未知 Montage/section/输入拒绝，以及
  MontagePlayer 作为 rootSource 时的独立时钟/notify 与 warp target 必需/提交路径。
  另新增 `InPlaceStripsSequenceRootMotion`、入口根源/Crossfade、BlendSpace 根源
  时钟和一次性完成测试，锁定原生图入口与 in-place 策略。
- `ScriptLocomotionTest.*`：40/40 通过；新增 `PoseGraph2DAndMontageBridge`
  验证双轴值、四个 2D 权重、`MontageId` 透传、`StartPosition=-1` 规范化与非法
  定义拒绝，另新增 `PoseNodeStateCompletedUsesEffectiveClip` 锁定 PoseNode
  状态的 `StateCompleted` 语义。
- 整套 `script_tests`：540 项中 531 通过、1 跳过；8 项失败为 7 个
  `ScriptBuildTest` 的临时目录拒绝与已知
  `FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows`，
  无新增动画失败。
- Release `main_game` 带 `script play` 冒烟退出 0；截图序列
  `artifacts/third-person-shots/acc2_*.bmp` 覆盖起步三帧、行走、跳跃、
  落地、落地后恢复和停止；日志确认
  `Jump → Fall Loop → Land → Locomotion`、`Walk / Run → Idle`，落地后正常回到 locomotion，
  未观察到穿地、卡死或飞出。截图使用临时 `AutoWalkTest`/`DebugLocomotion`
  覆盖和手动相机 pose，验收后资源工作树已恢复干净。

复核移动速度时发现原生 Pose Graph 绕过了 `Play()` 的 `inPlace` 参数：C# 的
`InPlace=true` 没有同步给原生播放器，Walk/Run 源根位移仍叠在胶囊位移上，表现为
角色在胶囊内滑步、速度感异常。现在 `AnimationGraphBlueprint` 通过
`poseGraphInPlace` 显式提交；实测 actor 仍为 230/500 cm/s，修复后的 foot 局部
Z 在 `[-55, 33] cm` 内摆动，而修复前会随根轨前进到 450 cm 以上。

继续独立复核又确认两处未对齐：原生图默认把**第一个 SequencePlayer（Idle）**
当成永久根源，导致 Walk/Jump 时反馈仍是 `MM_Idle`；状态入口切换也只换 Output，
没有入口时间与 Crossfade。现在 `SetPoseGraphEntry(id, startPosition,
blendSeconds)` 解析入口可达的根源、重置目标时钟，并用当前已发布姿态做线性
Crossfade。隔离运行在 500 cm/s 下反馈为 `MM_Walk_Fwd`，时间按 2.733 s 循环；
`EntrySwitchMovesRootClockAndCrossfades` 与 `BlendSpaceEntryProvidesRootClockAndClip`
锁定该行为。

继续核对落地链路还发现两处图语义缺口：PoseNode 状态没有静态 `Clip`，
`StateCompleted` 原先永远看不到实际 Jump 片段的完成事件；另一个一次性状态
完成后 `playing=false` 会被 Fall/Land 继承，导致新入口时钟停在 0。现在
`AnimationGraphRuntime` 用上一帧实际求值 `EffectiveClip` 判定完成，入口在
非显式暂停时重新开启目标播放器；实测状态序列恢复为
`Jump → Fall Loop → Land → Locomotion`，Land 时间正常推进。

另外以 UE 5.8 CDO 复核了官方移动参数：`MaxWalkSpeed=500`、
`MaxAcceleration=1500`、`BrakingDecelerationWalking=2000`、
`JumpZVelocity=700`、`AirControl=0.35`。VL 原先默认无 Shift 只走 230 cm/s，
且加速/刹车/跳跃为 2048/3000/500，确实不是模板默认手感。现在默认 500 cm/s，
Shift 改为 230 cm/s 的慢走观察输入；物理参数按 CDO 对齐。`Walk/Run` 动画资产
本身仍是官方 230/500 两个 sample，不需要改写 BS。

## 7. 2026-11-16：落地严重变形的根因与修复

实机复核 `Jump → Land` 时，落地中段仍出现腿部折叠/躯干压缩。独立对照 UE
源码和资产后，确认不是过渡时长问题，而是 additive 参考姿态与平移差值两处语义
同时不对：

- `MM_Land` 的 UE T3D 声明 `AdditiveAnimType=AAT_LocalSpaceBase`、
  `RefPoseType=ABPT_LocalAnimFrame`、`RefFrameIndex=26`，
  `SequenceLength=0.866667`、30 fps；sidecar 的 `numFrames=26`、
  `numKeys=27`。UE `UAnimSequence::GetSequencePose` 使用
  `FrameIndex / GetNumberOfSampledKeys() * PlayLength()`，因此参考时间是
  `26 / 27 × 0.866667 = 0.8333 s`。
- 旧实现按 `RefFrameIndex / frameRate` 取 `26 / 30 = 0.8667 s`，把参考姿态
  取成了末帧；同时 sidecar 原先没有 `refFrameIndex` 时还会回退绑定姿态。
  现在 sidecar 导出/解析 `rootMotion.refFrameIndex`，参考姿态优先按 UE 的
  采样键公式取本片段该时刻；只有未声明时才回退绑定姿态。
- `FAnimationRuntime::ConvertTransformToAdditive` 的平移是局部父空间直接相减
  `additive.translation - reference.translation`；旧实现把 additive/reference
  整体 TRS 做矩阵除法，等价于再次旋转参考平移，腿骨/手指这类带偏移的骨骼会被
  折叠。旋转仍为 `additive.rotation * inverse(reference.rotation)`，缩放保存
  ratio−1；累加按 `FTransform::AccumulateWithAdditiveScale` 做
  “平移相加、旋转预乘、缩放相乘”。

修复覆盖 `animationSequenceData`、`skeletalPlaybackTimeline` 的
`AdditiveReferencePose`、旧 additive 路径与原生 Pose Graph 的 `ApplyAdditive`；
资源侧 `MM_Land.anim.json` 增加 `refFrameIndex: 26`。回归测试
`LocalSpaceTranslationUsesParentSpaceDifference` 用一段参考帧姿态同时改变平移
和旋转的剪辑区分“直接相减”与旧矩阵除法，并锁定参考时间按采样键数而不是帧率。

最终验证（Release，`artifacts/third-person-test-build`）：

- `animation_preflight_tests`：当时 401/401；后续下游修复后为 412/412（第 8 节）。
- `ScriptLocomotionTest.*`：40/40 通过。
- 整套 `script_tests`：540 项中 531 通过、1 跳过；8 项既有沙箱/主干失败
  （7 个 `ScriptBuildTest` 临时目录拒绝 + 1 个
  `FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows`），
  无新增动画失败。
- Release `main_game` 在 `SC_third_person_playground` 上
  `--exit-after-frames 120` 退出 0。
- 连续窗口抓帧：`artifacts/third-person-shots/acc6_window/acc6_008.bmp` 到
  `acc6_019.bmp` 覆盖空中、落地压缩、恢复与回到跑动；接触表
  `artifacts/third-person-shots/acc6_contact.bmp`，状态日志
  `artifacts/third-person-shots/acc6_land.log`。日志为
  `Jump → Fall Loop → Land → Locomotion/Walk / Run`，Land 期间无甩飞、折叠、
  穿地或长时间冻结。抓帧使用临时资源副本中的 `AutoWalkTest`，验收后
  `config/config.json` 已恢复。

## 8. 2026-11-16：Pose Graph 下游交叉核验与修复

独立审查复核了原生/C# Pose Graph 的边界，确认落地主链已正确，同时发现以下
契约级缺口并修复：

- 空 Slot `weights` 现在展开为全身权重 1；未列出骨骼仍按白名单 0。
- `UseCachedPose` 把同名 `SaveCachedPose` 作为隐式拓扑依赖，并在每帧求值前清空
  帧内缓存；反向声明顺序也能同帧连接。
- MontagePlayer 的 entry `startPosition` 生效；正值是上升沿，非循环 Montage
  完成后持续正值不会自动重播，必须显式 `0 → 正值` 或使用节点 `Loop`；
  活动期间更换 warp targets 从当前时钟重建；父图切换 `InPlace` 会同步子播放器。
- 2D BlendSpace 的根源/完成参考统一为声明顺序第一个 sample；C# 增加
  `PoseGraphAxisY` 数据通道并传入 `AnimationGraphRuntime`；1D 图节点读取 BS 资产
  `weightSpeedPerSec`，不再固定 5/s。
- Additive 部分权重的旋转改为 UE `FastLerp` 后归一化；sidecar 增加
  `refPoseType` 区分，`MM_Land.anim.json` 明确
  `refPoseType: LocalAnimFrame`，外部 RefPoseSeq 的 `AnimFrame` 在导入期拒绝。
- 图内 Layer/Slot 支持 `BlendProfile` 资产名，先展开为白名单 `Weights` 再提交；
  C# `AnimationPoseGraph` 与原生校验保持一致。
- 非根源 Montage（Slot overlay）启动/重播不再改写父 SequencePlayer 的时钟、
  播放范围或 `Time`；`ClearPoseGraph()` 会清除旧入口，替换图后从 Output 重新
  解析根源。对应新增 `NonRootMontageDoesNotResetBaseClock` 与
  `ReplacingGraphClearsStaleEntry` 测试。

最终验证：

- `animation_preflight_tests`：412/412 通过（新增非根源 Montage 不改 base 时钟、替换图清旧入口测试）。
- `ScriptLocomotionTest.*`：41/41 通过；C# graph BlendProfile/2D/StateCompleted/Montage
  探针均通过。
- 整套 `script_tests`：541 项中 532 通过、1 跳过；仍是既有 7 个
  `ScriptBuildTest` 环境失败与 1 个
  `FormalGaspFootPlacementDynamicQualityUsesFrozenRawRootStanceWindows`，
  无新增动画失败。
- Release `main_game` 120 帧 smoke exit 0；`py -3 tool\validate-docs.py` 通过
  （最终结果以最后一次门禁为准）。
- 下游修复后的连续窗口抓帧保存在
  `artifacts/third-person-shots/acc8_window/`，接触表
  `artifacts/third-person-shots/acc8_contact.bmp`，日志
  `artifacts/third-person-shots/acc8_land.log`；第 8–19 帧落地段仍未出现
  折叠、甩飞或穿地（尾部黑帧是抓屏窗口关闭后的无效画面）。

仍保留的真实限制：图内 FrameCurves 只覆盖扁平路径/根源，尚未实现任意图节点的
完整曲线权重传播；UE `ReinitializeOnBecomingRelevant` 不适用 VL 每帧求值；
BlendProfile 曲线、virtual bones、多槽位 Slot、AimOffset/TurnInPlace/Pivot
以及外部 RefPoseSeq 仍未实现。这些不再被写成已完成行为。

## 9. 2026-11-16：Manny 双材质贴图修正

角色贴图异常不是 UV0 越界或 V 翻转。FBX 实际包含两个 section/material：

- mesh 0：`MI_Manny_02`，期望 `T_Manny_02_N`。
- mesh 1：`MI_Manny_01`，期望 `T_Manny_01_N`。

初版资源只导入了 `T_Manny_01_D/BN`，并把两个 material slot 都指向同一个
`MI_third_person_mannequin`，导致 mesh 0 使用 01 atlas，胸甲/肩部出现错误
UV 岛和色块。修正后：

- 从 UE `effect` 工程只读导出 `T_Manny_02_D/BN`，新增
  `T_ue_manny_02_albedo/normal` 与独立 MI。
- `SKM_ThirdPersonManny.json` 按 FBX slot 顺序映射
  `MI_Manny_02 → MI_third_person_mannequin_02`、
  `MI_Manny_01 → MI_third_person_mannequin_01`。
- 两个 section 的 UV0 都落在 `[0,1]`，没有零 UV 或需要翻转的通道证据；
  头部为 Manny 的平滑面罩造型，不是 UV 丢失。

验证：Release `main_game` 120 帧 smoke exit 0；近景证据
`artifacts/third-person-shots/texture_fix_probe/fixed_front.bmp` 与
`fixed_torso.bmp`，胸甲/肩部图案与 `T_Manny_02_D` atlas 对齐。
