# 原生 MotionWarping

> 状态：当前契约。描述 GASP traversal 使用的原生 `MotionWarpRuntime`：窗口所有权、
> 窗口门控、世界 target 到模型空间的换算、UE `WarpPointAnimProvider=Bone/Static` 的
> warp point 偏移、单 tick 根位移与 **yaw-only 旋转 warp**，以及 Montage 资产 →
> `PlayMontage` → 四个 traversal 动作的消费路径。只覆盖 UE `SkewWarp` 的平移与默认
> `Slerp` 旋转；非 `Slerp` 方法、旋转的 pitch/roll、Montage 混合与 additive/sync marker
> 不在本契约内。曲线采样、section 推进与 notify 执行见
> [动画 Montage：数据与播放](animation-montage.md)。

## 所有权

`MotionWarpRuntime` 拥有窗口队列、窗口门控、target 的模型空间换算、Bone/Static provider
的 warp point 偏移、本 tick 最终根位移与根 yaw 增量的修改权。每个 `SkeletalPlayback`
实例持有一份运行时；C# 的 `AnimationPlayback.PlayMontage(montage, targets)` 只提交
Montage id、target 位置/可选世界旋转与 loop/rate/inPlace 播放参数，窗口时间/provider/
骨名/warp 布尔/旋转方法全部来自 Montage 资产；C# 不持有窗口的激活/顺序/进度状态，
也不做窗口提取、顺序应用、target 换算、骨偏移、平移或旋转 warp 数学。

retarget 目标播放器传入 `AnimationAsset()` 的 source skeleton 和 source clip：
Bone provider 的骨名、末帧偏移及根轨道都保持 source 语义，目标骨长不参与 warp point
重算。因此普通 Montage 能播放不代表不同身材角色的手脚接触位置已通过校正。

| 职责 | 所有者 | 可核对位置 |
| --- | --- | --- |
| 按资产顺序装配窗口；不排序、不去重 | `BuildMontageWarpWindows` | `source/animation/animationScriptData.cpp` |
| 按提交顺序保存窗口；运行时不重排 | `MotionWarpRuntime::SetWindows` | `source/animation/motionWarping.cpp:210-218` |
| 每个播放实例一份窗口队列 | `SkeletalPlayback` | `source/animation/skeletalPlayback.h` |
| `PreviousPosition ∈ [Start,End)` 才激活 | `MotionWarpRuntime::Apply` | `source/animation/motionWarping.cpp:253-259` |
| 世界 target → Mesh/骨架模型空间 | `MotionWarpRuntime::Apply` | `source/animation/motionWarping.cpp:242-252,289-290` |
| Bone provider 的 EndTime 骨骼偏移 | `MotionWarpRuntime::Apply` / `SampleBoneModelTransform` | `source/animation/motionWarping.cpp:294-304,190-198` |
| Static provider 的 `Root(EndTime)*staticPose^-1` 平移偏移 | `MotionWarpRuntime::Apply` | `source/animation/motionWarping.cpp:312-326` |
| 本 tick 平移结果 | `MotionWarpRuntime::Apply` → `rootMotionDelta` | `source/animation/skeletalPlayback.cpp:560-576` |
| 本 tick 旋转结果（yaw 增量） | `WarpedRootYawDelta` → `Apply(outYawDelta)` → `RootMotionYawDelta` | `source/animation/motionWarping.cpp:104-144,261-271`；`skeletalPlayback.cpp` |
| 提交面：Montage id + target 位置 + 可选目标旋转 | C# `AnimationPlayback.PlayMontage` | `script/VulkanLearn.Runtime/AnimationPlayback.cs` |

窗口时间与片段本地时间同单位（秒）；门控比较的是播放器的 `previousPosition`，不是
Actor 位置、渲染帧号或 tick 计数（`motionWarping.cpp:158-159`）。`DebugState` 用同一个
`previousPosition` 给出 `waiting/active/done`（`motionWarping.cpp:124-136`）。
`SkeletalPlayback::SetMotionWarpWindows` 整体替换当前实例的队列，`ClearMotionWarp`
清空队列；两者都同时清空 `Warp` 读数（`skeletalPlayback.cpp:706-718`）。
`rootMotion`/`rootMotionTotal`/`rootMotionRemaining` 作为只读反馈发布
（`worldAnimation.cpp` 的 `ScriptAnimationFeedback`），原生 warp 直接采样片段根轨道，
不依赖这些 C# 反馈值。

## target 与骨偏移

