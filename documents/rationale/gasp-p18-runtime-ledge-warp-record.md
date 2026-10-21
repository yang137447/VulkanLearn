# GASP P18 运行时 Ledge Context + Warp 记录

状态：执行记录，非当前契约。FrontLedgeLocation 改为运行时计算，Climb 三个
gait 已带 warp 通过；warp 使用 remaining root 的 exact 同步空间
scale+shear。

## 运行时 context

`GaspCharacter` 在 traversal 命中后计算：

```text
front = (approach.Point.X, topHit.Point.Y, approach.Point.Z)
```

Climb 2.5 m 实测：

```text
P8_LEDGE_CONTEXT front=0,250,-120 backFloor=0,-0,-420 depth=300 height=250
```

`front` 是前沿顶边；角色原点到脚底还差 90 cm，所以 warp target 用：

```text
warpTarget = (front.X, top.Y + 90, front.Z) = 0,340,-120
```

`backFloor/depth` 目前用固定 300 cm 后探，结果 depth=300 不代表真实障碍
深度；BackLedge/BackFloor 还未用于 warp。

## Timing

按 P17 的 UE Montage：

```text
stand 2.750, warp 0.405–0.732
run   3.250, warp 0.366–0.885
walk  3.600, warp 0.310–1.294
```

## 结果

```text
stand: P8_WARP_WINDOW target=0,340,-120 start=0.41 end=0.73
       END 0,340,-166 grounded=True
run:   P8_WARP_WINDOW start=0.37 end=0.88
       END 0,340,-147 grounded=True
walk:  P8_WARP_WINDOW start=0.31 end=1.29
       END 0,340,-189 grounded=True
```

对比无 warp 基线：

```text
stand -178, run -139, walk -198
```

三个 gait 都保持 grounded；warp 已经改变水平落点但未破坏 Climb。

## exact warp 尝试

把 native 的同步空间 `scale+shear` 同样移植到 C# 后：

```text
stand: ABORT airborne pos=0,332,-122
run:   END 0,338,-133 grounded=True
walk:  END 0,340,-191 grounded=True
```

stand 失败，已回退到根方向 scale；exact 数学仍保留在 native 测试中。

改用 `RootMotionRemaining` 后，exact 同步空间 warp 已通过：

```text
Climb stand: END 0,340,-171 grounded=True
Climb run:   END 0,340,-174 grounded=True
Climb walk:  END 0,340,-196 grounded=True
Mantle:      END 0,190,-202 grounded=True
Vault:       END 0,90,-290 grounded=True
```

根因是 exact warp 需要“当前帧到片段末尾”的 remaining root，而不是整段 total。

## Remaining root

warp 计算改用 `RootMotionRemaining`（当前时间到片段末尾）而不是整段
`RootMotionTotal`。三个 gait 结果：

```text
stand: END 0,340,-174 grounded=True
run:   END 0,340,-198 grounded=True
walk:  END 0,340,-201 grounded=True
```

`RootMotionRemaining` 由 native feedback `rootMotionRemaining` 提供。

## UE 前探距离

`SandboxCharacter_CMC.GetTraversalCheckInputs` 里有：

```text
MapRangeClamped(Speed, 0, 500, 75, 350)
```

即前探距离 75–350 cm。VL 已改为：

```text
reach = clamp(75 + speed/500 * 275, 75, 350)
```

Climb/Auto 回归：

```text
Climb: END 0,340,-153 grounded=True
Auto:  END 0,190,-212 grounded=True
```

## BackLedge/BackFloor

用 16 个向下探针（深度 40–340 cm，步长 20）在 traversal 开始前批量查询：

```text
P8_LEDGE_CONTEXT front=0,250,-120 backLedge=0,250,-260
                 backFloor=0,0,-280 depth=140 backHeight=250
```

对应 `S_TraversalCheckResult`：

```text
HasFrontLedge    = true
FrontLedgeLocation = 0,250,-120
HasBackLedge     = true
BackLedgeLocation = 0,250,-260
HasBackFloor     = true
BackFloorLocation = 0,0,-280
ObstacleDepth    = 140
BackLedgeHeight  = 250
```

障碍盒半深 60，前沿 -120 到后沿 -260 正好 140 cm，和场景一致。

## 正式 Context

新增 C# `TraversalContext`，字段对齐 `S_TraversalCheckResult`：

```text
HasFrontLedge / FrontLedgeLocation / FrontLedgeNormal
HasBackLedge / BackLedgeLocation / BackLedgeNormal
HasBackFloor / BackFloorLocation
ObstacleHeight / ObstacleDepth / BackLedgeHeight
```

