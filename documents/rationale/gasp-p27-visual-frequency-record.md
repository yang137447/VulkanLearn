# GASP P27 视觉补齐、`--frame-limit` 与 30/60/120 Hz 矩阵记录

状态：执行记录，非当前契约。2026-10-21。覆盖 18 个 traversal 场景的 GASP 关卡可见
mesh 补齐、`--frame-limit` 启动参数，以及仓库内与独立包两条 30/60/120 Hz 矩阵。
行为以
[验证入口与"改动 → 验证"映射](../contracts/testing.md)、
[动画运行时契约](../contracts/animation-runtime-skinning.md) 与源码为准。

| 仓库 | 基线提交 | 本轮改动 |
| --- | --- | --- |
| 引擎 | `98d791d`（`【Mod】add render frame-limit launch option`，本轮实现提交；前序 `c4b2a0b`） | 4 个 `source/engine` 文件，+68 行 |
| 资源 | `882cc90`（`【Mod】render GASP traversal level geometry`，本轮实现提交；前序 `026d480`） | 18 个 `SC_gasp_traversal*.json`，+758/-18 |

最终运行二进制 `build-vscode/development/bin/main.exe`：936,061,953 B、
2026-10-21T13:21:08.6178914+08:00、SHA256
`7289BB5E15D10D29033C89E99047BD99A21430F16831972E95F7CDFD6457CB08`。
§2 的 smoke 与 §3 的三档矩阵都用这个二进制；§1 的截图用更早的
936,076,179 B（`CD9529…33D1`），差异见 §1.4。

## 1. 视觉补齐：18 场景 / 37 个可见 mesh

### 1.1 规则与清单

18 个 `Maps/SC_gasp/SC_gasp_traversal*.json` 各补了与 physics body 同 transform/尺寸的
`type: "mesh"` 可见对象；生成脚本 `artifacts/gasp-p27-visual/add_traversal_visuals.py`，
逐场景清单与插入位置见 `artifacts/gasp-p27-visual/scene-visuals.txt`：

| 规则 | 值 |
| --- | --- |
| 模型：障碍/墙/屏障/天花板 | `Maps/SC_gasp/Meshes/LevelTextured/SM_gasp_055.json`（`LevelTextured/MI_gasp_048`） |
| 模型：地面 | `Maps/SC_gasp/Meshes/LevelTextured/SM_gasp_025.json`（`LevelTextured/MI_gasp_018`） |
| 缩放 | `scale = 2 * halfExtents / 100`（100 cm 立方体，枢轴在最小角） |
| 位置 | `position = physics.position - halfExtents` |
| 旋转 | 复制 physics `rotation` |
| 材质 | 模型自带，**无 `materialOverride`**（18 个文件里该字符串只出现在 description 的说明句） |
| physics | 37 个 body 全部保留，`halfExtents`/`position`/`rotation` 未改 |

数量：**37** 个 mesh 对象 = 15 个场景 × 2（障碍 + 地面）+ `abort_stall` 3（+停滞屏障）
+ `reject_clearance` 3（+顶棚）+ `reject_nohit` 1（只有地面）。按模型分：
19 × `SM_gasp_055`、18 × `SM_gasp_025`。

示例（与 `scene-visuals.txt` 一致）：墙 `halfExtents=[40,50,60]`、中心 `(0,50,-200)`
→ `position (-40,0,-260)`、`scale (0.8,1.0,1.2)`；地面 `[10000,50,10000]`、
中心 `(0,-50,0)` → `position (-10000,-100,-10000)`、`scale (200,1,200)`；
`abort_void` 的短地面 `[10000,50,140]`、中心 `(0,-50,-120)`
→ `position (-10000,-100,-260)`、`scale (200,1,2.8)`。

每个场景的 `description` 在既有"非论文验证场景。"前插入一句说明（`auto`/`auto_high`
没有该标记句，追加到末尾）。资源侧 diff 为 18 行 description 被替换 + 758 行新增，
没有任何 physics 行被删改。

### 1.2 截图与标记（18/18）

采集脚本 `artifacts/gasp-p27-visual/capture-scenes.ps1`：`--no-dev-ui --initial-scene <scene>`，
等标记后 250 ms 注入引擎 `screenshot <绝对路径>`。结果
`captures 18/18, markers 18/18, passed 18/18`，全部 `exit=0`、stderr 0 B，
2026-10-21T13:15:41→13:18:06（约 145 s）。

