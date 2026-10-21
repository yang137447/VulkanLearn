# GASP P24 Catch 资产与运行时记录

状态：执行记录，非当前契约。2026-10-21。覆盖 Catch 资产导出、Catch 运行时
（MovementMode/Falling 入口/顶面落点）与验收。行为以
`contracts/animation-asset-import.md`、`contracts/animation-montage.md`、
`contracts/animation-motion-warping.md` 与源码为准。

## 1. 提交

| 仓库 | 提交 | 内容 |
| --- | --- | --- |
| 资源 | `3574a3a` | 9 个 Catch clip（FBX + sidecar）+ 9 个 `AM_*`；SM 从 22/10 变为 **31 clip / 19 montage** |
| 引擎 | `6963a9d` | 导出链新增 `--clips`/`--continue-on-error`/`VL_GASP_ANIM_CLIP_IDS`；新增 `tool/animation/audit_gasp_catch.py` |
| 资源 | `90125e4` | Catch 运行时（`MovementMode`、Falling 入口、`Catch_Hurdle_low/med/high`）与新场景 `SC_gasp_catch_high.json` |

资产统计（工作树核对）：31 个 sidecar **829,022 B**；19 个 montage **36,839 B**；
notify 合计 **188**（142 traversal + 46 Catch）；warp 窗口 **33**（21 + 12）。
Catch 9 个 clip：时长 3.333–5.000 s、5,708 个曲线键、46 条 notify、12 个 warp；
逐 clip 明细见 `artifacts/gasp-catch-export/validation.json`（`issues=0`，
duration 与 FBX 最大差 1.59e-7 s）。Catch 9 个 Montage 全部 1 个 section、
`DefaultSlot`；Hurdle 三个各 2 窗口（`Bone` + `None`），Cliff/Mantle/Vault 各 1 窗口
（`Bone`）。

## 2. 导出链

- `audit_gasp_catch.py` 在隔离 UE 工程副本内只读枚举
  `AM_M_Neutral_Traversal_Catch_*`，用 UE `ObjectExporterT3D` 导出 9 份 T3D，并从
  `SlotAnimTracks(... AnimReference=...)` 解析引用序列后导出 9 个 FBX；三个输出路径
  （`VL_GASP_CATCH_OUTPUT`/`VL_GASP_CATCH_T3D_DIR`/`VL_GASP_CATCH_FBX_DIR`）必须在
  UE 工程外，报告已存在即拒绝。
- Catch 的 montage id 与 clip id 不是简单 `AM_` 前缀关系：
  `AM_..._Catch_Cliff_high_standF` 播放 `M_..._Catch_Cliff_high_stand`。转换器从
  T3D 的 `AnimReference` 取 clip，不按文件名配对。
- `export_gasp_ue_raw.py` 的 `VL_GASP_ANIM_CLIP_IDS` 可只导出指定 clip；
  `export_gasp_sequences.py --clips ... --continue-on-error` 与
  `import_gasp_montages.py --clips ...` 支持批量/继续执行。
- 全量回归：用当前 31 clip 的 SM 重新生成 raw dump（不设 `VL_GASP_ANIM_CLIP_IDS`），
  再 `--force` 重导 31 个 sidecar 与 19 个 montage；50/50 与备份 SHA256 相同，
  `animation_preflight_tests` 252/252。命令与失败记录见
  `artifacts/gasp-catch-export/COMMANDS.md`。

## 3. 运行时

### 3.1 MovementMode

```csharp
public enum MovementMode { Walking, Falling, Catch }
```

`RefreshMovementMode()` 只从原生状态推导：`traversingCatch` → `Catch`；否则
`Character.IsGrounded` → `Walking`/`Falling`。在 `Tick` 开头、`LateTick` 物理步后、
`StartTraversal` 末尾、`END`/`ABORT` 复位后刷新；切换时打印
`P8_MOVEMENT_MODE from=... to=... grounded=... vy=... catch=...`。
`Falling` 覆盖跳跃上升与下坠；Catch 入口额外要求 `LinearVelocity.Y <= 0.1`。

### 3.2 入口

- `Walking + Auto`：原有 SphereCast → 顶面向下射线 → Chooser（Hurdle/Climb/Mantle/Vault）。
- `Walking` 且 Chooser 返回 `Catch_*`：`P8_CHOOSER_REJECT reason=catch-requires-falling`
  （`auto_high` 语义保持：不播 Montage）。