target 是 VL 世界空间位置，以**胶囊中心**表达，不是 Mesh 原点、脚底或碰撞点
（`motionWarping.h:23-26`）。原生换算先求
`capsuleCenterInModel = R^T(actorPosition - meshOrigin)`，再求
`targetModel = R^T(targetWorld - meshOrigin) - capsuleCenterInModel`
（`motionWarping.cpp:145-153,180-181`）。`R` 是可视 Mesh 世界矩阵的旋转部分，逐列归一化，
不消费缩放（`motionWarping.cpp:35-43`）；`actorPosition` 是胶囊中心世界位置
（`motionWarping.h:60-66`；character 根位置即胶囊中心，见
`documents/contracts/csharp-blueprint-runtime.md:496`）。

窗口可带 `warpPointBone`，对应 UE `WarpPointAnimProvider=Bone`；空串等同 UE `None`
（`motionWarping.h:52-64`）。非空时，原生在窗口 `EndTime` 分别采样根关节与目标骨
（`motionWarping.cpp:294-304`），求
`offset = RootModel * WarpPointModel^-1`，只把 `offset` 的平移列加到 `targetModel`
（`motionWarping.cpp:301-302`）。提交方仍按胶囊中心表达 `targetLocation`；骨偏移由原生
扣除，使该骨而不是根去对齐几何目标。骨骼采样复用绑定/轨道局部 TRS 的层级相乘，没有
轨道的关节回退 `restLocal`，父链必须拓扑序（`motionWarping.cpp:146-198`）；根关节按索引 0
采样（`motionWarping.cpp:298`）。`warpPointBone` 找不到、父链无效或骨名为空时不报错，
也不加偏移，保留无 Bone 的根目标（`motionWarping.cpp:190-198`；单测
`UnknownWarpPointBoneKeepsRootTarget`，`motionWarpingTests.cpp`）。

`provider=Static` 时，使用 Montage 资产里的 `staticPose`（mesh/组件空间、厘米）：
`offset = Root(EndTime) * staticPose^-1`，同样只把平移加到 `targetModel`
（`motionWarping.cpp:305-326`；UE `FTransform::GetRelativeTransform` 约定为
`this*Other^-1`）。`staticPose` 不进 catalog/C#；缺 `staticPose`、location 非有限或旋转
退化在装配期拒绝。`offset` 的旋转不参与旋转 warp。

`subtractRemainingRootMotion` 为真时，target 再减去 `EndTime` 之后到片段末尾的根位移
（`motionWarping.cpp:327-329`），使窗口在片段末尾到达 target，而不是在 notify 结束时到达。
`ignoreVertical` 把 target 的 Y 置零，垂直 delta 原样保留（`motionWarping.cpp:330`）；
它不影响旋转通道（旋转只消费 yaw）。这些标志、`warpPointBone`、`staticPose`、
`warpRotation`/`rotationTimeMultiplier`/`rotationMethod` 都来自 Montage 的
`notifies[].warp` 资产字段，由 `BuildMontageWarpWindows` 逐窗口复制到
`MotionWarpWindow`；C# 只提交 target 位置/可选旋转。

窗口时间必须是片段本地秒：导入层已保证 warp 窗口 `time + duration <= length + 1e-3`
且 `provider`/`easing` 已知；播放层再拒绝未知 provider、非 `Linear` easing、
`warpRotation=true` 时的非 `Slerp` 方法以及不可用的 `staticPose`
（`animationScriptData.cpp` 的 `ValidateMontageExecution`）。每个 Montage 最多 8 个
warp 窗口（`ParseMontage`），窗口名非空且不超过 64 字符。

## 旋转 warp（yaw-only Slerp）

`MotionWarpWindow` 的旋转字段：`warpRotation`、`rotationTimeMultiplier`、
`rotationMethod`（当前只支持 `"Slerp"`）、`hasTargetRotation`/`targetRotation`。
目标旋转由 C# `WarpTarget.Rotation` 提交（世界空间）；旧 3 分量调用没有朝向，
`hasTargetRotation=false`，即使 `warpRotation=true` 也保持原始根 yaw。

UE 公式（`URootMotionModifier_Warp::WarpRotation`，`RootMotionModifier.cpp:550-622`）的
yaw-only 等价：

```text
totalYaw     = Yaw(Root(EndTime)) - Yaw(Root(PreviousPosition))
remainingYaw = subtractRemaining ? Yaw(Root(duration)) - Yaw(Root(EndTime)) : 0
targetYaw    = WrapYaw(Yaw(targetRotation) - actorYaw - remainingYaw)
timeRemaining= (EndTime - PreviousPosition) * rotationTimeMultiplier
alpha        = clamp((CurrentPosition - PreviousPosition) / timeRemaining, 0, 1)
thisFrame    = totalYaw + alpha * WrapYaw(targetYaw - totalYaw)     // Slerp 最短路径
out          = WrapYaw(thisFrame - totalYaw + rawYaw + extraYaw)
```

- 全部旋转投影到 VL Y-up 的 yaw 通道；`RootMotionYawDelta` 是唯一输出，C# 把它加到
  Actor yaw（`GaspCharacter` 的 `TickTraversal`）。pitch/roll 不执行。
