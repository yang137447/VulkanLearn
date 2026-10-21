# GASP P23 旋转 warp、section 与 Static 执行记录

状态：执行记录，非当前契约。2026-10-21。本记录覆盖三项执行层落地：
(1) 旋转 warp（yaw-only Slerp）；(2) Montage section/link 推进；
(3) `WarpPointAnimProvider=Static`。行为以
`contracts/animation-motion-warping.md`、`contracts/animation-montage.md` 与源码为准。

## 1. 提交

| 仓库 | 提交 | 内容 |
| --- | --- | --- |
| 引擎 | `36de338` | `MotionWarpTarget`（位置 + 可选旋转）、`MotionWarpWindow.warpRotation/rotationTimeMultiplier/rotationMethod`、`Apply(actorYaw, outYawDelta)`、C# `WarpTarget` 与 `PlayMontage` 重载、桥接 3/7 分量 |
| 引擎 | `a74e29f` | Montage section/link 播放（Absolute/Relative/Ratio、段末推进）与 Static provider 执行 |
| 引擎 | `4d1475e` | Static warp point 字段暴露给脚本数据层（catalog 仍不含 `staticPose`） |
| 资源 | `b627bd0` | GASP `FrontLedge` 提交目标旋转（UE `MakeRotFromX(-FrontLedgeNormal)` 语义），`BackLedge`/`BackFloor` 只提交位置 |

测试：`animation_preflight_tests` 252（P22 为 224）。新增 28 项：`MotionWarping` 28
（+12）、`AnimationScriptData` 27（+15）、`AnimationMontage` 24（+1）。

## 2. 旋转 warp

### 2.1 UE 源码依据

- `URootMotionModifier_Warp::Update`（`RootMotionModifier.cpp:391-465`）：
  `CachedOffsetFromWarpPoint` 在窗口 `EndTime` 采样并缓存；`bSubtractRemainingRootMotion`
  决定是否扣 `RootMotionRemainingAfterNotify`（`:479-487`）。
- `URootMotionModifier_Warp::WarpRotation`（`RootMotionModifier.cpp:550-622`）：
  `Alpha = clamp(DeltaSeconds*PlayRate / (TimeRemaining*WarpRotationTimeMultiplier), 0, 1)`；
  `ThisFrame = Slerp(RootMotionTotal, TargetRotation, Alpha)`；
  `FinalRotation = ExtraRootMotion * DeltaOut * RootMotionDelta`。
- `URootMotionModifier_SkewWarp::ProcessRootMotion`（`RootMotionModifier_SkewWarp.cpp:194,
  248-252`）：平移与旋转分别累加，`ExtraRootMotion` 在 EndTime 之后补回。
- `WarpPointAnimProvider` 的 offset 约定：`FTransform::GetRelativeTransform` 是
  `this*Other^-1`（`Math/TransformVectorized.h:549`、`TransformNonVectorized.h:519`），
  调用点在 `MotionWarpingComponent.cpp:245-251` 与 `RootMotionModifier.cpp:426-454`。

### 2.2 VL 实现

`MotionWarping.cpp` 的 `WarpedRootYawDelta` 是 UE 公式的 yaw-only 等价：

```text
totalYaw     = Yaw(Root(EndTime)) - Yaw(Root(PreviousPosition))
remainingYaw = subtractRemaining ? Yaw(Root(duration)) - Yaw(Root(EndTime)) : 0
targetYaw    = WrapYaw(Yaw(targetRotation) - actorYaw - remainingYaw)
timeRemaining= (EndTime - PreviousPosition) * rotationTimeMultiplier
alpha        = clamp((CurrentPosition - PreviousPosition) / timeRemaining, 0, 1)
thisFrame    = totalYaw + alpha * WrapYaw(targetYaw - totalYaw)
out          = WrapYaw(thisFrame - totalYaw + rawYaw + extraYaw)
```

- `MotionWarpTarget` 增加 `hasRotation`/`rotation`；旧 3 分量 target 的 `hasRotation=false`，
  即使窗口 `warpRotation=true` 也保持原始根 yaw（不能把单位四元数当目标朝向）。
- `MotionWarpWindow` 增加 `warpRotation`/`rotationTimeMultiplier`/`rotationMethod`/
  `hasTargetRotation`/`targetRotation`；`BuildMontageWarpWindows` 从 Montage 的
  `notifies[].warp` 与 C# target 逐窗口复制。
- `Apply(..., actorYaw, outYawDelta)`：旋转分支在平移 early-out 之前执行；只有
  `outYawDelta` 非空、`warpRotation`、`hasTargetRotation`、`rotationMethod=="Slerp"`
  同时成立才写。没有旋转窗口时调用方保留原始根 yaw。