| tag | 场景（`Maps/SC_gasp/`） | 命中标记 | PNG 字节 |
| --- | --- | --- | --- |
| traversal | `SC_gasp_traversal.json` | `END pos=-0,190,-161` | 613,842 |
| abort_stall | `SC_gasp_traversal_abort_stall.json` | `ABORT stalled progress=10 pos=-0,122,-90` | 693,109 |
| abort_void | `SC_gasp_traversal_abort_void.json` | `ABORT airborne progress=324 pos=-0,-2745,-372` | 39,683 |
| auto | `SC_gasp_traversal_auto.json` | `END pos=-0,90,-335` | 652,258 |
| auto_high | `SC_gasp_traversal_auto_high.json` | `CHOOSER_REJECT catch-requires-falling` | 635,430 |
| climb | `SC_gasp_traversal_climb.json` | `END pos=-0,340,-175` | 598,414 |
| climb_run | `SC_gasp_traversal_climb_run.json` | `END pos=-0,340,-159` | 601,331 |
| climb_walk | `SC_gasp_traversal_climb_walk.json` | `END pos=-0,340,-158` | 600,411 |
| input | `SC_gasp_traversal_input.json` | `END pos=-0,190,-172` | 616,446 |
| mantle_run | `SC_gasp_traversal_mantle_run.json` | `END pos=-0,190,-206` | 612,972 |
| mantle_walk | `SC_gasp_traversal_mantle_walk.json` | `END pos=-0,190,-200` | 613,540 |
| reject_clearance | `SC_gasp_traversal_reject_clearance.json` | `REJECT clearance distance=95 bp=ceiling` | 650,341 |
| reject_high | `SC_gasp_traversal_reject_high.json` | `REJECT height=160` | 629,141 |
| reject_low | `SC_gasp_traversal_reject_low.json` | `REJECT height=50` | 620,699 |
| reject_nohit | `SC_gasp_traversal_reject_nohit.json` | `REJECT no-hit` | 616,749 |
| vault | `SC_gasp_traversal_vault.json` | `END pos=-0,90,-383` | 657,204 |
| vault_run | `SC_gasp_traversal_vault_run.json` | `END pos=-0,90,-342` | 654,370 |
| vault_walk | `SC_gasp_traversal_vault_walk.json` | `END pos=-0,90,-336` | 652,026 |

BMP 载荷均为 2,764,854 B（1280×720 24 位完整）；`capture.json` 另有
`screenshotSaved=true`、`nonBlack`（std 阈值）、`exitedCleanly=true`、`stderrBytes=0`。
截图运行的 END 数字与 §3 矩阵同场景略有差异（不同二进制、不同采样时刻），
这里只作视觉证据，不作为落点验收。

### 1.3 contact-sheet

`artifacts/gasp-p27-visual/contact-sheet.png`：1248×1626、1,257,113 B、3 列 × 6 行 18 格、
`missing=0`、均值 120.591/116.981/111.811、std 92.804/89.964/86.882。
`contact-sheet-manifest.json` 记录 18 个 tag 与源 PNG 路径。缩略图只用于快速浏览，
像素级判断仍看单张 PNG。

### 1.4 caveat

- `abort_void` 的截图接近纯黑：`pngMean 3.364/4.204/3.998`、PNG 仅 39,683 B。角色已坠到
  `y=-2745`，画面内没有可读几何；BMP 载荷完整，所以它只证明 ABORT 发生了，**不能**当作
  "场景可见 mesh 已出图"的证据。其余 17 张都有 GASP LevelBlock/地面可见。
- 截图二进制的 SHA256 是 `CD95290696A7D7A462BA31455D03DCA903D94ACDF86E668B966BC97AEC9633D1`
  （936,076,179 B / 2026-10-21T13:14:10.3517120+08:00），不是 §2 之后的最终二进制。
  采集脚本的启动参数里没有 `--frame-limit`，限速路径未参与，所以截图结论不受影响；
  `exeUnchangedDuringCapture=true`、临时改写 config 后 SHA256 字节级恢复（
  `configRestoredByteExact=true`）、18 个场景 hash 前后一致、无残留 `main.exe`。

## 2. `--frame-limit` 启动参数

### 2.1 语义

| 输入 | 行为 |
| --- | --- |
| 缺省（不传） | 默认 `0`，保持原有不限速渲染 |
| `1..1000` | 每个渲染帧从帧起点补足到 `1/hz` 节拍 |