- `alpha` 用状态间位移/剩余窗口时间，等价 UE 的 `DeltaSeconds * PlayRate`；
  `rotationTimeMultiplier` 缩放剩余时间。
- 旋转与平移是 UE modifier 的两个独立分支：窗口可以只旋转不平移，平移的
  `NearlyZero(total/delta)` early-out 不会跳过旋转；同理旋转失败也不改平移。
- 同一 tick 有多个激活窗口时，每个窗口都从本 tick 原始 `modelDelta` 重算，**最后一个
  实际执行旋转的窗口**覆盖 `outYawDelta`（与平移的“最后窗口生效”一致）。
- `subtractRemainingRootMotion` 会从目标 yaw 中扣除窗口结束后的剩余根旋转；
  `ignoreVertical` 不影响 yaw。
- 没有旋转窗口执行时，`Apply` 不写 `outYawDelta`，调用方保留原始根 yaw。
- `rotationMethod` 非 `Slerp` 在装配期拒绝（C#/原生）；直接构造的窗口在 `Apply` 中保守
  跳过旋转，保留原始 yaw。

资源接线（资源 `b627bd0`）：GASP 的 `FrontLedge` 用 UE `MakeRotFromX(-FrontLedgeNormal)`
语义提交目标朝向（世界 yaw = `atan2(-forward.X, -forward.Z)`），`BackLedge`/`BackFloor`
的资产窗口 `warpRotation=false`，只提交位置。直线助跑（前向 -Z）时目标 yaw = 角色 yaw，
旋转 warp 是 no-op；把目标 yaw 人为 +20° 的诊断得到 END yaw 20.0、x=-14/-9，证明原生
消费了目标旋转。4 个场景 ×2 验收全部 grounded、无 ABORT，END yaw 0.0、x=0，
窗口/触发/notify 与清理后基线逐字相同（见
[P23 记录](../rationale/gasp-p23-warp-execution-record.md)）。

## 单 tick 行为

`SkeletalPlayback::Tick` 先按片段根轨道求本 tick 的原始位移/根 yaw
（`RootTranslation`/`RootRotation`）；随后若窗口队列非空，就把这份模型空间位移、可视
Mesh 矩阵、胶囊中心、骨架与 `actorYaw` 交给 `MotionWarpRuntime::Apply`，用返回值
替换 `rootMotionDelta`、用 `outYawDelta` 替换 `rootMotionYawDelta`
（`skeletalPlayback.cpp:560-576`）。返回的是本 tick 的完整模型空间根位移，不是增量修正；
四个 traversal 动作经 C# 转世界后交给 Jolt，C# 通过 `rootMotion`/`rootMotionYaw`
反馈读取同一结果（`worldAnimation.cpp` 的 `ScriptAnimationFeedback`）。队列为空时保留
原始根位移/根 yaw，并清空 `Warp` 读数（`skeletalPlayback.cpp:566-576`）。`Apply` 需要
可视 Mesh 世界矩阵和胶囊中心；`Tick` 允许两者缺省，缺省时不调用 `Apply`，该 tick 保留
原始根位移与根 yaw（`skeletalPlayback.cpp:564-572`）。

`Apply` 的求值顺序是：

1. 无窗口或 `currentPosition <= previousPosition` 时原样返回 `modelDelta`
   （`motionWarping.cpp:239`）。
2. 只处理 `previousPosition` 落在 `[startTime, endTime)` 的窗口
   （`motionWarping.cpp:255-259`）。
3. **旋转分支先于平移 early-out**：`outYawDelta` 非空、`warpRotation=true`、
   `hasTargetRotation=true` 且 `rotationMethod=="Slerp"` 时，写入本窗口的根 yaw 增量
   （`motionWarping.cpp:261-271`）。
4. `total = Root(endTime) - Root(previousPosition)`，不是整段片段位移
   （`motionWarping.cpp:273-274`）；单测 `WindowTotalComesFromPreviousPositionToEndTime`
   固定该口径（`tool/animation-tests/motionWarpingTests.cpp`）。
5. `delta = Root(min(current, endTime)) - Root(previousPosition)`
   （`motionWarping.cpp:279-282`）；`current > endTime` 时另取 EndTime 之后的
   `extra`（`:284-287`）。
6. 世界 target 换到模型空间（`:289-290`）；Bone 加 EndTime 的
   `Root * WarpPoint^-1` 平移（`:294-304`），Static 加 `Root * staticPose^-1` 平移
   （`:312-326`）；再处理 `subtractRemaining` 与 `ignoreVertical`（`:327-330`）。
7. 在模型空间调用 `WarpTranslationToTarget(0, Identity, delta, total, targetModel)`，
   再加 `extra`（`:334-335`）。

