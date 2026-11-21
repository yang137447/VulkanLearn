# 地形 RVT P0：本次基线、实际资源需求与冻结门槛

状态：2026-11-21 的测量记录与后续验收目标；不是 V2 已实现契约。
仅使用 terrain-openworld 代码/资源工作树。代码 HEAD 为 5df9fbc，
资源 HEAD 为 e665a40；保留原有 dirty、未跟踪 VT 和无关脚本/.NET 改动。
P0 不实现 P1/P2，不提交、不 reset，也不修改原方案。

## 冻结文件

正式机器配置：[config/terrainRvtP0Frozen.json](../../config/terrainRvtP0Frozen.json)。
SHA256：

```text
43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd
```

校验文件为 `config/terrainRvtP0Frozen.json.sha256`。配置已签发，
独立复核尚待 Kierkegaard；不能把签发解释为 P1–P5 或画质目标已经通过。
P1–P5 和可选 P6 不得放宽预算、降低清晰度或改变比较条件以获得通过。

所有原始收据位于
`artifacts/terrain-rvt-production/p0/`。
冻结 JSON 的 `evidenceFiles` 给出绝对路径、字节数和 SHA256，
`acceptanceMeasurements` 给出统计量、采样口径、单位、最少样本及适用案例。

## 本次实测

硬件为 Ryzen 9 3950X / RTX 3080 10 GiB，驱动 621.01；
Release MinGW、1280×720、MSAA2、worker2、30 Hz pacing、
immediate present、UI 关闭、Vulkan validation 关闭。
GPU 正式分母使用真实 query 帧，CPU 使用未 clamp 的 render-frame 墙钟间隔；
CPU 数字包含 pacing/capture，不能解释为 CPU 工作量。

| 收据 | 本次结果 | 使用范围 |
| --- | --- | --- |
| `cold` | PID85500，九段巡游、World reload、exit0；751 CPU / 247 有效 WDDM 样本 | 启动/完整路径内存/功能证据，39 个 perf 快照不是帧分布 |
| `raw-cold` | 1081 个 raw GPU 帧，完整巡游与 reload 通过 | 完整绘制/上传覆盖及内存；CPU 尚无 raw 输出 |
| `raw-warm-r2` | 1083 raw GPU、1084 raw CPU，首帧图、巡游、reload 通过 | 首帧/内存/重载；不是正式 CPU 性能分母 |
| `quiet-final` | 1080 raw GPU、1081 raw CPU；370 个进程审计样本未见编译进程、无审计错误；exit0 | 正式 CPU/GPU 性能参考，无 World reload 的同一机位序列 |
| `producer-coverage-full` | 真实 tile00，8192，mip0–13，共5468页，heightBlend=3 | Coverage 生产/复制有限压力集合 |
| `producer-local-full` | 同样5468页，LocalLayerArray，heightBlend=3 | 批准的隔离 R32 上传格式映射修正后测量 |

quiet-final 的 GPU instrumented pass sum p99 为 **4.198024 ms**；
未 clamp render-frame interval p99 为 **65.54924 ms**。
两权重共10936页，生产最大观测 **2.50707 ms**，
readback-copy 最大观测 **3.28509 ms**，
同页生产加复制最大观测 **3.319266 ms**。
这些不是任意输入、驱动调度或硬件抢占下的 WCET 证明。

`numeric-oracle/report.json` 的 coverage/repeat/local/height/tail/missing/lod/area
八种 GPU 数值模式均 exit0、errors=0；missing 模式实际产生预期 status=4。
合成 oracle 与真实生产压力分开记账，不用前者代替真实输入吞吐。

## 缓存、负载与采样限制

baseline 的 cold 只表示 fresh process / 新驻留池。
V1 完整不可变页包保留，SPV 为热缓存，OS 文件/standby cache 未清空。
V2 的正式冷测试必须清空私有 fine-page cache 及其索引，保留生产输入/根页/允许的粗页；
包缺失或损坏、缓存缺失或损坏是不同案例。

