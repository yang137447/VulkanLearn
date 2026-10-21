# GASP P16 Traversal Chooser 脚手架记录

状态：阶段 C 中间记录，非当前契约。实现了 UE Chooser 行的 C# 镜像和
`TraversalAction=Auto`，但运行时 Hurdle/Catch 尚未闭环。

## UE 行

来自 `CHT_TraversalMontages_CMC`：

```text
Hurdle_1_0       ObstacleHeight <=125, Walking
Catch_Hurdle_low ObstacleHeight <=50
Catch_Hurdle_med 50–100
Catch_Hurdle_high 100–200
```

## VL 实现

`GaspCharacter` 增加：

```text
TraversalAction = "Auto"
P8_CHOOSER height=<cm> mode=Walking/Falling choice=<Hurdle/Catch_*>
P8_CHOOSER_REJECT choice=Catch_* reason=catch-runtime-not-implemented
```

选择函数：

```text
!falling && height<=125 -> Hurdle
height<=50 -> Catch_Hurdle_low
<=100 -> Catch_Hurdle_med
<=200 -> Catch_Hurdle_high
else Catch_Cliff_high
```

场景：

```text
SC_gasp_traversal_auto.json       1 m
SC_gasp_traversal_auto_high.json  2 m
```

## 运行结果

低障碍：

```text
P8_CHOOSER height=100 mode=Walking choice=Hurdle
P8_TRAVERSAL_END pos=0,190,-234 grounded=True
```

Chooser 选择正确，Hurdle 1 m 已完成落顶（y=190）。

高障碍：

- Auto 先低球、再局部 `y=120` 高球；2 m 障碍命中；
- `height=200` 选择 `Catch_Hurdle_high`，随后记录
  `catch-runtime-not-implemented`。

## 限制

- 只有 C# 表，不是 UE Chooser 资产；
- 没有 PoseSearch 入口帧选择；
- 没有 Catch 运行时，Catch 选择只记录拒绝；
- Hurdle 1 m 已通过；2 m 只到 Catch 选择；
- 高障碍仍是两档球查询，不是 UE 的 front/back ledge context。

## 证据

- `artifacts/gasp-auto-low.log/.err`
- `artifacts/gasp-auto-high.log/.err`