- `Falling + 非 Auto`：`P8_TRAVERSAL_REJECT reason=grounded-only`。
- `Falling + vy > 0.1`：`P8_TRAVERSAL_REJECT reason=not-falling`。
- `Falling + Auto`：同一球扫（低探针 `y=-30`，未命中再高探针 `y=+120`）→
  顶面向下射线 → `ChooseTraversalAction(height, falling=true)`。
- 只接受 `Catch_Hurdle_low/med/high`；其它家族
  `P8_CHOOSER_REJECT reason=catch-family-not-implemented`。
- Catch 的顶面向下射线从**角色中心 + 120** 起射（地面动作仍从接触点 + 120）；下坠时
  接触点可能在墙面下部，从接触点起射会落进障碍内部。

### 3.3 Catch 执行与落点

`Catch_*` 的 clip 映射为 `M_Neutral_Traversal_<action>_stand`，Montage 为 `AM_` 前缀；
本切片只接 Hurdle 三种。`traversingCatch=true`、mode 立即切 `Catch`；targets 全部由
Montage 窗口驱动：

| target | 值（Hurdle_high，2 m 场景） | 依据 |
| --- | --- | --- |
| `FrontLedge` | `(0, 290, -120)` + yaw 0° | 球扫球心 + 顶面 Y + 90；`MakeRotFromX(-FrontLedgeNormal)` 的 Y-up yaw |
| `BackFloor` | `(0, 290, -177)` | 顶面 + `distance_from_ledge` 在 BackFloor 窗口 End 的采样 56.756 cm；`dOut=0` |

收口：`state.Time >= 最后一个 warp 窗口 End` 且 grounded 且
`Position.Y >= traversalTarget.Y - 10` → `P8_CATCH_LANDED`，停止消费根运动；
Completed + grounded → `P8_MONTAGE_END`/`P8_TRAVERSAL_END`；完成仍 airborne 0.6 s →
`airborne`；最后窗口 +0.2 s 未到顶面 → `stalled`；播完低于顶面 40 cm →
`catch-missed-top`。

## 4. BackFloor 口径：有意偏离，不是 UE 等价

UE `AC_TraversalLogic.SetWarpTargets` 搜 `Catch` **0 命中**（`AC_TraversalLogic.copy`，
16,511 行）；`back-target-analysis.md` §5 给出 `BackLedge` gate 为 `Hurdle | Vault`、
`BackFloor` gate 为 **Hurdle only**，其它 action 都是 `RemoveWarpTarget`。导出 T3D 里
没有 Catch 专用 target 公式。

因此没有可移植的“UE Catch BackFloor”分支。Catch 的语义是抓住顶边爬上顶面，而地面
Hurdle 的 `BackFloorLocation` 是后沿外向下射线命中的地面（本场景 y=0），套 `(X, Y+90, Z)`
会把 Catch 落到 y≈90 的身后地面，与验收的顶面 y≈290 冲突。VL 的实现把 BackFloor 解释为
**顶面落点**：

```text
BackFloorLocation = (front + direction × curve(End), top.Y)
target           = (X, top.Y + 90, Z)       // 与其它 target 相同的胶囊中心形状
dOut             = 0                        // 没有 BackLedge 可算 |d_ledge-d_floor|
```

`z` 仍来自资产曲线在窗口 End 的采样（Hurdle_high 56.756 cm；曲线在 t≈2.47 s 才到
91.721 cm，窗口外不参与），没有硬编码世界坐标。C# 注释与
`P8_CATCH_BACKFLOOR basis=no-backledge-window/no-ue-catch-branch` 记录依据。**这是有意
偏离，不宣称与 UE 等价**。

## 5. 验收

### 5.1 新场景 ×3

`SC_gasp_catch_high.json`：障碍 `halfExtents=[40,100,60]`、position `(0,100,-200)`，
顶面 y=200；角色 `(0,210,-80)`，0.5 s 探针时 `mode=Falling`、`vy≈-392`。

