# 动画 PostProcess（Twist / 姿态后处理）

> 状态：当前契约。描述 `vl.anim-post-process.v1` 资产与原生 `PostProcessSolver`；
> 对应 UE `Echo_PostProcess_AnimBP` 里的 `Echo_Twist_CtrlRig` 与
> `Echo_Helpers_CtrlRig`。

## 1. 执行位置

UE 的 PostProcess AnimBP 在基础 locomotion 图（含斜坡 ControlRig）与足 IK 之后执行。
VL 的顺序一致：

```
基础姿态 → SlopeWarping → FootPlacement
        → PostProcess(Constraint → Twist → Helpers) → SkinPose 快照
```

Echo 的 PostProcess 图真实顺序（T3D 连线核对）：

```
LinkedInputPose
  → ModifyCurve("Remove PSD Curves")
  → 9 × PoseDriver(DriveCurves)
  → Constraint_1(Canister) → Constraint_0(Scarf) → Constraint_4(Pouch)
  → Twist ControlRig → Helpers ControlRig
  → RigidBody(Canister) → RigidBody(Skirt) → RigidBody(ShoulderPad) → RigidBody(Pouch)
  → Root
```

注意 `ModifyCurve` 在 **PoseDriver 之前**（清掉输入姿态里可能残留的 PSD 曲线），
而 `Constraint` 在 Twist/Helpers **之前**、`RigidBody` 在最后。

`SkeletalPlayback::ApplyPostProcess()` 只读 `footPlacementLocals`（足 IK 结果），
没有 IK 时退回 `slopeWarpedLocals`，再退回 `displayedLocals`。它只重写局部旋转，
不改 Actor/Jolt、不改 `rootMotionDelta`、不改 `preIkModels`（下一帧 FootPlacement
仍以 IK 前的姿态为求解源）。

## 2. 资产格式

```json
{
  "format": "vl.anim-post-process.v1",
  "id": "EPP_EchoPostProcess",
  "source": { "engine": "UE 5.8.4", "rig": "Echo_Twist_CtrlRig",
              "rigJsonBlake3": "..." },
  "twist": [
    {
      "name": "upperarm_l",
      "driverBone": "upperarm_l",
      "twistAxis": "X",
      "invert": true,
      "targets": [
        { "bone": "upperarm_twist_01_l", "weight": 1.0 },
        { "bone": "upperarm_twist_02_l", "weight": 0.5 }
      ]
    }
  ],
  "helpers": [
    {
      "name": "C_Scarf_A_Root_Jnt",
      "projections": [
        { "child": "C_Scarf_A_Root_Jnt", "oldParent": "clavicle_l",
          "newParent": "clavicle_l",
          "childInitial": true, "oldParentInitial": true, "newParentInitial": false }
      ],
      "blendT": 0.0,
      "targets": [ { "bone": "C_Scarf_A_Root_Jnt", "weight": 1.0 } ]
    }
  ],
  "constraints": [
    {
      "name": "C_Scarf_A_Root_Jnt",
      "bone": "C_Scarf_A_Root_Jnt",
      "transformType": "Parent",
      "offsetOption": "RefPose",
      "targets": [
        { "bone": "spine_05", "weight": 0.49 },
        { "bone": "clavicle_l", "weight": 0.51 }
      ]
    }
  ]
}
```

约束：`twist` / `helpers` / `constraints` 各自 0..64 项，但至少一段非空；`name` 在段内唯一；
`driverBone` / 投影骨名非空；`twistAxis` 只能是 `X/Y/Z`；`targets` 1..8 项；
**同一资产内目标骨不得重复**；`weight` 与 `blendT` 必须在 `[0,1]`。
未知字段、非有限值一律拒绝。

`constraints` 目前只接受 `transformType="Parent"` + `offsetOption="RefPose"`；
其他 UE 组合（Translation/Rotation/Scale/LookAt、`OffsetOption=None`）明确拒绝，
不静默近似。约束权重必须非负且总和 > 0。

## 3. 求解语义（与 ControlRig 逐项对齐）

每个条目：

1. `delta = initial(bind)^-1 * current`，两者都取驱动骨的 **LocalSpace** 变换。
   `bInitial=true` 在 ControlRig 里表示 **reference pose**（见
   `RigUnit_GetTransform.h`），不是上一帧姿态。
