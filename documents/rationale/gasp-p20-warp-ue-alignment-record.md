# GASP P20 MotionWarping UE 对齐记录

状态：执行记录，非当前契约。2026-10-20 已确认的 P20 交付：Hurdle 四窗口 +
`WarpPointAnimProvider=Bone`，以及 Mantle/Vault 单次消费原生 warped root。
当前行为以 [原生 MotionWarping 契约](../contracts/animation-motion-warping.md)
为准。

## 范围

P20 把 traversal 的 warp 从“能跑”推进到“按 UE montage 的窗口/目标/provider
执行”：

- 引擎支持 UE `WarpPointAnimProvider=Bone`：`MotionWarpWindow.warpPointBone`，
  窗口 EndTime 采样 `Root * WarpPoint^-1` 的平移并加到 target；
- Hurdle 改成 UE 的四个窗口，两个 `FrontLedge` 用 `attach`，`dOut` 用 UE 曲线
  采样值；
- Mantle/Vault 改成两个窗口（Vault stand 三个），target 改为胶囊中心，删除
  `Jump()` 与分段 `Move`，四个动作共用一条 `RootMotion` 消费路径；
- 新增落地收口与 `ClearRootMotion`，替代尚未实现的 montage blend-out。

## 代码改动

### 引擎

| 文件 | 改动 |
| --- | --- |
| `source/animation/motionWarping.h` | `MotionWarpWindow.warpPointBone`；`SampleBoneModelTransform`；`Apply` 增加 `const SkeletonMesh&` |
| `source/animation/motionWarping.cpp` | 新增 `SampleJointLocal` / `SampleJointModelByIndex` / `SampleBoneModelTransform`；非空 `warpPointBone` 在 EndTime 加 `Root * WarpPoint^-1` 的平移；父链拓扑序校验 |
| `source/animation/skeletalPlayback.cpp` | `Apply` 调用传入 `asset->mesh` |
| `source/world/worldAnimation.cpp` | 解析可选 `warpPointBone`，长度 ≤64 |
| `script/VulkanLearn.Runtime/AnimationPlayback.cs` | `MotionWarpTarget.WarpPointBone`，入口校验并序列化 |
| `tool/animation-tests/motionWarpingTests.cpp` | 新增 5 项：骨偏移、未知骨回退、层级采样、互斥窗口各自收敛、骨 attractor 收敛 |

提交：`a02793f`（引擎 Bone provider）、`685bc52`（骨 attractor 测试）。

### 资源

资源提交 `73bf14b`（Hurdle 对齐 UE montage）与 `a18840e`（Mantle/Vault 单次消费）。

`Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs` 的 `BuildWarpTargets`：

- Hurdle 四窗口，按 UE `Notifies` 顺序：FrontLedge `0.078149–0.234904`、
  FrontLedge `0.234925–0.398903`（两者 `WarpPointBone: "attach"`）、
  BackLedge `0.464920–0.598017`、BackFloor `0.599556–0.831335`；
- 四窗口 `subtractRemaining=false`、`ignoreVertical=false`；VL 不执行 warp rotation；
- `dOut = |34.098671 - 90.908951| = 56.810280` cm；两个曲线采样值内联在 C#，
  因为 VL 没有动画曲线通道；
- `BackLedgeNormal` 改为指向落地侧，BackFloor XY 沿该法线外移 `dOut`；
- Mantle/Vault 各两个 FrontLedge 窗口（两个 FrontLedge 均 `attach`；Vault stand
  另有 BackLedge `0.471075–0.603360`，provider 为 None），FrontLedge target 为
  前沿顶边 + 90（胶囊中心），BackLedge 为后沿顶边 + 90；落地收口另用
  `traversalTarget` 的出口高度（Vault 为后沿外地面 + 90）；
- `TickTraversal` 删除 `Character.Jump()` 与 `Move(traversalDirection * forward)`，
  四个动作统一走 `RootMotion → Actor yaw → 1500 cm/s 限幅 →
  Character.ConsumeRootMotion`；
- 新增 `traversalLanded` / `traversalLastWindowEnd` 落地收口；
  `animation.State.Completed` 或收口后 `ClearRootMotion()`。

## UE 证据

Hurdle montage `AM_M_Neutral_Traversal_Hurdle_1_0_stand_F_V2_Lfoot`：

```text
FrontLedge 0.078149–0.234904  Bone attach  bIgnoreZ=false  subtractRemaining=false
FrontLedge 0.234925–0.398903  Bone attach  bIgnoreZ=false  subtractRemaining=false
BackLedge  0.464920–0.598017  None         bIgnoreZ=false  subtractRemaining=false
BackFloor  0.599556–0.831335  None         bIgnoreZ=false  subtractRemaining=false
```

`Distance_From_Ledge` 在 BackLedge/BackFloor 窗口 EndTime 的采样：
`34.098671` / `90.908951` cm；`dOut=56.810280`。旧记录的
`0.231779–0.831335` 实际是 BackFloor 窗口，不是“第二个 FrontLedge”。

Bone provider 的 `attach` 是 root 的恒等子骨，动画固定在模型原点上方 100 cm；
`CachedOffsetFromWarpPoint = Root * WarpPoint^-1` 在窗口 EndTime 得到
`(0,-100,-45.76)` / `(0,-26.72,-14.61)`（VL 模型空间）。因此 FrontLedge 窗口是
把 `attach` 放到前沿，根仍在地面/上升途中。证据见
`artifacts/gasp-warp-hurdle/UE-EVIDENCE.md` 与 `artifacts/gasp-warp-hurdle/ue-hurdle-seq.json`。