- 实现位置：`source/engine/launchOptions.*`（解析与 usage）与
  `source/engine/engineLoop.*`（成员 `frameLimitHz`、`Run()` 补帧）。
- 只限**渲染主循环节拍**：`Tick()` 之后等到 `frameStart + 1/hz`。等待用
  `SDL_DelayPrecise(remaining - 0.2 ms)`，余下 0.2 ms 由自旋吸收唤醒/调度延迟。
- `RuntimeClock` 仍按真实经过时间推进，所以 `time.delta` 与动画/脚本 tick 的 dt 会随
  限速后的帧间隔变大；`ScriptRuntime` 内部的 1/60 accumulator 固定物理子步不变，
  即 **Jolt 仍是固定 60 Hz 子步**。30/60/120 Hz 口径与 P5/P6 的正式控制器模块夹具
  （`FormalFrameRates/ScriptAnimationRateTest`，11 项 × 30/60/120）一致。
- 不是 runtime test flag：`RejectRuntimeTestConflict` 只覆盖三个 runtime test 标志，
  `--frame-limit` 是普通启动参数，不改变三项 Vulkan runtime gate 的约束与串行要求。

### 2.2 参数校验

| 输入 | stderr | 结果 |
| --- | --- | --- |
| `--frame-limit`（缺值或下一个是选项） | `--frame-limit requires a rate in Hz between 1 and 1000.` | exit 1，打印 usage |
| `--frame-limit 0` / `abc` / 负值 / `>1000` | `--frame-limit rate must be an integer between 1 and 1000 Hz.` | exit 1，打印 usage |

三种失败都在约 0.4–0.45 s 内退出，`windowTitleAtStart=''`（未创建窗口），
`usagePrinted=True`。**显式 `0` 会被拒绝**：`0` 只是"不传参"的默认值语义，
不是可传入的取值。

### 2.3 实测（v3，最终二进制）

`artifacts/gasp-p27-frequency/frame-limit-smoke.txt`，三次窗口标题采样：

| 参数 | 采样 FPS | 退出 |
| --- | --- | --- |
| `--frame-limit 30` | 29 / 29 / 30 | CloseMainWindow，exit 0 |
| `--frame-limit 60` | 59 / 59 / 59 | exit 0 |
| `--frame-limit 120` | 119 / 119 / 119 | exit 0 |
| 不传 | 718 / 781 / 720 | exit 0 |

`SUMMARY failures=0`；判定阈值为 `±max(2, 5%)`（30→28–32、60→57–63、120→114–126）。

### 2.4 实现历史

| 版本 | 等待方式 | 30/60/120 采样 | smoke |
| --- | --- | --- | --- |
| v1 | `std::this_thread::sleep_for` 睡满 | 26–28 / 53–55 / 67–68 | `failures=2`（60、120 超 ±5%；30 有落在容差内的样本） |
| v2 | `CreateWaitableTimerExW(CREATE_WAITABLE_TIMER_HIGH_RESOLUTION)` 睡到 deadline−1 ms + 1 ms 自旋（创建失败回退 `sleep_for`） | 29–30 / 59–60 / 119–120 | `failures=0`，仍有唤醒抖动 |
| v3 | `SDL_DelayPrecise` 睡到 deadline−0.2 ms + 0.2 ms 自旋 | 29–30 / 59 / 119 | `failures=0` |

v1 的 120 Hz 只有 67–68，相对误差约 44%：`sleep_for`/`Sleep` 精度受系统定时器节拍限制
（v2 源码注释实测本机约 15.6 ms），睡满会吃掉 120 Hz 的 8.3 ms 帧预算。v2 把
误差压到 ±1 帧，v3 换成 SDL 自带的高精度等待并把自旋余量从 1 ms 收到 0.2 ms，
三档都稳定在目标 −1 帧（窗口标题按整数显示）。v2 实现备份在
`artifacts/gasp-p27-frequency/v2-waitable-timer-backup/`，v1/v2 的 smoke 原文分别是
`frame-limit-smoke-v1-sleep-for.txt`、`frame-limit-smoke-v2-waitable-timer*.txt`。

### 2.5 构建

4 个文件 **+68 行、0 删除**：`engineLoop.cpp +38`、`engineLoop.h +2`、
`launchOptions.cpp +24`、`launchOptions.h +4`。`main.exe` 链接成功
（`build-main.log`、`build-main-v5.log`），**C++ 编译 0 warning**。
同一日志里的 `GaspCharacter.blueprint.cs(439,24): warning CS8602` 是资源侧 C# 既有警告，
与本参数无关，不能记成"本轮 0 warning"的一部分。