首轮 World commit 观察为174.377 s，far 截图182.215 s；
正式 quiet-final commit 为153.748 s。里程碑来自250 ms日志轮询，
首图含120帧等待，均不是最早清晰事件。冻结以本次最小 commit 观察减250 ms
形成保守分母；不使用历史162.93 s或254.018567 s。

`quiet` 实际观察到 cmake/make/c++，因此明确排除。
raw-warm-r2 缺少无编译干扰的审计；不能用其181 ms的 CPU p99放大门槛。
quiet-final 的500 ms进程审计不能证明更短进程或所有无关背景负载绝对不存在。

WDDM 500 ms只是请求间隔；cold 实际 median 为1.5390375 s，
max为1.989162 s。helper exit1和末尾 unavailable 样本保留，
不能用旧 `missingEvidence=[]` 推导完整无缺样。
`raw-analysis.json` 明列不可用行及实际 cadence。
WDDM 是 PID allocation/residency 采样近似，不是精确瞬时 GPU high-water。
完整含 reload 的三次进程 private / 同时 dedicated+shared 峰值分别供同比；
不拿无 reload 的 quiet-final 较低峰值与重载案例混比。

## 计时修复和旧路径隔离

补闭合 lightCulling、particleSoftDepth、volumetricFog 三个专用分支的 timestamp；
此前未闭合区间使后续 pass 缺失。
增加 `terrainCompositedProductionAndCopy`、raw query 与 raw CPU interval。
geometry 是含基础材质采样、TES和其他 geometry 的完整绘制阶段；
不是独立 TES 或纯地形子阶段。GPU pass sum不含未被计时的环境工作。

同步 Baker 对生产与 output→host readback-copy 分别做 query。
V1 Game 的服务 query 包含真实 atlas 上传批次；不能把 readback-copy叫成未来 V2 atlas copy。
V2 必须实际计时并满足已冻结单页/整帧目标。

原 source 全快照、计时源、二进制/构建清单保留在 p0。
`raw-cpu-link/build.json` 只用冻结 archive 替换计时对象；
后续 Store/Runtime 源码修改不进入基线。
Local 原失败是 HostImage R32_FLOAT 未映射 Vulkan 格式。
批准的隔离修补仅增加 R32_FLOAT→eR32Sfloat，不改变精度和上传语义；
`local-producer-link-r2/mapping.patch` 与 build.json 可独立复核。
原 producer-coverage 的冗余默认 MI 拒绝、原 producer-local 的格式失败均保留。

## 资源账本与硬预算

磁盘闭包19,792,957,594 B，其中材质数据19,418,136,576 B，
另有16个文件头共256 B。`ledger-final.json` 不把 headers混为材质 texel数据。
旧已知 image payload 下界1,163,627,364 B已包含全局 coverage
134,221,824 B和16个 sourceMap 131,072 B；这不是完整GPU分配量。
全驻留几何885,297,152 B是含 vertex/triangle/patch index的逻辑包络，
不是声称当前同时驻留。ABI probe 实测 Vertex=112 B、index=4 B。

allocation probe 只创建 resource并查询 memoryRequirements，不分配memory或dispatch：

| 资源集合 | payload或逻辑值 | 实际 memoryRequirements |
| --- | --- | --- |
| 新512槽三通道池 | 113,639,424 B | **226,492,416 B** |
| 旧16×192槽三通道池 | 681,836,544 B | **1,358,954,496 B** |
| 32份513² normal/tangent frame | 134,742,528 B | **169,082,880 B** |
| 16份515²×7 local float overlay | 118,820,800 B | **151,388,160 B** |
| 双权重输入完整集合 | 分类见allocation收据 | **759,801,856 B** |

