# GASP P22 Montage 迁移与清理记录

状态：执行记录，非当前契约。2026-10-20。本记录覆盖三件事：(1) Montage/catalog/C#
迁移与 notify 执行；(2) 删除 `skinning.root` 与脚本 `warpTargets` 的清理；
(3) Climb walk 真窗口修复与清理后全矩阵。行为以
`contracts/animation-montage.md`、`contracts/animation-motion-warping.md`、
`contracts/animation-asset-import.md` 与源码为准。

## 1. 提交与范围

| 仓库 | 提交 | 内容 |
| --- | --- | --- |
| 引擎 | `0356b7f` | notify/notifyState 区间执行（`FiredNotifies`、`ActiveNotifyStates`、Stop/切片段/循环/Seek 语义） |
| 引擎 | `b3a5e79` | playback range end 按片段 clamp（`SetPlaybackRange`） |
| 引擎 | `e5a92f6` | `animationScriptData.*`、catalog/assets、`playMontage` 请求、C# `CurveValue`/`Montage`/`PlayMontage`、feedback `notifies`/`montage` |
| 引擎 | `0de7bcc` | 删除 `skinning.root`（出现即拒绝）、强制 `skinning.skeleton`、缓存键只保留 `skeleton=` 段、C# 删除 `MotionWarpTarget`/`Play(warpTargets)`、`clip` 请求出现 `warpTargets` 显式拒绝、Motion DB 输入加 SK 摘要 |
| 资源 | `aa78096` | SM 接 `skeleton` + 22 个 `sequence` + 10 个 `montages`；`GaspCharacter` 改 `PlayMontage` + `Montage().Windows` + `CurveValue`；Climb walk 竖直闭环推迟到首窗 |
| 资源 | `ac0fe0c` | SM 删除残留 `skinning.root` |

本记录只新增文档，没有改源码、资源或 C#；证据目录在 `artifacts/`（gitignored）。

## 2. Montage 迁移

### 2.1 资源接线

`Maps/SC_gasp/Meshes/SM_mannequin.json`：`skinning.skeleton` 指向 SK；22 个 clip 各加
`sequence`；10 个 `montages`。Montage 的窗口先按 T3D 核对：
`verify_montage_windows.py` 得 `montages=10 mismatches=0`（21 个窗口的时间/provider/bone
与 T3D 一致，容差 2e-6 s），因此本轮不改 `AM_*.json`。

`GaspCharacter.blueprint.cs` 删除手写 notify 名数组、`BuildWarpTargets`、四个动作的
窗口/attach/时间硬编码；`BuildTraversalTargets` 只按 `Montage().Windows` 的 target 名
构造世界位置，`dOut` 用 `CurveValue(clip, "distance_from_ledge", window.End)` 算；
`animation.Play` → `animation.PlayMontage(montage, targets, false, 1, true)`。
时间/notify 全部来自反馈：`State.Duration/Time/Completed`、`State.MontageLength`、
`State.MontageRange`、`State.Notifies`（`P8_MONTAGE_NOTIFY` 输出
`name/time/phase/track`）。反馈比脚本 tick 晚一拍，`TickTraversal` 在
`State.Montage` 尚未切到本次 Montage 时直接返回，避免用上一片段的时间/notify。

### 2.2 notify 来自资产

`verify_notify_times.py` 对 20 份 traversal 日志逐条核对 `P8_MONTAGE_NOTIFY`：356 条事件、
0 条未匹配；350 条与资产作者时间相差 ≤2 ms，6 条是 `Completed` 时被播放范围截断的
`end`（时间等于 Montage 末尾）。Hurdle 的 `P8_WARP_DOUT` 为
`dledge=34.098 dfloor=90.909 dout=56.811`，与独立 Python 从 sidecar 采样
（34.098387 / 90.908946 / 56.810559）一致；旧 C# 内联常数 `|34.098671-90.908951|`
已删除。

## 3. 清理：删除兼容字段

- `skinning.root` **出现即拒绝**（即使同时给了 `skinning.skeleton`）：
  `Mesh skinning.root is no longer supported; use skinning.skeleton`。
