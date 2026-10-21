# GASP P21 场景 reach 重做与矩阵记录

状态：执行记录，非当前契约。2026-10-20。本轮只改探针夹具的场景触发距离与放置，
不改引擎动画代码、不改 motion warping 运行时；落点数字为 Development 构建实测。
**本记录的矩阵已被 P22 清理后矩阵取代，见
[P22 Montage 迁移记录](gasp-p22-montage-migration-record.md)；下文保留当轮实测原文。**
当前行为以 `contracts/animation-motion-warping.md` 与 `contracts/testing.md` 为准。

## 1. 本轮改了什么

- 资源仓库 `8dfc3b4`（`【Mod】place traversal probes at UE reach distances`）：
  自动探针的触发距离改为 UE `GetTraversalCheckInputs` 的 reach 口径；17 个 traversal
  场景按 reach 重新放置。
- C# 夹具 `Maps/SC_gasp/Blueprints/GaspCharacter.blueprint.cs` 的 `RequestTraversal`
  改动 7 insertions / 1 deletion（`git diff --numstat`）：探针速度取**名义 gait**
  （Run 500 / Walk 200 / Stand 0），真实输入路径仍用 `Character.LinearVelocity`。
- managed `Gameplay.dll` 需重编（`scripts.enabled` 只加载程序集）：
  `cmake --build build-vscode\development --target vl_gameplay -j 8`。`main.exe` 未重编。

夹具代码：

```csharp
float speed = TraversalProbe
    ? TraversalGait == "Run" ? 500 : TraversalGait == "Walk" ? 200 : 0
    : Character!.LinearVelocity.Length();
float reach = Math.Clamp(75 + speed / 500 * 275, 75, 350);
```

`TraversalProbe` 只对 C# 自动探针为 true。此前探针用 Jolt 实际速度：速度接近 0 时
reach 只有 75 cm，无法在 walk/run montage 的接近段之前触发（夹具注释原文即此意，
见 `GaspCharacter.blueprint.cs` 的 `RequestTraversal`）。

## 2. reach 与 S_min / S_anim 推导

数据源：`artifacts/gasp-mantle-vault-warp/ue-mantle-vault-seq.json`、
`artifacts/gasp-warp-hurdle/ue-hurdle-root.json`、`artifacts/gasp-p10/climb-fbx-inspect.json`
与 Climb walk/run 的 FBX 导出；脚本是 `artifacts/gasp-reach-matrix/derive_reach.py`，
输出 `reach-derivation.json`。

```text
reach(gait) = clamp(75 + speed/500*275, 75, 350)   stand 0 -> 75, walk 200 -> 185, run 500 -> 350
A = root forward displacement 0 -> first window Start
W = root forward displacement inside the first window
attractor = FrontLedge target + flip(root*attach^-1) at the first window End
S_min  = attractor_z + A          (window opens exactly on the attractor)
S_anim = attractor_z + A + W      (window runs at scale 1)
trigger envelope = wall_face + reach + sweep_radius(20) - 2 cm margin
```

墙面前沿取 -140（场景障碍前表面）。C# 球扫半径 20、长度 reach，因此可触发上界为
`-140 + reach + 20`，再留 2 cm 余量：stand -47 / walk +63 / run +228。实测 z=-80
（球心距墙 60）命中 `distance=40`；z=-20（距墙 120）得到 `P8_TRAVERSAL_REJECT no-hit`
（`tests/ztest-mantle-run-far.log`）。

| action/gait | reach | A | W | attractor z | S_min | S_anim |
|---|---:|---:|---:|---:|---:|---:|
| Mantle/Stand | 75 | 10.14 | 20.56 | -82.5 | -72.4 | -51.8 |
| Vault/Stand | 75 | 4.88 | 31.59 | -79.9 | -75.0 | -43.4 |
| Mantle/Walk | 185 | 80.74 | 100.29 | -91.3 | -10.6 | 89.7 |
| Vault/Walk | 185 | 106.74 | 82.03 | -82.9 | 23.8 | 105.8 |
| Mantle/Run | 350 | 175.00 | 116.67 | -62.6 | 112.4 | 229.1 |
| Vault/Run | 350 | 316.66 | 108.34 | -77.7 | 239.0 | 347.3 |
| Hurdle/Stand | 75 | 5.21 | 24.03 | -74.2 | -69.0 | -45.0 |
| Climb/Stand | 75 | 1.09 | 32.41 | -120.0 | -118.9 | -86.5 |
| Climb/Walk | 185 | 62.06 | 219.41 | -120.0 | -57.9 | 161.5 |
| Climb/Run | 350 | 172.29 | 173.57 | -120.0 | 52.3 | 225.9 |