- 旋转只走 VL 的 Y-up yaw 通道（`RootMotionYawDelta`），pitch/roll 不执行；同 tick
  多个窗口时最后一个实际执行旋转的窗口生效；`subtractRemainingRootMotion` 扣剩余
  yaw，`ignoreVertical` 不影响旋转。
- 非 `Slerp` 方法（`SlerpWithClampedRate`/`ConstantRate`/`Scale`）在装配期拒绝；
  `RotationType`/`AdditionalOffset`/`MaxRotationRate` 尚未进入 VL 数据。

### 2.3 GASP 接线与验收

资源 `GaspCharacter.blueprint.cs` 的 `BuildTraversalTargets` 改为
`Dictionary<string, WarpTarget>`：`FrontLedge` 提交位置 + 目标朝向，
`BackLedge`/`BackFloor` 的资产窗口 `warpRotation=false`，只提交位置（`Rotation=null`）。
`FrontLedgeRotation()` 优先用 `-FrontLedgeNormal`，否则退回 `traversalDirection`，按
`atan2(-forward.X, -forward.Z)` 换算 VL 世界 yaw，并记录
`P8_WARP_ROTATION target=FrontLedge source=... yaw=...`。不能读 `Transform.Rotation`：
`PlayMontage` 先提交，Actor 朝向在 `StartTraversal` 末尾才写入。

验收（`artifacts/gasp-rotation-wiring/RESULTS.md`）：Mantle stand、Climb stand、
Climb walk、Hurdle(Auto) 各 2 次，8/8 grounded、无 ABORT/REJECT；END yaw 全为 0.0、
x=0；`P8_WARP_ROTATION` 每窗口一行且 yaw=0.0；`P8_WARP_WINDOWS`/`P8_MONTAGE_NOTIFY`/
`REQUEST/HIT/LEDGE_CONTEXT` 与 P22 清理后基线逐字相同。四个片段的 FBX 根轨道 yaw 全键
恒 0，直线助跑时目标 yaw = 角色 yaw，因此旋转 warp 是 no-op。

正向诊断：临时把目标 yaw 加 20°，Mantle 两次得到 END `(-14,190,-158)` / `(-9,190,-146)`、
END yaw **20.0**、grounded，证明原生确实消费 `WarpTarget.Rotation`。诊断不属于验收。

## 3. Montage section / link

### 3.1 数据与换算

`MontageSection` 的 `nextSection`/`nextTime`/`linkMethod` 从 T3D 原样保留。
`ResolveMontageSectionLinkTime` 按 UE `FAnimLinkableElement`
（`AnimLinkableElement.cpp:148-194`，枚举在 `AnimLinkableElement.h:14-26`）：

```text
""（未设置，按 UE 默认）或 Absolute: nextTime
Relative:                          section.startTime + nextTime
Ratio（UE Proportional）:          section.startTime + nextTime * (endTime - startTime)
```

UE 的 `SegmentBeginTime`/`SegmentLength` 在 VL 数据里没有独立 slot segment 表，按当前
section 的 start/length 取值。**UE 5.8 runtime 已不消费 NextTime**：段末直接跳到下一段
起点（`AnimMontage.cpp:2722-2728`）；VL 执行的是数据层保留的 NextLink 语义，注释已写在
`animationMontage.h`。

### 3.2 播放语义

- 从 section 0 开始，播放范围 = `[first.startTime, first.endTime]`；多 section（或单段
  自连接）安装链，段末按 `nextSection` 切到下一段并改范围；`loop=true` 拒绝。
- 装配期校验：每段 `endTime` 在已加载 clip 内；`nextSection` 存在；link 落点非有限或
  落在目标 section `[start,end)` 之外（容差 1e-3 s）拒绝；未知 `linkMethod` 拒绝。
- `nextSection` 为空：停在段末、保留姿态，`Completed()` 为真。
- 单 tick 最多 10 个 sub-step（UE `AnimMontage.cpp:2575`），超过时放弃本 tick 剩余
  时间，避免自连接死循环。
- 根运动只累加实际移动的子区间；段跳变本身不产生根位移/根旋转。连续过渡（链接时间 ==
  段末）不结束活动 notifyState；向后跳在段末补 `end(interrupted)`；向前跳过区间不重放
  被跳过的事件，活动 notifyState 若结束点落在被跳过区间，会在新段第一次扫描时补
  `end`（时间记为扫描起点）。
- 段边界按半开区间扫描，正好落在段末的即时 notify 属于下一段、只触发一次。
- 跳变推进 `discontinuity`，姿态历史按硬跳变处理。

### 3.3 已知近似