- 骨骼 SM 必须给 `skinning.skeleton`；根骨、关节表、曲线注册表唯一来源是 SK。
- `modelCacheKey` 只保留 `|skeleton=<path>` 与 loader 的 `|skeletonBlake3=<hex>`，
  旧 `|skinning=<root>` 段消失。测试逐字固定：
  `<modelDataPath>|skeleton=<SK path>|clip=<id>:<path>`。
- C# `MotionWarpTarget` 类型与 `Play(..., warpTargets)` 重载删除；warp target 由
  Montage 资产声明，C# 只提交位置。旧 `clip` 请求携带 `warpTargets` 时原生拒绝：
  `Script.WarpTargetsDefinedByMontage`。
- Motion DB：`CaptureMotionDatabaseInputs` 把 SK 文件摘要加入输入（源/schema/model/FBX
  之后、member FBX 之前）。identity 变化使旧缓存失效；`artifacts/gasp-post-cleanup-matrix`
  记录新缓存 `48a87df7…0db3.json`（608,702 B、17 项 inputs、含 SK blake3
  `ff326902…7583`）在清理构建首次运行 playground 时自动重建，本次再运行命中且未改写。

## 4. Climb walk：真窗口、UE 对照与修复

### 4.1 真窗口

T3D 反序列化字段（`Notifies` 的 `LinkValue`/`EndLink.LinkValue`/`Duration`）给出：

```text
Notifies(1): LinkValue=0.983966  EndLink.LinkValue=1.293677  Duration=0.309711
Notifies(2): LinkValue=1.293831  EndLink.LinkValue=1.603542  Duration=0.309711
```

P17 记录的 `0.309711–1.293677` 把 **Duration** 当成了 Start；旧 C# 窗口
`[0.309711, 1.293677]` 是同一转录错误。Montage JSON 没有数据错误。

### 4.2 UE 5.8 源码对照

- `URootMotionModifier_Warp::Update` 的 `CachedOffsetFromWarpPoint` 在 **EndTime** 采样并
  整个窗口缓存（`RootMotionModifier.cpp:391-465`），与 VL 每 tick 用 EndTime 重算数值等价。
- `bWarpToFeetLocation` 默认 true、目标空间与 VL 同向；同一 fixture 下 UE 参考积分也会
  发散（projected scale 为负），不是 VL 独有。
- 根因是窗口打开时的几何 gap：零点 ≈ UE-B 82 cm / VL 90 cm，而原夹具只有 16 cm。
  `gap = z0 + 120 - A_eff`，最大 `350 - A_eff`；实测 `A_eff` 在 198–246 cm 抖动，
  需要 reach 350（名义 speed 500）才能覆盖 95 cm 的 gap。

### 4.3 修复

1. 探针 walk 名义速度 200 → **500**（reach 185 → 350；真实输入仍用
   `Character.LinearVelocity`，这是夹具口径）。
2. `SC_gasp_traversal_climb_walk.json` 的 probe `z=62 → +228`（触发 348，留 2 cm）。
3. Climb+Walk 的竖直闭环从旧错误的 montage t=0 **推迟到首个真实窗口 Start**；
   窗口前只走根运动。旧起点会把胶囊先抬高约 178 cm，使 attach 目标竖直分量变负、
   SkewWarp 反向；窗口列表为空时退化为从 0 开始，不影响 stand/run。

验收 3 次：`END (0,340,-167) / (0,340,-165) / (0,340,-167)`，grounded、无 ABORT；
stand/run 回归 `(0,340,-153/-163)`、`(0,340,-172/-164)` 在历史范围内。

## 5. 清理后矩阵

`artifacts/gasp-post-cleanup-matrix/RESULTS.md`：35 次运行（29 正式 + 2 次
`reject_high` 复跑 + 4 次额外复跑），34/35 `exit=0`，1 次关闭路径交换链 OOM；
无残留 `main`，config 前后 SHA256 均为 `F7BB70…F102`。

| 场景 | clip | #1 END | #2 END |
|---|---|---:|---:|
| traversal | Mantle stand | -160 | -159 |
| mantle_walk | Mantle walk | -202 | -202 |
| mantle_run | Mantle run | -204 | -202 |
| vault | Vault stand | -365 | -376 |
| vault_walk | Vault walk | -335 | -340 |
| vault_run | Vault run | -346 | -338 |
| climb | Climb stand | -164 | -162 |
| climb_walk | Climb walk | -168 | -155 |
| climb_run | Climb run | -182 | -175 |
| auto | Hurdle (Auto) | -325 | -342 |