**首窗非奇异判据是 `startZ >= S_min`**：窗口打开时角色已经在 attractor 之后，
warp 才不会把角色往身后拉。10 个 traversal 片段中 9 个满足；Vault run 的
`S_min=+239`、场景起点 z=227，距 S_min 差 12 cm（reach=350 的触发上界只到 +228），
见 §5。

## 3. 场景放置（17 个改动 + 1 个保留例外）

规则：首选 `S_anim`；超过触发上界时取上界并记录差；`startZ >= S_min` 是首窗非奇异硬约束。
整数取整比 2 cm 余量再多 1 cm（-48/+62/+227 对 -47/+63/+228），避免球扫恰好压在边界上。
下表 18 行含 `_abort_stall`——它**没有随本轮改动**（保持 -80），单列只为说明例外。

| 场景 | action/gait | z | reach | S_min | S_anim | 触发上界 | 非奇异 | 依据 |
|---|---|---:|---:|---:|---:|---:|---|---|
| _root（traversal） | Mantle/Stand | -52 | 75 | -72.4 | -51.8 | -47.0 | 是 | S_anim |
| _mantle_walk | Mantle/Walk | 62 | 185 | -10.6 | 89.7 | 63.0 | 是 | S_anim clamped |
| _mantle_run | Mantle/Run | 227 | 350 | 112.4 | 229.1 | 228.0 | 是 | S_anim clamped |
| _vault | Vault/Stand | -48 | 75 | -75.0 | -43.4 | -47.0 | 是 | S_anim clamped |
| _vault_walk | Vault/Walk | 62 | 185 | 23.8 | 105.8 | 63.0 | 是 | S_anim clamped |
| _vault_run | Vault/Run | 227 | 350 | 239.0 | 347.3 | 228.0 | **否** | S_anim clamped；S_min 不可达 |
| _climb | Climb/Stand | -86 | 75 | -118.9 | -86.5 | -47.0 | 是 | S_anim |
| _climb_walk | Climb/Walk | 62 | 185 | -57.9 | 161.5 | 63.0 | 是 | S_anim clamped |
| _climb_run | Climb/Run | 226 | 350 | 52.3 | 225.8 | 228.0 | 是 | S_anim |
| _auto | Hurdle/Stand | -48 | 75 | -69.0 | -45.0 | -47.0 | 是 | Chooser 在 height=100 选 Hurdle |
| _auto_high | Hurdle/Stand | -52 | 75 | -69.0 | -45.0 | -47.0 | 是 | Chooser 在 height=200 拒绝（Catch） |
| _reject_clearance | Mantle/Stand | -52 | 75 | -72.4 | -51.8 | -47.0 | 是 | 拒绝夹具 |
| _reject_high | Mantle/Stand | -52 | 75 | -72.4 | -51.8 | -47.0 | 是 | 拒绝夹具（height=160） |
| _reject_low | Mantle/Stand | -52 | 75 | -72.4 | -51.8 | -47.0 | 是 | 拒绝夹具（height=50） |
| _reject_nohit | Mantle/Stand | -52 | 75 | -72.4 | -51.8 | -47.0 | 是 | 拒绝夹具（no-hit） |
| _input | Mantle/Stand | -52 | 75 | -72.4 | -51.8 | -47.0 | 是 | 输入通路夹具（`TraversalProbe=false`） |
| _abort_void | Vault/Stand | -48 | 75 | -75.0 | -43.4 | -47.0 | 是 | 中断夹具；保留 airborne abort |
| _abort_stall | Vault/Stand | -80 | 75 | -75.0 | -43.4 | -47.0 | 否 | 中断夹具；**例外**，见 §6 |

