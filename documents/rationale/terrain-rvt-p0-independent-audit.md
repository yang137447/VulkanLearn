# 地形 RVT P0 独立最终验收

状态：**PASS（P0 基线与门槛冻结）**。审计日期：2026-11-21，Asia/Shanghai。
本记录不是运行时契约，也不表示 P1–P5、P6 或最终交付通过。
验收者未参与 P0 实现；本次仅读取既有文件并用独立 Python 标准库计算，
未启动 GPU、Vulkan probe、renderer、Cook 或构建，未修改实现和冻结配置。

## 验收对象和结论边界

验收对象为 [冻结配置](../../config/terrainRvtP0Frozen.json)、
`config/terrainRvtP0Frozen.json.sha256` 和
[基线记录](terrain-rvt-p0-baseline.md)。
依据 [生产方案](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)
的 P0 要求，分别核对测量、资源算术、后续目标及其适用范围。
未以 P1 尚未完成作为 P0 缺陷，也未运行冻结脚本重新签发配置。

冻结文件的实际 bytes SHA256 为：

```text
43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd
```

该值与 sha 文件、基线文档一致。`evidenceFiles` 的 **27 个文件，共
8,996,623 B** 全部存在；逐文件重新计算长度与 SHA256，
同时交叉核对 `evidenceSha256`，差异为 **0**。
证据根为 `artifacts/terrain-rvt-production/p0/`。
这里验证的是内容一致性，不把 SHA256 当作第三方时间戳或作者身份签名。

另外独立核对 `raw-cpu-stage/p0-input-hashes.json` 中的
**262 个文件、73,868,171 B**，哈希差异为 0。
实际基线 `raw-cpu-stage/bin/main_game.exe` 与构建收据的
`cb95bacad01ad42d92cf2826f061c03d71fbb51d85f504efad4c2d1f9a9091f8` 一致；
兄弟目录 `../p0-cook-s/bin/main_editor_r32.exe` 与隔离 Local 构建收据的
`9c0c89981cbd00d7bfaf32695b77d43de51d68b36da3879197dc0fa2345c0948` 一致。
R32 patch 仅声明 R32_FLOAT → eR32Sfloat 映射，没有改为有损权重格式。
本次没有对全部约 19.79 GB 旧资产重新执行 payload 哈希扫描；
不把账本中大包的尺寸/mtime 当作本次重新证明的内容摘要。

## 原始统计复算

直接解析 JSONL，不调用实现者的统计函数。分位数采用排序后
`(N-1)*q` 的相邻样本线性插值。每帧 GPU 值先求该帧各 pass 之和，
然后对帧求分位数，没有相加各 pass 的 p99。

| quiet-final 数据 | 原始数量 | p50 ms | p95 ms | p99 ms | 最大观测 ms |
| --- | ---: | ---: | ---: | ---: | ---: |
| instrumented GPU pass sum | 1080 | 2.057040 | 3.4107168 | 4.198024 | 4.976608 |
| geometry，含 TES | 1080 | 1.358336 | 2.5482752 | 3.41225472 | 4.225024 |
| 未 clamp render-frame 墙钟间隔 | 1081 | 33.3648 | 44.5673 | 65.549240 | 306.509 |

数量、minimum、p50、p95、p99、maximumObserved 全部与冻结配置一致。
检查隔离计时源码，CPU_RAW 在 exposure clamp **之前**输出；
不是用 120 帧滚动均值推导上述分位数。
GPU timer 源码读取设备 period/graphics validBits，按帧槽完成后读 query；
上述 GPU 总和仍然只代表被计时的阶段。

两种实际生产 JSONL 各有 **5468 个唯一 (mip,x,y)**，
与 8192、innerExtent=128、mip0…13 的完整预期页键集合逐项相等，
没有用重复页凑样本数。Coverage 的全部样本为 `(weightSource=0,heightBlend=3)`，
Local 为 `(1,3)`；各报告的 samples 与 JSONL 逐项相等。