`total` 或 `delta` 的绝对值不超过 `2e-4` 时跳过该窗口的**平移分支**（旋转已在第 3 步
处理，不受此 early-out 影响；`motionWarping.cpp:277,282`）。
同一 tick 有多个窗口激活时，代码按提交顺序逐个计算并覆盖 `finalTranslation`；
每个窗口都从本 tick 原始 `modelDelta` 重算，最终生效的是队列里最后一个实际产生平移的
激活窗口，不是多次 warp 的复合（`motionWarping.cpp:252-337`；单测
`MultipleActiveWindowsUseLastQueuedTarget`、`MultipleActiveWindowsUseLastRotationTarget`）。

`WarpTranslationToTarget` 是 UE `RootMotionModifier_SkewWarp` 的平移路径：在根同步空间
（X 沿窗口总位移方向，Z 由 `MakeFromXZ` 从当前旋转轴中选择）内做
`scale * shearXY * shearXZ`，shear 写矩阵第 0 列（`motionWarping.cpp:340-411`），再变换
回原坐标系。它只做平移；旋转由独立的 `WarpedRootYawDelta` 写 yaw 通道。根 yaw 默认由
播放器从原始根旋转发布；只要有窗口执行旋转 warp，`outYawDelta` 就替换为本 tick 的
warp 结果（`skeletalPlayback.cpp` 的 `Tick`/`TickMontageSections`）。

多 section Montage 的 `TickMontageSections` 会先累加各 sub-step 的根平移/yaw，再在 tick
末尾调用一次 `Apply`（`previousTime → 最终 position`）。因此“同一 tick 内多次段跳变 +
活动 warp 窗口”时，warp 看到的是整 tick 的位移/时间，不是逐 sub-step 求值；当前没有
资产同时触发这三个条件。

## 消费路径

四个动作都走 `AnimationPlayback.PlayMontage` → 原生 `PlayMontage`；窗口集合、时间、provider、
骨名、warp 布尔与旋转方法全部来自 Montage 资产（`Maps/SC_gasp/Animations/AM_*.json`），
C# 按 `Montage(id).Windows` 的 distinct target 名构造世界位置；`FrontLedge` 额外提交
目标朝向（`WarpTarget.Rotation`），其余 target 只提交位置：

| 动作 | 窗口（资产） | 提交 target | 位移/旋转来源 |
| --- | --- | --- | --- |
| Climb | stand/run 1 个、walk 2 个 `FrontLedge`（`attach`） | 前沿顶边 + 90（胶囊中心）+ FrontLedge 朝向 | `RootMotion/RootMotionYaw → 世界化 → 1500 cm/s → ConsumeRootMotion`；yaw 单独加到 Actor |
| Hurdle | 4 个：2×`FrontLedge`（`attach`）、`BackLedge`、`BackFloor` | 两个 FrontLedge 用前沿顶边 + 90 + 朝向；BackLedge 后沿顶边 + 90；BackFloor 后沿 XY + 法线 × `dOut`、地面 + 90 | 同上 |
| Mantle | 2 个 `FrontLedge`（`attach`） | 前沿顶边 + 90（胶囊中心）+ 朝向 | 同上 |
| Vault | 2 个 `FrontLedge`（`attach`）；stand 另有 1 个 `BackLedge` | FrontLedge 前沿顶边 + 90 + 朝向；BackLedge 后沿顶边 + 90 | 同上 |

资产窗口（`AM_*.json`，秒；`parse` 值，运行时日志四舍五入到 3 位）：

```text
Hurdle FrontLedge 0.078149–0.234903  attach
Hurdle FrontLedge 0.234925–0.398904  attach
Hurdle BackLedge  0.464920–0.598016  None
Hurdle BackFloor  0.599556–0.831335  None
Mantle stand 0.140674–0.364971, 0.364973–0.699243  attach
Mantle walk  0.392615–0.810631, 0.810655–0.933427  attach
Mantle run   0.333432–0.567740, 0.567743–0.666616  attach
Vault stand  0.082488–0.270581, 0.271043–0.466705  attach
Vault stand  BackLedge 0.471075–0.603360  None
Vault walk   0.517863–0.899950, 0.899954–1.066703  attach
Vault run    0.618665–0.835370, 0.836009–0.900047  attach
Climb stand  0.405359–0.732000  attach
Climb walk   0.983966–1.293677, 1.293831–1.603542  attach
Climb run    0.518358–0.884773  attach
```

