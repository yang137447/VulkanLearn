# 合成 VT S3 压力验收记录

日期：2026-11-20。本文为实测证据记录，**不是运行时契约**。
对应 [资源生命周期计划 S3](../plan/rendering/terrain-composited-vt-lifecycle-plan.md)；
当前行为仍以 [合成 VT 契约](../contracts/terrain-composited-vt.md) 和源码为准。
本轮工作区含主线程及其他 agent 尚未提交的改动，不能用 HEAD 代替完整测量版本。

## 结论与范围

最终 `terrain-runtime-stress-run03` 的 baseline64 与 budget16 均为
`failures=[]`、`missingEvidence=[]`，总 verdict 为 `passed`。
两个私有 Cook 和两个 runtime 子进程均 exit 0，无 Vulkan validation、
DeviceLost、页读取失败或已采样帧超过 500 ms。renderer 已全部退出并交回主线程，
本验收不再启动额外阶段。

入口为 `tool/validation/terrain/runtime_stress.py`，只使用已有启动 console 经
CommandBus 提交 `camera pose/get`、`terrainvt stats`、`perf json`、
`screenshot`、`loadworld`、`quit`；未修改生产 renderer，也没有新增 runtime flag。
采集覆盖近景、远景、中景、24 次快速移动/转头、teleport、两次 World reload，
最后回到 near，验证新需求仍能响应。

夹具：

```text
Generated/Validation/TerrainCompositedVT/Variants/Near100m_R1024_Default/SC_composited_vt_100m.json
terrain|Landscape_Near100m
```

运行使用最新 Release `main_editor.exe`，两线程，12 Hz pacing，
初始等待 120 ticks，每视角 72 settle ticks 加 8 个独立 tail ticks；
WDDM/CPU 内存采样配置周期为 1000 ms。两轮分别复制 scene closure、
shader/config/UI 到短路径私有 staging，baseline 保持 64 槽，
budget16 仅将私有 TR 的 `residentPages` 改为 16，然后各自重新 Cook。
未写共享作者资源或产品包。

## 从失败到最终闭环

原 `run01` 和 `run02b` 保持失败，不用后续判据重写历史结果。
`run02b` 延长观察后，baseline mid 仍是 resident 62/64、
queued=0、reading=1，末尾 feedback 不断增加而读取未收敛。
低预算也持续有 fine 请求，不能把它解释成仅等待时间不足。

生产修复由主线程集成：超预算 feedback 工作集使用祖先粗化、选集保护，
取消过期 fine 请求，避免重复 I/O。最终 run03 沿用 run02b 的视角、
容量与等待时间，没有扩大 baseline 容量或继续延长 settle 来隐藏问题。

| run03 各阶段稳态 | baseline64 | budget16 |
| --- | --- | --- |
| mid budgetMipBias / selectedPages | 0 / 51 | 2 / 6 |
| far budgetMipBias / selectedPages | 0 / 27 | 1 / 8 |
| fast-turn 稳态 budgetMipBias / selectedPages | 0 / 16 | 1 / 8 |
| 所有阶段 queued / reading | 0 / 0 | 0 / 0 |
| 所有阶段尾部 readsStarted 增量 | 0 | 0 |
| 所有阶段 stale / dirty / previewReadbacks | 0 / 0 / 0 | 0 / 0 / 0 |
| peak resident | 64 / 64 | 16 / 16 |
| peak queued，过渡期间 | 36 | 8 |
| peak budgetMipBias | 0 | 2 |
| peak queuedCancel | 30 | 5 |
| peak evictedReads | 22 | 54 |
| peak retiring，过渡期间 | 2 | 2 |
| generation：原 World → reload1 → reload2 | 1 → 2 → 3 | 1 → 2 → 3 |

baseline 本轮没有触发 bias，并不声称它曾粗化；它通过淘汰/取消和选集保护
实现了中景收敛。budget16 的实际 bias=2 证明了预算压力与降质；
不要求全部 16 槽同时为 resident，退休或过渡槽合理。
两轮后续摄像需求和 reload 后 near 均再次收敛。低预算末段持续有 feedback，
新增 CPU 映射 miss 为 0。父回退计数只是 CPU 映射估计，
不是所有 GPU 像素或物理槽位正确性的独立证明。

## 帧耗时实测

单位 ms；所有运行包含 reload，不排除慢样本。严格 stall 阈值为 500 ms。

| 指标 | baseline64 | budget16 |
| --- | ---: | ---: |
| CPU 有效样本数 | 75 | 76 |
| CPU p50 | 83.372 | 83.3235 |
| CPU p95 | 91.0807 | 91.34975 |
| CPU p99 | 91.9237 | 93.8875 |
| CPU max | 92.623 | 100.000 |
| GPU 有效样本数 | 75 | 76 |
| GPU p50 | 0.087 | 0.086 |
| GPU p95 | 0.1182 | 0.125 |
| GPU p99 | 0.12226 | 0.13525 |
| GPU max | 0.123 | 0.139 |
| 已采样 >500 ms 的 stall 数 | 0 | 0 |