warp target 从 context 取 FrontLedgeLocation。

## Mantle/Vault

窗口按 UE Montage：

```text
Mantle stand 0.224–0.699, run 0.099–0.667, walk 0.123–0.933
Vault  stand 0.132–0.603, run 0.064–0.900, walk 0.167–1.067
```

Vault 时长也改回 UE：

```text
stand 2.2, run 2.133, walk 2.933
```

回归：

```text
Mantle stand/run/walk: 0,190,-202 / -201 / -201
Vault  stand/run/walk: 0,90,-290 / -290 / -290
Hurdle:                END 0,190,-246（不接 warp）
```

Mantle/Vault 全 gait 都 grounded，落点无回归；Hurdle 保持独立。

## Hurdle 多目标尝试

按 UE Hurdle 的 4 个窗口切 FrontLedge → BackFloor → BackLedge →
FrontLedge 目标：

```text
P8_LEDGE_CONTEXT front=0,100,-120 backLedge=0,100,-260 backFloor=0,0,-280
ABORT airborne progress=134 pos=0,305,-214
```

Hurdle 被抬到 y=305 后 airborne，已回退；Hurdle 仍不接 warp。

进一步查 UE `SetWarpTargets`：BackFloor 的 TargetLocation 不是直接取
`BackFloorLocation`，而是：

```text
MakeVector(
  X = PromotableOperator_5,
  Y = PromotableOperator_5,
  Z = CallFunction_18)
```

FrontLedge 才是 `FrontLedgeLocation + (0,0,0.5)`。所以 Hurdle 的
BackFloor/BackLedge 目标需要继续拆 Blueprint 运算链，不能直接拿
context 位置替换。

子代理已解出 UE 链：

```text
BackLedge = BackLedgeLocation（无 offset，rotation 未接）

BackFloor = MakeVector(
  X = BackLedgeLocation.X + BackLedgeNormal.X * |dBackLedge - dBackFloor|,
  Y = BackLedgeLocation.Y + BackLedgeNormal.Y * |dBackLedge - dBackFloor|,
  Z = BackFloorLocation.Z)
```

其中 `d*` 是动画曲线 `Distance_From_Ledge` 在各自 warp window EndTime 的
采样值；链里没有 capsule half height。

这说明 VL 之前把 `BackFloorLocation` 直接当目标、并切换
Front→BackFloor→BackLedge 是不对的：UE 是多个 warp window 各自目标，
且 BackFloor 的 X/Y 沿 BackLedgeNormal 偏移。

按公式实现多 window（同时激活、顺序叠加）后：

```text
Hurdle: END 0,180,-120 grounded=True
```

角色停在障碍前沿，没有越过；说明顺序叠加也不等价于 UE 的多个
RootMotionModifier。已回退，Hurdle 仍不接 warp。

## 原生 Modifier 移植尝试

子代理把 `MotionWarpRuntime`/`MotionWarpModifier` 移植到 C++，C# 只提交
target/window；Hurdle 只登记三个窗口（去掉第二个 FrontLedge）。结果：

```text
P8_WARP_WINDOW target=0,190,-120 start=0.16 end=0.83
native=BackLedge:active,FrontLedge:waiting,BackFloor:waiting
Hurdle: END 0,90,-110 grounded=True
```

角色落到前沿地面，仍没有越过；已整体回退。C++ 边界是对的，但原生的
target 变换/窗口根位移还没有对齐 UE。

改为“每个 active window 独立 warp 后等权平均”：

```text
Hurdle: END 0,189,-137 grounded=True
```

仍然停在障碍前沿，等权混合也不能复现 UE 的 modifier 语义。已回退。

## 批量探针的重复启动修复

批量 BackLedge 探针延迟 `StartTraversal` 后，`traversalPending` 在
`ProcessTraversalApproach` 开头就被清掉，导致探针未完成时下一帧再次发起
traversal；多个 Hurdle 根运动叠加，出现：

```text
Hurdle: ABORT airborne pos=0,362,-284
```

修复：探针期间保持 `traversalPending=true`，只在 `StartTraversal`/拒绝分支
清除。回归：

```text
Hurdle: END 0,190,-220 grounded=True
Mantle: END 0,190,-202 grounded=True
```

## 限制

- warp 已用 exact scale+shear；Hurdle 仍未接；
- BackLedge/BackFloor 已计算，但还没作为 Vault/落地 warp target；
- Mantle/Vault 全 gait 已接；Hurdle 仍未接；
- 没有 30/60/120 Hz 或包内验证。

## 证据

- `artifacts/gasp-contextwarp-stand/run/walk.log`
- `artifacts/gasp-ledge-context.log`