Hurdle 四窗口全部 `subtractRemaining=false`、`ignoreVertical=false`；`dOut` 由
`BuildTraversalTargets` 用 `CurveValue(clip, "distance_from_ledge", window.End)` 对
BackLedge/BackFloor 两个窗口 End 采样得到（当前值 `|34.098 - 90.909| ≈ 56.810` cm，
日志 `P8_WARP_DOUT`），不再内联 UE 常数；缺曲线/缺窗口会显式
`ABORT missing-curve|missing-window`。`BackLedgeNormal` 指向落地侧，BackFloor XY 沿该
法线外移 `dOut`（资源 `GaspCharacter.blueprint.cs` 的 `BuildTraversalTargets`）。
Hurdle 只接 `stand_F_V2_Lfoot` 这一套窗口；其他 Hurdle montage 的窗口集合不同。

Catch（Hurdle/Cliff/Mantle/Vault 家族）由 C# 的 `MovementMode=Falling` 触发；
`FrontLedge` 与 Hurdle 同形（`top.Y + 90` + 目标朝向）。只有 Hurdle Catch 的资产带
`BackFloor` 窗口，Cliff/Mantle/Vault 只有一个 `FrontLedge` 窗口。Catch Hurdle 没有
BackLedge 窗口，`BackFloor` 目标按**顶面落点**解释：

```text
BackFloor = (front + direction × curve(distance_from_ledge, window.End), top.Y + 90)
dOut      = 0
```

Hurdle_high 的 `curve(End)=56.756` → `BackFloor=(0,290,-177)`。UE
`AC_TraversalLogic.SetWarpTargets` 搜 `Catch` 0 命中，`BackFloor` 的 gate 是 Hurdle only，
没有 Catch 专用 target 分支；这里的顶面口径是**有意偏离**地面 Hurdle 的“后沿外地面”，
依据写在 C# 注释与 `P8_CATCH_BACKFLOOR basis=...`，不宣称与 UE 等价。

`FrontLedge` 的目标朝向来自 `FrontLedgeRotation()`：优先 `-FrontLedgeNormal`，未探测到
法线时退回 `traversalDirection`；两者都换算成 VL Y-up 的世界 yaw
（`atan2(-forward.X, -forward.Z)`），写入 `WarpTarget.Rotation`，并记录
`P8_WARP_ROTATION target=FrontLedge source=... yaw=...`。不能读 `Transform.Rotation`：
`PlayMontage` 先提交，Actor 朝向在 `StartTraversal` 末尾才写入。`BackLedge`/`BackFloor`
资产窗口 `warpRotation=false`，只提交位置。

`TickTraversal` 在 `dt > 0.0001`、未收口且未 `Completed` 时读取
`animation.State.RootMotion`，按 Actor yaw 转世界、乘 `Transform.Scale.Y`，水平速度
限幅 1500 cm/s，再调用 `Character.ConsumeRootMotion`；`State.RootMotionYaw` 在位移之后
单独加到 Actor yaw（资源 `GaspCharacter.blueprint.cs` 的 `TickTraversal`）。
`Character.Jump()` 与 `Move(traversalDirection * forward)` 已删除，四个动作不再有第二套
位移来源。Climb+Walk 的竖直闭环从**首个真实 warp 窗口 Start** 起接管（窗口列表为空时
退化为从 0 开始）；窗口之前只走根运动，见「Climb walk：真窗口与 gap」。

Mantle/Vault 在最后一个 warp 窗口结束后，用 `traversalLanded` 标记落地收口：
`traversalTime >= traversalLastWindowEnd`、`progress >= traversalTargetDistance` 且到达
出口高度（这里的 `target.Y` 是 `traversalTarget`：Mantle 顶面 + 90、Vault 后沿外地面
+ 90，不是窗口的 FrontLedge target；Mantle 要求 `Y >= target.Y - 10`，Vault 要求
`Y <= target.Y + 10`），不依赖单帧 `IsGrounded`。收口或 `Completed` 后进入 `else`
分支 `ClearRootMotion()`，正常结束/中断也清根，把速度交回重力/地面，避免持续
`ConsumeRootMotion(0)` 把每帧重力压成匀速下坠（资源 `GaspCharacter.blueprint.cs` 的
`TickTraversal`/`AbortTraversal`）。这是未实现的 montage blend-out 的替代路径。

原生物理在 `rootMotionActive` 时用该速度替代 walk、jump 与地面速度
（`source/script/scriptPhysics.cpp:429-437`）。中断仍保留：
`abort_void` 在无 BackLedge 的场景继续下坠到 `y≈-2435` 才触发 `ABORT airborne`
（P22 清理后矩阵；P21 为 -2792）；
`abort_stall` 在最后窗口结束 0.2 s 后仍无进展时记录 `ABORT stalled progress=10`
（`artifacts/gasp-post-cleanup-matrix/`）。

## 场景触发距离（探针口径）

