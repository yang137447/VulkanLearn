# GASP 严格 UE 复刻计划

状态：计划，非契约。目标是用 UE GASP 的运行时语义替换 VL 当前的
`TraversalAction` 手写分支；不复用 2/3 m ground stub。

## 范围边界

严格复刻只承诺 UE GASP 本身存在的能力：

- 1 m Hurdle/Mantle/Vault；
- 2.5 m Climb_Start；
- 高层 `Catch_Hurdle_low/med/high`（MovementMode=Falling/Catch）；
- 不承诺 ground-start 2/3 m Mantle/Vault（UE 也没有）。

已从主资源移除的自研 2/3 m 资产不进入本计划。

## 五项实现

> 2026-10-21 注：阶段 A/B/C 的“中间状态”保留写作当时的记录。P22/P23 之后 Montage
> section/link、notify/notifyState、曲线 `CurveValue`、旋转 warp 与 reach 场景已落地；
> 当前行为以 `documents/contracts/` 下的动画契约与 P22/P23/P24 记录为准。

### 1. AnimMontage

- 数据：Montage 资产（clip、sections、notifies、root motion 策略）；
- 运行时：PlayMontage、section 跳转、notify/notifyState、结束/中断；
- 验收：1 m Mantle 的 Entry/Exit section 与 notify 日志；包内运行。

阶段 A 中间状态：已接入 `Default` section + UE notify 名称 + Montage 日志；notify 时间
仍按 `duration*(index+1)/(n+1)` 均分近似，只有 `Default` section，没有 section
跳转/链接和 NotifyState 起止窗口。详见
[P14 记录](../../rationale/gasp-p14-montage-runtime-record.md)。

### 2. MotionWarping

- `WarpTarget`（位置 + 朝向）；
- warp window（notifyState 起止）；
- `SkewWarp` 平移：同步空间 scale + shear，不是逐轴终点比例；
- 验收：1 m Mantle/Vault 的目标落点、旋转；包内运行。

阶段 B：原生 `MotionWarpRuntime` 拥有窗口队列、`PreviousPosition ∈ [Start,End)` 门控、
target 模型空间换算、UE `WarpPointAnimProvider=Bone` 骨偏移和单 tick 根位移；
Hurdle/Climb/Mantle/Vault 都已消费 warped `RootMotion`，Hurdle 已按 UE 四窗口
（两个 FrontLedge `attach`、BackLedge、BackFloor）与 Bone provider 接入。平移
`WarpTranslationToTarget` 与骨偏移已实现，`MotionWarping.*` 28 项 GoogleTest 覆盖
平移、窗口门控、subtractRemaining、Bone 回退/收敛、旋转 warp 与 Static warp point；
旋转 warp 已由 P23 落地（yaw-only Slerp）。
核心数学过程见 [P15 记录](../../rationale/gasp-p15-motion-warping-core-record.md)，
当前运行时/消费边界见
[原生 MotionWarping 契约](../../contracts/animation-motion-warping.md)、
[P19 记录](../../rationale/gasp-p19-native-motion-warping-runtime.md) 与
[P20 记录](../../rationale/gasp-p20-warp-ue-alignment-record.md)。

剩余：非 `Slerp` 旋转方法/旋转参数、pitch/roll、第二窗自由空间参考发散。

### 3. Chooser + PoseSearch

- 上下文：`HasFrontLedge`、`HasBackLedge`、`HasBackFloor`、
  `ObstacleDepth`、`BackLedgeHeight`、`MovementMode`、`Speed`；
- Chooser 行镜像 UE `CHT_TraversalMontages_CMC`；
- PoseSearch 选入口段/采样帧；
- 验收：同一障碍高度下选择结果与 UE 表一致；高层选 Catch。

阶段 C 中间状态：`TraversalAction=Auto` 已镜像 UE 行并输出 Chooser 日志；
Hurdle/Climb/Mantle/Vault 已提交并消费原生 warp（Hurdle 四窗口 + Bone）；
2 m 地面选择 `Catch_Hurdle_high` 但保持拒绝（地面入口不播 Catch），PoseSearch 未接。
详见 [P16 记录](../../rationale/gasp-p16-chooser-scaffold-record.md)。

