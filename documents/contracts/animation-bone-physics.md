# 动画骨骼物理（Jolt）

> 状态：当前契约。描述 `vl.bone-physics.v1` 资产与原生 `BonePhysicsSolver`；
> 对应 UE `Echo_PostProcess_AnimBP` 里的 `AnimNode_RigidBody`。

## 1. 所有权与执行位置

- 资产：`Maps/SC_valley_echo/Rig/BPH_EchoPostProcess.json`，从 4 个 `PA_*` 物理资产导出。
- 求解器：`source/physics/bonePhysics.{h,cpp}`，**每个角色一份独立 Jolt PhysicsSystem**，
  只在 GT 上调用；不依赖动画运行时头文件（输入/输出都是世界空间全局矩阵，厘米）。
- 顺序（与 UE 一致）：

```
PostProcess(Constraint → Twist → Helpers)
  → BonePhysics: kinematic 跟随姿态 → Jolt step → simulated 写回
  → SkinPose 快照
```

`World` 在 `TickScriptAnimations` 中调用；`SkeletalPlayback::CopyPoseGlobals` /
`ApplyPoseGlobals` 负责局部姿态与世界矩阵的转换。

## 2. 资产格式

```json
{
  "format": "vl.bone-physics.v1",
  "id": "BPH_EchoPostProcess",
  "sets": [
    {
      "name": "canister", "asset": "PA_Echo_Canister", "baseBone": "root",
      "gravity": [0, -980, 0],
      "bodies": [
        { "bone": "R_Canister_A_Joint2_Jnt", "type": "simulated",
          "shapes": [{ "type": "capsule", "radius": 5.05, "halfHeight": 5.52,
                       "center": [1.92, 0.52, -11.38], "rotation": [x,y,z,w] }],
          "mass": 10.0, "linearDamping": 2.0, "angularDamping": 15.0 }
      ],
      "constraints": [
        { "name": "...", "child": "<simulated>", "parent": "<kinematic>",
          "swing1": {"motion":"limited","limit":30}, "swing2": {...}, "twist": {...},
          "stiffness": 0, "damping": 0,
          "childFrame": {"position":[x,y,z],"rotation":[x,y,z,w]},
          "parentFrame": {...} }
      ]
    }
  ]
}
```

约束只接受 capsule 形状、`child=simulated` / `parent=kinematic`；`mass<=0` 表示按体积估算。

## 3. 求解语义

- 刚体：UE `SphylElem`（轴 X）→ Jolt capsule（轴 Y），形状旋转为
  `elementRotation * Rz(-90°)`，`Center` 作为形状偏移。
- 单位：资产是 UE 厘米，Jolt 侧统一乘 `0.01`；读回时再乘 100。
- 约束：`SwingTwistConstraint`，`LocalToBodyCOM` 空间（与角色 component 无关）；
  swing1→plane、swing2→normal（`Pyramid` 类型）；twist→min/max；
  `locked` 为 0、`limited` 为角度、`free` 为 ±180°。
- 首帧把 kinematic 与 simulated 全部吸附到当前姿态并清零速度，避免从绑定姿态“跳”进模拟。
- 根骨单帧位移 > 50 cm 视为传送：simulated 直接吸附回姿态。

## 4. 当前状态与已知差距

- 运行时已接通：Echo 场景日志 `EchoBonePhysics bodies=8 constraints=8 reason=simulating`，
  21 个刚体中 8 个是 simulated，8 条有效约束（裙摆 `C_Skirt_A_Back1_Jnt` 的
  kinematic↔kinematic 残留约束按诊断跳过）。
- **Echo 蓝图默认关闭**（`BonePhysicsEnabled = false`）。原因：裙摆 6 节链的约束帧
  尚未与 UE 对照校准，开启后裙摆明显走形。
- 未完成：UE `Stiffness/Damping`（skirt 500/50）在 Jolt `SwingTwistConstraint` 中
  没有对应软限位参数；需要改用 `SixDOFConstraint` 的 spring 设置或按 UE 对照调参。
- 未完成：与场景静态几何/地面的碰撞（当前只有骨骼刚体之间的碰撞）。

## 5. 验证

- `tool/physics-runtime-tests/bonePhysicsTests.cpp`（4 项）：解析、120 步约束链模拟、
  非 capsule 拒绝、真实 Echo 资产 4 sets / 21 bodies / 9 constraints。
- `animation_preflight_tests` 保持全绿（骨骼物理不改变动画契约）。
