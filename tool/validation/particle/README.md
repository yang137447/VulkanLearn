# 粒子特效验收工具

`particle_niagara_acceptance.py` 为 `documents/plan/rendering/niagara-effects-alignment-plan.md`
的 49 个 UE Niagara 目标建立可重复验收。它只读资产、启动 `main.exe` 做 headless
冒烟，不修改粒子 runtime 行为。

## 目标推导

扫描 `NiagaraExamples/**/NS_*.uasset`（当前 67 个），排除：

- `FX_Markers/**`：编辑器标记（2）
- `NS_Boundary*`：Boundary 调试系统（4）
- `Utilities/**`：SpriteGeneration 工具（11）
- `Materials/**`：SubUV 参考工具（1）

余下 49 个即验收目标。数量与 `--expect-targets` 不一致时脚本直接失败，避免 UE
工程变化后静默漏验。

## 映射规则

1. README 中同一行同时出现 `NS_*` 与 `PS_*.json` 时按显式映射。
2. 没有显式映射时，在提到该 NS 的 gallery 目录内按名称相似度选择 PS。
3. 仍未命中且只有一个 gallery 提到该 NS 时，使用该目录全部 PS（目录级兜底）。

`acceptance-matrix.md` 的 `mapping_source` 会记录实际命中的规则。

## 静态检查

- 每个 `PS_*.json` 可解析，`type=particleSystem`，1..16 个 emitter，名称唯一。
- `spawnSource.parentEmitter` 指向存在的兄弟 emitter，`perEvent` 在 `[1,64]`。
- `texture` / `textureAsset` / `path` / `materialInstancePath` / `materialOverride` /
  `albedoMap` / `normalMap` / `ormMap` 等引用必须存在（先解析 resource root，再解析
  代码根）。
- 每个 gallery 场景可解析，`particleSystem.particlePath` 指向存在的 PS。
- 每个 PS 至少被一个 UE target 映射；未被引用的 PS 记为 `orphan_particle_systems`。
- 去除 emitter 名称与位置后结构完全相同的 PS 记为重复。
- 每个 PS 输出静态指标：emitter 数、`renderMode` 分布、`maxParticles` 总和与
  单 emitter 最大值、纹理引用数（去重后）。
- 静态指标异常记 WARN，不阻断验收：`maxParticles` 总和为 0、总和超过
  100000、单 emitter 超过 65536、同一 emitter 有多个启用 renderer、
  `maxParticles` 缺失。

## 冒烟

`-Smoke` 对每个 gallery 执行：

```powershell
main.exe --initial-scene <scene> --no-dev-ui --frame-limit 60 `
  --console-command "particlestats" `
  --console-command "particlestats" `
  --console-command "particlestats" `
  --console-command "screenshot <name>.bmp" `
  --console-command-delay 120
```

`ConsoleSubsystem` 从第 120 帧起每帧提交一条启动命令，因此三次
`particlestats` 分别落在第 120/121/122 帧，截图在第 123 帧提交；这样
截图前 stdout 已经包含全部统计行。

`main.exe` 没有退出命令，脚本在截图落盘并稳定后强制终止进程（`documents/rationale/pitfalls.md`
的 `[no-quit-path]`）。每次使用带时间戳的截图名，避免误读旧图。

采集项：`Scene.LoadFailed`、`EngineLoop init failed`、`VUID-*`、
`particles ... emitters= ... particles= ... drawCalls= ... vertices= ... gpuMs= ...
gpuAvgMs= ... gpuMaxMs=`、截图字节数、耗时。

## 性能预算

预算文件：`tool/validation/particle/particle_performance_budget.json`。
工具从 `particles` / `drawCalls` / `vertices` / `gpuMs` 取三次采样的峰值，
`gpuAvgMs` / `gpuMaxMs` 取最后一次累计值。

判定口径：

- `particles`、`drawCalls`、`vertices`、`gpuAvgMs` 参与 PASS/OVER。
- `gpuMs` 是单帧瞬时值，`gpuMaxMs` 是整轮最坏单帧，二者只展示，不参与
  PASS/OVER；启动上传/管线创建的单个尖峰不应把整轮判成超预算。
- 每个 gallery 的预算 = baseline × (1 + `--budget-margin`/100)，默认 20%。
- `--write-budget` 会把本次测量并入 baseline，按逐指标最大值合并，避免
  GPU 计时抖动把预算压得过低。
- `--budget-file <path>` 可切换预算文件，`--budget-margin <percent>` 可修改
  余量；默认分别为上面的预算文件与 20%。
- 结果矩阵和 JSON 的 `budget.status` 为 `PASS` / `OVER` / `MISSING`；
  `OVER` 会按 particle/draw/vertex/GPU 维度给出调整建议。

## 输出

默认写到 `artifacts/particle-niagara-acceptance/`（gitignored）：

- `acceptance-matrix.md`：49 行验收矩阵、失败项、VUID 汇总、运行统计、README
  近似/替身摘录、17 个 gallery 的性能预算表和 48 个 PS 的静态统计。
- `acceptance-result.json`：同内容的机器可读版本。
- `smoke/<scene>.stdout.txt` / `.stderr.txt`：原始输出。
- `smoke/<screenshot>.bmp`：截图副本。

## 命令

```powershell
# 只做静态检查
.\tool\validation\particle\run_particle_acceptance.ps1

# 全量静态 + 全部 gallery 冒烟
.\tool\validation\particle\run_particle_acceptance.ps1 -Smoke

# 重新测量并写入/合并性能预算
py -3 tool/validation/particle/particle_niagara_acceptance.py --smoke --write-budget

# 单个 gallery 冒烟
.\tool\validation\particle\run_particle_acceptance.ps1 -Smoke -Only SC_particle_ue_misc
```

退出码：`0` 表示无 FAIL；`1` 表示存在 FAIL；`2` 表示环境/目标数量错误。

2026-11-10 全量基线：49 target / 48 PS / 17 gallery；49 PASS、0 WARN、
0 FAIL、VUID 0；17/17 gallery 在预算内。