- 全部 grounded、y=190/90/340、无 ABORT；x≈0。
- `P8_WARP_WINDOWS` 20/20 与迁移后基线（`artifacts/gasp-montage-wiring/matrix`，
  climb_walk 用 `artifacts/gasp-climbwalk-fixture/accepted`）**集合逐字相同**；
  `P8_MONTAGE_NOTIFY`、`REQUEST/HIT/LEDGE_CONTEXT` 同样逐字相同。差异只在最终落地。
- 特殊场景 8/8 语义正确：`reject_clearance`/`height=160`/`height=50`/`no-hit`、
  `auto_high` Catch、`abort_stall stalled progress=10`、`abort_void airborne`
  （y≈-2435，P21 为 -2792，均先走无 BackLedge 回退）、`input` END `(0,190,-176)`。
- 抖动/离群：climb_walk 正式第 2 次 -155，额外复跑 -167/-168（4 次中 3 次与基线重合；
  窗口打开 gap 逐次可差约 20 cm）；climb_run 两次正式 -182/-175，额外 -163；
  Hurdle 聚在 -325/-326/-342，仍在历史 -320…-374 带宽内。三处都未改窗口/notify 数据。
- `reject_high` 首次运行语义正确（`height=160`）但关闭时
  `Swapchain out-of-date recovery failed: ErrorOutOfDeviceMemory`，`exit=2`；随后复跑
  2 次 `exit=0`、stderr 空，记为**一次性关闭路径 GPU 内存/交换链竞态**，不是 traversal
  语义回归。

### 5.1 测试

```text
animation_preflight_tests
  注册 224 项 / 22 套件
  SkeletalImport 30 / AnimationSkeleton 17 / AnimationSequenceData 14 /
  AnimationMontage 23 / AnimationScriptData 12 / NotifyPlayback 11

scriptAnimationTests（C# ON 文件）
  注册 ScriptAnimationTest.* 34（其中 7 项 CurveValue/Montage/PlayMontage/notify）
  + ScriptAnimationWorld.* 8 = 42
  含新增 ScriptAnimationWorld.RejectsClipRequestWithWarpTargets
```

## 6. 未闭环

1. Climb walk 的 -155 是单次落点抖动；`A_eff` 有约 ±25 cm 抖动，窗口打开 gap 逐次
   可差约 20 cm，仍偶发短端。
2. Climb run 两次正式运行同向偏远，额外复跑回到 -163；窗口/notify 逐字相同，
   属脚本 tick 与动画 tick 对齐的根位移消费差。
3. Hurdle 聚在 -325/-326 是历史带宽短端，未做 landing 稳定性切片。
4. `reject_high` 关闭路径 OOM 未复现，未查 swapchain teardown。
5. 没有清理前二进制的同会话 A/B，落点差异不能定量归因到清理提交；能确认的是触发、
   窗口、notify、拒绝/中断/落地语义无回归。
6. Vault run 的 `S_min=+239` 仍比场景起点 227 远 12 cm，首窗略奇异（P21 遗留），
   矩阵由 Jolt 墙面限制与收口兜住。

## 7. 证据

```text
artifacts/gasp-montage-wiring/
  RESULTS.md, matrix/, rerun/, diagnostics/, combined-summary.json,
  montage-window-verification.json, notify-verification.json,
  verify_montage_windows.py, verify_notify_times.py,
  sm-mannequin.diff, gasp-character.diff

artifacts/gasp-climbwalk-fixture/
  RESULTS.md, fixture-derivation.json, derive_fixture.py,
  accepted/, climb-walk/, probe/, regress/,
  my-change.diff, ydefer.diff, scene.diff, results.json

artifacts/climb-walk-ue-parity/
  README.md（UE 5.8 源码逐行对照）, parity.py, warpmath.py,
  fixture_sensitivity.py / fixture-sensitivity.json, climb-walk-parity.json

artifacts/gasp-post-cleanup-matrix/
  RESULTS.md, summary.json, endpoints.tsv, logs/manifest.tsv,
  logs/*.log/*.err, logs/rerun/, logs/extra/,
  run-matrix.ps1, compare_warp.py, compare_notifies.py, compare_probe.py,
  playground-cache-before.json, config.json.orig
```