Mantle/Vault 窗口与 `attach` 证据见
`artifacts/gasp-mantle-vault-warp/UE-EVIDENCE.md`、`ue-mantle-vault.json`。

## 实测

> 2026-10-20 追加：本节的落点数字已被 P21 场景 reach 重做取代，见
> [P21 场景 reach 矩阵记录](gasp-p21-reach-matrix-record.md)。下文保留当轮实测原文。

独立复跑（Development，`artifacts/verify-mv/`）：

| 场景 | END | grounded |
| --- | --- | --- |
| Hurdle | `0,90,-334` | True |
| Mantle stand / walk / run | `-167` / `-202` / `-209` | True |
| Vault stand / walk / run | `-383` / `-331` / `-341` | True |
| Climb stand | `-201` | True |

作者轮 Hurdle 为 `-333..-354`（`artifacts/gasp-warp-hurdle/RESULTS.md`），独立复跑
`-334` 落在作者区间。Mantle stand 的 UE 参考积分（无碰撞/重力）为
`(0,190.4,-169.6)`，实测 `-167`，差约 2.6 cm。A/B 中 `dOut=0` 时为
`0,175,-367 grounded=False`（ABORT airborne），说明 BackFloor 沿法线外移不是
“凑落点”，而是避免胶囊被后立面顶住。

异常语义：

```text
abort_void : ABORT airborne progress=258 pos=0,-2676,-338 grounded=False
abort_stall: ABORT stalled progress=10 pos=0,126,-90 grounded=False
```

`abort_void` 在无 BackLedge 的场景里继续下坠到 `y≈-2676` 才触发 airborne abort；
这是现有中断语义，不是新的落地路径。

测试：

```text
animation_preflight_tests --gtest_filter=MotionWarping.*
  16 tests, 16 passed   (P19 11 → P20 16)

animation_preflight_tests
  145 tests from 17 suites, 145 passed   (P19 140 → P20 145)
```

## P19 数字裁定

P19 记录正文的 Hurdle `0,188,-266` 与“回归”节的 `0,180,-280` 曾并存。P20 按
时间线裁定：

- `artifacts/nativewarp-p20/final-hurdle.log` 写于 **16:29:35**，内容是旧的
  三窗口日志与 `0,180,-280`；
- 资源提交 `91f98e7`（submit all traversal warp windows natively）在 **16:37:29**；
- `-280` 属于迁移前的旧三窗口/C# 提交配置，`-266` 是迁移后 native 结果，
  不是同一构建的抖动。

因此 P19 的 Hurdle 落点不应被当作稳定值；P20 的四窗口版本另见上表。

## 限制

- **场景触发距离不匹配**：探针场景在离前沿 40 cm 处触发，而 montage 首窗前
  接近段有 80.7–316.7 cm 根位移（Mantle walk 80.7、Mantle run 166.7、
  Vault walk 106.7、Vault run 316.7）。5 个片段的 UE 自由积分因此奇异
  （attractor 落在胶囊身后，SkewWarp 负缩放/大 shear），只能对窗口 EndTime 的
  attractor 做逐窗口对照，不能宣称厘米级落点对齐。
- **没有动画曲线通道**：Hurdle 的 `dOut` 仍是 C# 内联的 UE 采样常数，不是运行时
  读曲线；Hurdle 只接 `stand_F_V2_Lfoot`，其他 Hurdle montage 的窗口集合不同。
- **没有旋转 warp**：`Apply` 只加骨偏移的平移，不消费 `bWarpRotation`；
  `CachedOffsetFromWarpPoint` 的 Mesh 相对旋转共轭未实现。Mantle/Vault 六个片段的窗口
  EndTime root/attach 旋转都是单位四元数，所以平移版本与 UE 等价；旋转非单位时
  不成立。
- **`FrontLedgeLocation` 仍是球扫球心**，比真实前沿面短约 20 cm；Hurdle 最终落点
  由 BackLedge 重新锚定，主要影响两个 FrontLedge 窗口的中间姿态。
- **没有运行时 30/60/120 Hz 矩阵或包内 warp 验证**；只有
  `MotionWarping.FixedStepConvergesToTargetAt30_60_120Hz` 的合成片段覆盖。
- **Montage notify 时间仍是均分近似**，不是 UE 资产时间；montage blend-out 未实现，
  `traversalLanded` / `ClearRootMotion` 是替代品。
- **Climb 没有 attach**：仍是一个 FrontLedge 窗口、无 Bone provider；P20 只把
  Climb 并入统一 root 消费路径。

## 证据

```text
artifacts/gasp-warp-hurdle/
  RESULTS.md / UE-EVIDENCE.md / ue-hurdle-seq.json / hurdle-*.log
artifacts/gasp-mantle-vault-warp/
  RESULTS.md / UE-EVIDENCE.md / ue-mantle-vault.json / fin-*.log
artifacts/verify-mv/
  hurdle.log / m-*.log / v-*.log / climb.log / abort-void.log / abort-stall.log
artifacts/nativewarp-p20/final-hurdle.log
```
