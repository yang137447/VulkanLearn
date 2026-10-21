# GASP P15 MotionWarping 核心数学记录

状态：阶段 B 中间记录，非当前契约。实现并测试了根方向 SkewWarp 的核心
平移缩放；尚未接 Climb/Mantle 运行时，也不是完整 UE SkewWarp。

## UE 参照

`RootMotionModifier_SkewWarp::WarpTranslation`：

- 在根运动同步空间里做 scale + shear；
- 沿根运动方向缩放，保留垂直分量；
- 目标位置通过 warp window 传入。

## VL 实现

新增：

```text
source/animation/motionWarping.h
source/animation/motionWarping.cpp
tool/animation-tests/motionWarpingTests.cpp
```

函数：

```text
WarpTranslationToTarget(currentLocation, currentRotation, delta, total, targetLocation)
```

当前算法（按 UE `RootMotionModifier_SkewWarp.cpp` 移植）：

1. 构造根同步空间（X=根方向，Z 由当前旋转轴选择）；
2. 把 `delta`、目标偏移、整段根位移转入同步空间；
3. 计算目标/根的 Z、Y 平面夹角；
4. `projectedScale = dot(worldSync, rootNorm) / |remainingRoot|`；
5. 构造 `scale * shearXY * shearXZ`；
6. 变换当前 delta 回世界空间。

UE 的 `SetAxis(0, ...)` 是写矩阵列 0，shear 放在第 0 列；这是之前横向
shear 失败的根因，已修正。

`SkeletalPlayback::RootMotionTotal()` 已通过 `rootMotionTotal` 反馈暴露到
C# `AnimationState.RootMotionTotal`，供下一步 warp window 使用。

## 运行时接入尝试

把 exact warp 放进 Climb 的 20%–80% 窗口后：

```text
Climb stand: ABORT airborne pos=0,323,-110
Climb run:   ABORT airborne pos=0,327,-92
Climb walk:  END 0,340,-208（walk 未走 warp 分支）
```

回退后 Climb stand 恢复 `END 0,340,-173`。说明只替换 root delta 不够；
warp target 需要 UE 的 target/window/root motion 空间，且 Climb 的 target
还包含 Montage/CMC 的落点语义。该集成已回退，没有留在主分支。

## WarpTarget/window 数据

现在 Climb 会建立独立 warp target/window，但**不修改根运动**：

```text
P8_WARP_WINDOW target=0,340,-200 start=0.67 end=2.67
P8_TRAVERSAL_END pos=0,340,-169 grounded=True
```

target 是 Montage 目标点（顶面 + 前向 50），窗口是动作 20%–80%。
这一步只固定数据边界，等 CMC/root motion 消费顺序确定后再应用 exact warp。

## 水平 warp 尝试

只消费 warp 的 X/Z、保留根竖直：

```text
Climb stand: END 0,339,-135（z 短）
Climb run:   ABORT airborne pos=0,332,-104
Climb walk:  END 0,340,-204
```

仍不稳定，已回退；主分支只保留 target/window 日志。

## native CMC 消费尝试

把 warp 移到 `scriptPhysics.cpp` 的 rootMotion 速度写入
`CharacterVirtual` 之前，C# 只传 target/total/alpha：

```text
Climb stand: ABORT airborne pos=0,327,-110
Climb run:   ABORT airborne pos=0,331,-72
Climb walk:  END 0,340,-192（未走 warp）
```

native 消费点仍失败，已整体回退。说明问题在 **warp target 的空间定义**，
不在 C#/native 消费层；当前 `target=0,340,-200` 是障碍顶面+前向偏移，
不是 UE WarpTarget 的 root-motion 目标空间。

## FrontLedge target 尝试

按 UE 的 `FrontLedgeLocation` 改 target 为“碰撞面点 + 顶面高度”：

```text
P8_WARP_WINDOW target=0,250,-120
Climb stand: airborne pos=0,330,-85
Climb run:   airborne pos=0,330,-99
```

仍失败，已回退。`topHit.Point` 的 Y=250 并不是 UE 的 FrontLedgeLocation，
说明我们的 ledge 查询/高度基准也不对。

## 测试

`animation_preflight_tests --gtest_filter=MotionWarping.*`：

```text
IdentityTargetKeepsDelta          PASS
DoubleTargetScalesAlongRoot       PASS
PerpendicularDeltaIsPreserved     PASS
ZeroDeltaOrTotalReturnsZero       PASS
LateralTargetAddsShear            PASS
```

5/5 通过。

## 限制

- 尚未接 traversal 运行时；`RootMotionTotal` 已通过反馈暴露，但还没有
  在 C# 中调用 exact warp；
- 未实现旋转 warp、warp window、NotifyState；
- 未验证与 Montage NotifyState 的时间窗口绑定；
- 未做 30/60/120 Hz 或包内验证；
- `currentRotation` 参数保留给后续 UE 同步空间实现，当前未使用。

## 下一步

1. 把函数接到 Climb/1 m Mantle 的根运动消费；
2. 增加 WarpTarget/window 数据；
3. 补 UE 同步空间 shear 与旋转；
4. 包内场景验证落点。

## 证据

- `artifacts/gasp-warp-test-build.log`
- `animation_preflight_tests --gtest_filter=MotionWarping.*` 输出
