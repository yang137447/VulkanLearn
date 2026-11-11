# Echo R5 SlopeWarping 实现记录

> 状态：rationale（非契约）。记录为什么 R5 只做 SlopeWarping、如何缩小
> `Echo_SlopeWarping_CtrlRig`，以及当前证据与明确差距。
>
> 证据日期：2026-11-04。

## 1. 范围决定

`documents/rationale/echo-r5-feature-audit.md` 的结论是：Echo locomotion 不需要
BlendSpace、AimOffset、TurnInPlace、Pivot、LayeredBoneBlend、OptionalLayer、
FootPlacement 或 additive；主图真正影响基础姿态的 R5 节点是
`Inertialization -> Echo_SlopeWarping_CtrlRig -> Root`。因此 R5 不按 UE 节点清单
平铺实现，而只为 Echo 做一个正式原生后处理。

ControlRig 的 `DisableIK` 曲线没有接线：当前启用/权重来自
`AnimationPlayback.SetSlopeWarping(...)`，Echo 资源蓝图默认 `enabled=true`。
这样既不把参数写进角色移动逻辑，也不假装已经执行 ControlRig 图。

## 2. 求解口径

`SlopeWarpingSolver` 只修改局部 TRS，不写 Actor/Jolt，也不改
`RootMotionDelta`/`RootMotionYaw`：

1. 从 `GroundProbe` 取 support point/normal；先探骨盆，失败再探双脚，
   support identity 为空或 normal 非法时 no-op。
2. 角色局部 up 与地面 normal 做最短轴对齐；角度按 `maxSlopeDegrees` clamp，
   再乘指数平滑权重。
3. 旋转绕 root 自身位置施加，只改 root local rotation；root translation 保持。
4. 用双脚到支撑平面的最低有符号距离做高度补偿，叠加 `pelvisHeightOffset`，
   合成世界平移后转回组件局部，加到 root translation；幅度受
   `maxPelvisOffset` 限制。

选择整身 root 对齐而不是逐骨/双足 IK 的原因是：Echo 不使用 FootPlacement，
现有骨骼链没有鞋底/脚趾接触数据，逐骨复刻 ControlRig 会需要新的 UE 对照和
更多资产字段；整身对齐 + 最低脚平面补偿已经能满足“斜坡上姿态有修正、脚不穿地”
的 R5 验收，同时保持 root motion 归胶囊。

## 3. 接入与反馈

- 原生：`SkeletalPlayback::SetSlopeWarpingEnabled`、
  `SetSlopeWarpingParameters`、`ApplySlopeWarping`、`ResetSlopeWarping`、
  `SlopeWarping()`。
- 姿态阶段：`ApplyVisualRoot` → `ApplySlopeWarping` → `ApplyFootPlacement`；
  斜坡结果写入 `slopeWarpedLocals`，作为 FootPlacement 的 pre-IK 源。
- C#：`AnimationPlayback.SetSlopeWarping(...)` 与 `State.SlopeWarping`；
  Echo 资源蓝图 `EchoAnimBlueprint.cs` 的 Expose 参数默认启用。
- 反馈：`enabled/active/slopeAngle/targetSlopeAngle/appliedSlopeAngle/weight/
  pelvisOffset/groundNormal/supportIdentity/reason`。

## 4. 测试与场景

`tool/animation-tests/slopeWarpingTests.cpp` 覆盖：

- 平地/离地 exact no-op；
- 20°/30°/45° 对齐、脚底平面误差与最大角度 clamp（60° → 45°）；
- 权重 0、显式禁用、缺 `foot_l` 时安全停用；
- 30/60/120 Hz 终态稳定；
- `RootMotionDelta`/播放时钟不被 SlopeWarping 消费。

真实资源探针为
`Maps/SC_valley_echo/SC_valley_echo_slope_probe.json`：`BP_echo_floor`
绕 Z 旋转 25° 形成静态斜坡，Echo 角色站在坡面上。场景只验证视觉姿态，
不宣称 UE ControlRig 逐骨等价。

本地运行证据（2026-11-04，`artifacts/echo-r5-slope/`，artifacts 被 gitignore）：

- `animation_preflight_tests.exe --gtest_brief=1`：326 项通过；
- `script_tests.exe --gtest_filter=ScriptLocomotionTest.*`：27 项通过；
- 完整 `script_tests.exe`：502 项通过、1 项跳过、1 项既有 GASP 动态足部质量阈值
  失败（drift 3.74 > 3、supportError 11.0 > 1，单独复现同样失败）；该用例不经过
  SlopeWarping，属于既有基线问题，未在本次改动中修复；
- `SC_valley_echo_slope_probe.json` 真实加载：World/graph generation=1，
  `script play` 返回 state 1；
- Echo 日志：`EchoSlopeWarping active=True angle=25.0 applied=23.8 weight=0.95
  pelvisOffset=-8.4 reason=active`；
- `slope_probe.bmp` 为 1280×720、2,764,854 字节，角色在坡面上整体倾斜且支撑脚
  贴近坡面；stderr 为空。截图由进程强制终止前后采集，只证明渲染姿态，
  不构成正常关闭流程证据。

同批次 `play/transitionTo` 与随后的 `configureSlopeWarping` 现在按候选值顺序组合；
非 slope 命令仍保持“同一组件同一 batch 只允许一条”的旧边界，slope 配置作为
附加候选应用到刚提交的播放状态上。若把配置放在播放命令之前，旧拒绝路径会让
斜率配置被丢掉。

## 5. 风险与未完成

- 不是 UE `Echo_SlopeWarping_CtrlRig` 的逐骨/双足 IK 等价物；脚型、膝轴与
  支撑多边形仍是近似。
- `DisableIK` 曲线、`Speed`/`EnableFBIK` custom pin 未消费；启用/权重由 C# 提交。
- 没有 FootPlacement 时的双足独立贴地；左右脚只通过“最低脚”补偿整身高度。
- 没有斜坡上的转向/横移专用参数；最大坡度与高度上限目前是单组标量。
- 真实场景截图只能证明姿态方向与脚底高度趋势，不能替代 UE 像素级对照。
