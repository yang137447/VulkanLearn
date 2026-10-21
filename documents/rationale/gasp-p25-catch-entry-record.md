# GASP P25 Catch 入口时间、家族与自动触发记录

状态：执行记录，非当前契约。2026-10-21。覆盖 Montage 入口 `startTime`、
Catch Cliff/Mantle/Vault 家族、Falling 自动抓边与新场景验收。行为以
`contracts/animation-montage.md`、`contracts/animation-runtime-skinning.md` 与源码为准。

## 1. 提交

| 仓库 | 提交 | 内容 |
| --- | --- | --- |
| 引擎 | `a97cc58` | 两个 `PlayMontage` 重载新增 `float startTime = 0`；原生视作初始 seek；wire/原生校验；3 项模块测试 |
| 资源 | `e0b7a8d` | `GaspCharacter.blueprint.cs` +298/−60（入口时间、家族、自动触发）；`BP_gasp_traversal_probe.json` 暴露 `CatchOnFall`/`CatchJumpProbe`/`CatchJumpDelay`；新场景 `SC_gasp_catch_jump.json` |

测试：`animation_preflight_tests` **255/255**（P24 为 252），新增 3 项：
`PlayMontageStartTimeSkipsEarlierNotifiesAndRootMotion`、
`PlayMontageRejectsStartTimeOutsideSectionZero`、
`MontageSectionChainStartsFromEntryTime`。`script_tests` startTime 用例 3/3：
`ScriptAnimationWorld.PlayMontageStartTimeAppliesEntryOffsetAndSkipsEarlierNotifies`、
`ScriptAnimationWorld.PlayMontageStartTimeWireRejectsInvalidValues`、
`ScriptAnimationTest.PlayMontageStartTimeOverloadSkipsPreamble`。
`scriptAnimationTests` 注册 47（`ScriptAnimationTest.*` 36 + `ScriptAnimationWorld.*` 11）。

## 2. 入口 startTime

### 2.1 API 与语义

```cpp
PlayMontage(player, montage, targets, loop, rate, inPlace, float startTime = 0)
```

- 秒、clip-local；`0` = 从 section 0 起点播放，旧调用逐字兼容。
- 正值必须落在 section 0 的可播放范围
  `[firstSection.startTime, min(firstSection.endTime, clipDuration) - 1e-3]`；
  越界/剩余不足 1 ms 拒绝，诊断带 montage id、section 与范围。非有限/负数拒绝。
- **一次初始 seek**：entryTime 之前的 notify 不补发；根位移/根 yaw 只从 entryTime
  起积分；窗口用 `previousPosition=entryTime` 门控，结束早于 entryTime 的窗口不再
  激活；包含入口的窗口从 entryTime 算 total。
- 多 section 链只截短第一段，后续段仍按 `nextSection`/`linkMethod` 推进。
- wire 层：`startTime` 不是数字、非有限或负数 → `Script.InvalidMontageStartTime`；
  C# 只做有限/非负校验（`ArgumentOutOfRangeException`），不复制资产范围判断。
- 失败保持当前播放器不变（候选副本提交）。

### 2.2 测试证据

- `PlayMontageStartTimeSkipsEarlierNotifiesAndRootMotion`：入口前的 notify 0 条、
  根位移从入口起、窗口按入口门控。
- `PlayMontageRejectsStartTimeOutsideSectionZero`：越界值逐一拒绝，播放器/姿态/
  Montage id 不变。
- `MontageSectionChainStartsFromEntryTime`：多段链从入口开始，入口前事件不补发。

## 3. 资源侧

### 3.1 入口时间公式

`StartTraversal` 在 `Montage(id).Notifies` 中找**第一个** `PoseSearchBranchIn`：

```text
startTime = first(PoseSearchBranchIn).Time
entry     = "PoseSearchBranchIn"
无该 notify: startTime = 0, entry = "none"
startTime >= montage.Length - 0.001: entry = "invalid", startTime = 0
```

入口只对 Catch 计算；非 Catch 保持 0。日志 `P8_MONTAGE_START` 只在 Catch 行追加
`startTime=... entry=...`，非 Catch 文本逐字不变。

### 3.2 家族映射

- `SelectCatchClip`：`M_Neutral_Traversal_<action>`（Vault 无 `_stand`，其余带
  `_stand`）。