2. `(swing, twist) = FQuat::ToSwingTwist(delta, TwistAxis)`。
3. `value = invert ? twist^-1 : twist`。
4. 对每个目标骨执行 `SetRotation(LocalSpace, value, weight)`：
   `ControlRig` 的 Weight 是 `FControlRigMathLibrary::LerpTransform(当前局部旋转, value, weight)`，
   对应四元数最短路径 slerp；`bPropagateToChildren=false` 只影响该骨自身。

VL 的骨骼局部姿态是 UE 局部变换的基底共轭，而 twist 轴 `X` 在该基底下不变，
所以可以直接在 VL 空间计算，结果与 UE 等价。

### 3.1 helpers（Project To New Parent）

每个 helper 条目对应 `FRigUnit_ProjectTransformToNewParent` + `SetRotation`：

1. 每个投影取三根骨的**全局旋转**：
   `child`（`childInitial` 决定 bind/current）、`oldParent`（同理）、
   `newParent`（`newParentInitial`，Echo 为 `false` → 当前姿态）。
2. `relative = oldParent^-1 * child`，`value = newParent * relative`。
3. 两个投影时 `value = slerp(a, b, blendT)`（Echo 的裙摆前中骨 `blendT=0.5`，
   此时 slerp 与 normalized-lerp 等价）。
4. `SetRotation(target, GlobalSpace, value, weight)`：
   先对**全局**旋转做 slerp，再换算回局部：`local = parentGlobal^-1 * blended`。
   `bPropagateToChildren=true` 在本实现里等价于“只改目标骨的局部旋转”，
   子骨由 FK 自然跟随。

helpers 在 twist 之后执行，且每写一根骨都重算一次 FK（`bNewParentInitial=false`
必须看到最新姿态）。

### 3.2 constraints（AnimNode_Constraint）

`FAnimNode_Constraint` 在 **组件空间** 计算，`Parent` 覆盖完整 transform：

1. 绑定参考姿态下保存偏移：`Offset_i = Target_ref^-1 * Source_ref`。
2. 当前结果：`C_i = Offset_i * Target_cur`。
3. 混合：`w' = w / max(1, Σw)`；平移/缩放加权求和，旋转按
   `AccumulateWithShortestRotation`（同名分量先对齐符号再加权）后归一化。
4. 结果写回 `BoneToModify` 的组件空间变换，再换算成局部：
   `local = parentGlobal^-1 * blended`（写平移 + 旋转）。

Echo 的 3 个约束权重和都为 1，因此等价于归一化加权平均。

## 4. Echo 的实际数据

`Maps/SC_valley_echo/Rig/EPP_EchoPostProcess.json` 由
`tool/animation/extract_post_process.py` 从
`artifacts/ue-echo-export/ctrlrig-links3.json` 生成，8 组 / 16 目标：

| driver | targets | invert |
| --- | --- | --- |
| `upperarm_l/r` | `upperarm_twist_01/02`（1.0 / 0.5） | true |
| `thigh_l/r` | `thigh_twist_01/02`（1.0 / 0.5） | true |
| `hand_l/r` | `lowerarm_twist_01/02`（1.0 / 0.5） | false |
| `foot_l/r` | `calf_twist_01/02`（1.0 / 0.5） | false |

helpers 7 项（`bPropagateToChildren=true`）：

| target | 投影 | weight |
| --- | --- | --- |
| `C_Scarf_A_Root_Jnt` | child/old/new = 自身 / `clavicle_l` / `clavicle_l` | 1.0 |
| `C_Scarf_A_Joint2_Jnt` | `spine_04` / `spine_04` | 0.5 |
| `C_Scarf_A_Joint3_Jnt` | `spine_04` / `spine_04` | 0.5 |
| `L_Skirt_A_BackRoot_Jnt` | `pelvis` / `pelvis` | 1.0 |
| `R_Skirt_A_BackRoot_Jnt` | `pelvis` / `pelvis` | 1.0 |
| `C_Skirt_A_BackRoot_Jnt` | `pelvis` / `pelvis` | 1.0 |
| `C_Skirt_A_FrontRoot_Jnt` | 左前根 与 右前根 两次投影，blendT 0.5 | 1.0 |

constraints 3 项：