`_abort_stall` 保持 -80：stall 判据是 `progress < 20`；放到 reach 上界 -48 时实测
END `(0,90,-90)`、`progress=42`，不再 ABORT，因此该文件不在资源仓库 diff 中。
`_auto_high` 用 -52：高探针在 y=210 命中 200 cm 障碍的顶边，留 4 cm 触发余量。
相机/障碍/`description` 未改；`verify_scene_diff.py` 把 probe.position 还原为
`[0,90,-80]` 后与 git HEAD 逐字段比对，17 个文件均无其他差异。

## 4. 38 次矩阵

Development 构建，每场景 23 s（冷启动约 15.5 s），`CloseMainWindow()` +
`WaitForExit(30000)`；38 次运行全部 `exit=0`，结束后 `Get-Process main` 计数 0。
10 个 traversal 场景 ×3 次全部 grounded、越过障碍、无 ABORT；首窗 attractor 判定
30/30 通过。

首窗判定阈值：t 采样间隔 0.25 s，取距首窗 EndTime 最近的样本；水平 ≤150 cm、
竖直 ≤100 cm，且“同侧”（z ≥ -140 或 y ≥ 180，即未穿墙/已在顶面）。

| 场景 | clip | # | END | grounded | ABORT | 首窗 h/v | 首窗判定 |
|---|---|---|---|---|---:|---:|---|
| traversal | Mantle stand | 1 | (0,190,-164) | True | - | 10 / 0 | ok |
| traversal | Mantle stand | 2 | (0,190,-174) | True | - | 12 / 0 | ok |
| traversal | Mantle stand | 3 | (0,190,-170) | True | - | 10 / 0 | ok |
| mantle_walk | Mantle walk | 1 | (0,190,-202) | True | - | 9 / 33 | ok |
| mantle_walk | Mantle walk | 2 | (0,190,-201) | True | - | 10 / 31 | ok |
| mantle_walk | Mantle walk | 3 | (0,190,-201) | True | - | 23 / 33 | ok |
| mantle_run | Mantle run | 1 | (0,190,-202) | True | - | 49 / 36 | ok |
| mantle_run | Mantle run | 2 | (0,190,-204) | True | - | 34 / 36 | ok |
| mantle_run | Mantle run | 3 | (0,190,-209) | True | - | 28 / 36 | ok |
| vault | Vault stand | 1 | (0,90,-380) | True | - | 2 / 1 | ok |
| vault | Vault stand | 2 | (0,90,-377) | True | - | 2 / 1 | ok |
| vault | Vault stand | 3 | (0,90,-375) | True | - | 4 / 1 | ok |
| vault_walk | Vault walk | 1 | (0,90,-348) | True | - | 30 / 73 | ok |
| vault_walk | Vault walk | 2 | (0,90,-344) | True | - | 24 / 66 | ok |
| vault_walk | Vault walk | 3 | (0,90,-334) | True | - | 30 / 75 | ok |
| vault_run | Vault run | 1 | (0,90,-346) | True | - | 1 / 6 | ok |
| vault_run | Vault run | 2 | (0,90,-350) | True | - | 1 / 7 | ok |
| vault_run | Vault run | 3 | (0,90,-335) | True | - | 24 / 53 | ok |
| climb | Climb stand | 1 | (0,340,-231) | True | - | 15 / 1 | ok |
| climb | Climb stand | 2 | (0,340,-188) | True | - | 10 / 42 | ok |
| climb | Climb stand | 3 | (0,340,-229) | True | - | 16 / 1 | ok |
| climb_walk | Climb walk | 1 | (0,340,-204) | True | - | 10 / 46 | ok |
| climb_walk | Climb walk | 2 | (0,340,-195) | True | - | 10 / 46 | ok |
| climb_walk | Climb walk | 3 | (0,340,-213) | True | - | 10 / 46 | ok |
| climb_run | Climb run | 1 | (0,340,-187) | True | - | 6 / 1 | ok |
| climb_run | Climb run | 2 | (0,340,-226) | True | - | 7 / 20 | ok |
| climb_run | Climb run | 3 | (0,340,-230) | True | - | 16 / 5 | ok |
| auto | Hurdle stand (Auto) | 1 | (0,90,-329) | True | - | 7 / 0 | ok |
| auto | Hurdle stand (Auto) | 2 | (0,90,-320) | True | - | 1 / 0 | ok |
| auto | Hurdle stand (Auto) | 3 | (0,90,-334) | True | - | 3 / 0 | ok |

