# GASP P10 Climb 独立包性能与闭合记录

状态：执行记录（rationale），非当前契约。记录 2026-10-19 对
`artifacts/gasp-p10-climb-package-04` 的独立测量；只读源码与资源，未提交，
未改动既有文档。

## 1. 包与清单

包根：`artifacts/gasp-p10-climb-package-04`。

`game-export-manifest.json`：

| 字段 | 值 |
|---|---|
| `format` / `hash` | `1` / `BLAKE3-256` |
| `initialScene` | `Maps/SC_gasp/SC_gasp_traversal_climb.json` |
| `files` | 1721 |
| 清单 `size` 合计 | 1,592,357,001 B（约 1.59 GB / 1.48 GiB） |
| `dynamicAssets` | 8 项，含 stand/run/walk 三个 Climb 场景 |

必需文件按路径全部存在（大小为清单值）：

| 路径 | 字节 |
|---|---:|
| `main.exe` | 907,060,671 |
| `managed/Gameplay.dll` | 39,424 |
| `resources/Maps/SC_gasp/SC_gasp_traversal_climb.json` | 3,327 |
| `resources/Maps/SC_gasp/SC_gasp_traversal_climb_run.json` | 3,328 |
| `resources/Maps/SC_gasp/SC_gasp_traversal_climb_walk.json` | 3,333 |
| `resources/Maps/SC_gasp/SC_gasp_traversal.json` | 2,451 |
| `resources/Maps/SC_gasp/SC_gasp_traversal_vault.json` | 3,189 |
| `resources/Maps/SC_gasp/Source/M_Neutral_Traversal_Climb_Start_2_5_stand_F_Lfoot.fbx` | 1,487,344 |
| `resources/Maps/SC_gasp/Source/M_Neutral_Traversal_Climb_Start_2_5_run_F_Lfoot.fbx` | 1,678,864 |
| `resources/Maps/SC_gasp/Source/M_Neutral_Traversal_Climb_Start_2_5_walk_F_Lfoot.fbx` | 1,500,128 |

磁盘当前为 1723 个文件、1,593,327,045 B：清单 1721 项加
`game-export-manifest.json` 本身，再加运行期生成的
`resources/Generated/MotionMatching/e2b9274b...json`（608,569 B），
以及 32 个生成类文件相对清单的 +3,996 B 增量。
1721 个清单路径无缺失；这 32 个文件（24 个 parameter include、8 个 SPV
`.debug`）当前大小与清单不同，说明清单是导出时快照，运行后生成物尺寸已变化。

包内 `config/config.json` 为 1280×720、MSAA 2、`scripts.autoPlay=true`、
developer UI 关闭，初始场景同为 Climb stand。

## 2. 测量方法

- 工作目录为 `C:\Users\Public\vl-gasp-perf-record`，在包目录之外通过绝对路径启动
  `...\gasp-p10-climb-package-04\main.exe`，参数
  `--initial-scene Maps/SC_gasp/<scene>.json`（runner：
  `C:\Users\Public\vl-gasp-perf-record\run-scene.ps1`）。`main.cpp` 检测到包内
  清单后会把进程工作目录切到包根；日志中的 `Shader folder path`、场景绝对路径
  都指向该包，未复制包，连续 14 次包内运行复用了同一份包。
- 每次运行都用 Win32 `WM_CLOSE` 关闭最后一个窗口，SDL 发出 `SDL_EVENT_QUIT`；
  日志均出现 `Quit event received` 与 `EngineLoop teardown complete`，
  进程退出码 0，`.err` 为空。
- 1280×720 由运行期 `GetClientRect` 实测，14 次包内运行均为 `1280x720`。
- FPS 来源：源码 `source/debugConsole.cpp` 的 help 中没有 `stats`/`fps` 命令，
  `launchOptions.cpp` 也没有对应启动项；唯一可自动读取的引擎 FPS 是窗口标题
  `VulkanRenderer - FPS: N`（`source/fpsTool.h`，`engineLoop.cpp` 每帧
  `SetTitle`，每约 1 s 用 `frameCount/timeElapsed` 更新整数 FPS）。
  因此 `--console-command` 无法用于 FPS；runner 在 console ready 后每 200 ms
  用 `GetWindowText` 读取标题，记录每次变化。首 1–2 s 含场景装载/JIT 预热，
  本次首秒样本为 186–434 FPS，不计入稳态范围。

## 3. 包内结果

每个场景列 3 个稳态样本（读取时刻相对 console ready）；范围含同一轮全部
t≥2.5 s 的样本，约 2 s 的装载后首批样本不计入。