## 3. 仓库内 30/60/120 Hz 矩阵

### 3.1 口径

21 个场景 × 3 档 = **63 次**，每场景 23 s 采集窗口（单档约 8.5–9.5 分钟）；
脚本 `artifacts/gasp-p27-frequency/run-matrix.ps1`，逐场景断言
`exit=0`、stderr 0 B、config 恢复、无残留、期望的 END/ABORT/REJECT/chooser 标记。
场景集合：`package-probe`；Mantle/Vault/Climb 各 stand+walk+run（9）；
`auto-hurdle`；`auto-high-reject`；`reject-high`；`abort-void`；`abort-stall`；
`input-mantle`；`catch-high`；`catch-jump`；`catch-family-hurdle/mantle/vault`。
三档使用同一二进制（SHA256 `7289…CB08`）与同一 config（SHA256 `F7BB…F102`）。
包内三档矩阵（§4）改用 35 s 窗口与 20/30 s FPS 采样，口径不同，只对照硬判据、
不逐值比较。

| 档位 | 通过 | 失败 | overall | 平均 FPS（5 s 原始口径） |
| --- | --- | --- | --- | --- |
| 60 Hz | 21/21 | 0 | PASS | 59.14（59–60） |
| 30 Hz | 20/21 | 1（重试 1 次仍失败） | FAIL | 28.14（13–29） |
| 120 Hz | 21/21 | 0 | PASS | 100.6（56–119） |

**62/63**。失败只有 `catch-family-mantle@30 Hz`，`deterministic-fail`，重试后 `passedAfterRetry=False`。

### 3.2 30 Hz `catch-family-mantle`（仓库内 2/2 失败）

两次运行都缺 `P8_CATCH_LANDED` 与 `P8_TRAVERSAL_END`，并多出
`P8_TRAVERSAL_ABORT reason=catch-missed-top progress=0`：

```text
P8_CHOOSER mode=Falling family=Mantle depth=160 height=81 choice=Catch_Mantle_med
P8_CATCH_AUTO_TRIGGER mode=Falling height=81 action=Catch_Mantle_med pos=0,200,-110 vy=-154   （重试 -170）
P8_WARP_WINDOWS asset=FrontLedge[0.838,1.002]=(0,290,-120)
P8_CATCH_STATE time=1.08/2.99 pos=0,186,-86  ... warp=FrontLedge:done landed=False targetY=290
P8_CATCH_STATE time=1.35/2.99 pos=0,224,-93  ... landed=False
P8_CATCH_STATE time=3.83/2.99 pos=0,162,-110 ... landed=False
P8_MOVEMENT_MODE from=Catch to=Walking grounded=True vy=-60 catch=False
P8_TRAVERSAL_ABORT reason=catch-missed-top progress=0 pos=0,90,-110
```

- warp 窗口报 `FrontLedge:done`，但整段 montage（3.833 s / 2.994 s）没有抬到顶面 290：
  y 从 200 掉到 186 再弹到峰值 **224（首跑）/ 234（重试）**，blend-out 时回落到
  162/173，最后在 `y=90`（地面）转 Walking。
- z 从起点 **-110** 在窗口内回退到 **-86（首跑）/-93（重试）**，随后回到 -110 并保持。
- 60 Hz 与 120 Hz 同场景通过：`P8_CATCH_LANDED 1.815/2.994 y=288`、`1.763/2.994 y=288`，
  END `y=290`（z=-162 / -155）。

**候选解释（未做源码级证明）**：触发瞬间下落速度差一个量级——30 Hz 为 `vy=-154/-170`，
60/120 Hz 为 `-72/-40`；30 Hz 每渲染帧含 2 个固定物理子步，抓边判定落在更深的下降段，
warp 窗口起步时剩余竖直位移不足。这是相关性记录，不能写成已定位根因，也没有为它调参
或改资产；该现象与
[P26 记录](gasp-p26-chooser-families-record.md) §6.5 与 §7 第 5 条已写的"Mantle 换 GASP
mesh 后偶发丢窗口（`warp=FrontLedge:done landed=False`）"是同一类脆弱点。

