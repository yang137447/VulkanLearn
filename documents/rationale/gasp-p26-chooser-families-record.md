# GASP P26 Catch 家族、跳跃 gating 与视觉返工记录

状态：执行记录，非当前契约。2026-10-21。覆盖 Chooser 家族阈值、跳跃/走落 gating、
GASP 关卡视觉资产、入口采样窗口研究结论与验收。行为以
`contracts/animation-montage.md`、`contracts/animation-runtime-skinning.md` 与源码为准。

## 1. 提交

| 仓库 | 提交 | 内容 |
| --- | --- | --- |
| 资源 | `026d480` | Chooser 家族（纵深→Hurdle/Mantle/Vault）、`jumpInitiated` gating、`CatchOnWalkOff`、5 个 Catch 场景换 GASP `LevelTextured` 可见资产、新增 3 个家族场景 |
| 引擎 | `1b203a8` | `ScriptAnimationWorld.MontageBranchInWindowsAreSamplingRanges`：9 Catch 窗口 ≤1.2 帧、10 非 Catch `time==0`，并锁定只对 montage、不扫 clip |

测试：`animation_preflight_tests` **255/255**（本轮无模块改动）；
`scriptAnimationTests` 注册 **48**（`ScriptAnimationTest.*` 36 +
`ScriptAnimationWorld.*` 12，新增 1 项 branch-in 范围断言）。

## 2. Chooser 家族

### 2.1 阈值

资源侧启发式，不是 UE 逐行复刻。唯一长度基准是胶囊直径
`diameter = 2 × CapsuleRadius`（场景 `physics.radius=30` → 60 cm）：

| 顶面可站立纵深 `depth` | family | 高度档 |
| --- | --- | --- |
| `≤ 2 × diameter` | Hurdle | ≤50 low / ≤100 med / ≤201 high / 否则 Cliff_high |
| `≤ 3 × diameter` | Mantle | ≤50 low / ≤100 med / 否则 Cliff_high |
| `> 3 × diameter` | Vault | ≤50 low / ≤80 med / 否则 high |
| 无后沿命中（`depthKnown=false`） | Vault | 取 `BackProbeReach` 当最深 |

地面入口不量纵深，保持原高度行（≤125 Hurdle；更高返回 Catch 行由
`catch-requires-falling` 拒绝）。`depth` 是后沿扫描深度、
已减前向球扫半径，量化步长 ±20 cm。

### 2.2 依据

- Hurdle 抓边落点 `distance_from_ledge ≈ 57`（≈1 个直径）+ 身体直径才够站住，所以
  “薄”取 2 个直径；更薄顶面只能按薄墙处理。
- Mantle 是可站立厚台；100–200 档在 UE 表里就是 `Catch_Cliff_high`，不另造资产。
- Vault 抓边后仍向前消费约 185 cm 根运动（P25 实测 landing z=-137 → END z=-322，
  ≈3 个直径），只有更深顶面才不会被带出后沿。
- 档位边界来自 UE `CHT_TraversalMontages_CMC` 的 nested chooser 行；UE 原生用
  `HasBackLedge/HasBackFloor/BackLedgeHeight` 决定家族，这里只按纵深+高度的差别
  已在 §9.1 记录。

## 3. 跳跃 / 走落 gating

```text
jumpInitiated：Jump 输入或 LocomotionState.Jump 置位；回到 Walking 清零
allowAuto = jumpInitiated ? CatchOnFall : CatchOnWalkOff
eligible  = allowAuto && TraversalEnabled && !TraversalProbe && !traversing &&
            !traversalPending && MovementMode==Falling && LinearVelocity.Y <= 0.1
cooldown  = 0.15 s（不满足时归零）
```

- `CatchOnFall` 默认 **true**：只认“跳跃失败”的 Falling。
- `CatchOnWalkOff` 默认 **false**：走落不自动抓；显式打开才抓。
- `TraversalProbe=true` 的探针场景不跑自动触发（保持既有 `P8_*` 序列）。
- 每次进入 Falling 写一条
  `P8_CATCH_GATE jumpInitiated=... catchOnFall=... catchOnWalkOff=... vy=...`，
  走落不触发时能直接证明是 gating 而非探测失败。

## 4. 视觉资产（GASP LevelTextured）