| 场景 | `P8_TRAVERSAL_START` | 结束事件 | FPS 样本（t） | 稳态范围 | 退出码/teardown/stderr |
|---|---|---|---|---|---|
| Climb stand | height=250 action=`..._Climb_Start_2_5_stand_F_Lfoot` pos=0,90,-80 | `END pos=-0,340,-168 grounded=True yaw=0.0` | 699(3.0s) / 721(6.1s) / 681(9.0s) | 677–721 | 0 / 完整 / 空 |
| Climb run（r3） | height=250 action=`..._Climb_Start_2_5_run_F_Lfoot` pos=0,90,-80 | `END pos=-0,340,-169 grounded=True yaw=0.0` | 661(3.2s) / 688(6.1s) / 685(8.2s) | 629–688 | 0 / 完整 / 空 |
| Climb walk | height=250 action=`..._Climb_Start_2_5_walk_F_Lfoot` pos=0,90,-80 | `END pos=-0,340,-199 grounded=True yaw=0.0` | 688(3.1s) / 709(6.1s) / 675(10.0s) | 667–709 | 0 / 完整 / 空 |
| Mantle 1.0 | height=100 action=`..._Mantle_1_0_stand_F_Lfoot` pos=0,90,-80 | `END pos=0,190,-202 grounded=True yaw=0.0` | 646(3.1s) / 664(6.2s) / 662(8.0s) | 646–678 | 0 / 完整 / 空 |
| Vault 1.0 | height=100 action=`..._Vault_1_0_stand_F_Lfoot` pos=0,90,-80 | `END pos=0,90,-290 grounded=True yaw=0.0` | 668(2.9s) / 674(6.0s) / 668(8.1s) | 668–685 | 0 / 完整 / 空 |

run 场景重复 10 次：7 次 `END`，落点 y 均为 340，z 分别为 -184、-170、-169、
-167、-168、-169、-184；3 次 `ABORT`，位置分别为
`pos=-0,250,-110`、`pos=-0,223,-110`、`pos=-0,242,-110`，均为
`reason=airborne progress=30 grounded=False`（首次在 clip 4.51/4.00 s，
随后仍正常 teardown）。3 次 ABORT 的首个 1 s FPS 为 186、312、383，7 次成功
为 389–434；低启动帧率与 ABORT 明显相关，但样本小，原因未定位。ABORT 轮次在
clip 1.75 s 时 y=269–296 且 z 仍 -110，成功轮次为 y=330–344、z=-120/-121/-123；
失败轮次的位移进度落后约 35–75 cm，至 clip 结束仍未落到顶面。ABORT 轮次的
后续 FPS 在 584–679，未计入稳态范围；重复运行另出现 315、837、887 三个与
相邻样本不连续的孤立标题值，也未纳入范围。

## 4. 与既有记录的对比

- FPS：既有 P9 记录为无负载 1280×720 的 589–639 FPS。本次表列五场景稳态为
  629–721 FPS（表列样本中位数 678）；追加 10 次 Climb run 后，全包稳态样本
  扩展到 555–721（r9 低至 555、r4 高至 708、stand 高至 721）。既有区间落在
  本次观测范围内：未复现稳定的单向下降，也不能宣称提升；不同轮次/场景的
  差异大于与既有记录的差异。注意二者是不同独立包，比较只作量级参考。
- 340 cm：stand 为 340,-168，walk 为 340,-199，run 的 7 次成功均为 y=340
  （z -167…-184）。既有记录为 stand -167、run -187、walk -200；y 一致，
  z 有数厘米差异。run 的 3/10 ABORT（y=223/242/250，z=-110）说明
  “落顶 340 cm”在当前包内不稳定，单次成功不能作为闭合证明。
- Mantle `190,-202`、Vault `90,-290` 与既有记录一致。
- 结论：性能上没有可复现的单向回归；闭合上，同一包本次 run 3/10 ABORT，
  说明既有 `340,-187` 记录只覆盖了成功路径，不能作为稳定闭合证明。该失败
  在同一包内即可出现，不能归因于本次构建改动。
- 仓库 `build/bin/main.exe`（`CMAKE_BUILD_TYPE=Debug`）仅作对照跑了 stand
  （`--initial-scene Maps/SC_gasp/SC_gasp_traversal_climb.json --no-dev-ui`）：
  启动到 console ready 44.2 s（18 个 shader cache miss、重编译 10.6 s），
  稳态约 26–31 FPS（4 s 处有 12 的瞬时值），`END pos=-0,338,-133`；进程
  退出码 0、teardown 完整、stderr 空。仓库 Debug 与包不是同一性能基线，
  故没有用仓库数值做 FPS 对比；其 30 FPS 下 y/z 与包内 340,-168 不同，
  只说明两次运行轨迹不完全一致，原因未定位。该运行重建了本仓库 shader
  缓存（生成物，未产生受版本控制的改动）；临时写入的
  `scripts.autoPlay=true` 已按备份恢复，`git diff -- config` 为空。