注意这只是 **in-repo 环境**的可重复失败：包内同一场景、同一二进制、同一 30 Hz cap 首跑
通过（§4.4）。因此结论是"30 Hz catch 入口/warp 窗口对启动/帧相位敏感"，不是单一确定性
Mantle 缺陷；in-repo 2/2 与包内通过两边的证据并列保留。

### 3.3 帧率与一致性

**仓库内稳态帧率**（15 s 窗口，C 切片最终报告）：59.05 / 29.00 / 118.10，
偏差 −1.6% / −3.3% / −1.6%。`analysis.txt` 的 5 s 口径（59.14 / 28.14 / 100.6，
最低 59/13/56）包含启动预热，原始值保留，不作为稳态数字；`P7_PROBE` 显示启动偏移
约 2.4–3.0 s。包内 warm 后的 20/30 s 采样为 59.05 / 29.0 / 115.31（§4.6），
120 Hz 比目标低约 4%。

**帧率效应**：`package-probe` 每帧一条 `P7_PROBE`，23 s 窗口行数
60=1231、30=617、120=2349（30/60=0.50、120/60=1.91），三档最后一条 z=-1285/-1300/-1300。
`P8_TRAVERSAL action=` 采样行按蒙太奇时间 0.25 s 节流，30/60/120 比值 0.92–1.10，
**不能**当帧率证据（`analysis.txt` 的 sample-count ratio 一栏同理）。

**跨档一致性**：所有有 END 的 16 个场景里，15 个三档齐全且 END y 完全一致——
Mantle 190、Vault 90、Climb 340、Hurdle(auto) 90、input Mantle 190、Catch 290；
`catch-family-mantle` 只有 60/120 两档（290）。Catch 的 LANDED y=288–290
（`catch-high` 290/290/290、`catch-jump` 290/290/290、`family-hurdle` 290/290/290、
`family-vault` 289/290/289、`family-mantle` 288/—/288），LANDED 时间 1.28–1.99 s。
END z 的跨档散布：Catch 场景 19–71 cm（`catch-jump` 19、`family-vault` 43、
`catch-high` 45、`family-hurdle` 71、`family-mantle` 两档 7），非抓边场景 ≤18 cm
（`input-mantle` 18、`climb-stand` 11、`vault-run` 11、`climb-walk` 8、`mantle-walk` 7）。
y 稳定、水平落点随帧率有厘米级散布（触发/收口落在不同渲染帧所致）。

## 4. 独立包三档矩阵

证据：`artifacts/gasp-p27-package/RESULTS.md`（逐场景表、§3 冷启动、§4、§5 对照）、
`comparison.md`、`analysis.json`。导出与包状态见 4.1，三档结果见 4.2，冷启动见 4.3，
30 Hz 对照见 4.4，硬判据/END z 见 4.5，FPS 见 4.6。

### 4.1 导出与包状态

```powershell
build\bin\vl_export_game.exe `
  --project-root D:\YYBWorkSpace\GitHub\VulkanLearn `
  --initial-scene Maps/SC_gasp/SC_gasp_traversal.json `
  --output artifacts\gasp-p27-package-01 `
  --dynamic-assets artifacts\gasp-p27-package\dynamic-assets.json `
  --binary-directory build-vscode\development\bin `
  --dotnet-root artifacts\dotnet-sdk
```

- 2026-10-21T14:05:59→14:07:36（96.3 s），exit 0，stderr 0 B。
- 导出 **1810 files / 1,638,002,923 B**，与 `game-export-manifest.json` 的 files/bytes
  一致（manifest 自身 376,726 B 不列入清单；含 manifest 的包内实际为 1811 文件 /
  1,638,379,649 B）。
- `dynamic-assets.json` **23 项** = `MotionMatching/DB_gasp.json`、`PS_gasp.json`
  + **21 个矩阵场景**（不是 18 个 traversal）；`resources/Maps/SC_gasp/` 下 21 个
  `SC_*.json` 与 `artifacts/gasp-p27-matrix.json` 一致，23 项逐项存在。
- 包内 `main.exe` 与仓库二进制同一：936,061,953 B、SHA256 `7289…CB08`；`managed/`
  含 `Gameplay.dll` 与私有 .NET 8.0.32；`config/config.json` 改为包内相对路径
  （`resourcePath=resources`、`scripts.autoPlay=true`、开发 UI 关闭），1017 B、
  SHA256 `4886…9B0B`，运行前后不变。