拒绝 / 中断 / 输入场景 ×1：

| 场景 | 期望 | 实测 | 判定 |
|---|---|---|---|
| auto_high | Chooser height=200 -> Catch 拒绝 | `P8_CHOOSER_REJECT choice=Catch_Hurdle_high reason=catch-runtime-not-implemented` | ok |
| reject_clearance | REJECT clearance | `P8_TRAVERSAL_REJECT clearance` | ok |
| reject_high | REJECT height=160 | `P8_TRAVERSAL_REJECT height=160` | ok |
| reject_low | REJECT height=50 | `P8_TRAVERSAL_REJECT height=50` | ok |
| reject_nohit | REJECT no-hit | `P8_TRAVERSAL_REJECT no-hit` | ok |
| abort_stall | ABORT stalled | `P8_TRAVERSAL_ABORT reason=stalled progress=10 pos=-0,127,-90 grounded=False` | ok |
| abort_void | ABORT airborne | `P8_TRAVERSAL_ABORT reason=airborne progress=289 pos=-0,-2792,-337 grounded=False` | ok |
| input | Interact 注入 -> Request/HIT/START/END | `P8_TRAVERSAL_END pos=-0,190,-173 grounded=True yaw=0.0` | ok |

`input` 场景 `TraversalProbe=false`，用 `--console-command "injectinput Interact down"`
触发；日志 `P8_TRAVERSAL_REQUEST` → `HIT` → `START` → `END grounded=True`。

## 5. UE 自由空间参考的可用性

`reference_integration.py` 按 60 Hz `ExtractRootMotionFromAnimation` + attach offset +
单侧碰撞 clamp 复算 UE 参考，`reference-at-new-starts.json` 保存新起点下的 6 个
Mantle/Vault 片段积分。

| clip | start | 首窗误差 cm | 第二窗误差 cm | 结论 |
|---|---:|---:|---:|---|
| Mantle_1_0_stand_F_Lfoot | -52 | 0.3 | 0.6 | 首窗与第二窗均收敛 |
| Vault_1_0_stand_F_Lfoot | -48 | 6.4 | 1222.8 | 首窗可用，第二窗发散 |
| Mantle_1_0_walk_F_Lfoot | 62 | 8.2 | 317.6 | 首窗可用，第二窗发散 |
| Vault_1_0_walk_F_Rfoot | 62 | 0.5 | 550.1 | 首窗可用，第二窗发散 |
| Mantle_1_0_run_F_Lfoot | 227 | 29.3 | 2.0 | 首窗 29 cm；第二窗数值收敛但 final 被墙面 clamp，不作落点基准 |
| Vault_1_0_run_F_Lfoot | 227 | 118.5 | 3677.7 | 首窗偏差 118 cm（S_min 差 12 cm）；第二窗发散 |

- **首窗可作为验收基准**：Mantle stand 两窗 0.3/0.6 cm，参考 final
  `(0,190.4,-169.6)`；矩阵 END -164/-174/-170，差 0.4–5.6 cm。
- **Hurdle 参考与起点无关**（窗口收敛到 attractor）：`simulate-ue-warp.py` 给
  `(0,89.4,-336.3)`；auto 矩阵 END -329/-320/-334，差 2/16/2 cm。
- **第二窗自由空间参考发散**，不是场景位置能修的：Mantle walk 8.2/317.6、
  Vault stand 6.4/1222.8、Vault walk 0.5/550.1、Vault run 118.5/3677.7 cm。
  原因是第二窗之后的参考积分没有 Jolt 碰撞/重力约束，属于碰撞依赖；本轮因此改用
  “首窗 attractor 判定”，不宣称第二窗厘米级对齐。
