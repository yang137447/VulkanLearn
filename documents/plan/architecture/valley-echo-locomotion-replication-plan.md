# Valley of the Ancient Echo 角色运动复刻计划

状态：计划，非契约。本文描述**尚未实现**的目标、资产来源与分阶段门禁；当前运行时行为仍以
`documents/contracts/` 下的动画契约和源码为准。

日期：2026-11-04

已确认决定（2026-11-04）：

1. 直接导入 Echo 自有骨架，不先 retarget 到 UEFN Mannequin。
2. 离散步态状态机作为 `VulkanLearn.Runtime` 的正式复用组件，源码独立维护，
   不写进角色蓝图文件；蓝图只负责输入映射、组件组装和动画应用。

## 0. 选题结论

同一台机器上有两个候选工程，结论不同：

| 工程 | 实际内容 | 适合度 |
| --- | --- | --- |
| `SlayAnimationSample` | 纯 Content 的过场动画工程：31 个 shot 目录，角色动作为逐镜头 `TF0xxx_Echo` / `TF0xxx_Oni` AnimSequence，由 Sequencer 驱动；没有 AnimBP、角色移动 C++ 或可玩状态机。可复用循环只有 `Slay_Walk_Oni_T01`、`Slay_HeavyBreathing_Oni_T03` 等少数片段 | 只适合做姿态/演出参考，不适合作为运动系统来源 |
| `ValleyoftheAncient` | 可玩示例：`Enum_LocomotionState` = `Idle` / `Walk` / `Jog`；`Echo_AnimBP` 内含 `AnimationStateMachineGraph`（状态机 `Locomotion`）、`DetermineLocomotionState`、`UpdateLocomotionValues`、`UpdateOnWalkEntry`、`UpdateOnJogEntry`、`UpdateCharacterRotation` 与 `Locomotion State Data`；有方向起步/停止、跳跃下落落地、VaultOver/ReachOut、SpringArm 相机和 Enhanced Input | **主来源** |

Valley 的 Echo 动画集（`Content/AncientContent/Characters/Echo/Animations`）共 27 个资产
（24 段 AnimSequence + 3 个 AnimMontage）：
`Idle`、`Idle_Long`、`Idle_To_Jog_Fwd` / `Idle_to_Jog_Fwd_Left/Right_90/180`、
`Idle_to_Walk_Fwd` / `Idle_to_Walk_Fwd_Left/Right_90/180`、`Walk_Fwd`、`Jog_Fwd`、
`Walk_Fwd_to_Idle`、`Jog_Fwd_to_Idle`、`Jump_Idle_Fall`、`Jump_Idle_Land`、
`VaultOver` / `VaultOver_Montage`、`ReachOut_Start/Loop/End`、
`ReachOut_Montage_RM`、`ReachOut_End_Montage`、`Sitting_Idle`、`StandFromSitting`。
这正是 VL 当前缺少的“离散地面步态 + 方向过渡 + 起停 + 动作分层”素材。

Slay 仍保留价值：Echo/Oni 的镜头级表演可用于对照关键姿势、节奏和镜头感，但**不进入**
本计划的 locomotion 资产集。

## 1. 当前 VL 基线

已经存在、不要重复实现：

- 骨骼导入与运行时蒙皮：`source/animation/animationAssets.*`、`source/animation/skeletalPlayback.*`、
  `source/animation/skinPose.h`；权重不截断，保留原始正权重。
- Montage / section / notify / 曲线：`source/animation/animationMontage.*`、
  `source/animation/animationSequenceData.*`、`source/animation/animationScriptData.*`。
- Motion Warping、Foot Placement、Inertialization、Root Offset：对应源文件与
  `contracts/animation-motion-warping.md`、`contracts/animation-runtime-skinning.md`。
- Motion Matching 与运行时数据库：`source/animation/motionMatching.*`、`motionRuntime.*`、
  `motionRuntimeDatabase.*`；C# 入口 `AnimationPlayback.Match(...)`。
- 通用状态机：`script/VulkanLearn.Runtime/StateMachine.cs`，按优先级降序、一次 `Update` 最多切换一次。
- C# 角色物理、根运动消费与输入：`script/VulkanLearn.Runtime/AnimationPlayback.cs`、
  `source/script/scriptPhysics.cpp`、`source/input/*`。