- 运行后包内 1812 文件 / 1,638,992,679 B：34 个 shader/material 生成物（24 个
  `shader/glsl/generate/*.glsl`、6 个 material debug spv、4 个 deferredLighting
  debug/spv）与新增 1 个 `resources/Generated/MotionMatching/<hash>.json`；
  `main.exe`、`config.json`、`game-export-manifest.json` 未被改写。包体积/文件数以
  manifest 的 1810 项为准。

### 4.2 三档结果

每场景一个进程，`WorkingDirectory` 固定包根、只跑包内 `.\main.exe`，参数
`--no-dev-ui --initial-scene <scene> --frame-limit <hz>`（`input-mantle` 追加
`injectinput Interact down`）；不 patch 包 config；35 s 窗口，20 s/30 s 采样窗口标题
FPS，并记录 startup（进程启动→首条 `P7_PROBE`/`P8_TRAVERSAL_START`）。

| rate | 首跑通过/失败 | 重试后 | 重试场景 | FPS avg/min/max | startup avg/min/max (s) |
| --- | --- | --- | --- | --- | --- |
| 60 | 21/0 | **21/0** | 无 | 59.05 / 59 / 60 | 4.08 / 3.28 / 5.88 |
| 30 | 20/1 | **21/0**（重试 1 次） | `catch-family-hurdle` | 29.0 / 29 / 29 | 4.70 / 3.61 / 6.37 |
| 120 | 21/0 | **21/0** | 无 | 115.31 / 111 / 119 | 4.56 / 3.36 / 5.36 |

- 64 次矩阵运行（63 首跑 + 1 重试）全部 `exit=0`、stderr 0 B、包内 config SHA 不变
  （`4886…9B0B`）、无残留 `main`、`packagePathEvidence=True`（shader 根与 scene
  路径都指向包目录；含 warm-up 共 65/65）。
- 30 Hz 唯一首跑失败：`catch-family-hurdle` `ABORT stalled progress=184
  pos=-0,247,-294 grounded=False`，重试 PASS（LANDED 290、END 290/-200）。
- 120 Hz 的 115.31 比目标低约 4%（包内运行开销/采样相位），不写成"精确 120"。

逐场景（y/z = `P8_TRAVERSAL_END`，catch 记 `LANDED y` + END y/z；cm）：

| tag | 60 Hz | 30 Hz | 120 Hz |
| --- | --- | --- | --- |
| package-probe | z=-1300 yaw=180 | z=-1300 yaw=180 | z=-1300 yaw=180 |
| mantle-stand | 190/-173 | 190/-167 | 190/-166 |
| mantle-walk | 190/-203 | 190/-209 | 190/-200 |
| mantle-run | 190/-209 | 190/-208 | 190/-208 |
| vault-stand | 90/-361 | 90/-376 | 90/-376 |
| vault-walk | 90/-338 | 90/-335 | 90/-339 |
| vault-run | 90/-336 | 90/-323 | 90/-336 |
| climb-stand | 340/-174 | 340/-166 | 340/-180 |
| climb-walk | 340/-174 | 340/-164 | 340/-165 |
| climb-run | 340/-169 | 340/-168 | 340/-162 |
| auto-hurdle | 90/-337 Hurdle/h100 | 90/-336 | 90/-334 |
| auto-high-reject | `catch-requires-falling` | 同左 | 同左 |
| reject-high | `height=160` | 同左 | 同左 |
| abort-void | `airborne` | 同左 | 同左 |
| abort-stall | `stalled` | 同左 | 同左 |
| input-mantle | 190/-165 | 190/-167 | 190/-170 |
| catch-high | L290 290/-173 | L290 290/-210 | L290 290/-161 |
| catch-jump | L290 290/-205 | L290 290/-149 | L290 290/-169 |
| catch-family-hurdle | L290 290/-192 | (retry) L290 290/-200 | L290 290/-192 |
| catch-family-mantle | L289 290/-176 | L288 290/-161 | L288 290/-173 |
| catch-family-vault | L290 290/-351 | L290 290/-365 | L289 290/-326 |

`P7_PROBE_FINISHED` 三档都有，最后一条 `P7_PROBE t≈32.1/31.9/32.1`、`yaw=180`、
`z=-1300`；`input-mantle` 有 REQUEST/START/MONTAGE_START/END，样本 0 属该 gameplay
路径的预期。

### 4.3 冷启动 vs warm-up（独立事实，不计入矩阵）

