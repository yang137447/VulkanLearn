# GASP 根运动 UE 对照记录

状态：研究记录，非当前契约。记录 UE 5.8 的根运动消费模型、两次 VL 实验
和为什么当前没有把 raw root motion 直接接到 traversal。

## UE 5.8 的实际路径

源码位置：

- `Engine/Source/Runtime/Engine/Private/Components/CharacterMovementComponent.cpp`
  - `UCharacterMovementComponent::SimulateRootMotion`（约 2096 行）
  - `CalcAnimRootMotionVelocity`（约 2156 行）
  - `ConstrainAnimRootMotionVelocity`（约 2170 行）
  - `PerformMovement` 的 anim root motion 分支（约 2952 行）
- `Engine/Source/Runtime/Engine/Private/Components/SkeletalMeshComponent.cpp`
  - `ConvertLocalRootMotionToWorld`（约 4461 行）
  - `ConsumeRootMotion`（约 4495 行）
- `Engine/Source/Runtime/Engine/Private/Animation/AnimInstance.cpp`
  - montage root motion 实例与提取（约 2293–2310 行）
- `Engine/Source/Runtime/Engine/Classes/GameFramework/CharacterMovementComponent.h`
  - `RootMotionParams`、`AnimRootMotionVelocity`、`ConstrainAnimRootMotionVelocity`

关键行为：

1. 根运动是**一段 transform delta**，不是持续速度。
2. 先由 mesh component/actor 的相对变换转成世界空间 delta，包含旋转和缩放，
   不是只按 Actor yaw 旋转。
3. `delta / DeltaSeconds` 得到本帧根运动速度，然后**覆盖**当前速度，
   不是加到当前速度上。
4. 覆盖后的速度仍走正常 CMC 物理：碰撞、台阶、重力、地面吸附。
5. `IsFalling()` 时 `ConstrainAnimRootMotionVelocity` 保留当前 Z，
   根运动 Z 不硬抬胶囊；水平根运动继续。
6. 根运动旋转在位移完成后单独应用。
7. `RootMotionParams` 被消费后清空；默认只从 montage/root motion 源提取，
   普通 MM 行走不把根运动当位移。

## 两次 VL 实验

### 第一次：ActionVelocity 累加

把 `RootMotionDelta / dt` 写进 `Character.ActionMove`，原生每 fixed step
把 action 加到现有速度，且 action 持久。结果：

```text
pos y: 90 → 108 → 968 → 2147 → 8627
ABORT reason=airborne progress=89
```

原因不是根数据，而是应用模型错了：持续加法 + 根 Z 与重力叠加。

### 第二次：UE 式 override + 简单距离缩放

改成 `Character.ConsumeRootMotion`，原生用根速度**覆盖**本帧速度，
并加 `RootMotionDistance` 做水平 motion warping。结果：

```text
Mantle stand: rootDist=100, warp=1.20, END pos=0,190,-169 grounded=True
Vault stand:  rootY=-1513（下落）, rootZ=672, warp=0.20, END pos=0,90,-110 grounded=True（停在障碍前）
Mantle walk: ABORT reason=airborne progress=643
```

竖直失控消失，Mantle 能稳定落到顶面；但暴露了更关键的事实：

- 1 m Mantle 的根水平距离约 100 cm，目标约 120 cm；
- 1 m Vault 的根竖直下落约 **1513 cm**、实际水平约 672 cm，远超 1 m 障碍；
- 仅按 `target / rootDistance` 缩放，Vault 会在障碍前被碰撞挡住，
  Mantle walk 的根路径也会越过目标后仍在空中。

也就是说，这批 traversal clip 的根运动不是“刚好 1 m 的位移”，
UE GASP 依赖 **Motion Warping** 把动作根轨迹压到运行时目标；
只做 root → velocity 还不够。

### 第三次：逐帧目标校正

按“剩余目标 / 剩余根位移”逐轴校正当前 delta，并把 Mantle 目标高度
改为 `topY + 胶囊半高 90`。结果：

```text
Mantle stand: END pos=0,190,-200 grounded=True（命中顶面中心）
Mantle walk:  END pos=0,190,-203 grounded=True
Vault stand:  END pos=0,90,-110 grounded=True（仍停在障碍前）
```

Mantle 已能精确落到目标；Vault 的根剩余 Y 在末段约 -636 cm，
把竖直分量按“目标终点 Y 不变”压成 0，动作无法完成上-过-下。

### 第四次：整段根总位移常量缩放

改用整段根总位移与目标总位移求常量逐轴 scale，保留竖直曲线形状：

```text
Mantle stand: 停在 z=-110，y≈97，未上障
Mantle walk:  END y=190, z=-176（短于目标）
Vault stand:  仍停在 z=-110
```

方向/轴向混合后，常量缩放同样不能同时满足水平压缩和竖直起落。

### 第五次：只压缩水平，保留根竖直曲线

按整段根在移动方向上的投影求常量 scale，只作用于 X/Z，Y 原样保留：

```text
Mantle stand: warp=1.02, END y=190, z=-180
Mantle walk:  warp=0.17, END y=190, z=-205
Vault stand:  warp=0.25, END y=90, z=-110（停在障碍前）
Vault run:    warp=0.17, END y=90, z=-110
```

Mantle 的竖直起落恢复正常，但 Vault 日志显示根 Y 为负：

```text
root = 0.0,-1.4,0.4
```

### 根轨道原始数据核对

用 `vl_animation_probe` 检查 1 m Vault 的 FBX root：

```text
first = (0, 75, 0)
last  = (0, -671.7, -1512.8)
Y min/max = -671.7 / 75
```

这不是“上-过-下”的地面 Vault。按 VL 基 `{x,z,-y}` 换算：
原始 FBX 的 `Y=-671.7` 变成水平 `Z=671.7`，`Z=-1512.8` 变成竖直
`Y=-1512.8`。即根竖直下落约 1513 cm、水平约 672 cm。该 clip 只能用于
下落/抓边类动作；没有可用的地面 1 m Vault 根轨迹。

Mantle 的根则是从 `(0,118,0)` 到 `(0,0,100)`，有明确的竖直上升，
所以 Mantle 能配合水平 warp 完成。

即这个 `Vault_1_0` clip 的根路径在末段是向下的，属于 catch/下落型动作，
不是从地面起跳越过 1 m 障碍的根轨迹。只做水平 warp 无法让它上障。

## 结论

UE 经验可以吸收的部分：

1. 根 delta 先转世界空间；
2. `delta / dt` 覆盖速度，不累加；
3. 下落时保留当前 Z；
4. 根旋转在位移后应用；
5. 根运动消费一次即清空。

VL 还缺的部分：

- 目标变换（位置 + 朝向）和 warp window；
- 不能只按终点逐轴缩放；UE SkewWarp 是在根运动同步空间里做
  scale + shear，保留动作路径，而不是每帧重算终点比例；
- 动作结束/中断时恢复普通移动；
- 每个动作的根距离/目标距离数据，而不是拿 1 m 场景硬套；
- Vault 根竖直下落约 1513 cm、水平约 672 cm 且末段根 Y 向下，说明该 clip 是
  catch/下落型动作，缺少 1 m 地面 Vault 所需的上-过-下根轨迹；
  需要换 clip 或补专用的根路径/目标数据，不能靠 1 m 场景反推。

## 当前状态

五次实验代码均已回退；当前 traversal 仍是已验证的 jump + target distance 版本：

```text
P8_TRAVERSAL_END pos=0,190,-199 grounded=True
```

本记录保留 UE 对照、失败边界和下一步设计，避免再次把 raw root motion
直接加到 CharacterVirtual 上。