- **Mantle run 的第二窗**数值收敛（2.0 cm），但参考 final 停在墙面 clamp 的 z=-110，
  不能当落点基准；Vault run 同理落在 z=-4223 的自由空间尾段，均由矩阵的 Jolt
  grounded 结果兜底。
- **Vault run 的 S_min=+239**：场景起点 z=227，距 S_min 差 12 cm（触发上界只到 +228），
  首窗投影为负、首窗误差 118.5 cm；运行时由 Jolt 墙面限制与收口兜住，矩阵仍
  grounded/无 ABORT，但不宣称与 UE 首窗对齐。

## 6. abort_stall 例外与回归口径

`abort_stall` 是**中断夹具**，不是 reach 放置：为保留 `progress < 20` 的 stall 语义
留在 -80，实测 `stalled progress=10`。把它移到 reach 上界 -48 后 `progress=42`，
中断不再触发，所以该场景是本轮唯一的放置例外。

Climb/Hurdle 的起点变了，落点不能与旧距离逐厘米比较；按“顶面 grounded、无 ABORT、
仍在历史抖动邻域”口径回归：

| 场景 | 旧基线 END | P21 END（3 次） | grounded |
|---|---|---|---|
| auto | -339/-329/-335/-353/-352；UE 参考 -336.3 | -329/-320/-334 | True |
| climb | -153..-228（P19 -207） | -231/-188/-229 | True |
| climb_walk | -195..-213（历史 -204/-206） | -204/-195/-213 | True |
| climb_run | -147..-189（历史 -185/-189） | -187/-226/-230 | True |

Climb stand 三次 -188/-229/-231（历史 -153..-228，最大超出 3 cm）；Climb walk 与历史
一致；Climb run 比历史更远，但起点远了 306 cm，且仍落在顶面 (y=340) 且 grounded，
记为通过、数值不宣称。

## 7. 限制

1. Vault run 的 S_min 超出 reach 上界 12 cm；首窗仍略奇异，未对齐 UE attractor。
2. Mantle walk / Vault stand / Vault walk / Vault run 的第二窗参考积分发散；本轮只做
   首窗 attractor 判定，未改参考数学。
3. `abort_stall` 保持 -80，不是 reach 放置；唯一场景放置例外。
4. 探针 reach 是夹具口径（名义 gait 速度）；真实 gameplay 路径仍用
   `LinearVelocity`，未改。
5. 场景 camera 未随 probe 远移（仍 look_at 原点方向）；画面不参与验收。
6. C# 夹具改动需要重编 `Gameplay.dll`；`main.exe` 未重编（引擎源码未改）。

## 8. 证据

```text
artifacts/gasp-reach-matrix/
  derive_reach.py / reach-derivation.json     UE A/W/attractor/S_min/S_anim + 触发包络
  apply_placements.py / placements.json/.md    场景放置与逐条依据
  reference_integration.py                     UE SkewWarp 参考积分（首窗/第二窗误差）
  reference-at-new-starts.json                 新起点下 6 个 MV 片段的参考积分
  run-matrix.ps1 / matrix/*.log                38 次矩阵原始日志 + manifest.tsv
  matrix-summary.json / summarize.py           END/ABORT/REJECT/首窗判定
  tests/                                       预跑/失败复现日志（含 z=-20 no-hit）
  config.json.orig / restore-config.ps1        config 字节备份与恢复校验
```

`tests/ztest-mantle-run-far.log` 是 probe z=-20 的 `P8_TRAVERSAL_REJECT no-hit`
（reach 上界实测）；临时场景 `SC_gasp_traversal_ztest.json` 已删除。
运行时配置临时插入 `"msaa": 2` 与 `"scripts": {"enabled": true, "autoPlay": true}`，
恢复后 SHA256 与备份一致：`F7BB70235FFA36D6EBDA6BC5D3F1CE9941FC8E71C6918B903361070C9054F102`。