### 4. Catch / MovementMode

- 增加 Walking/Falling/Catch 状态；
- `HasFrontLedge/HasBackLedge/HasBackFloor` 查询；
- Catch_Hurdle low/med/high 接入；
- 验收：空中抓边/落顶场景；高层不再走 ground 2/3 动作。

阶段 D 中间状态（2026-10-21，大部分落地）：`MovementMode { Walking, Falling, Catch }`、
Falling 入口、Hurdle/Mantle/Vault 家族 Chooser（按顶面纵深 2/3 个胶囊直径）、
`jumpInitiated` gating（`CatchOnFall`/`CatchOnWalkOff`）、入口时间与 GASP 关卡视觉验收
已实现。入口时间取 Montage 第一个 `PoseSearchBranchIn` 的 Time，即 UE 采样窗口下界；
Catch 窗口≈1 帧，与 UE 选帧差 ≤1 帧。原生 PoseSearch 选帧未做：Catch 无收益，
非 Catch 家族（窗口 2–31 帧）的 B1 待做；更细的收口字段与真实 gameplay 复杂接触
仍未接。详见 [P24 Catch 运行时记录](../../rationale/gasp-p24-catch-runtime-record.md)、
[P25 Catch 入口记录](../../rationale/gasp-p25-catch-entry-record.md) 与
[P26 家族/研究记录](../../rationale/gasp-p26-chooser-families-record.md)。

### 5. CMC Root Motion

- 根 delta/dt 覆盖 velocity、下落 Z 约束、根旋转后置；
- 与 Montage root motion/warping 合流，不能双消费；
- 验收：Climb + 1 m Mantle/Vault 的轨迹、碰撞、落地；
- 30/60/120 Hz 与包内运行。

阶段 E 中间状态：Climb/Hurdle/Mantle/Vault 都已消费 C# 转交的 CMC 根运动
（`ConsumeRootMotion` → `source/script/scriptPhysics.cpp` 的 `CharacterVirtual`
速度），Mantle/Vault 的 `Jump`/分段 `Move` 已删除；落地收口与
`ClearRootMotion` 是 montage blend-out 的替代。30/60/120 Hz 场景矩阵：仓库内
21 场景 × 3 档 = 62/63（30 Hz `catch-family-mantle` 2/2 `catch-missed-top`）；
独立包同一二进制 60/120 全过、30 Hz 首跑 20/21 + 重试 1 次后 21/21（首跑
`catch-family-hurdle` `stalled`），且包内 30 Hz Mantle 首跑通过、未复现仓库内失败。
30 Hz catch 入口/warp 窗口仍 flaky，门禁未全绿。详见
[P27 记录](../../rationale/gasp-p27-visual-frequency-record.md)。

P28（引擎 `dd62695`）已把脚本与角色的时间轴改成每帧变 dt：`Tick` 每帧一次
`tick(dt=frameDt*timeScale)` → `StepPhysics(dt)` → `late(dt)` →
`TickScriptAnimations(dt)` → `StepPhysics(0)`，只有刚体求解保留 `1/60` 子步；
0.1 s `simulationDelta` 只做 hitch 防护。P28 只重跑 6 场景子集：**30 Hz 5/6**
（仅 `catch-family-mantle` 2/2 `catch-missed-top`）、**60/120 Hz 6/6**；
`catch-high` 经资源 `b75b2de` 重标定 probe y=260 后三档全过。**30 Hz Mantle
catch 未闭**（变帧 dt + clamp 未修复 warp 窗口），且上面的 P27 21 场景矩阵是旧
固定步语义，不能称为 P28 已重验。详见
[P28 帧语义记录](../../rationale/gasp-p28-frame-semantics-record.md)。

## 阶段门禁

| 阶段 | 交付 | 门禁 |
| --- | --- | --- |
| A | Montage 数据/运行时 | 1 m Mantle section/notify 场景 |
| B | MotionWarping SkewWarp | 1 m Vault/Mantle 落点 |
| C | Chooser/PoseSearch | 选择表对照 + 高层 Catch 选择 |
| D | Catch/MovementMode | 空中 Catch 场景 |
| E | CMC 合流 | 根运动/旋转/频率矩阵。P28 已改每帧变 dt：6 场景子集 30 Hz 5/6、60/120 Hz 6/6；30 Hz Mantle catch 未闭；P27 21 场景矩阵为旧固定步语义，不是 P28 重验 |