**测量边界：**现有 perf JSONL 每墙钟秒输出一个帧快照，不是逐帧追踪。
这些分位数仅描述采样快照，不能排除采样之间的瞬时 stall，
也不把 GPU 计时回读值宣称为设备总忙碌时间。
12 Hz pacing 会进入 CPU frame delta，所以约 83 ms 的 p50 不代表
不受限运行的渲染性能。bootstrap Cook 独立计时，不混入 runtime 分位数。

## 包含源资源的进程内存峰值

单位 MiB（bytes / 1048576）。表中是 runtime 目标 child PID 的采样峰值，
不是由 cache payload 估算或设备总量替代。原始样本保留 PID 和时间。

| 指标 | baseline64 | budget16 |
| --- | ---: | ---: |
| CPU 有效内存样本数 | 92 | 91 |
| CPU working set 峰值 | 1127.344 | 1134.652 |
| CPU private bytes 峰值 | 5410.656 | 5376.676 |
| WDDM 有效内存样本数 | 45 | 44 |
| WDDM Dedicated 峰值 | 4257.965 | 4276.879 |
| WDDM Shared 峰值 | 199.766 | 317.137 |
| **同一采样时刻 Dedicated + Shared 峰值** | **4452.926** | **4594.016** |

CPU 使用 `GetProcessMemoryInfo`，GPU 使用 Windows
`\GPU Process Memory(*)\Dedicated Usage` / `Shared Usage`，
先按已知 child PID 的 `pid_<pid>_*` 实例过滤再输出，只记录自身目标进程。
Cook 进程也有独立内存序列与峰值，未与 runtime 混为一轮峰值。

**WDDM 是进程级分配/驻留近似，包含源纹理、驱动和 renderer 其他资源；
不是 Vulkan cache 专属值，也不是精确瞬态 high-water。**
Dedicated 和 Shared 的独立峰值可能不同时发生，合计峰值必须来自同一行；
无有效通道时 checker 返回缺证据，不使用设备总量兜底。退出边界处的
unavailable 样本保留，未用零值填充有效序列。这些数字不能单独证明
16 槽配置降低整进程分配峰值。

## 来源守护、版本和证据身份

两轮均通过以下 SHA256 前后比较：

- 原始资源 closure 未改变；
- 私有 runtime 作者输入和 Cook 包未改变；
- staged shader 源文件未改变（排除运行时生成 includes）。

两份私有 Cook 均为 generatorVersion=2，
encoding=`gpu-center-mip0-area4-coarse-rgba8-height-fade1-v2`，
1024×1024，92 页。residentPages 进入当前依赖配置，
因此 baseline 与 budget16 各自 Cook，不能复用不同预算的旧包身份。

| 身份 | SHA256 / 内容 hash |
| --- | --- |
| 被测 exe SHA256 | `a2dae74171f3d83df443892572965f048efc56d08eeac88c782d6dd2ed5216bd` |
| baseline contentHash | `9ad743c53929b0f3033b38fc6ba3e7b3d9af8049feff2369314ced9bca46a08c` |
| budget16 contentHash | `feb482fd97cd9a77f335dca2f8c1344058f1b7e017dda94ce3daa33b7e74333b` |
| 原始 report.json SHA256 | `a6514456de0ff989b17d8a5d87fa21d70cf8cb5f3dbc8f1f687ec354bccea63f` |
| actual-summary.json SHA256 | `f75b1a02b8c49f834a06ee478e820b7b276d21ce40b51aacfc7221ebbb2cbefd` |

完整原始路径（`artifacts/` 被 Git 忽略，以上关键结论在本文长留）：

```text
artifacts/terrain-runtime-stress-run03/report.json
artifacts/terrain-runtime-stress-run03/run.json
artifacts/terrain-runtime-stress-run03/actual-summary.json
artifacts/terrain-runtime-stress-run03/baseline.log
artifacts/terrain-runtime-stress-run03/budget16.log
artifacts/terrain-runtime-stress-run03/*.perf.jsonl
artifacts/terrain-runtime-stress-run03/*.cpu-memory.jsonl
artifacts/terrain-runtime-stress-run03/*.gpu-memory.jsonl
artifacts/terrain-runtime-stress-failure-archive/failure-archive.json
```

`run.json` 保留完整 argv、私有 staging、相机计划、section 序号、
Cook 描述、exe 摘要及资源守护；report 保留逐 section 最终 stats、
尾段 readsStarted 增量、内存有效样本与缺失判据。
旧 run01/run02b 的失败报告、峰值、分位数及证据文件摘要独立归档未抹除。

未覆盖的范围仍包括慢盘故障注入、损坏包、上传失败、设备丢失恢复和
逐 GPU 采样/槽位退休证明；本轮不是这些故障矩阵项的通过声明。
24 个离线 checker 测试用于拒绝错误证据，不替代上述真实运行结果。