- `TryResolveCatchMontage`：先试 `AM_<clip>`，再试 `AM_<clip>F`（Cliff 资产多一个 F）；
  两者都不存在时拒绝并输出 `tried=AM_...,AM_...F`，不猜其它命名。
- 已验收：Cliff high（`…standF`）、Mantle med（`…stand`）、Vault high（无 `_stand`）。
- `ChooseTraversalAction` 仍只返回 Hurdle low/med/high 与 Cliff high；Mantle/Vault
  需显式 `TraversalAction=Catch_Mantle_*`/`Catch_Vault_*`（Falling 入口允许）。

### 3.3 自动触发

```text
eligible = CatchOnFall && TraversalEnabled && !TraversalProbe && !traversing &&
           !traversalPending && MovementMode==Falling && LinearVelocity.Y <= 0.1
cooldown = 0.15 s（不满足时归零）
```

- `CatchOnFall` 默认 true；只在非探针（`!TraversalProbe`）生效，保证既有探针日志
  序列不变。
- 触发后临时把 `TraversalAction` 置 `Auto` 并抑制探测日志，成功进入 Catch 时写一条
  `P8_CATCH_AUTO_TRIGGER mode=... height=... action=... pos=... vy=...`。
- 是“任意 Falling + 前方有可抓边”，不区分由 Jump 还是走落导致；这是 gameplay 语义
  选择。
- `CatchJumpProbe` 只产生移动/跳跃输入，不调用 `RequestTraversal`；`CatchJumpDelay`
  默认 0.2 s，用于 `SC_gasp_catch_jump.json`。

## 4. 验收

### 4.1 入口时间（`SC_gasp_catch_high`）

3 次 + final：`P8_MONTAGE_START … startTime=0.877 entry=PoseSearchBranchIn`；
首条 `P8_TRAVERSAL time=1.12`（旧 0.24，0.877 s 前摇消失）；
`P8_CATCH_LANDED` 2.082/2.198/2.131 y=290；END y=290。
父代理独立复核：`startTime=0.877`、`CATCH_LANDED 2.064/3.456 y=290`、END y=290。

### 4.2 自动抓边（`SC_gasp_catch_jump`）

场景：地面 y=0、2 m 墙（顶面 200、前沿 -140）；角色 `(0,90,120)` 以 450 cm/s 助跑，
0.2 s 按一次 Jump；起点距墙 2.6 m，无硬编码落点。

| 次 | `P8_CATCH_AUTO_TRIGGER` | 入口 | 落地 | END |
| --- | --- | --- | --- | --- |
| 01 | `height=77 action=Catch_Hurdle_med pos=0,213,-110 vy=-7` | 0.834 | 1.702 y=290 z=-182 | y=290 z=-182 |
| 02 | 同上 | 0.834 | 1.759 y=290 z=-213 | y=290 z=-213 |
| 03 | 同上 | 0.834 | 1.702 y=290 z=-180 | y=290 z=-180 |
| final | 同上 | 0.834 | 1.702 y=290 z=-158 | y=290 z=-158 |

每次恰好 1 条自动触发；`Walking→Falling→Catch→Walking` 完整，无 REJECT/ABORT；
日志中没有 `P8_TRAVERSAL_REQUEST`（自动路径不调用它）。
父代理独立复核：`height=77`、`CATCH_LANDED 1.723/3.166 y=290`、END y=290。

### 4.3 家族（各 3 次）

| 家族 | 场景 | clip/montage | 入口 | 落地/END |
| --- | --- | --- | --- | --- |
| Cliff | 2 m 墙、y=210、显式 `Catch_Cliff_high` | `…Cliff_high_stand` → `AM_…standF` | 0.905 | 288/288/288 → y=290；END z=-182/-188/-187 |
| Mantle | 1.11 m 墙、y=210、显式 `Catch_Mantle_med` | `…Mantle_med_stand` → `AM_…stand` | 0.839 | 199/199/201 → y=201；END z=-174/-169/-176 |
| Vault | 3 m 深台面、y=290、显式 `Catch_Vault_high` | `…Vault_high`（无 `_stand`）→ `AM_…Vault_high` | 0.853 | 289/290/290 → y=290；END z=-322/-341/-331 |

三次均无 ABORT，`P8_CATCH_TARGET target=FrontLedge … source=montage`、
`P8_WARP_WINDOWS` 只有一个 FrontLedge 窗口、`P8_WARP_ROTATION … source=front-normal`。

### 4.4 回归