5 个 Catch 场景（catch_high、catch_jump、family hurdle/mantle/vault）的墙/地面换成
资源里已有的 GASP 关卡导出，**无 materialOverride**：

| 用途 | SM | 源 OBJ | 自带材质 | 尺寸/枢轴 |
| --- | --- | --- | --- | --- |
| 墙/台面 | `SM_gasp_055` | `Source/Level/level_004.obj` | `LevelTextured/MI_gasp_048`（`M_gaspGrid` + `T_Hologrid`/`T_Grid_Crosses`/`T_Paint_*`） | 100 cm 立方体，枢轴最小角 |
| 地面 | `SM_gasp_025` | 同 OBJ | `LevelTextured/MI_gasp_018` | 100 cm 立方体，枢轴最小角 |

对齐规则：`scale = 2*halfExtents/100`、`position = center − halfExtents`（枢轴在最小角）；
physics body 全部保留、尺寸不变。例：墙 `[40,100,60]` → `position (-40,0,-260)`、
`scale (0.8,2,1.2)`；地面 `[10000,50,10000]` → `position (-10000,-100,-10000)`、
`scale (200,1,200)`。家族场景墙深 100/160/300 → `scale z=1.0/1.6/3.0`。

截图 `artifacts/gasp-chooser-families/capture-mantle-landed.png`（1280×720，
604,545 B；BMP 原图另存）：橙色网格 GASP LevelBlock、灰色网格 GASP 地面、角色站在台面顶。由
`capture-landing.ps1` 在 `P8_CATCH_LANDED` 后 0.25 s 注入 `screenshot` 捕获；
`screenshotSaved=true`、config 恢复、无残留。截图只证明几何/材质可见，不是像素级对比。
返工前的占位盒（`SM_cube` + materialOverride）与纯色 `SM_gasp_016` 已弃用。

## 5. PoseSearch branch-in 研究结论

来源 `artifacts/posearch-branchin-research/REPORT.md`（gitignored，只读研究）：

1. **`UAnimNotifyState_PoseSearchBranchIn` 不选帧**：它只把动画在 DB 的可采样范围钉成
   `[Time, Time+Duration]`（`PoseSearchDatabase.cpp:1302-1315`），并作为“可被 PoseMatch
   列选中”的声明。
2. **真正选帧在 Chooser 的 Pose Match 列**：
   `OutputStartTime = BestSearchResult.GetAssetTime()`（`PoseSearchChooserColumn.cpp:1016-1019`），
   再由 GASP 调用方传 `PlayMontage(StartTime)`。notify 本身没有这段逻辑。
3. **量化**：9 个 Catch montage 的窗口 33.3–34.9 ms（≈1 帧 @30fps），
   Hurdle_high branch-in `0.877453` 与 FrontLedge warp `0.877738` 差 0.285 ms。
   VL 取窗口下界，与 UE 选帧差 **≤1 帧（≤0.035 s）**，Catch 上没有额外收益。
4. **非 Catch 家族**：窗口 `[0, 0.07–1.03]`（2–31 帧），VL 恒从 0 起播（=下界）；
   这才是 B1 的收益点。
5. **规则限定**：`PoseSearchBranchIn` 也存在于 locomotion sequence（Run/Sprint/Crouch/
   Jump_Land），通用“扫 clip”会读错时间；VL 只对 Montage notify 生效，并由
   `MontageBranchInWindowsAreSamplingRanges` 锁定（9 Catch ≤1.2/30 s、10 非 Catch
   `time==0`）。
6. **方案**：A（文档/日志/测试断言，已落地）；B1（per-member 采样区间 + 独立 traversal
   DB + 一次性 query，约 4–6 人日）；B2（只做 Catch）收益 ≤1 帧，不值得；C（完整
   PoseSearch ≥15k–25k 行）不建议。

## 6. 验收

### 6.1 家族（各 2–3 次）

| 场景 | 关键日志 | 落地/END |
| --- | --- | --- |
| `SC_gasp_catch_family_hurdle` | `family=Hurdle depth=100 height=77 → Catch_Hurdle_med` | y=290（z=-227/-202） |
| `SC_gasp_catch_family_mantle` | `family=Mantle depth=160 height=77 → Catch_Mantle_med` | 288/289 → y=290 |
| `SC_gasp_catch_family_vault` | `family=Vault depth=280 → Catch_Vault_high` | y=290（END z=-303/-341/-375） |