- 包内第一次启动（`package-probe`，23 s 窗口）：`Shader cache miss:
  pass/deferredLighting`，依赖 `pass/generate/M_deferredLightingParamter.glsl` 变化，
  `hits=17 misses=1 compiled=1 shaderc=4 elapsedMs=725.937`；23 s 内 `P7_PROBE` 只到
  `t=4.47`，无 `P7_PROBE_FINISHED`，13.5 s/15 s 标题采样为空。
- 冷启动耗时约 13–18 s；5 次冷启动运行（`_cold-start/`，含冷 `mantle-stand`
  END y=188）整体作废、原样保留，**不写成场景失败**，也不计入统计。
- 40 s warm-up（`_warmup/`）后：`Shader cache hit`、`hits=18 misses=0`、
  `elapsedMs=109.0`、startup 4.11 s、`END pos=-0,190,-173`。矩阵在 warm 状态下运行，
  startup 稳定在 3.28–6.37 s。

### 4.4 30 Hz 对照：不是单一确定性 bug

| 环境 | 场景 | 结果 |
| --- | --- | --- |
| in-repo | `catch-family-mantle` | **2/2 FAIL** `catch-missed-top progress=0`，缺 LANDED/END，z -110→-86/-93 |
| 包内 | `catch-family-mantle` | **PASS（首跑）** LANDED 1.839/2.994 y=288、END 290/-161、chooser Mantle/160 |
| 包内 | `catch-family-hurdle` | 首跑 **FAIL** `stalled progress=184 pos=-0,247,-294`；重试 PASS（LANDED 290、END 290/-200） |
| in-repo | `catch-family-hurdle` | 30 Hz 首跑 PASS（END 290/-147） |

同一二进制（SHA256 相同）、同一 30 Hz cap，两个环境各暴露一例不同的 catch abort，
各自的另一例通过。因此 **30 Hz 是 catch 入口/warp 窗口在低帧率 + 启动/帧相位下的
脆弱点，对环境敏感**，不是可稳定复现的单一 Mantle 缺陷；in-repo 的 2/2 与包内首跑
通过并列保留，不能用一边否定另一边。未改代码/参数，不声称修复。

### 4.5 硬判据与 END z

- 硬判据全部一致：END y（mantle 190 / vault 90 / climb 340 / hurdle·catch 290 /
  input 190）、grounded、LANDED y 288–290、chooser family/depth/choice、
  reject/abort 原因、probe `z=-1300`（-1304±40）、`yaw=180`。
- END z 差异 1–53 cm：非 catch 0–23 cm，catch 最大 53 cm（`catch-family-hurdle`
  30 Hz 重试 -200 vs in-repo -147），仍在 in-repo 自身 catch 跨档散布 19–71 cm 内。
  `catch-jump` 包内跨档散布 56 cm（in-repo 19 cm）是最大一项，但三档 LANDED/END
  y=290、grounded=True、auto-trigger height 77–80 都满足，记为 catch 落地相位散布放大。
- `vault-stand` 60（+14）、`vault-run` 30（+23）、`catch-high` 120（+46）、
  `catch-family-mantle` 60/120（-14/-18）略超同场景 in-repo 跨档散布，但 y/grounded/
  事件无差异，归为"略超散布、硬判据一致"。

### 4.6 FPS 对照

| rate | 包内 20/30 s | in-repo 5/15 s | 说明 |
| --- | --- | --- | --- |
| 60 | 59.05（59–60） | 59.14（59–60） | 一致 |
| 30 | 29.0（29/29） | 28.14（13–29） | in-repo 低值来自 5 s 采样撞上 catch 卡顿；包内 warm 后稳定 29 |
| 120 | 115.31（111–119） | 100.6（56–119） | 包内更接近 120 cap，仍低约 4% |

## 5. 未闭环与结论

1. **P9 三帧率门禁未全绿**：仓库内 62/63（60/120 全过，30 Hz `catch-family-mantle`
   2/2 `catch-missed-top`）；包内 63 次首跑 + 1 次重试后三档均 21/21，但 30 Hz
   `catch-family-hurdle` 首跑 `stalled`、而包内 30 Hz `catch-family-mantle` 首跑通过。
   两个环境在 30 Hz 各暴露一例不同的 catch abort，60/120 两边零失败——**不能写
   "30 Hz 通过"，也不能据此声称修复**（未改代码/参数）。
2. 包内矩阵已完成（§4）；冷启动与首次 `deferredLighting` 重编已隔离成独立事实
   （§4.3），不计入场景失败。包内结论覆盖 21 场景 × 3 档的硬判据，仍不等于资产/画质
   验收（P9/P10/P11 的 1 m 子集结论不变）。