| 场景 | 结果 |
| --- | --- |
| `SC_gasp_traversal` | Mantle END `-0,190,-174` |
| `SC_gasp_traversal_auto` | `CHOOSER height=100 Hurdle` → END y=90 z=-354 |
| `SC_gasp_traversal_auto_high` | `Catch_Hurdle_high` → 仍 `catch-requires-falling` |
| `SC_gasp_traversal_reject_high` | `height=160` |
| `SC_gasp_traversal_abort_void` | `airborne`（y=-3040，无地面场景） |
| `SC_gasp_traversal_abort_stall` | `stalled progress=10` |
| `SC_gasp_traversal_input` | Interact → Mantle END y=190 z=-169 |
| `SC_gasp_playground` | 无 P8/Catch 日志 |
| 临时 `jump_no_wall` | 普通跳跃 Walking→Falling→Walking，无 Catch |
| 临时 `catch_off` | `CatchOnFall=false` 不触发 |
| 临时 `unknown_catch` | `catch-montage-missing … tried=AM_…,AM_…F` |

全部 `exit=0`、stderr 0 B、config 恢复、无残留进程。

### 4.5 截图

`artifacts/gasp-catch-entry/catch-jump-landed.png`（1280×720，59 KB；
`catch-jump-landed.bmp` 原图）。由 `capture-landing.ps1` 在 `P8_CATCH_LANDED` 后
0.25 s 注入 `screenshot` 捕获；`screenshotSaved=true`、config 恢复、无残留。场景墙/地面
是 physics-only blueprint（没有 visual mesh），截图只证明角色落地姿态，不能作为墙体
几何或像素级验证。

## 5. 启发式与已知行为

1. **入口时间来自 notify 名**：第一个 `PoseSearchBranchIn` 的 Time；原 notifyState 的
   Duration 不参与。没有该 notify 则 0；越界回退 0 并标 `invalid`。这不是原生
   PoseSearch branch-in，只是资产时间锚点。
2. **单 FrontLedge 家族收口是 0.5 s 根运动启发式**（非资产字段）：Cliff/Mantle/Vault
   在第一次触顶后继续消费 0.5 s 根运动再收口；Vault 的越障根运动会带过浅台面，验收
   使用 3 m 深台面。
3. **Hurdle BackFloor 后竖直钳制**：窗口结束后若角色仍高于顶面，根竖直速度钳到
   −200 cm/s，防止残余上冲（实测可到 y=500+）；水平根运动保留。
4. **自动触发不分 Jump/走落**：任意 Falling + 前方可抓边；0.15 s 冷却；
   `!TraversalProbe` 门控；`CatchOnFall` 默认 true。
5. **Chooser 未接 Mantle/Vault**：`ChooseTraversalAction` 仍只选 Hurdle/Cliff；
   Mantle/Vault 需显式 Catch action。
6. **`P8_CATCH_STATE`** 只出现在自动路径/非 Hurdle 家族；Hurdle 探针序列保持原样。
7. **BackFloor 顶面口径**仍是 P24 的有意偏离，不宣称 UE 等价。

## 6. 未闭环

1. 原生 PoseSearch branch-in 未实现；入口时间依赖资产 notify 名与 Time，换资产/换
   section 需要重新确认。
2. Chooser 不选 Mantle/Vault Catch；需要另开任务改 Chooser 行并回归 `auto_high`。
3. 0.5 s 收口启发式没有资产字段支撑；若未来资产提供 `distance_from_ledge`/BackFloor
   窗口，应改为按窗口/曲线收口。
4. 自动触发只验证新场景与临时回归；真实 gameplay 的复杂接触（窄墙、斜墙、移动平台）
   未测。
5. 截图无墙体视觉网格；没有像素级/姿态对比。
6. 没有 30/60/120 Hz 或包内 Catch 矩阵。

## 7. 证据

```text
artifacts/gasp-catch-entry/
  RESULTS.md, GaspCharacter.blueprint.cs.diff,
  BP_gasp_traversal_probe.json.diff, SC_gasp_catch_jump.json,
  catch-high-01..03 + final-high, catch-jump-01..03 + final-jump,
  family-{cliff,mantle,vault}-01..03,
  regress-*.{log,err.log,json},
  capture-jump-landing.{bmp,png,json,log},
  capture-landing.ps1, run-scene.ps1,
  scenes/（临时家族场景副本）, dev/, sha256.txt, config.json.orig
```