- 相机：`script/VulkanLearn.Runtime/SpringArm.cs`、`source/render/camera/*`。
- 现有可玩样例：`Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs` 已实现
  `LocomotionState { Idle, Walk, Run, Jump, Fall, Land }`、`MovementMode`、Motion Matching 候选族
  与 traversal。

明确缺口：

- 没有可复用的“离散步态状态机”组件；GASP 的 `LocomotionState` 只存在于样例蓝图。
- 没有按输入角度选择方向起步片段（Fwd / Left90 / Left180 / Right90 / Right180）的数据驱动规则。
- 没有行走/慢跑起停的显式状态与资产选择；现有能力依赖 Motion Matching 数据库。
- 没有上下半身分层/Anim Layer 等价物；现有 Montage 是全身替换。
- SlopeWarping 已有正式原生子集（整身 root 对齐 + 脚部平面高度补偿）；
  Twist/Helpers ControlRig 与逐骨/双足 IK 仍未实现，现有后处理为 Foot Placement、
  Root Offset 与 Slope Warping。
- 相机只有 SpringArm 基础，没有 Valley 的 camera mode stack 行为。

## 2. 范围边界

第一轮只复刻**地面运动**，不把 Valley 整个改造一遍：

1. `Idle` / `Walk` / `Jog` 三态与相互切换；
2. 方向起步（5 个方向 × Walk/Jog）与正向停止；
3. 跳跃、下落、落地；
4. 方向起步内建的转向（Valley 没有独立原地转身片段，不单独实现）；
5. 相机/输入/转向手感；
6. `VaultOver`、`ReachOut`、`Sitting/Stand` 作为 Montage 一次性动作接入。

不在第一轮：

- GAS / Gameplay Ability / AttributeSet；Valley 的 `AbilitySystem` 不迁移。
- Lyra 式完整 camera mode stack；只迁 SpringArm lag / 碰撞 / 俯仰限制。
- Echo 的完整材质、Groom、布料、面部表情与物理资产；运动验证不需要它们全部落地。
- 把 Valley 片段并入 `DB_gasp.json`。除非建立并验证显式 retarget profile，否则两套骨架不共享
  Motion Matching 数据库。
- 用 Slay 的镜头动画充当 gameplay locomotion。

## 3. 资产与导出

### 3.1 落点

资源仓库新增独立地图，避免污染 `SC_gasp`：

```text
Maps/SC_valley_echo/
  Skeletons/SK_Echo.json
  Meshes/SKM_Echo.json
  Animations/*.json
  Source/*.fbx
  Source/*.anim.json
  Blueprints/EchoCharacter.blueprint.cs
  SC_valley_echo_playground.json
```

代码侧不复制 GASP 的专用脚本。`tool/animation/` 是唯一 UE 导出路径；优先把现有
`export_gasp_ue_raw.py` / `export_gasp_sequences.py` / `export_gasp_skeleton.py` 中硬编码的
`ANIMS`、`MESH`、`SKELETON_RELATIVE` 与 Hurdle 专用断言参数化，而不是新增第二套并行导出器。

### 3.2 导出内容

- 骨架与网格：`/Game/AncientContent/Characters/Echo/Meshes/Echo_Skeleton`、`Echo`。
- 24 段 AnimSequence 与 3 个 Montage；保留原始时长、帧率、`enable_root_motion`、`force_root_lock`、
  `root_motion_root_lock`、notify、float curve 与 sync marker。
- `Echo_AnimBP` 的只读 T3D/文本导出，用于复刻状态机节点语义；不要求 VL 解析 AnimBP 图。
- `BP_EchoCharacter` 的移动参数（Walk/Jog 速度、转向、相机臂）只读导出，作为手感基准。

### 3.3 UE 侧约束

- 只在 UE 工程副本上运行 Python commandlet，不保存、不升级原工程；沿用
  `rationale/gasp-p0-asset-baseline-record.md` 的隔离与 BLAKE3 记录方式。
- Valley 的 `.uproject` 声明 `EngineAssociation=5.5`，但 Content 内 uasset 头显示
  `++UE5+Release-5.0-EarlyAccess`；导出必须记录实际编辑器版本，不能按扩展名推断。
- 当前机器未检测到可用的 `UnrealEditor.exe`。资产导出门禁因此先于代码实现：
  没有匹配编辑器时，本阶段保持阻塞，不用手写或猜测 clip 数据替代。

