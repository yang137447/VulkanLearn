# GASP P17 UE Montage 时长与 MotionWarping 窗口记录

状态：执行记录，非当前契约。从 UE Montage T3D 导出并修正 Climb 的时长/
窗口；当前只修时长，未应用 warp。

## UE 导出

对 `/Game/Characters/UEFN_Mannequin/Animations/Traversal` 下 34 个
`AM_M_Neutral_Traversal_*` 导出 T3D，解析 `SequenceLength` 与
`Notifies` 中的 `MotionWarping`：

```text
Climb stand Lfoot: length 2.750, warp 0.405359 → 0.732001, FrontLedge, bone=attach
Climb run   Lfoot: length 3.250, warp 0.366415 → 0.884772, FrontLedge, bone=attach
Climb walk  Lfoot: length 3.600, warp 0.309711 → 1.293677, FrontLedge, bone=attach
```

Montage 里的 Notify：

```text
AnimNotifyState_MotionWarping
  RootMotionModifier_SkewWarp
    WarpTargetName="FrontLedge"
    WarpPointAnimProvider=Bone
    WarpPointAnimBoneName="attach"
    bIgnoreZAxis=False
    WarpRotationTimeMultiplier=0.8
```

## VL 修正

`GaspCharacter` 原先使用：

```text
Climb stand 3.333, run 4.000, walk 4.233
window = 20%–80%
```

改为 UE Montage 时长/窗口：

```text
stand 2.750, warp 0.405–0.732
run   3.250, warp 0.366–0.885
walk  3.600, warp 0.310–1.294
```

这一步只改时长/窗口日志，没有把 warp 应用到根运动。

## 运行结果

```text
stand: P8_WARP_WINDOW target=0,340,-200 start=0.41 end=0.73
       END 0,340,-178 grounded=True
run:   P8_WARP_WINDOW start=0.37 end=0.88
       END 0,340,-139 grounded=True
walk:  P8_WARP_WINDOW start=0.31 end=1.29
       END 0,340,-198 grounded=True
```

三个 gait 都恢复 grounded，说明之前的 3.33/4.0/4.23 时长本身就和 UE
Montage 不一致。

## 限制

- target 仍是 `top+forward` 日志值，不是 UE `FrontLedgeLocation`；
- warp 未应用；
- 其余动作（Mantle/Vault/Hurdle）窗口已导出，但尚未逐个接入；
- 这是时长/窗口修正，不是 warp 运行时完成。

## 证据

- `artifacts/gasp-warp-audit/montages/*.copy`（34 个 Montage）
- `artifacts/parse-montages.py`
- `artifacts/gasp-uetiming-stand/run/walk.log`