| 限额 | 稳态 | Candidate+Active+Retiring合计 |
| --- | --- | --- |
| PhysicalPool GPU | 224 MiB、512槽 | 448 MiB、1024槽 |
| shared生产/动态Input GPU | 768 MiB | 1536 MiB |
| Workspace GPU | 32 MiB | 64 MiB |
| RVT GPU合计 | 1024 MiB | 2048 MiB |
| RVT CPU合计 | 512 MiB | 1024 MiB |
| 地形GPU总账，含geometry | 2048 MiB | **3072 MiB** |
| 地形CPU总账 | 2048 MiB | **3072 MiB** |

P2 Renderer owner消费 peakRvtOwnerCaps，同时必须落实单World steadyRvtCaps；
外层 terrainTotalCaps 对geometry等再执行联合准入。
并非各子cap都能在两个World同时用满：超过外层3 GiB峰值必须拒绝候选并保留旧World，
或经验证分阶段复用。旧帧、在途任务和retirement租约都计入，身份相同的实际共享资源只计一次。

GPU用实际 image/buffer memoryRequirements.size，不能只用格式算术。
Workspace内含每surface页表、反馈/状态、scratch、共享上传环、参数及
每World **4 MiB descriptor/opaque GPU metadata显式reserve**。
CPU采用总cap加有界关系：queue metadata16、index32、其他host metadata16、
upload/readback64、CPU page cache128、decoded inputs256 MiB，合计512 MiB。
全局最多4096队列键、4在途生产任务；不允许队列键携带无界整页payload。

## 已冻结目标

单页生产≤4 ms，copy≤4 ms，单页合计及每帧生产/复制合计≤6 ms；
每帧最多生产1页221,956 B，最多上传16页/4 MiB；全局4任务。
稳态 raw GPU sum p99≤4.618 ms，geometry含TES p99≤3.754 ms；
streaming GPU sum p99≤10.199 ms。
raw CPU interval p95≤49.025 ms、p99≤72.105 ms；各至少1000原始帧。
World commit≤76.748 s、解除初次加载≤86.748 s、每机位/瞬移收敛≤10 s。
同条件含reload的 private峰值≤8,194,532,966 B，
WDDM峰值≤6,977,722,777 B，均要求比本次对应完整路径参考至少改善20%。

画质上限bias=1；解除加载必须有全部根页与独立TES高度闭包、
零未知/过期身份，≥99%有效可见反馈样本解析到target+1以内；
target<12时不能用root-only结果解除加载。
队列空与累计estimated histogram不构成画质通过。
base三通道各最大1 byte误差；lit terrain ROI MAE≤2、RMSE≤3、
p99 error≤8 byte；silhouette XOR≤0.1%，TES高度误差≤2个R16单位。
验证错误、旧revision发布均必须为0。
超预算无法在bias1内收敛就报告不足，不再降mip或扩容。

V2独立Game包目标≤1536 MiB，本地fine cache≤256 MiB；
最多发布mip4…13的448个粗页，16个根页强制可用。
这些是后续目标，P0没有宣称新Game包、清缓存生产链或视觉比较已完成。

## 复现入口

详尽argv/cwd保存在每个run的 `command.json` / `report.json`。
所有命令从现有 terrain-openworld/VulkanLearn 显式作为workdir执行：

```powershell
py -3 tool/validation/terrain/rvt_p0_quiet.py --stage artifacts/terrain-rvt-production/p0/raw-cpu-stage --output <新目录>
py -3 tool/validation/terrain/rvt_p0_run_producer.py --stage ../p0-cook-s --resources ../p0-coverage-r --output <新目录>
py -3 tool/validation/terrain/rvt_p0_run_producer.py --stage ../p0-cook-s --resources ../p0-local-r --binary ../p0-cook-s/bin/main_editor_r32.exe --output <新目录>
py -3 tool/validation/terrain/rvt_p0_analyze.py --run <run目录>
```

raw GPU/CPU/Baker输出需父进程设置 `VL_GPU_TIMING_RAW=1`。
复测不得覆盖原收据，不得重链共享新Store替代旧基线。
freeze工具使用exclusive-create，已签发配置不能被重新生成覆盖。