## 4. 状态机语义

### 4.1 状态划分

镜像 Valley 的两层划分，不把空中状态塞进 `LocomotionState`：

- `LocomotionState { Idle, Walk, Jog }`：只描述地面步态。
- `MovementMode { Walking, Falling, Catch }`：沿用 VL 现有模式，由物理接地与竖直速度决定。
- `Fall/Land` 是一次性片段，由动画反馈的 `Completed` 驱动，不是长期步态状态。

### 4.2 转移规则

| 从 | 到 | 条件 | 片段 |
| --- | --- | --- | --- |
| Idle | Walk | 有移动输入且目标速度 ≤ walk 阈值 | 按输入相对朝向选 `Idle_to_Walk_Fwd*` |
| Idle | Jog | 松开慢走键（或速度意图 ≥ jog 阈值） | 按输入相对朝向选 `Idle_To_Jog_Fwd*` |
| Walk | Jog | 速度意图跨过阈值（带滞回） | `Walk_Fwd` → `Jog_Fwd` |
| Jog | Walk | 速度意图低于阈值（带滞回） | `Jog_Fwd` → `Walk_Fwd` |
| Walk | Idle | 输入归零且速度低于停止阈值 | `Walk_Fwd_to_Idle` |
| Jog | Idle | 输入归零且速度低于停止阈值 | `Jog_Fwd_to_Idle` |
| 任意地面 | Falling | 未接地 | 跳跃/走落入口 |
| Falling | Land | 接地 | `Jump_Idle_Land` |

方向选择按**输入方向相对角色当前朝向的带符号 yaw** 计算；角度分界、滞回宽度、
停止阈值必须来自可配置参数并在测试中固定，不能散落在条件表达式里。
Valley 的 `Idle_to_*_Left/Right_90/180` 是“带转向的起步”而不是独立的原地转身；
没有输入速度时不得触发这些片段。

### 4.3 状态机落点

- R3 起规则表由 `AnimationGraphDefinition` 表达，`AnimationGraphRuntime` 求值；
  `script/VulkanLearn.Runtime/LocomotionStateMachine.cs` 仅保留为旧行为对照/兼容探针。
  样例蓝图只负责输入映射、组件组装与日志。
- 片段名到 clip id 的映射放在独立的 `LocomotionClipSet`，状态机本身只输出
  状态/方向/一次性转移，不依赖具体资源名。
- 动画请求仍只走 `AnimationPlayback.Play(...)` / `PlayMontage(...)`；
  状态机不持有播放时钟、不做姿态求值。
- 同一 Mesh 上不得同时运行离散状态机与 Motion Matching；`SC_gasp` 继续用 MM，
  `SC_valley_echo*` 用离散状态机。二者共享底层 Montage/曲线/根运动能力，但选择器互斥。

## 5. 控制与相机

- 移动：沿用 Jolt `CharacterVirtual`；速度、加速度/减速度、转向速率与输入映射
  对齐 `DefaultInput.ini` 的 `MoveForward` / `MoveRight` / `Turn` / `LookUp` / `Jump`。
- 起步/停止的第一帧就提交动画意图；物理速度与动画时钟解耦，避免“先滑后播”。
- 相机：复用 `SpringArm.cs`，补齐 Valley 的臂长、lag、碰撞回缩、俯仰限制；
  不迁移 camera mode stack。
  验收用固定场景的轨迹截图与数值（臂长、回缩距离、角度变化率），不靠主观描述。

## 6. 阶段与门禁

| 阶段 | 交付 | 门禁 |
| --- | --- | --- |
| A 资产盘点 | UE 只读导出 Echo 骨架/网格/24 clip + 3 Montage；记录引擎版本、输入哈希、root motion 标志 | raw export `status=complete`；clip 数=24 且无重名；FBX/UE 关节与边长交叉校验通过 |
| B 导入预览 | `SK_Echo.json`、`SM_Echo.json`、24 个 `*.anim.json` 与 3 个 Montage；预览场景 | 片段时长/帧率与 UE 一致；骨架 rest 误差在既有阈值内；预览可逐段播放 |
| C 步态状态机 | `Idle/Walk/Jog` + 方向起步/停止；可复用 C# 组件 | 转移表 GTest 全绿；陡峭输入/连续反向/阈值抖动用例无连跳 |
| D 空中与根运动 | Jump/Fall/Land；根运动消费与落地收口 | 跳跃落地场景；30/60 Hz 各一次；无根运动双消费 |
| E 一次性动作 | VaultOver、ReachOut、Sitting/Stand 接入 | Montage 播放/中断/结束；动作锁期间步态状态机不抢占 |
| F 手感与后处理 | SpringArm 对齐、Slope Warping / Twist 评估 | 固定场景截图与数值对照；不达标的项显式保留为差距 |

