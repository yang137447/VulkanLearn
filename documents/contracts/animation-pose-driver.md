# 动画 PoseDriver（Pose Space Deformation）

> 状态：当前契约。描述 `vl.anim-pose-driver.v1` 资产、原生 RBF 求值与运行时接线；
> UE 的 `AnimNode_PoseDriver` 只复刻到 `DriveCurves` 子集，具体边界见 §4。

## 1. 所有权与执行位置

- 原生：`source/animation/poseDriver.{h,cpp}`。`PoseDriverSolver` 绑定一份
  `SkeletonMesh`，按关节名解析 `SourceBones`/`EvalSpaceBone`，在构造期预解 RBF 系数。
- 姿态阶段：`SkeletalPlayback::SamplePoseDriver()` 读取**本帧最终基础姿态**
  （`slopeWarpedLocals`，未启用斜坡修正时是 `displayedLocals`）。它不读
  `footPlacementSource`，因为 UE 的 PostProcess AnimBP 在足 IK 之前执行。
- 输出：只读命名曲线 `PoseDriverFrame::curves`。求解不修改局部 TRS、不写 Actor/Jolt、
  不参与状态机决策，也不改变 `rootMotionDelta`。
- C#：`AnimationPlayback.SetPoseDriver(enabled, asset)` 只提交开关与资源路径；
  `AnimationPlayback.PoseDriver` / `TryGetPoseCurve` 读取反馈。曲线不会被 C# 回写。

资产由资源侧声明（Echo 为
`Maps/SC_valley_echo/Rig/EPD_EchoPostProcess.json`），不写入 `SKM_*.json`，
因此同一份驱动数据可以绑定到任意具备同名 `SourceBones` 的骨架。

## 2. 资产格式

`format` 必须是 `vl.anim-pose-driver.v1`；`id` 非空。`drivers` 为 1..64 项，
`name` 唯一。

| 字段 | 语义 |
| --- | --- |
| `sourceBones` | 1..8 个骨名；缺失骨按 UE 语义以 identity 参与，并产生诊断 |
| `evalSpaceBone` | 可选；非空时在组件空间求 `SourceBone^-1 * EvalSpace` 的相对变换 |
| `evalFromRefPose` | 相对绑定姿态求值；与 `evalSpaceBone` 互斥（UE 同样不允许并用） |
| `driveSource` | `Rotation` / `Translation` |
| `driveOutput` | 只接受 `Curves` |
| `rbf` | 见 §3；字段必须写全，运行时不猜默认值 |
| `targets[].transforms[]` | 数量必须等于 `sourceBones`；`translation` 为 cm，`rotation` 为 UE `Pitch/Yaw/Roll`（度） |

`targets[].applyCustomCurve` 必须为 `false`。`FRichCurve` 映射尚未实现，
解析期直接拒绝而不是静默忽略。`distanceMethod`/`function` 可为 `Default`
（继承 `rbf` 的全局值）。

## 3. RBF 语义（UE 5.8 对齐）

与 `FAnimNode_PoseDriver` / `FRBFSolver` 逐项一致：

- `rbf` 默认值取 **`FAnimNode_PoseDriver` 构造函数**的 CDO：
  `distanceMethod = SwingAngle`、`twistAxis = X`、`weightThreshold = 1e-4`、
  `normalizeMethod = OnlyNormalizeAboveOne`、`medianMin/Max = 45/60`。
  注意不是 `FRBFParams` 的 `Euclidean` 默认。
- 距离：每个源骨取 `FRBFEntry` 的 `(Roll, Pitch, Yaw)` 三元组；
  多骨按 `sqrt(Σ dᵢ²)` 合成，再转角度。`SwingAngle` 是
  `swing(q)` 的 `acos(2·dot²−1)`；纯 twist 不计入距离。
- 核函数：`Linear/Gaussian/Exponential/Cubic/Quintic`。Interpolative 使用
  `Radius`（度→弧度）作为 kernel width；Additive 使用 `Radius × TargetScale`，
  并走 UE 的 backward-compat 公式（`max(1−x,0)` / `exp(−x²)` / `1/exp(x)`）。
- Interpolative：构造 `K[i][j] = kernel(distance(targetᵢ, targetⱼ))`，
  `Coeffs = K⁻¹`；输入权重 `w = Coeffs · kernel(input, targets)`，逐项
  clamp 到 `[0,1]`，再乘 `TargetScale`。行列式接近 0 时整体不输出权重
  （与 UE 的 `bIsValid=false` 一致），并记录 `singular-kernel` 绑定诊断。
- 归一化：`total > 1` 一律 `1/total`；否则按 `normalizeMethod`。
  只有 `weight > weightThreshold` 的目标写入曲线；同名曲线后写覆盖
  （等价 `FBlendedCurve::Union`）。

## 4. 明确不支持

- `driveOutput = DrivePoses`：需要 `UPoseAsset` 姿态混合，解析期拒绝。
- 自定义 `FRichCurve`：同上，解析期拒绝。
- `Echo_Twist_CtrlRig` / `Echo_Helpers_CtrlRig`、`Constraint`、`RigidBody`：
  这些是 PoseDriver 曲线的**下游消费者**，当前运行时未执行。因此 Echo 的
  PSD 曲线会被正确求值并发布，但还没有节点把曲线转成最终扭转/辅助骨姿态。
  这一层不得用“改骨名”或预烘焙姿态伪造。

## 5. 诊断

`PoseDriverFrame.diagnostics` 与 `SkeletalPlayback` 反馈：

- `missing-source-bone`：绑定期发现骨架缺骨（该骨以 identity 参与）。
- `missing-eval-space-bone`：`EvalSpaceBone` 不存在。
- `singular-kernel`：Interpolative 核矩阵不可逆。
- `no-valid-source-bone`：单帧内所有源骨都缺失，本驱动跳过。

反馈形状（每帧一次）：

```json
{
  "enabled": true, "active": true, "solvedDrivers": 9,
  "asset": "Maps/SC_valley_echo/Rig/EPD_EchoPostProcess.json",
  "digest": "<BLAKE3-256>",
  "curves": { "<curve name>": 0.77 },
  "diagnostics": [ { "driver": "...", "code": "...", "detail": "..." } ]
}
```

## 6. 验证

- `tool/animation-tests/poseDriverTests.cpp`（12 项）：解析拒绝矩阵、
  Interpolative 在目标点复现 1/0 权重、`SwingAngle` 忽略纯 twist、
  Additive 归一化、缺骨诊断、Translation 源、`bEvalFromRefPose`，
  以及真实 Echo 资产（9 driver / 65 target）在 Echo 骨架上全部求解。
- 真实场景 `SC_valley_echo_playground.json`：`EchoPoseDriver drivers=9 curves=18`。
- 资产由 `tool/animation/extract_pose_driver.py` 从
  `Echo_PostProcess_AnimBP.t3d`（UTF-16LE）逐值导出，BLAKE3 记录在 `source` 段。