## 5. 限制

- 未做长时间 GPU 压力、温度/功耗或显存压力测试；每场景仅数秒，包内共 14 次运行。
- 未做 per-pass GPU timestamp、frame-time 分布或 PresentMon/ETW；FPS 是引擎
  自带约 1 s 整数窗口值，不含 1% low。
- 无真实输入：全部由场景内探针自动触发，未验证键鼠/手柄驱动下的表现。
- 单机单次会话；未确认实际选用的 GPU（测试机含 RTX 4090），未记录 GPU
  时钟/温度，轮次间 555–721 FPS 的离散可能包含环境因素。
- run 场景存在 3/10 ABORT，样本量仍不足以给出稳定复现率；需更多重复运行定位
  启动帧率与 root motion/高度闭环的关系。该问题已在 package-05 用后半段竖直
  闭环修复到 0/10，见 §7。

## 6. 证据文件

均在 `artifacts/` 下，`.err` 为空，同名 `.summary.json` 为 runner 采集摘要：

- `gasp-p10-perf-record-pkg-stand.log` / `.log.err`
- `gasp-p10-perf-record-pkg-run.log` / `.log.err`（ABORT 首次）
- `gasp-p10-perf-record-pkg-run-r1.log` / `.log.err`
- `gasp-p10-perf-record-pkg-run-r2.log` / `.log.err`
- `gasp-p10-perf-record-pkg-run-r3.log` / `.log.err`
- `gasp-p10-perf-record-pkg-run-r4.log` / `.log.err`
- `gasp-p10-perf-record-pkg-run-r5.log` / `.log.err`
- `gasp-p10-perf-record-pkg-run-r6.log` / `.log.err`（ABORT）
- `gasp-p10-perf-record-pkg-run-r7.log` / `.log.err`（ABORT）
- `gasp-p10-perf-record-pkg-run-r8.log` / `.log.err`
- `gasp-p10-perf-record-pkg-run-r9.log` / `.log.err`
- `gasp-p10-perf-record-pkg-walk.log` / `.log.err`
- `gasp-p10-perf-record-pkg-traversal.log` / `.log.err`
- `gasp-p10-perf-record-pkg-vault.log` / `.log.err`
- `gasp-p10-perf-record-repo-stand.log` / `.log.err`

## 7. 后续修复（package-05）

本文的 run 3/10 ABORT 是 package-04 的结果。随后在 Climb 的 stand/run
后半段加入目标高度闭环（上限 ±200 cm/s），只改竖直收口、水平继续用根轨迹，
重新导出 `artifacts/gasp-p10-climb-package-05`。

同一包内 run 连续 10 次：

```text
10/10 P8_TRAVERSAL_END y=340, z=-151…-196, grounded=True yaw=0.0
0 ABORT
```

stand `END 340,-173`、walk `END 340,-193`，stderr 均为空。
因此本文第 3 节的 3/10 ABORT 是修复前基线；package-05 的 run 闭合为 0/10
失败。package-05 未重新做完整 FPS 采样，性能数字仍只代表 package-04
（同一二进制量级，修复只增加少量竖直闭运算）。

## 8. 60 s 稳定冒烟（package-05）

package-05 Climb stand 连续运行 60 s，每 5 s 读窗口标题 FPS 和进程工作集：

```text
FPS 649–696（12 个样本）
WorkingSet 352.5–354.6 MB
stderr 空，正常 WM_CLOSE teardown
```

这是单场景 60 s 冒烟，不是长时 GPU 压力；没有 1% low、GPU timestamp、
温度/功耗或显存峰值。日志 `artifacts/gasp-p13-stress.log/.err`。

## 9. 300 s 稳定冒烟（package-05）

同一 Climb stand 连续 300 s，每 30 s 采样：

```text
FPS 667–701（10 个样本）
WorkingSet 354.3–359.3 MB；90–300 s 稳定在 354.3–354.7 MB
stderr 空，正常 WM_CLOSE teardown
```

仍然只是长时间运行冒烟，不含 GPU timestamp、1% low、温度/功耗、显存峰值；
日志 `artifacts/gasp-p13-stress300.log/.err`。