自动探针按 UE `GetTraversalCheckInputs` 的 reach 口径放置：
`reach = clamp(75 + speed/500*275, 75, 350)`。探针的 `speed` 取**名义 gait**：
Run 500、Walk 500（walk 的 500 只是夹具口径，为 Climb walk 的 0.984 s 首窗留出接近段）、
Stand 0；真实输入路径仍用 `Character.LinearVelocity`（资源
`Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs` 的 `RequestTraversal`，资源仓库不在
本仓库 glob 范围）。因此 walk/run 的 reach 都是 350，stand 是 75。

墙面前沿 -140、球扫半径 20、长度 reach，可触发上界为 `-140 + reach + 20 - 2`，
即 stand -47 / walk +228 / run +228。17 个 traversal 场景当前放置：Mantle stand
（`_root`）/ auto_high / 三个 reject / input 为 -52；Vault stand / auto / abort_void
为 -48；Climb stand -86；Mantle walk / Vault walk 为 +62、Climb walk 为 +228；
Mantle run / Vault run 为 +227；Climb run 为 +226；`abort_stall` 保留 -80（唯一例外）。

首窗非奇异判据是 `startZ >= S_min`（`S_min = attractor_z + A`）。Climb walk 的真首窗在
0.983966 s（旧 C# 把 Duration 当 Start）下 `S_min=+94.557`，因此场景放到 +228；其余片段
的首窗判据与放置依据见
[P21 场景 reach 矩阵记录](../rationale/gasp-p21-reach-matrix-record.md)。Vault run 的
`S_min=+239`、场景起点 227（触发上界 +228），距 S_min 差 12 cm，首窗仍略奇异，运行时由
Jolt 墙面限制与收口兜住，矩阵仍 grounded/无 ABORT，但不宣称与 UE 首窗对齐。
`abort_stall` 为保留 `progress < 20` 的 `stalled` 语义留在 -80：移到 reach 上界后实测
`progress=42`，中断不再触发。

## Climb walk：真窗口与 gap

Climb walk 的 MotionWarping 窗口在 T3D/`AM_*.json` 中是 `0.983966–1.293677` 与
`1.293831–1.603542`，10 个 Montage、21 个窗口与 T3D 0 mismatch。P17 记录的
`0.309711` 是 notify 的 **Duration**，不是 Start；旧 C# `[0.309711, 1.293677]` 是转录
错误，`AM_*.json` 没有数据错误，本轮没有修改 Montage 资产。

窗口打开时的几何 gap 决定 SkewWarp 的 projected scale 符号：UE 5.8 的
`CachedOffsetFromWarpPoint` 在 EndTime 采样（与 VL 一致）、`bWarpToFeetLocation=true`、
目标空间一致；同一 fixture 下 UE 参考积分也会发散（projected scale 为负）。零点约为
UE 82 cm / VL 90 cm，而原夹具只有 16 cm。直接按 walk 原 reach 185 放不进去：
`gap = z0 + 120 - A_eff`，最大 `350 - A_eff`；实测 `A_eff` 在 198–246 cm 抖动，
需要 reach 350（名义 speed 500）才能覆盖 95 cm 的 gap。修复因此是三件事一起：
探针 walk 名义 500（reach 350）、`climb_walk` 场景 `z=+228`、以及把 Climb+Walk 的
竖直闭环从旧错误的 montage t=0 推迟到**首个真实窗口 Start**（窗口前只走根运动，
否则胶囊先被抬高约 178 cm，attach 目标竖直分量变负、SkewWarp 反向）。
验收结果 `END (0,340,-167) / (0,340,-165) / (0,340,-167)`，grounded、无 ABORT。

## 当前限制

- **第二窗 UE 自由空间参考发散**：首窗 attractor 可判定，但 Mantle walk / Vault stand /
  Vault walk / Vault run 的第二窗参考积分误差 318/1223/550/3678 cm，属碰撞依赖，不是场景
  位置能修的；Mantle run 的第二窗数值收敛但 final 被墙面 clamp，也不作落点基准。
- **旋转只做 yaw-only Slerp**：目标旋转与骨骼 offset 的完整四元数投影到 VL 的 Y-up yaw；
  pitch/roll、非 `Slerp` 方法（`SlerpWithClampedRate`/`ConstantRate`/`Scale`）以及
  `RotationType`/`AdditionalOffset`/`MaxRotationRate` 尚未进入 VL 数据，执行层显式拒绝
  或保留不执行。
- **`FrontLedgeLocation` 仍是球扫球心**，比真实前沿面短约 20 cm；Hurdle 最终落点由
  BackLedge 重新锚定，主要影响两个 FrontLedge 窗口的中间姿态。
- **没有运行时 30/60/120 Hz 矩阵或包内 warp 验证**；只有
  `MotionWarping.FixedStepConvergesToTargetAt30_60_120Hz` 的合成片段覆盖
  （`motionWarpingTests.cpp:189-215`），不覆盖 GASP 关卡、Jolt 碰撞或包内运行。