| 次 | Chooser | 窗口 | `P8_CATCH_LANDED` | END |
| --- | --- | --- | --- | --- |
| 09 | `height=169 Falling choice=Catch_Hurdle_high` | `FrontLedge[0.878,1.012]=(0,290,-120)` `BackFloor[1.750,2.000]=(0,290,-177)` | `time=2.005 y=290 grounded=True` | `(0,290,-164) Walking` |
| 10 | 同上 | 同上 | `time=2.126 y=290 grounded=True` | `(0,290,-197) Walking` |
| 11 | 同上 | 同上 | `time=2.001 y=290 grounded=True` | `(0,290,-176) Walking` |

三次都有 `Walking→Falling→Catch→Walking`、`exit=0`、stderr 0 B、无残留 `main`、
config 字节恢复。

### 5.2 独立复核

父代理独立运行新场景 1 次：`P8_CHOOSER height=176 mode=Falling choice=Catch_Hurdle_high`，
`P8_CATCH_LANDED time=2.736/4.333 y=290`，`END y=290 z=-224 grounded`。

### 5.3 既有场景回归（各 1 次）

| 场景 | 关键日志 | 结论 |
| --- | --- | --- |
| `SC_gasp_traversal` | Mantle `END (0,190,-170) grounded` | 通过 |
| `SC_gasp_traversal_auto` | Hurdle `END y=90,z=-362` | 通过 |
| `SC_gasp_traversal_auto_high` | `CHOOSER Catch_Hurdle_high` → `REJECT reason=catch-requires-falling` | 语义保持，reason 更新 |
| `SC_gasp_traversal_reject_high` | `REJECT height=160` | 通过 |
| `SC_gasp_traversal_abort_void` | `ABORT airborne` | 通过 |
| `SC_gasp_traversal_abort_stall` | `ABORT stalled progress=10` | 通过 |
| `SC_gasp_traversal_input` | 注入 Interact → Mantle `END y=190,z=-177` | 通过 |

全部 `exit=0`、stderr 0 B、无残留进程。

## 6. 未闭环

1. **Cliff / Mantle / Vault Catch**：未接线。Chooser 会产生 `Catch_Cliff_high`，当前显式
   `catch-family-not-implemented`；Cliff montage id 多一个 `F`、Vault/Mantle 没有
   `_stand`，需要先定“按 clip 查 Montage”或补资产别名。
2. **PoseSearch 入口帧**：Montage 从 t=0 播放，UE 的入口在 `PoseSearchBranchIn`
   notify（0.877 s）。前 0.877 s 是作者标注的下坠前摇（模型根 −957 cm），因此会先
   落到墙前地面、再由 FrontLedge warp 拉起；C# `PlayMontage` 没有 start time，
   需要 entry-section/PoseSearch 支持。
3. **地面 2 m**：`auto_high` 仍拒绝；UE 该行可能该走 Climb，但为保持既有语义未改
   Chooser。
4. **BackLedge 版 Catch**：Hurdle Catch 无 BackLedge，`dOut=0`；接 Cliff/Vault Catch 时
   需要补 dOut 分支。
5. **MovementMode 不细分 Jump**：上升段仍记 `Falling`，用 `vy<=0` 过滤。
6. **真实 gameplay 入口**：只验收 Auto 探针；玩家跳跃失败撞墙的自动触发未实现/未测。
7. **视觉验证**：仅日志验收，无截图/像素级确认；前摇+拉起是已知视觉问题。

## 7. 证据

```text
artifacts/gasp-catch-export/
  REPORT.md, COMMANDS.md, validation.json, ue-catch-audit.json,
  ue-anim-raw-catch.json, ue-anim-raw-31.json,
  t3d/, backup/, smoke/, logs/,
  sm-mannequin.before.json, sm-mannequin.diff,
  wire_sm_catch.py, validate_catch.py

artifacts/gasp-catch-runtime/
  RESULTS.md, GaspCharacter.blueprint.cs.diff, SC_gasp_catch_high.json,
  catch-high-09/10/11.{log,err.log,json,window.log},
  catch-high-01…08（迭代记录）, regress-*.{log,err.log,json,window.log},
  fbx-probe/, run-scene.ps1, sha256.txt, config.json.orig

工具：
  tool/animation/audit_gasp_catch.py
  tool/animation/export_gasp_ue_raw.py（VL_GASP_ANIM_CLIP_IDS）
  tool/animation/export_gasp_sequences.py（--clips/--continue-on-error）
  tool/animation/import_gasp_montages.py（--clips）
```