同一 tick 内“多段 + 空洞 + 活动 warp 窗口”时，`TickMontageSections` 先累加 sub-step 的
根平移/yaw，再在 tick 末尾调用一次 `Apply`（`previousTime → 最终 position`）；warp 看到
的是整 tick 位移/时间，不是逐 sub-step 求值。当前没有资产同时触发三个条件。

## 4. Static provider

`provider=Static` 使用 Montage 资产里的 `staticPose`（mesh/组件空间、厘米）：

```text
offset = Root(EndTime) * staticPose^-1      （与 Bone 的 Root*WarpPoint^-1 同一约定）
targetModel += offset.translation           （只消费平移）
```

`BuildMontageWarpWindows` 把 `staticPose` 的 location/rotation 复制到
`MotionWarpWindow`；缺 `staticPose`、location 非有限或旋转退化在装配期拒绝。
`staticPose` **不进入 catalog/C#**：`BuildAnimationScriptAssets` 不发布它，C# 无对应
字段；`4d1475e` 只把字段暴露到脚本数据层（`animationScriptData.h`）。

## 5. 测试

```text
animation_preflight_tests
  注册 252 项 / 22 套件
  SkeletalImport 30 / AnimationSkeleton 17 / AnimationSequenceData 14 /
  AnimationMontage 24 / AnimationScriptData 27 / NotifyPlayback 11 /
  MotionWarping 28

新增（相对 P22）：
  MotionWarping +12：90°/180° 收敛、时间倍率、窗口门控、多窗口末项、
    旋转/平移独立、缺目标旋转、subtractRemaining、ignoreVertical、
    非 Slerp 跳过、Static↔Bone 等价、Static offset
  AnimationScriptData +15：目标旋转透传、非 Slerp 拒绝、Static pose 映射/拒绝、
    warped yaw 写通道、section Absolute/Relative/Ratio、空 nextSection、
    未知/越界 link、边界 notify、前向空洞跳过/补 end、后向跳变 interrupted
  AnimationMontage +1：rotationMethod 缺省 Slerp / 保留已知值 / 非字符串拒绝

scriptAnimationTests
  注册 ScriptAnimationTest.* 35 + ScriptAnimationWorld.* 9 = 44
  PlayMontageRejectsUnsupportedExecutionWithDiagnostics 现为 6 项拒绝
    （未知 montage、缺/多 target、非 Linear、loop、rate）
  新增 PlayMontageAcceptsWarpTargetRotationOverload、
    PlayMontageTargetWireAcceptsPositionOrPositionPlusRotation
```

定向运行（脚本侧）通过；未全量重跑 `ScriptAnimation*`。

## 6. 未闭环 / 风险

1. **C# 入口已补齐**：`8e61949` 删除 `PlayMontageCore` 的 `Sections.Count != 1` 与
   `provider == "Static"` 旧拒绝，改为只校验执行层仍不支持的 provider/easing/rotation
   method 与 target 集合；当前 C# 可提交多 section/Static Montage。
2. **非 Slerp 方法未执行**：`SlerpWithClampedRate`/`ConstantRate`/`Scale` 数据可保留，
   执行拒绝；`RotationType`/`AdditionalOffset`/`MaxRotationRate` 未进入数据。
3. **旋转只做 yaw**：pitch/roll 与目标四元数的完整旋转不执行；GASP 当前目标都是纯 yaw。
4. **多段 + 空洞 + warp 同 tick** 是整 tick 近似（§3.3），没有现网资产触发。
5. **UE 5.8 不消费 NextTime**：VL 执行数据层 NextLink 语义，与当前 UE runtime 不同；
   若后续按 UE runtime 对齐，需要重新定义链接落点。
6. 旋转验收的落点抖动与 P22 同级（同配置 z 差 2–21 cm）；用 END yaw/x、窗口/notify
   逐字对照与 +20° 诊断判定，不把单点 z 当旋转判据。

## 7. 证据

```text
artifacts/gasp-rotation-wiring/
  RESULTS.md, gasp-character.diff,
  logs/（4 场景 ×2）、logs/control/（旧 C# 对照）、logs/extra/、logs/extra-rotation/、
  logs/final/、logs/diagnostic/（+20°）、
  summary.json, all-runs.json, rotation-noop-analysis.json, fbx/*.json,
  root_yaw_scan.py, rotation_noop_analysis.py, summarize*.py,
  run-matrix.ps1, control-matrix.ps1, rerun-*.ps1, diagnostic-rotation.ps1,
  final-smoke.ps1, config.json.orig, vl_gameplay-build*.log

模块测试：
  tool/animation-tests/motionWarpingTests.cpp
  tool/animation-tests/animationScriptDataTests.cpp
  tool/animation-tests/animationMontageTests.cpp
  tool/script-tests/scriptAnimationTests.cpp
```