- **Montage blend-out 未实现**：`traversalLanded` + `ClearRootMotion` 是替代品；片段
  尾段仍带 locomotion 位移。
- **`abort_void` 的下坠语义**：无 BackLedge 时角色会坠到 `y≈-2435` 才判 airborne，
  不是新的落地路径。
- **未知 `warpPointBone` 静默回退**：骨名不在骨架时不加偏移，也不报错；提交方只能从
  `Warp` 反馈和落点判断。
- **落点抖动**：Climb walk/run、Hurdle 的 1–2 次离群已在 P22 复跑；窗口/notify/HIT 逐字
  相同，差异只在最终落地帧。矩阵数字与逐场景复跑见 P22 记录，不在本契约固化。
- **Catch 的 BackFloor 是有意偏离**：Catch 没有 UE 专用 target 分支，Hurdle Catch 把
  BackFloor 解释为顶面落点（`topY + 90`、`dOut=0`），不套用地面 Hurdle 的“身后地面”
  口径；依据写在 C# 注释与 `P8_CATCH_BACKFLOOR basis=...`，不宣称与 UE 等价。

## 验收入口

模块回归为 `MotionWarping.*`，共 28 项：5 项平移数学、4 项窗口/运行时行为
（门控、窗口 total、subtractRemaining、互斥窗口各自收敛）、1 项 DebugState、
1 项多窗口末项生效、1 项 30/60/120 Hz 固定步收敛、4 项 Bone provider、
12 项旋转/Static（90°/180° 收敛、时间倍率、窗口门控、多窗口末项、平移/旋转独立、
缺目标旋转保持原 yaw、subtractRemaining 旋转、ignoreVertical 不影响 yaw、
非 Slerp 跳过、Static 与等效 Bone 一致、Static offset 平移）
（`tool/animation-tests/motionWarpingTests.cpp`）：

```powershell
.\build\bin\animation_preflight_tests --gtest_filter=MotionWarping.*
```

全量 `animation_preflight_tests` 为 255/255（P25 版；P23 为 252/252、P22 为 224/224、
P21 为 198/198、P20 为 145/145、P19 为 140/140）。相关套件：`SkeletalImport` 30、
`AnimationSkeleton` 17、`AnimationSequenceData` 14、`AnimationMontage` 24、
`AnimationScriptData` 30、`NotifyPlayback` 11。运行期证据是 traversal 日志中的 `P8_WARP_WINDOWS`、
`P8_WARP_ROTATION`、`P8_MONTAGE_NOTIFY`、`Warp` 反馈与 `P8_TRAVERSAL_END`；
落点/旋转数值按阶段记录在
[P19](../rationale/gasp-p19-native-motion-warping-runtime.md)、
[P20](../rationale/gasp-p20-warp-ue-alignment-record.md)、
[P21](../rationale/gasp-p21-reach-matrix-record.md)、
[P22](../rationale/gasp-p22-montage-migration-record.md) 与
[P23](../rationale/gasp-p23-warp-execution-record.md) 记录，不在本契约固化。

P22 清理后矩阵（`artifacts/gasp-post-cleanup-matrix/RESULTS.md`）：10 个 traversal 场景
×2 全部 grounded、无 ABORT；20/20 次 `P8_WARP_WINDOWS` 与全部 `P8_MONTAGE_NOTIFY` 与
迁移后基线逐字相同。END：Mantle stand `-160/-159`、walk `-202/-202`、run `-204/-202`；
Vault stand `-365/-376`、walk `-335/-340`、run `-346/-338`；Climb stand `-164/-162`、
walk `-168/-155`（额外复跑 `-167/-168`）、run `-182/-175`（额外复跑 `-163`）；
Hurdle(auto) `-325/-342`（额外 `-326`）。`reject_high` 首次 `exit=2` 是关闭路径交换链
`ErrorOutOfDeviceMemory`，复跑 2 次 `exit=0`，拒绝语义不受影响。

P23 旋转接线验收（`artifacts/gasp-rotation-wiring/RESULTS.md`）：Mantle stand、Climb
stand、Climb walk、Hurdle(Auto) 各 2 次，8/8 grounded、无 ABORT/REJECT；END yaw 全为
0.0、x=0；`P8_WARP_ROTATION` 每窗口一行且 `yaw=0.0`；窗口/触发/notify 与 P22 基线逐字
相同。人为把目标 yaw 加 20° 的诊断得到 END yaw 20.0、x=-14/-9，证明旋转通道被原生消费
（诊断不属于验收）。

## 可核对点

