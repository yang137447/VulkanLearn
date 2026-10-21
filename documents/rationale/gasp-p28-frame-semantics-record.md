# GASP P28 UE 帧语义对齐记录

状态：执行记录，非当前契约。2026-10-21。记录删除脚本固定步、改为 UE CMC 式每帧变 dt
的动机、实现、测试、6 场景帧率矩阵与未闭环项。当前行为以
[C# 蓝图运行时](../contracts/csharp-blueprint-runtime.md)、
[测试与验证](../contracts/testing.md) 与源码为准。

| 仓库 | 提交 | 本轮改动 |
| --- | --- | --- |
| 引擎 | `dd62695`（`【Mod】align gameplay simulation with UE frame semantics`） | 23 个文件，+207/−262：`source/script/`、`source/engine/engineLoop.cpp`、`source/editor/ui/editorLogPanel.cpp`、`script/VulkanLearn.Runtime/`、脚本测试夹具与 C++ 测试 |
| 资源 | `b75b2de`（`【Mod】retime GASP catch probe for per-frame physics`） | `Maps/SC_gasp/SC_gasp_catch_high.json` 1 个文件，+2/−2：probe `position.y` 210 → 260，并改写 description |

运行二进制（两者只差 `engineLoop.cpp` 的一条注释，行为相同）：

| 用途 | 路径 | 大小 | SHA256 |
| --- | --- | --- | --- |
| 30/60/120 Hz 矩阵 | `build-vscode/development/bin/main.exe` | —（未单独记录） | `E7CB1432E4E0251A523DFD31BDAE7C2737FCAE213B0A8FFAD0CE4770029D474E` |
| 最终构建 | 同上，2026-10-21T17:16:42 | 936,011,526 B | `E9B95FC573DF9F89EFFF452EA33FCCA7FB57B9816A6F59B55217B8FD3C57AD60` |

矩阵的所有运行（6 场景 × 3 档 + 30 Hz 一次重试，共 19 次）记录的是 `E7CB…474E`；
`E9B95…AD60` 是 A3 注释修改后的重建。两者源码只差一条注释，哈希差异来自重建，
不代表行为差异。

## 1. 动机

- UE 的 `CharacterMovementComponent` 按帧推进：每个角色每帧消费一个变长 `dt`，在内部以
  `MaxSimulationTimeStep = 0.05` 和 `MaxSimulationIterations = 8` 拆分迭代；它不是
  “全局固定 60 Hz gameplay tick”。本仓库 `MoveCharacter` 的循环直接对应这两个常量。
- GASP 项目的 `DefaultEngine.ini` 只在 `/Script/Engine.PhysicsSettings` 配置
  `bSubstepping=True`、`MaxSubstepDeltaTime=0.016667`、`MaxSubsteps=16`，没有
  `bUseFixedFrameRate`；这是**刚体子步**配置，不是全局 fixed gameplay update。
  证据：`artifacts/gasp-p0/ue-project-20261018/Config/DefaultEngine.ini`。
  因此“脚本回调固定 1/60、渲染帧只做追赶”会把 VL 的语义推离 UE。
- P27 及以前 `ScriptRuntimeSubsystem` 维护 60 Hz accumulator、`FixedTick` 回调与
  `fixedFrameIndex/fixedSteps/droppedSteps/droppedTime`。固定步的时间损失/追赶会改变
  同一场景在不同帧率下的实际下落时间；P28 的 `catch-high` 就是直接受害者：旧场景的
  probe 高度依赖固定步时间损失，改成真实变帧 dt 后角色在 0.5 s 内落满 122 cm，
  chooser 以 `catch-requires-falling` 拒绝。

所以本次不是“把 60 Hz 调成别的数字”，而是删除脚本层固定步，让脚本、角色和动画共享
同一个帧时钟；只有刚体求解保留 1/60 子步，因为那是 Jolt 的积分参数，不是 gameplay 时钟。

## 2. 实现

### 2.1 每帧调用顺序

`ScriptRuntimeSubsystem::Tick` 在 Playing 下每帧执行一次：

1. `tick(dt = frameDt * timeScale)`，交付输入、上一物理帧状态、接触前的一次性命令；
2. `StepPhysics(dt)`：领取 Tick 写入的角色/刚体指令，角色按帧 dt 移动，刚体按内部子步推进；
3. 接触/Trigger 与射线结果交付，`late(dt)` 执行；
4. `TickScriptAnimations(dt)` 在最终物理反馈与 LateTick 决策之后求值；
5. `StepPhysics(0)` 只同步 LateTick 写入的指令/接触并执行已排队查询，不推进时间，
   指令留给下一物理帧。

`time.frame`（`frameIndex`）每帧 +1；没有 accumulator、没有 `fixed` 回调，也没有固定步数/
丢步/丢时间字段。Pause 不回调；TimeScale=0 仍每帧 Tick/LateTick（dt=0），非倍率时钟与
帧号继续，物理和 timer 冻结。

### 2.2 物理唯一入口

`ScriptPhysicsWorld::Step(states, declarations, dt)` 是唯一物理入口，同步执行在 GT：

- 角色：`MoveCharacter(dt)` 每帧一次，内部按 `0.05 s` / 8 次迭代分段，每段重新求重力、
  root/walk 速度并调用 Jolt `CharacterVirtual::ExtendedUpdate`；jump 只在第一个迭代消费。
- 刚体：累计 `dt`，按 `1/60` 子步推进，单帧最多 16 步；未命中上限时保留不足一步的
  分数余量，命中上限时丢弃整步多余量。该 accumulator 只服务刚体积分，不驱动 gameplay。
- `dt=0`：不累计、不移动任何实体；只同步指令与接触/查询，供 LateTick 的零时步边界与
  传送/纯转向使用。
- 接触按稳定 Blueprint ID 去重；物理回传根 TRS、速度与着地状态，托管下一次 Tick 读取。

### 2.3 帧 delta 上限

`EngineLoop` 对游戏与动画使用 `simulationDelta = min(rawDelta, 0.1 s)`，对应 UE
`MaxUndilatedFrameTime`/`MaxSimulationTimeStep` 的 hitch 防护：一次长停顿不会让角色、
动画或脚本推进整段真实时间。FPS 统计、脚本构建轮询与资产变更轮询仍使用原始 `dt`，
因此 0.1 s clamp 不改变这些轮询的节奏。

### 2.4 C# API 与诊断删除

- 删除 `Blueprint.FixedTick` / `Component.FixedTick`。
- 删除 `Time.FixedDeltaTime` / `IsFixedTick` / `FixedFrameIndex`；`FrameIndex` 每帧 +1。
- 删除 NativeBridge 的 `"fixed"` 命令与 FixedTick callback 位；帧回调掩码只剩
  Tick=1、LateTick=2。Tick/LateTick 每帧一次，`dt` 为同一帧时钟。
- `editorLogPanel` 的脚本摘要改为 `Time: frame <frameIndex> | dt … | physics …`，
  不再读取 fixed/dropped 字段。

## 3. 测试

| 检查 | 结果 | 证据 |
| --- | --- | --- |
| `script_tests` | **400/400**，1278.8 s | `artifacts/p28/script-tests.log` |
| `animation_preflight_tests` | **255/255** | `artifacts/p28/animation-preflight-tests.log` |
| main 构建 | exit 0，0 error，0 native warning | `artifacts/p28/build-main-final.log` |

`script_tests` 从 401 变为 400：删除 3 个 fixed-step 用例，新增 2 个 per-frame 用例，
净 −1。夹具侧把 `"fixed"` ABI 调用改为 `"tick"`/`"late"`，阶段循环改为
`{"Tick","LateTick"}`；`scriptTimeTests` 用
`TickAndLateTickRunOncePerFrameWithVariableDeltaTime` 与
`OneFrameVariableDeltaAdvancesRigidBodyByFrameTime` 取代固定 accumulator 用例；
`scriptAnimationTests` 的 33 个 30/60/120 Hz 速率用例未改且全部通过。

最终 main 构建日志只有 CMake 作者/弃用警告，Gameplay 段为
`已成功生成。 0 个警告 0 个错误`；资源侧既有 nullable C# 警告
（`GaspCharacter.blueprint.cs:439`，`CS8602`）出现在更早的
`artifacts/p28/build-main-native.log`，不在最终构建中。原生编译 0 warning，链接成功。

## 4. 6 场景 × 3 档运行时矩阵

只重跑以下 6 个 P28 代表场景，每档 23 s，`--frame-limit 30/60/120`，其余配置不变：

| 场景 | 30 Hz | 60 Hz | 120 Hz |
| --- | --- | --- | --- |
| `SC_gasp_package_probe` | PASS | PASS | PASS |
| `SC_gasp_traversal`（mantle-stand） | PASS | PASS | PASS |
| `SC_gasp_catch_high` | PASS（chooser Falling Hurdle/120，LANDED/END 290） | PASS（同左，height 176） | PASS（同左，height 168） |
| `SC_gasp_catch_jump` | PASS（trigger height 87） | PASS（height 78） | PASS（height 77） |
| `SC_gasp_catch_family_mantle` | **FAIL** 2/2 `catch-missed-top` | PASS | PASS |
| `SC_gasp_catch_family_hurdle` | PASS | PASS | PASS |

汇总：**30 Hz 5/6、60 Hz 6/6、120 Hz 6/6**。全部运行 `exit=0`、stderr 0 B、
`config/config.json` 按字节恢复、无残留 `main.exe`。

### 4.1 catch-high 资源重标定

旧 `catch-high` 的 probe 依赖 P27 固定步的时间损失。P28 改成真实变帧 dt 后，probe 在
0.5 s 时已经在物理上落满 122 cm 并落地，chooser 拒绝 `catch-requires-falling`。
0.1 s clamp 不改变这一点（probe 时间和物理消费同一个 clamp 后的 dt）。经授权的资源修改
`b75b2de` 把 probe `position.y` 210 → 260，并保留试错记录：

- `y=430`：`P8_TRAVERSAL_REJECT no-hit`（probe 高过前向扫球）；
- `y=264`：60/120 通过，30 Hz `ABORT stalled progress=216 pos=-0,249,-296`；
- `y=260`：30/60/120 全过，chooser `mode=Falling family=Hurdle depth=120`、
  `CATCH_LANDED y=290`、`TRAVERSAL_END y=290`。

### 4.2 catch-jump 契约放宽

30/60/120 的触发 height 分别为 87/78/77；P28 把断言放宽为
`family=Hurdle depth=120` + AUTO_TRIGGER 存在 + `CATCH_LANDED`/`TRAVERSAL_END y=290`。
height 只作为记录值，**不再是契约**；`animation_preflight_tests` 与场景资产没有为它增加固定阈值。

### 4.3 30 Hz Mantle 未闭环

`SC_gasp_catch_family_mantle` 在 30 Hz 两次都失败：catch 正确开始
（`family=Mantle depth=160`，FrontLedge 窗口 0.838–1.002），胶囊从 y≈196 只升到 y≈230
就回落，而 warp 目标是 y=290；60/120 Hz 都能到 y=290 并着陆。失败标记为
`P8_TRAVERSAL_ABORT reason=catch-missed-top progress=0`。每帧变 dt + 0.1 s clamp
没有修复这个 warp 窗口/root motion 问题，**30 Hz Mantle catch 是 P28 未闭环项**，
不能按通过记录。同档 `catch-family-hurdle` 三次都通过；P27 环境里的 hurdle 首跑
`stalled` 本轮未复现。

### 4.4 证据范围与边界

- 只重跑 6 场景，不是全场景回归。**P27 的 21 场景 × 3 档矩阵是旧固定步语义，
  不能称为 P28 已重验**；P27 的仓库内 62/63 与包内结果保持其历史口径。
- 矩阵驱动 `artifacts/p28/frequency/run-matrix.ps1` / `run-scene.ps1` 复用 P27 断言，
  按 P28 决定放宽 catch-jump height；逐次 FPS、标记和原因在
  `matrix-30/60/120.txt` 与 `summary-*.json`。
- 为保证窗口不抢焦点，运行器使用 `-WindowStyle Hidden` + `SetWindowPos` 移出屏幕，
  不调用 `SetForegroundWindow`、不截图。30 Hz 有 3 次前台观察、120 Hz 有 80 次
  （其中一次 75/75 轮询均为前台）；这是离屏方式的限制，不作为验证证据。
  最小化窗口会导致 resize 内存失败或提前退出，已记录在
  `artifacts/p28/frequency/minimize-attempt-crash/`，最终矩阵不使用最小化。
- 本次矩阵没有独立包复跑；P28 没有重新导出游戏包。

## 5. 未闭环与与 UE 的剩余差异

- 30 Hz `catch-family-mantle` 的 `catch-missed-top`（warp 窗口/root motion）未修复。
- 动画仍在 LateTick 之后求值：`TickScriptAnimations(dt)` 产生的 root motion 由帧末
  `StepPhysics(0)` 只领取，下一帧才推进角色，因此 root motion 有一帧滞后。本次未改求值顺序。
- 渲染插值未做；渲染帧仍然直接显示最近一次物理/动画求值结果。
- UE 引擎级 `bUseFixedFrameRate` 未使用；P28 走的是 UE CMC 的逐帧变 dt 路径。
- P28 不是“GASP M2 全部完成”：6 场景子集之外没有全矩阵重验，30 Hz catch 仍有一项失败，
  PoseSearch B1、非 `Slerp` 旋转、Montage 质量等 P27 遗留项仍在。

## 6. 提交与证据

- 引擎：`dd62695` `【Mod】align gameplay simulation with UE frame semantics`，
  23 文件 +207/−262。
- 资源：`b75b2de` `【Mod】retime GASP catch probe for per-frame physics`，
  `SC_gasp_catch_high.json` +2/−2。
- 测试：`artifacts/p28/script-tests.log`（400/400）、
  `artifacts/p28/animation-preflight-tests.log`（255/255）、
  `artifacts/p28/build-main-final.log`（0 error / 0 native warning）。
- 矩阵：`artifacts/p28/frequency/matrix-30.txt`、`matrix-60.txt`、`matrix-120.txt`、
  `summary-30.json`、`summary-60.json`、`summary-120.json`、`rate-30|60|120/`。
- 调参：`artifacts/p28/frequency/p28-clamp/`、`catch-high-y264/`、
  `pre-clamp/`（0.1 s clamp 之前的矩阵）。
- 总报告：`artifacts/p28/p28-test-slice-report.md`。

相关历史与计划见 [P27 记录](gasp-p27-visual-frequency-record.md)、
[GASP 与 C# 动画开发计划](../plan/architecture/gasp-csharp-animation-plan.md) 与
[UE 严格复刻计划](../plan/architecture/gasp-ue-strict-replication-plan.md)。
