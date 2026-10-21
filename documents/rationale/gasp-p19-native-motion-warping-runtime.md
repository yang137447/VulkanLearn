# GASP P19 原生 MotionWarping Runtime 记录

状态：执行记录，非当前契约。通用 modifier 队列、窗口根位移提取和目标空间
换算已移到 C++；C# 只提交 target/window。Hurdle、Climb、Mantle、Vault 的
目标与窗口均已由 C# 提交给原生 `MotionWarpRuntime`；Climb 消费原生 warped
root，Mantle/Vault 的位移仍由既有 Move/jump 代码驱动（原生 root 反馈未消费）。

## 原生设计

`source/animation/motionWarping.h/.cpp`：

```text
MotionWarpWindow
  targetName / world location / start / end
  subtractRemainingRootMotion / ignoreVertical

MotionWarpRuntime
  SetWindows
  Apply(clip, previousPosition, currentPosition, modelDelta, meshTransform, actorPosition)
  DebugState
```

`SkeletalPlayback::Tick(delta, meshTransform, actorPosition)` 在模型空间调用
runtime；`SetMotionWarpWindows`/`ClearMotionWarp` 由播放请求提交。

C# `AnimationPlayback.Play(..., warpTargets)` 只组装数据；原生 JSON 请求
提交 `warpTargets`，target 切换、窗口顺序应用与 warp 数学均不在 CLR。
Climb/Mantle/Vault 与 Hurdle 共用 `BuildWarpTargets()` 提交 `MotionWarpTarget[]`，
C# 的 `WarpTranslation`、`traversalWarpStart/End`、`traversalWarpTotal` 与
`warpedAction` 块已删除。

## 目标空间

按 UE `RootMotionModifier_SkewWarp`：

```text
MeshTransform = visual offset * actor transform
TargetModel = inverse(MeshTransform) * TargetWorld
             - RootMotionRemainingAfterNotify
WarpTranslation(Identity, Delta, Total, TargetModel)
FinalRootMotion = Warped + ExtraRootMotion
```

VL 的 target 以胶囊中心表达，Mesh 原点在脚底，runtime 用
`capsuleCenterInModel` 还原偏移，避免重复减 90。

## Hurdle

窗口：

```text
BackLedge 0.133096–0.598017 -> (0,190,-260)
FrontLedge 0.156754–0.234904 -> (0,190,-120)
BackFloor 0.163979–0.398903 -> (0,90,-260)
```

`BackFloor` 的 `Distance_From_Ledge` 曲线还没有，`dOut` 取 0，退化为后沿
正下方；后续只需替换 dOut。

结果：

```text
P8_WARP_WINDOWS native=BackLedge[0.133,0.598]=(0,190,-260)
  FrontLedge[0.157,0.235]=(0,190,-120)
  BackFloor[0.164,0.399]=(0,90,-260)
Hurdle: END 0,188,-266 grounded=True
```

越过障碍并落地。

## Climb / Mantle / Vault

窗口（来自 UE montage 的 MotionWarping notify state，gait 对应片段）：

```text
Climb  stand 0.405359–0.732001, run 0.366415–0.884772, walk 0.309711–1.293677
Mantle stand 0.224297–0.699242, run 0.098873–0.666616, walk 0.122772–0.933426
Vault  stand 0.132285–0.603360, run 0.064038–0.900047, walk 0.166749–1.066703
target = (FrontLedgeLocation.X, traversalTarget.Y, FrontLedgeLocation.Z)
```

Climb 由 C# 消费原生 warped `RootMotion`；Mantle/Vault 的位移仍是既有
`Character.Move` + jump 路径，提交的窗口只进入原生反馈，不驱动胶囊。

## Climb 原生基线

```text
Climb stand: 0,340,-207
Climb run:   0,340,-207
Climb walk:  0,340,-210
```

全部 grounded。旧 C# 基线是 -147 / -147 / -204，差异来自窗口语义而不是
移植误差：

- 旧 C# 用 `RootMotionRemaining`（当前帧到片段末尾）作为 warp 的 total，
  把修正摊到整段动画，落点略微越过前沿；
- UE `RootMotionModifier_SkewWarp` 只在 notify 窗口内 warp，窗口结束后把
  `ExtraRootMotion` 原样加上。窗口结束时角色正好到达 target
  (0,340,-120)，随后剩余根位移把角色带到 -207 左右；
- UE 5.8 源码与本包 montage 导出确认该窗口
  `bSubtractRemainingRootMotion=false`、`bIgnoreZAxis=false`；
- 实测把 `subtractRemaining` 设为 true 会落在 target -120，但该点在实际
  障碍面（-140）前 20 cm，胶囊无法站立，三个 gait 全部 ABORT airborne，
  因此不采用；
- 新落点在最深 140 cm 的平台内约 67–70 cm，比旧 -147（仅进入 7 cm）更稳定。

## 回归

```text
Hurdle:            0,180,-280
Climb stand/run:   0,340,-207 / -207
Climb walk:        0,340,-210
Mantle stand/run/walk: 0,190,-202 / -201 / -201
Vault stand/run/walk:  0,90,-290 / -290 / -290
```

全部 grounded。

## 限制

- Hurdle 只登记三个窗口，未接第二个 FrontLedge（会把角色拉回前沿）；
- BackFloor 的动画距离曲线未接；
- Mantle/Vault 已提交原生窗口，但位移仍由 C# Move/jump 驱动，原生 warped
  root 未被消费；
- 未做 30/60/120 Hz 或包内验证；
- 原生 runtime 的覆盖在 `tool/animation-tests/motionWarpingTests.cpp`
  （窗口门控、窗口 total、subtractRemaining、DebugState）。

## 证据

- `artifacts/nativewarp-p20/final-*.log`（Development 构建，10 个 traversal 场景）
- `artifacts/gasp-nativewarp2-hurdle.log`
- `artifacts/gasp-nativewarp2-climb.log`
- `artifacts/gasp-nativewarp2-mantle.log`
- `artifacts/gasp-nativewarp2-vault.log`
- `artifacts/gasp-nativefinal-*.log`

## 后续裁定（P20，2026-10-20）

`artifacts/nativewarp-p20/final-hurdle.log` 写于 **16:29:35**，资源提交 `91f98e7`
在 **16:37:29**；`0,180,-280` 是迁移前的旧构建结果（旧三窗口/C# 提交配置），
`0,188,-266` 是迁移后 native 结果，二者不是同一构建的抖动。P20 的四窗口/Bone
版本改用新的窗口集合，不再沿用本记录的三窗口数据；完整时间线与新结果见
[P20 记录](gasp-p20-warp-ue-alignment-record.md)。