三次均无 REJECT/ABORT，`P8_TRAVERSAL_END grounded`、END y=顶面+90=290。
depth 量化 ±20：Vault 报 280 是 300 cm 台面减一个探针半径后的口径。

### 6.2 既有 Catch 场景

- `SC_gasp_catch_high`：`family=Hurdle depth=120`，height 117–127/149/155 →
  `Catch_Hurdle_high`，y=290。
- `SC_gasp_catch_jump`：`family=Hurdle depth=120 height=77` →
  `Catch_Hurdle_med`，`P8_CATCH_AUTO_TRIGGER` 每次恰好 1 条，y=290。

### 6.3 gating

| 场景 | 结果 |
| --- | --- |
| 走落（`CatchOnWalkOff=false`） | `P8_CATCH_GATE jumpInitiated=False ... catchOnWalkOff=False` → 无 Catch |
| 走落（`CatchOnWalkOff=true`） | 触发 `family=Vault depth=none height=117 → Catch_Vault_high`，`CATCH_LANDED y=190`，随后因 80 cm 薄顶+无后沿被 Vault 根运动带出 → `catch-missed-top` |
| `CatchOnFall=false`（跳跃失败） | `jumpInitiated=True catchOnFall=False` → 无自动触发 |
| `jump_no_wall` | `jumpInitiated=True catchOnFall=True`，探测 no-hit 静默 → 正常跳跃/落地 |

### 6.4 回归（8/8）

`SC_gasp_traversal` Mantle END -176；`auto` Hurdle END y=90 z=-330；
`auto_high` 仍 `catch-requires-falling`；`reject_high` `height=160`；`abort_void`
`airborne`；`abort_stall progress=10`；`input` Mantle END -167；`playground` 无 P8 日志。
全部 `exit=0`、stderr 0 B、config 恢复、无残留。

### 6.5 独立复核与失败记录

父代理独立复核：`family=Mantle depth=160 height=77 → Catch_Mantle_med`、
`CATCH_LANDED 1.938 y=288`、END y=290。

失败保留：

- `catch_jump` 一次越顶 `ABORT stalled progress=180`（BackFloor 落点余量小；3 次矩阵
  全成功，未改资产）。
- Mantle 换 GASP mesh 后一次截图运行丢窗口（`warp=FrontLedge:done landed=False`、
  z 全程不动）→ `catch-missed-top`；重试 1.706 s 落顶。疑似首帧/材质加载卡顿跳过
  warp 窗口，属既有窗口门控脆弱点。

## 7. 未闭环

1. 家族阈值是资源侧启发式，不是 UE 逐行：厚而低平台在 UE 是 Mantle，这里 3 个直径内
   也是 Mantle；深台面在 UE 仍可能落 Mantle，这里按 Vault。
2. 走落+薄墙悬空会 `catch-missed-top`（落差→Vault 规则的直接后果）；复杂走落未验收。
3. `catch_jump` 的 BackFloor 落点离后沿最近约 4 cm，存在越顶风险。
4. Mantle 的 Cliff_high 档（高度 100–201）只由 UE 表推导，未在低台面验证。
5. Mantle/Cliff 换 mesh 后有偶发丢窗口，需单独查首帧/材质加载与 warp 门控。
6. 原生 PoseSearch 选帧未做；非 Catch B1 待做（约 4–6 人日），Catch 无收益。
7. 无 30/60/120 Hz、包内矩阵与像素级姿态对比。

## 8. 证据

```text
artifacts/gasp-chooser-families/
  RESULTS.md, GaspCharacter.blueprint.cs.diff, BP_gasp_traversal_probe.json.diff,
  SC_gasp_catch_high.json.diff, SC_gasp_catch_jump.json.diff,
  family-{hurdle,mantle,vault}-01..03.*, catch-high-01..03.*, catch-jump-01..03.*,
  calib-*, final-*, regress-*.log/.err.log/.json,
  capture-mantle-landed.{bmp,png,json,log},
  dev-capture-jump-overshoot.{log,json}, dev-capture-mantle-missed.txt,
  scenes/, run-scene.ps1, run-matrix.ps1, capture-landing.ps1,
  matrix.txt, sha256.txt, config.json.orig

artifacts/posearch-branchin-research/
  REPORT.md, catch_branchin_timings.json, parse_catch_branchin.py
```