3. PoseSearch B1（per-member 采样区间 + 独立 traversal DB + 一次性 query）未做；
   Catch 侧无收益（窗口≈1 帧），非 Catch 家族才是收益点。
4. 旋转仍只有 yaw-only `Slerp`：非 `Slerp` 旋转方法/旋转参数、pitch/roll 未做。
5. Montage 质量（混合、非 `Linear` easing、section 内 rate/offset）仍是后续项。
6. 30 Hz catch 的机制只有相关性解释（`vy` 更深 + 每渲染帧 2 个物理子步 +
   启动/帧相位），未做源码级定位。
7. `abort_void` 截图近黑，不构成几何可见性证据。
8. **P27 不是 M2/最终交付完成**：三帧率门禁未全绿（30 Hz catch flaky）、资产质量与
   上面的旋转/PoseSearch 项仍在。

## 6. 证据

```text
artifacts/gasp-p27-visual/
  summary.txt, capture.json, capture-meta.json, contact-sheet.png,
  contact-sheet-manifest.json, scene-visuals.txt, add_traversal_visuals.py,
  capture-scenes.ps1, capture_evidence.py,
  <tag>.{bmp,png,log,err.log}（18 组：traversal, abort_stall, abort_void, auto,
  auto_high, climb, climb_run, climb_walk, input, mantle_run, mantle_walk,
  reject_clearance, reject_high, reject_low, reject_nohit, vault, vault_run,
  vault_walk）

artifacts/gasp-p27-frequency/
  analysis.txt, matrix-60.txt, matrix-30.txt, matrix-120.txt,
  frame-limit-smoke.txt（v3）, frame-limit-smoke-v1-sleep-for.txt,
  frame-limit-smoke-v2-waitable-timer.txt, frame-limit-smoke-v2-waitable-timer-run1.txt,
  build-main.log, build-main-v2..v5.log,
  hz30/hz60/hz120/default.{stdout,stderr}.log,
  invalid-0|invalid-abc|invalid-missing.{stdout,stderr}.log,
  probe*.stdout.log, probe-30/120.*.log,
  rate-30/catch-family-mantle.log, rate-30/catch-family-mantle-retry1.log,
  rate-30/, rate-60/, rate-120/, summary-{30,60,120}.json,
  run-matrix.ps1, run-scene.ps1, run-smoke.ps1, summarize.ps1,
  config.json.orig, sleep-probe*.cpp, sdl-delay-probe.cpp,
  v2-waitable-timer-backup/

artifacts/gasp-p27-package/
  EXPORT.md, RESULTS.md, comparison.md, result-table.md, analysis.json,
  export-stdout.log, export-timing.json, dynamic-assets.json,
  matrix-30.txt, matrix-60.txt, matrix-120.txt,
  summary-30.json, summary-60.json, summary-120.json,
  rate-30/（含 catch-family-hurdle-retry1）, rate-60/, rate-120/,
  _cold-start/（5 次作废冷启动）, _warmup/,
  run-scene.ps1, run-matrix.ps1, analyze.py, make-table.py
artifacts/gasp-p27-package-01/（导出 1810 项；运行后 +1 个 MM 缓存、34 个
  shader/material 文件变化）
```

`artifacts/` 被 gitignore，所以数字结论只活在本记录里；单次运行的原始日志不随提交保存。

## 7. 提交与边界

| 仓库 | 提交 | 内容 |
| --- | --- | --- |
| 引擎 | `98d791d` | `【Mod】add render frame-limit launch option`：4 个 `source/engine` 文件，+68 行 |
| 资源 | `882cc90` | `【Mod】render GASP traversal level geometry`：18 个 `SC_gasp_traversal*.json`，+758/-18 |
| 引擎（文档） | 本记录所属 Doc 提交 | 本记录 + [测试与验证](../contracts/testing.md) 的 P27 段落 + 两个 plan 的同步更新 |

- `source/` 与资源仓库在提交后没有本轮残留改动；`--frame-limit` 的 4 个文件与 18 个场景
  分别落在 `98d791d`、`882cc90`。
- `documents/routing.md`（M）与 5 个未跟踪的 `documents/plan/rendering/*.md` 属于另一条
  工作流，本轮**未改、未提交**。
- `artifacts/` 被 gitignore，证据目录不入库（见 §6 首段说明）。