## 当前差距

- A Montage：已接入 UE notify 真实时间、NotifyState 区间与 section/link（P22/P23）；
  剩余非 `Linear` easing、Montage 混合与 section 内 rate/offset。
- B 原生 warp：`MotionWarpRuntime` 已接 Hurdle/Climb/Mantle/Vault，四个动作都消费
  warped `RootMotion`；Hurdle 已按 UE 四窗口 + `WarpPointAnimProvider=Bone` 接入，
  Mantle/Vault 两个 FrontLedge 窗口（Vault stand 另有 BackLedge）均 `attach`；
  曲线 `dOut`、旋转 warp 与 reach 场景已落地（P21–P23）。剩余：非 `Slerp` 旋转方法/
  旋转参数、pitch/roll、第二窗参考发散。
- C Chooser/PoseSearch：C# 镜像 `CHT_TraversalMontages_CMC` 行并扩展为纵深家族；
  PoseSearch 选帧未接（入口取 branch-in 窗口下界；Catch 差 ≤1 帧，非 Catch B1 待做）。
- D Catch/MovementMode：Hurdle/Mantle/Vault 家族 Chooser、跳跃/走落 gating、入口时间与
  GASP 关卡视觉验收已落地；原生 PoseSearch 选帧（非 Catch B1）、更细收口字段与复杂
  gameplay 接触未接；包内矩阵 60/120 全过、30 Hz 重试后 21/21（catch 仍 flaky，见 P27）。
- E CMC 根运动：Climb/Hurdle/Mantle/Vault 都已消费 CMC 根运动。P28（`dd62695`）
  已改每帧变 dt：`Tick` → `StepPhysics(dt)` → `LateTick` → 动画 → `StepPhysics(0)`，
  只有刚体保留 1/60 子步。P28 只重跑 6 场景子集：30 Hz 5/6（仅
  `catch-family-mantle` 2/2 `catch-missed-top`）、60/120 Hz 6/6；**30 Hz Mantle
  catch 未闭**。P27 的仓库内 62/63 与包内结果都是旧固定步语义，不能称为 P28
  已重验。详见 [P28 记录](../../rationale/gasp-p28-frame-semantics-record.md)。

## 对 UE 资产的实测修正

2026-10-20 用 UE Python 检查 Traversal 的 `AM_*`：

- `AM_M_Neutral_Traversal_Mantle_1_0_stand_F_Lfoot`：1 个 section `Default`；
- `Vault_1_0`：1 个 `Default`；
- `Hurdle_1_0`：1 个 `Default`；
- slot 都是 `DefaultSlot`，长度 2.0/2.2/1.667 s；
- `AnimMontage.Notifies` 是 protected，Python 不能直接读；
- 已审计的 ABP 里没有 `MotionWarping` 节点，只有
  `AnimationWarping` 的 Steering/OrientationWarping；
- Traversal 序列的 notify track 是 `Footstep/Handstep/1`，
  曲线包含 `distance_from_ledge`、`contact_l/r`、`disable*pelvis/footik`。

因此“五项”中的 **MotionWarping** 不能假设为 GASP traversal 一定使用
`MotionWarpingComponent`；需要按实际 AnimBP/曲线路径复核，避免把 UE 的
AnimationWarping/ledge-distance 曲线误实现成 MotionWarping。

P17/P19 后续：从 34 个 `AM_*` 的 T3D 导出到
`AnimNotifyState_MotionWarping` / `RootMotionModifier_SkewWarp`
（`WarpTargetName="FrontLedge"`、`WarpPointAnimBoneName="attach"`、
`bIgnoreZAxis=False`），说明 traversal 的 warp 由 Montage notify 驱动；ABP 里没有
`MotionWarping` 节点与这一点不冲突。

## 非目标

- 2/3 m ground Mantle/Vault stub；
- 自研根缩放/Hurdle 2/3 运行时；
- 用 `Catch` 根轨迹反推地面动作。