| bone | targets（权重） |
| --- | --- |
| `C_Scarf_A_Root_Jnt` | `spine_05` 0.49 / `clavicle_l` 0.51 |
| `R_Canister_A_Joint1_Jnt` | `spine_02` 0.212 / `spine_01` 0.283 / `pelvis` 0.505 |
| `R_LargePouch_A_Joint1_Jnt` | `spine_02` 0.05 / `spine_01` 0.25 / `pelvis` 0.70 |

> `R_LargePouch` 的权重在 T3D 的 AnimGraph 页与 ExecuteUbergraph 副本里不一致
> （`1,1,1` vs `0.05,0.25,0.70`）。运行时以 **编译后的 CDO** 为准，工具默认采信
> Ubergraph 副本并在 `source.divergences` 记录差异；`1,1,1` 会被归一化成三等分，
> 与 UE 实际姿态明显不同，不能当默认值。

## 4.1 与 RigidBody（骨骼物理）的边界

3 个 Constraint 里：

- `C_Scarf_A_Root_Jnt`：下游**没有** RigidBody 节点，围巾由 `Constraint + Helpers`
  完全确定，本契约的实现即可复刻其可见姿态。
- `R_Canister_A_Joint1_Jnt` / `R_LargePouch_A_Joint1_Jnt`：只是把物理链的根骨钉到
  `spine_01/02/pelvis` 的加权姿态上；真正的运动来自后面的 `RigidBody_5`
  （`PA_Echo_Canister`）与 `RigidBody_4`（`PA_Echo_Pouch`）。
- `RigidBody_1`（`PA_Echo_Skirt`）与 `RigidBody_2`（`PA_Echo_ShoulderPad`）没有前置
  Constraint，直接模拟。

因此：Constraint 必须实现（它是物理输入的根姿态），但**只做 Constraint 不会让
罐子/腰包出现动态**——那需要 RigidBody。围巾则立刻可见。

## 5. 与 PoseDriver 的关系（重要）

Echo 的 9 个 PoseDriver 是 `DriveOutput=DriveCurves`，产出的 95 条
`*_Jnt_*` 曲线在 PostProcess 图里**没有被任何节点读取**：两个 ControlRig
（`Echo_Twist_CtrlRig`、`Echo_Helpers_CtrlRig`）只读骨骼变换，最后的
`ModifyCurve`（注释 `Remove PSD Curves`）以 `Scale=0` 把这些曲线清零。

因此：

- PoseDriver 的曲线求值已经按 UE 对齐（`contracts/animation-pose-driver.md`），
  但当前资产里它是**孤立输出**，不会改变可见姿态；
- 真正改变可见姿态的是 Twist（本契约）、Helpers、Constraints 与 RigidBody。
  若要复刻 Echo 的 PSD 外观，需要这些消费者，而不是继续改 PoseDriver。

## 6. API 与反馈

C#：

```csharp
AnimationPlayback.SetPostProcess(bool enabled, string asset = "");
PostProcessState PostProcess { get; }
```

反馈（每帧一次）：

```json
{
  "enabled": true,
  "asset": "Maps/SC_valley_echo/Rig/EPP_EchoPostProcess.json",
  "digest": "<BLAKE3-256>",
  "entries": 15,
  "twistEntries": 8,
  "helperEntries": 7,
  "applications": 1234,
  "rotatedBones": 16,
  "maxTwistDegrees": 42.6,
  "diagnostics": [ { "entry": "...", "code": "...", "detail": "..." } ]
}
```

诊断代码：`missing-driver-bone`、`missing-target-bone`。缺骨条目整条跳过
（不写一半、避免左右不对称），不会使播放器构造失败。

## 7. 明确未实现

- `RigidBody`（4 个附件/布料骨骼模拟，见 §4.1）。
- 曲线驱动骨骼（`ModifyCurve` / curve→pose）。

## 8. 验证

- `tool/animation-tests/postProcessTests.cpp`（10 项）：解析拒绝矩阵、
  全权重写入、0.5 权重 slerp、`invert` 方向、纯 swing 不产生 twist、
  缺骨降级、helper 重挂父骨、双投影 0.5 插值，真实 Echo 资产 8 组 twist /
  7 项 helper 绑定。
- 真场景 `SC_valley_echo_playground.json`：
  `EchoPostProcess entries=18 twist=8 helpers=7 constraints=3 applications=… diagnostics=0`。