| 结论 | 位置 |
| --- | --- |
| 窗口由 Montage 资产按 notify 顺序装配，无排序/去重 | `animationScriptData.cpp` 的 `BuildMontageWarpWindows` |
| 窗口队列按提交顺序保存，C# 不维护窗口运行状态 | `motionWarping.cpp:210-218`；`AnimationPlayback.cs` 的 `PlayMontage` |
| 窗口只在 `PreviousPosition ∈ [Start,End)` 激活 | `motionWarping.cpp:255-259`；`motionWarpingTests.cpp` |
| target 是世界空间胶囊中心，换算时减去胶囊中心偏移 | `motionWarping.h`；`motionWarping.cpp:242-252,289-290` |
| Bone provider 在 EndTime 加 `Root * WarpPoint^-1` 平移 | `motionWarping.cpp:294-304`；`motionWarpingTests.cpp` |
| Static provider 在 EndTime 加 `Root * staticPose^-1` 平移 | `motionWarping.cpp:305-326`；`motionWarpingTests.cpp` |
| 未知骨静默回退到无偏移根目标 | `motionWarping.cpp:190-198`；`motionWarpingTests.cpp` |
| 骨偏移的根收敛点为 `target + (root - warpPoint)` | `motionWarpingTests.cpp` |
| 窗口 total 取 previous 到 EndTime，不是整段片段 | `motionWarping.cpp:273-274`；`motionWarpingTests.cpp` |
| EndTime 之后的根位移作为 extra 加回 | `motionWarping.cpp:284-287,334-335` |
| `subtractRemainingRootMotion` 从 target 扣除片段末尾剩余根位移 | `motionWarping.cpp:327-329`；`motionWarpingTests.cpp` |
| 多窗口同 tick 激活时最后一个实际平移/旋转的窗口覆盖结果 | `motionWarping.cpp:252-337`；`motionWarpingTests.cpp` |
| 本 tick `rootMotionDelta` 被 warp 返回值替换 | `skeletalPlayback.cpp` 的 `Tick`/`TickMontageSections` |
| 旋转只走 yaw 通道，target 世界朝向减 Actor yaw | `motionWarping.cpp:104-144`；`motionWarpingTests.cpp` |
| 旋转与平移独立，平移 early-out 不跳过旋转 | `motionWarping.cpp:261-282`；`motionWarpingTests.cpp` |
| 非 Slerp/缺目标旋转保持原始 yaw | `motionWarping.cpp:261-271`；`animationScriptData.cpp` |
| C# 只提交 Montage id + target 位置/可选旋转，窗口/布尔/骨名来自资产 | `AnimationPlayback.cs` 的 `PlayMontage`/`WarpTarget`；`animationScriptData.cpp` |
| 旧 `clip` 请求携带 `warpTargets` 显式拒绝 | `worldAnimation.cpp` 的 `Script.WarpTargetsDefinedByMontage` |
| Hurdle 四窗口与两个 `attach` | 资源 `AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot.json` |
| Hurdle `dOut≈56.810` 由 `CurveValue` 在两个窗口 End 采样 | 资源 `GaspCharacter.blueprint.cs` 的 `BuildTraversalTargets`；`P8_WARP_DOUT` |
| Mantle/Vault/Climb 窗口（Vault stand 含 BackLedge） | 资源 `Maps/SC_gasp/Animations/AM_*.json` |
| GASP 只给 FrontLedge 提交目标朝向，来源 `-FrontLedgeNormal` | 资源 `GaspCharacter.blueprint.cs` 的 `FrontLedgeRotation`；`P8_WARP_ROTATION`；P23 记录 |
| 四个动作共用 `RootMotion → 1500 cm/s → ConsumeRootMotion`，无 Jump/timed Move | 资源 `GaspCharacter.blueprint.cs` 的 `TickTraversal` |
| `RootMotionYaw` 在位移后单独加到 Actor yaw | 资源 `GaspCharacter.blueprint.cs` 的 `TickTraversal` |
| 落地收口与 `ClearRootMotion` 是 blend-out 替代 | 资源 `GaspCharacter.blueprint.cs` 的 `TickTraversal` |
| Climb/Hurdle/Mantle/Vault 消费原生根运动并经 Jolt 胶囊 | 资源 `GaspCharacter.blueprint.cs` 的 `TickTraversal`；`scriptPhysics.cpp:429-437` |
| Notify 时间来自资产（非均分），播放器执行区间 | `animationScriptData.cpp`；资源 `GaspCharacter.blueprint.cs` 的 `TickTraversal`；P22 记录 |
| 探针按 reach 放置（stand -52/-48/-86、Mantle/Vault walk +62、Climb walk +228、run +226/+227）；`abort_stall` 保留 -80 | 资源 `GaspCharacter.blueprint.cs` 的 `RequestTraversal`；P22 记录 |
| 30/60/120 Hz 仅有合成片段单测，无场景/包内矩阵 | `motionWarpingTests.cpp`；P20/P21/P22/P23 记录“限制” |