## 7. 验证

- 纯逻辑：状态机转移、方向分桶、滞回、阈值、动作锁，用 GoogleTest 或现有 managed 测试宿主；
  一个 `TEST` 只验证一个行为。
- 资产：扩展 `tool/animation-tests/`，验证 clip 数、时长、root motion 标志与 skeleton 身份。
- 运行时：新增可玩场景与输入注入脚本；沿用现有 console `editor <JSON>` 与截图入口，
  **不新增 runtime test flag**。
- 文档：改动 `documents/**` 后运行 `py -3 tool/validate-docs.py`。

## 8. 非目标与风险

- Valley 是 Epic 示例内容；来源与使用范围按 GASP P0 的方式记录，不因本机能打开就推定许可。
- Echo 骨架与 UEFN Mannequin 不同；直接导入优先，retarget 是后续可选项，不是第一阶段捷径。
- 已确认导出的 24 段 AnimSequence 都是 `enable_root_motion=false`（原地）；后续替换资产时
  必须重新核对，不能沿用这一结论。
- `Echo_AnimLayerInterface`、`Echo_PostProcess_AnimBP`、`Echo_SlopeWarping_CtrlRig`
  只作为后续阶段的语义参考；VL 当前没有等价的 layered blend / ControlRig 执行路径。
- Slay 的逐镜头动画可能含镜头切换、道具与面部驱动，直接当 locomotion 会产生不可用的根轨迹；
  只做姿势对照。

## 9. 待确认

UE 5.8.4 已安装并完成只读导出。剩余是 3 个 Montage 的 T3D/FBX 导出与状态机接线，
不再有编辑器阻塞。

## 10. 动画蓝图落点

动画逻辑按“动画蓝图”而不是“角色蓝图里的几个字段”组织：

- `script/VulkanLearn.Runtime/AnimationBlueprint.cs`
  - 继承逻辑组件 `Component`，在 `OnCreate` 绑定 owner 的 `MeshComponent.Animation`；
  - 不拥有骨骼、动画时钟或姿态求值，只提供 `OnAnimationCreate/BeginPlay/Tick/LateTick/EndPlay/Reload`；
  - `MeshComponentId` 为空且 owner 只有一个 Mesh 时自动绑定，多 Mesh 必须显式指定。
- `script/VulkanLearn.Runtime/LocomotionAnimationBlueprint.cs`
  - 拥有 `AnimationGraphRuntime` 与图配置；
  - `SetLocomotionInput(in LocomotionInput)` 接收角色最终世界方向、速度、朝向与接地状态；
  - 在组件 `LateTick` 求值。脚本回调顺序是 owner 先、逻辑组件后，动画求值在整批之后，
    所以角色可以在自己的 `LateTick` 提交物理结果，不需要额外一帧延迟；
  - `ApplyDecision` 只按图输出提交 `AnimationPlayback.Play(...)` / `PlayTransition(...)`。
- `Maps/SC_valley_echo/Blueprints/EchoAnimBlueprint.cs`
  - 资源侧只继承 `LocomotionAnimationBlueprint` 并把 Expose 参数转给
    `EchoLocomotionGraph.CreateConfig()`；
  - `Maps/SC_valley_echo/Blueprints/EchoLocomotionGraph.cs` 持有全部 Echo clip/时间/曲线名；
  - 角色蓝图通过场景 `scriptComponents` 引用它，例如
    `{"id":"anim","class":"ValleyEcho.EchoAnimBlueprint","properties":{"MeshComponentId":"visual"}}`。

约束：

- 同一角色只能有一个动画蓝图组件；它只服务一个 MeshComponent。
- 动画蓝图不得直接推进物理、输入或 World；这些仍由角色蓝图拥有。
- 状态机/片段映射仍可单独测试，不要求真实动画资产或 Mesh 加载。