| 实际负载 | 最大 production ms | 最大 output→host copy ms |
| --- | ---: | ---: |
| CoverageAtlas | 2.507070 | 2.985820 |
| LocalLayerArray | 2.227040 | 3.285090 |

合计 10936 页；同一页 production+copy 最大值为 **3.319266 ms**。
它不是两个独立最大值相加。冻结中的三个最大观测值均复算一致。
这证明有限 tile00 集合的测量，不证明所有 placement、邻接积分或未来 producer 的最坏执行时间。
copy 是同步读回参考；后续 GPU→atlas copy 仍须实测。

## 完整路线内存与启动参考

直接读取各 run 的 CPU/WDDM 流，筛选 available 样本，核对 PID，
逐时刻相加 dedicated+shared 后取最大值。四次日志的实际 SHA256
均与各自报告 `process.logSha256` 一致。

| run | 有效 CPU / WDDM | private 峰值 B | 同刻 dedicated+shared 峰值 B | 最大有效 WDDM 间隔 s |
| --- | ---: | ---: | ---: | ---: |
| cold | 751 / 247 | 10,251,661,312 | 8,722,862,080 | 1.989162 |
| raw-cold | 744 / 246 | 10,243,166,208 | 8,723,415,040 | 1.665305 |
| raw-warm-r2 | 758 / 251 | 10,273,546,240 | 8,722,153,472 | 1.626379 |
| quiet-final | 380 / 125 | 6,907,260,928 | 5,774,319,616 | 1.639130 |

与报告一致。前三次含 reload；取前三次 private/WDDM 各自最小峰值作为严格比较参考，
得到 **10,243,166,208 B / 8,722,153,472 B**。
没有拿 quiet-final 无 reload 的较低峰值替代它们。
cold、raw-warm-r2、quiet-final 各有一个末尾 unavailable WDDM 行，
raw-cold 没有；未发现中间 unavailable 行。helper exit1 被保留，
不能据旧报告 `missingEvidence=[]` 声称没有缺样。

首次 commit 观察分别为 174.3773837、174.9647824、
175.8325527、153.7479062 s。最小值减 0.250 s 后为
**153.4979062 s**，与冻结一致。该减法是针对轮询不确定性的保守比较口径，
不是从日志观察反推出精确引擎事件时间的证明。
后续 V2 应使用真实 monotonic readiness/commit 事件核对。

## quiet-final host audit

直接解析 370 行，available 全为 true，compilerProcesses 全为空，
无 error 行；与 summary 的 busy/error 计数一致。
首末 elapsed 为 **0.0024264 / 197.0190529 s**；
相邻间隔中位数 **0.5309574 s**、最大 **0.5542298 s**。
驱动脚本先启动 watcher，再同步运行测试，最后结束 watcher；
renderer 报告运行时长为 195.6289746 s，审计时域与包围式调用关系相容。
因此“采样时未观察到指定编译进程”成立。

存在明确限制：进程名白名单不覆盖所有工作负载；短于采样间隔的编译可能漏检，
Process32First/Next 结束路径也未在每次结束时证明没有枚举错误。
故不能把 `available=true` 扩大成“主机绝对无任何干扰”。
冻结文本已经限制了该主张，不构成本次相互矛盾。

quiet-final 的 `worldReloadRequested=false` 且仅一次 commit。
报告中 `worldReloadGenerationAdvanced=true` 来自
`no_world_reload or (...)` 的条件门禁，不代表实际重载。
“reload”段在该模式只是重复 near 机位。本审计没有用这些名字证明重载，
正式重载/内存证据取自前三次完整路线。后续证据消费者必须保留这种区分。

## allocation requirements 与冻结逻辑

读取 allocation probe 源码核对用途：仅 create resource / query requirements，
未执行 memory allocation 或 dispatch。本审计没有重新运行 probe。
按 `allocationBytes*multiplicity` 独立求和：

| 分类 | 复算 B | 冻结稳态 cap B |
| --- | ---: | ---: |
| 三通道共享池 | 226,492,416 | 234,881,024 |
| 全输入包络，含两种权重表示 | 759,801,856 | 805,306,368 |
| 已列页表、buffer及4 MiB descriptor reserve | 28,740,672 | 33,554,432 |

第三行尚余 **4,813,760 B**，不能视为所有后续 scratch/反馈目标已经测全。
输入包络余量 **45,504,512 B**；物理池余量 **8,388,608 B**。
探针对未来资源采用的 flags/format/extent 是测量条件；
实现若改变这些条件，必须重新查询实际 requirements 并在原 cap 内准入。
不允许沿用 payload 算术绕过准入，也不能因此扩容冻结预算。

CPU 六项 reserve 相加为 512 MiB。RVT GPU 子类
224+768+32=1024 MiB；峰值子类相加为2048 MiB。
外层 terrain peak=3072 MiB 与内层峰值不是可以各自独立用满的额度：
两个完整1 GiB RVT加两个几何世界可能被外层拒绝，配置已明确允许拒绝候选保旧。
几何 885,297,152 B 是全驻留 payload 包络，不是实际 Vulkan 分配高水位。
磁盘分类和为 **19,792,957,594 B**；材质数据与256 B文件头分开，口径一致。

独立复算目标公式：

- GPU p99×1.10 向上取三位为4.618 ms；加6 ms为10.199 ms。
- geometry p99×1.10 为3.754 ms，CPU p95/p99×1.10 为49.025/72.105 ms。
- 启动参考×0.5向下取三位为76.748 s；加10 s为86.748 s。
- 两个完整路线内存参考×0.8向下取整为8,194,532,966 / 6,977,722,777 B。
- Full16 mip4…13页数为16×(16+4+8)=448，根页16，和冻结集合上限相符。

这些目标没有用失效 quiet 或 raw-warm-r2 CPU p99 放大正式分母。
`raw-warm-r2` 同时出现在有效功能证据和 excludedRunIds 中，
后者明确只排除正式 CPU 分母；不能解释成整份运行既有效又无效。

## 目标风险、弱项与最终判定

1. 每帧1页、30 Hz、10 s收敛只给出约300次新生产机会，
   512槽还包含根页、Reserved和Retiring。复杂近景或多块首次可见可能不足；
   P0没有证明其可达性。必须在后续真实反馈工作集上报告成功或预算不足，
   不得增加slots、放宽bias或延长10 s。
2. workspace剩余约4.59 MiB，未来邻接累积scratch、不可变descriptor、
   状态和反馈目标可能消耗它。固定4任务不自动证明工作区满足预算。
3. 生产样本只有tile00、固定heightBlend=3；readback不等于GPU直拷，
   合成oracle八模式也不能替代实际跨块生产、画质或TES误差验收。
4. GPU sum不含未计时环境工作，geometry含其他几何；CPU间隔包含pacing和capture。
   period/validBits有源码读取路径，但raw行没有自带这两个设备值；
   这是复现元数据弱项，不能将当前数值推广到其他设备或声称纯TES耗时。
5. WDDM约1.54 s采样会漏瞬时峰值，首张截图含主动等待，
   不证明最早可交互画质。本次冻结明确承认这些边界，并定义了后续真实事件、
   实际页身份、图像ROI及TES检查；不能用队列空/估算histogram替代。
6. 固定Full16参考路线不等于逐块、24内部边和9交点全覆盖。
   当前数据足以冻结这一参考路线；最终独立Game仍须补全方案规定覆盖。

**最终判定：PASS。** P0所声明的有限基线、资源需求、预算和目标冻结有原始证据，
本次独立复算未发现影响该结论的哈希、字节数、统计或比较分母矛盾。
上述弱项作为验收边界保留；没有将未来V2目标当成已经达标的测量。
P1–P5/P6的实际满足情况为后续验收事项，本报告不代签。
冻结文件仍保持原 `independentReview=pending`，本报告是独立验收收据；
未修改其内容或SHA。任何后续不达标应如实报告，禁止放宽现有门槛。

文档门禁：`py -3 tool/validate-docs.py`，exit 0，`documents layering OK`。