实现状态（2026-11-04）：

- `LocomotionStateMachine.cs`、`LocomotionClipSet.cs`、`AnimationBlueprint.cs`、
  `LocomotionAnimationBlueprint.cs` 已落地；资源侧 `EchoAnimBlueprint.cs` 已建立。
- `ScriptLocomotionTest` 27 项与曲线帧率参数化 3 项 GoogleTest 全通过，覆盖起停、
  方向分桶、滞回、空中/落地、片段映射、参数校验、速度/转向曲线条件、
  30/60/120 Hz 稳定性、动作层进入/完成/取消/base 恢复，以及“动画蓝图组件拥有决策”的集成路径。
- 阶段 A/B 已完成 UE 5.8.4 只读导出与 VL 导入：134 关节、24 段 AnimSequence、
  20 个材质槽；3 个 Montage 已导出并接入（阶段 E）。
- `Maps/SC_valley_echo/SC_valley_echo_preview.json` 真实加载通过：20 个蒙皮 section、
  506,847 顶点、7,928 组；截图见 `artifacts/ue-echo-export/echo-preview.bmp`。
- 24 段均为 `enable_root_motion=false`，适合控制器驱动、动画原地播放。
- 朝向必须按**实际动画姿态**验证，不能从 UE 的 +X 前向直接推导：本项目实测
  视觉 Mesh 需要额外 180° yaw 才与状态机的 `yaw=0 -> -Z` 对齐；+90° 会让角色
  在第三人称相机里侧身，移动方向与朝向不一致。`BP_echo_character.json`、
  `SC_valley_echo_preview.json` 与 `SKM_Echo.json` 描述已按该结论同步。
- UE 动画轨道中的 `prop_01` 不属于网格骨架，sidecar 转换为只保留真实骨骼 track。
- 源 FBX 存在零长度法线；骨骼导入改为用相邻三角面积加权法线修复（无相邻三角仍拒绝）。
- 骨骼网格命名规则已固定：新骨骼网格必须 `SKM_*.json`，`SM_*.json` 只用于非骨骼模型
  （历史 `SM_*` 骨骼资产在触碰时迁移）；当前 Echo 资产为 `SKM_Echo.json` / `SK_Echo.json`。
- `LocomotionClipSet` 的 Walk 正向起步名已修正为资产实际的 `Idle_to_Walk_Fwd`；
  旧拼写 `Idle_To_Walk_Fwd` 会在运行时被动画播放器判为未知 clip。
- 停步响应修正：起步片段实际长 2.07s / 2.8s，旧逻辑会等它播完才回 Idle；现在
  目标步态一变化就打断可中断的起步片段。停步片段本身长 5.67s / 3.47s；默认
  `StopTransitionMaxSeconds=0` 忠实播完整段，由 UE 自动规则在片段结束后回 Idle；
  空中则循环 `Jump_Idle_Fall`，不再在 Fall 播完后错误切回地面循环。
- 阶段 C 完成：`BP_echo_character.json` 提供 Jolt 胶囊、SpringArm 与相机，
  `EchoCharacterBlueprint.cs` 只负责输入/转向/物理，`EchoAnimBlueprint` 通过
  `scriptComponents` 独立驱动动画；`SC_valley_echo_playground.json` 为可操控验证场景。
- 可玩场景真实加载通过：World/Graph generation=2 提交，20 个蒙皮 section、
  506,847 顶点；`script play` 后 Idle 正常播放，`injectinput Jump` 让胶囊离地并
  切到 Fall 姿态。截图见 `artifacts/ue-echo-export/echo-playground-rot180.bmp`
  与 `echo-playground-jump2.bmp`。
- R3 数据驱动图已完成：`AnimationGraphDefinition`/`AnimationGraphRuntime`/
  `AnimationGraphCondition` + `LocomotionAnimationGraph` 模板；`EchoLocomotionGraph.cs`
  独立持有 Echo 片段、方向分支、同步组、曲线名和过渡时间。`LocomotionAnimationBlueprint`
  只剩图宿主与播放边界；`EchoAnimBlueprint` 不再覆写片段表。GoogleTest 覆盖
  schema 拒绝矩阵、假角色换图、Echo 图探针、旧状态机逐帧对照与 30/60/120 Hz；
  真实 `SC_valley_echo_playground.json` 加载并 `script play` 通过。
- R4 动作层已完成：`vl.anim-montage.v1` 增加可选 `segments[]` 与 Montage
  `rateScale`；`ReachOut_Montage_RM` 按 2 段 / `Default→Loop→Loop` / `RateScale=0.5`
  正式导入，`SKM_Echo.json` 登记 3 个 Montage。原生段边界切 clip、section 跨段自循环、
  sidecar notify/warp 段映射与多段根运动已有模块回归；C# 新增
  `LocomotionAnimationBlueprint.PlayAction(AssetId, targets?)`/`CancelAction()`，
  动作期间不提交 base clip，原生完成/取消后回 base。`EchoCharacterBlueprint` 用
  `Input.WasPressed("Interact")` 触发无 warp 的 `AM_ReachOut_End_Montage`；
  `VaultOver` 保留 `PlayAction`/warp target 入口，不在场景里硬编码目标。
  真实验证：`SC_valley_echo_playground.json` 加载 generation=1，`script play` 后
  `injectinput Interact down` 输出 `EchoActionTrigger` 并切到 ReachOut 姿态，
  约 2 s 后 `EchoActionReturn clip=Idle`；截图见
  `Generated/Screenshots/echo-action-live.bmp` 与 `echo-action-return.bmp`。
- UE AnimBP 状态机已从只读 T3D 审计并落到 VL：外层 Idle/Walk/Jog 转移、
  Idle 内层停步选择、Walk/Jog 内层方向起步，以及 0.2/0.3/0.5s 的过渡时间都已
  记录在 `documents/rationale/valley-echo-anim-state-machine-audit.md`。
  `AnimationPlayback.PlayTransition(...)` 和原生 `mode=transitionTo` 使用
  `SkeletalPlayback::TransitionTo` 的交叉淡入/Inertialization。
- `DetermineLocomotionState` 的条件已同步：Jog 用 `Speed>=1 && MaxSpeed>=300 &&
  |InputVector|>=0.5`，Walk 用 `Speed>=1 && |InputVector|>=0.01`，倒走点积
  `< -0.5` 保持 Idle；Idle 内层按 `Speed>200` 播 Jog Stop、`Speed>1` 播 Walk Stop。
  角色蓝图加入地面加速/刹车，避免瞬时 `Move(0)` 跳过停步阈值。
- 同步组与曲线正式接线：`extract_echo_sync.py` 从 T3D 抽取 23 个 Sequence Player
  绑定（`SyncGroup/Role/Method/SyncGroupScope/StartPosition/Loop`）；24 个 `.anim.json`
  写入 `players` 并显式声明 `syncGroupScope=Local`，`syncMarkers` 改为结构化校验并由
  catalog 发布。R1 已落地 `AnimationSyncGroupRegistry`：每 tick 的 leader election、
  `SyncGroup`/`MarkerSync` follower 持续校正、`TransitionLeader/Follower` 语义与
  Local/Component scope；`TransitionToSynced` 只负责入口第一帧对齐。反馈新增
  `syncActive/syncLeader/syncRole/syncMethod/syncEffectiveMethod/syncScope/syncDiagnostic`；
  C# 暴露 `Players`、`StartPosition`、`HasSyncGroup`、`SyncPhase`、`CurrentCurve`。
  R2 已补齐只读曲线运行时：原生 `SkeletalPlayback::SampleCurve`、C# 曲线名/存在性/
  显式快照，以及 `MoveData_Speed`/`MoveData_WalkRotationDelta`/
  `MoveData_JogRotationDelta` 对 Echo locomotion 的显式条件输入。Echo 在曲线缺失时
  每类只诊断一次并回退旧输入路径；仍不自动执行 AnimBP 曲线图。

运行方式：

```powershell
build\bin\main.exe --initial-scene Maps/SC_valley_echo/SC_valley_echo_playground.json
# 控制台输入 script play；WASD 移动、Shift 跑步、Space 跳跃、鼠标转视角
```

当前过渡时间取自 `Echo_AnimBP`（默认 0.2 / 0.3 / 0.5s），不是 VL 自拟参数；
如果要调整，改 `LocomotionAnimationBlueprint` 的 `*BlendSeconds` 属性即可。
