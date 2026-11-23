# 合成地形 VT：最终证据索引

> 本文记录前一阶段 2×2 成品。后续用户扩大到完整 16 地块并要求旧运行路径退出，
> 当前切换与验收见 [完整岛屿切换记录](terrain-full-island-cutover.md)。
> 不用本页历史包摘要替代后续完整岛屿身份。

日期：2026-11-20。本文为执行记录，不是实现契约。
源码分支 `codex/terrain-openworld`，资源分支 `codex/terrain-openworld-assets`；
两者有未提交内容，不能用 HEAD 代替实际源码/产物摘要。
对应 [计划](../plan/rendering/terrain-composited-vt-lifecycle-plan.md)、
[实现合同](../contracts/terrain-composited-vt.md)、
[操作入口](../reference/rendering/terrain-composited-vt-acceptance.md)。

## 1. 最终配置

- 真实 2×2 Pegasus 地块，每块 1008 m，不以缩尺场景替代真实尺寸质量。
- 每块虚拟分辨率 8192²，约 0.123046875 m/texel。
- 每块 192 个物理驻留槽；World cache admission 仍为 256 MiB，
  源数组及 driver/renderer 其他内存另计。
- mip0 中心评价，粗 mip 4×4 面积过滤，三通道 RGBA8；
  filter rule 为 `mip0-center-coarse-area-local-normal-v2`，generator 为 2。
- 最多 8 tap 虚拟各向异性，中心页 border 优先；不预取每页九页邻域。
- 静态高度混合显式设置 `u_heightBlendDistanceFade=0`，所有距离使用合成基础属性。
- 近场 tiling 高度独立常驻 R16 mip2；canonical texel 空间四点双线性。

每块完整虚拟 mip 链为 5,468 页，材质 payload 1,213,633,536 bytes。
Cook 逐页产生并写包，单页 payload 221,952 bytes；不整包聚合内存。

## 2. S1 数值与真实图像

真实 GPU/helper 的有限范围数值收据：

| 门禁 | 结果 | 原始证据目录（artifacts/terrain-composited-vt 下） |
| --- | --- | --- |
| 8 tap footprint/反馈/材质采样 | 18,496 样本，颜色/N/ORM 各最多 1 byte，法线角差 0.280° | `anisotropic-8final/` |
| 缺邻页使用中心 border | 9 次 dispatch，单中心页与全驻留数据最大差异 0 byte；旧查页负控被拒绝 | `center-border-8final-v2/` |
| 实际高度 helper 对独立 double oracle | 221,952 样本，最大 0.004916192 R16 单位；源 mip2/border 数据不匹配 0 | `independent-gpu-bake/canonical-helper-real-v1/` |

实际绘制收据为
`independent-gpu-bake/s1-r8192-budget192-run/report.json`，
driver 退出 0，状态 `screened-pass-awaiting-human-review`。
18 次 renderer 覆盖 direct/composite、远中近、轮廓 mask/基础色/光照图。
数值对照固定同一 LOD0、无 TES，避免把几何 LOD 法线差异混入缓存误差。

门槛在对照前固定，未因失败提高：地形 union 与所有有效 4×4 屏幕网格 ROI 的
RGB 平均误差 ≤3/255、P95 ≤8/255，轮廓 XOR=0。

| 视角 | 基础色平均/P95 | 光照图平均/P95 | 所有 ROI | 轮廓 XOR |
| --- | --- | --- | --- | --- |
| 远 | 2.09 / 7 | 1.41 / 5 | 通过 | 0 |
| 中 | 0.88 / 3 | 0.63 / 2 | 通过 | 0 |
| 近 | 0.80 / 3 | 0.58 / 2 | 通过 | 0 |

主线程通过图像查看工具检查了中景 direct/composite 和近景 composite：
地表细节连续，未发现明显物理页缝、串层或方向翻转。
这是模型的视觉复核，不冒充用户的最终人工验收。
正常 LOD/TES 产品视图单独检查，不以固定 LOD0 数值门禁替代它。

旧 `s1-r4096-main-run`、`s1-filter-v2-r4096-run`、
`s1-filter-v2-r8192-run` 和首次 center-border 运行保留失败状态：
分别记录各向同性/重复低通、九页邻域预算扩张、64 槽不足及窗口 0×0 重建。
最终通过不覆盖或删除失败证据。

## 3. S3 运行与预算

`artifacts/terrain-runtime-stress-run03/report.json`：
baseline64/budget16 均 `failures=[]`、`missingEvidence=[]`，总状态 passed。
覆盖两线程、近中远、24 次移动/转头、瞬移、两次 World reload。
两档所有稳态 queued/reading/stale/dirty/readback 均为 0，
尾部 readsStarted 增量为 0；低 16 槽实际使用 budgetMipBias=2，
选中 6 页，证明超预算时粗化而不是无限细页读取。

包括源资源的目标 child PID 采样峰值和计时分位数见
[压力记录](terrain-composited-vt-stress-record.md)。
CPU private 峰值约 5.3 GiB，WDDM 同时刻 Dedicated+Shared 采样峰值约
4.35/4.49 GiB。该值包含 driver 和整个 renderer，不是 VT cache 专属值，
也不是采样之间不可遗漏的精确瞬态 high-water。
12 Hz pacing 的 CPU 时间包含节拍等待，不宣称为无限速吞吐 benchmark。

## 4. S4 实际编辑

组合证据：

- `artifacts/terrain-editor-s4-worker2-monitorfix01/editor-s4-report.json`：
  failures/missingEvidence 均为空；旧 GPU 结果拒绝从 0 增为 1。
- `artifacts/terrain-editor-s4-worker1-monitorfix01/editor-s4-report.json`：
  无失败、正常退出、无验证层错误；该同步运行没有产生旧任务完成窗口，
  该观测由 worker2 与 Residency 迟到 token 回归补足，原 missing 保留。
- `artifacts/terrain-editor-s4-final/final-record.md`：组合关闭结论与原始文件索引。

测试经真实 CommandBus→EditorHost 处理 Paint、Sculpt、Undo/Redo、连续编辑、
实际材质 preview/revert/reject、外部作者冲突、Save、独立进程 Cook、
重开与 World reload。Undo/Redo 和材质恢复在实际编辑像素上残差为 0；
冲突没有覆盖内存编辑；保存 manifest 认证落盘 Paint bytes。

实际失败修复分别有收据：旧 sampler descriptor 退休导致 device lost、
PaintData 原地修改导致 Undo 未推进页面、monitor 与 Cook 摘要算法漂移。
修复后正常绘制/版本/保存重开记录不以 CPU-only checker 代替。

几何流式的高度 revision、静态/动态能力、注销重建、三槽 ACK 和隐藏 LOD
由真实 worker/queue 的 CPU 回归覆盖；不宣称已遍历所有驱动和所有 IO 故障。

## 5. 构建、模块与最终成品

当前模块收据：

| 目标 | 结果 |
| --- | --- |
| terrain_tests | 523 项，520 通过、3 条件跳过；真实资源依赖用例另行通过 |
| editor_ui_tests | 1110 项，1105 通过、5 条件跳过 |
| vl_renderer_backend_tests | 305 通过 |
| material_instance_thread_safety_tests | 5 通过 |
| shader_build_core_tests | 9 通过、1 symlink 权限跳过 |
| engine_launch_options_tests | 19 通过 |
| game_export_tests filter-v2 suite | 67 项，66 通过、1 symlink 权限跳过 |

UI 的旧 Blueprint 预览用例时间竞争已用受控时钟复现并修正夹具，
没有弱化鼠标命中/选择断言；两用例 40 次重复与 139 项 Blueprint 回归通过。

最终 filter-v2 8K 产品包正在独立重复正常冷启动、负向输入、恢复及官方 validator。
较早 `game-delivery-final/final-r4096-04` 是 filter v1 的通过收据，
不能用它证明新版包已经通过。新版最终路径、manifest/exe SHA 与包大小以随后
`game-delivery-final/final-filter-v2/` 的权威结果补充。

### 最终包补充收据（2026-11-20 续）

`game-delivery-final/final-filter-v2/acceptance-results.json` 已证明新版冷启动、
七类错误输入的 worker1/2 拒绝、逐项恢复、最终 verify 与官方 validator 均通过。
产品为同级 `vt-game`，563 文件、5,118,836,628 bytes。
`main_game.exe` SHA256：
`d9adce053b41089b69709067f27edcd7999cd962b5f973def2e3895eff5236f4`；
`game-export-manifest.json` SHA256：
`ef4341de7916c9e3dabe1955b31053ee2efaad0fccdd7078217960d499cd0627`。

同目录 `product-cost/cost-results.json` passed：目标 PID 86012，
双线程、正常 LOD、近景 1,800 帧、60 Hz pacing，正常退出，无验证层错误或 shader miss。
31 个计时快照的 CPU p50/p95/p99 为 16.690/17.150/21.7244 ms，
GPU 为 0.097/0.112/0.1134 ms。CPU 包含节拍与 present 等待，
不作为无限速吞吐结论，也不把抽样快照当逐帧分布。
启动加运行采样 CPU private 峰值 5,478,002,688 bytes，
同一采样时刻 WDDM dedicated+shared 峰值 4,968,931,328 bytes；
最后 30 秒分别为 4,006,813,696 / 3,306,500,096 bytes。
这些是整进程、含源纹理和驱动的抽样值。
World VT cache payload 为 210,644,416 bytes，源资源另计 186,696,804 bytes；
四表面的 queued/reading/dirty/stale 均为 0。运行前后包 manifest、
包 shader cache 和共享 shader cache 摘要一致。

`product-views/capture-results.json` 的中/近/坡面均正常退出，未强制 LOD；
实际绘制同时包含 LOD0/1，detail MI 启用 TES，包内有 TCS/TES 输出。
主线程已查看三图，坡面左侧存在可疑矩形明暗边界，近/中景可见少量黑点。
该视觉疑点尚需同姿态 direct/composited 对照定位，不能将截图存在、
退出码或 TES 宏当作最终视觉通过。固定 LOD0 数值门禁保持通过，
不替代此处的正常 LOD/TES 门禁。

### 坡面诊断续记

`slope-diagnosis/` 的相同姿态 direct/composite 证明两条路径都有块边；
`slope-diagnosis-fixed/` 保留 TCS 外边映射与 TES 宏面插值修复后的实际图。
修复降低了近景黑点，坡面块仍存在；shadow 通道没有相应边界，
不能把先前的阴影嫌疑写成已证根因。
关闭 detail、macro 后块仍存在，二者也不是充分解释。

`slope-isolation/CONCLUSION.md`：height-blend strength=0 后仍有块；
直接输出 mesh normal 时左侧小块明显消失，top4 权重图有相应边界。
源码确认 mode0 使用 nearest 层 ID，却线性过滤按排名存储的权重，
相邻 texel 的 ID 排序变化时没有对齐同层权重。
当前共享 ID-aware coverage 重建修复与 CPU tile 候选保守性调查正在进行。
旧包功能/成本收据保持其原快照有效性，不视为修复后身份的验收。

后续 `coverage-id-aware-actual07/report.json` 已通过：136 个实际 GPU 查询，
最大权重误差 9.55e-8，跨地块污染权重为 0；旧排名槽过滤与物理邻页负控分别
37/20 项失败，生产 bake 八模式及四阶段编译链接通过。
`coverage-direct/CONCLUSION.md` 记录修复后的真实 direct 图仍有块边，
因此上述确定的采样错误不是该大块现象的充分解释。
`artifacts/terrain-candidate-probe/report.json` 对 tile05 的 64 页、1,048,576
texel 和全层参考逐 byte 一致，当前七层均在候选中；这只排除候选剔除，
不把共用 BakeTile 的参考当作全部覆盖计算已独立证明。
最终正常产品视觉审阅及新源码身份的 Cook/包重验仍未完成。

最新 shader 的 S4 重验见
`artifacts/terrain-editor-s4-worker2-finalshader01/editor-s4-report.json`：
failures/missingEvidence 均为空，bootstrap Cook、edit、saved Cook、reopen/reload
四进程均 exit0、无 validation/08114/DeviceLost。
Paint/Sculpt Undo/Redo 及 material revert 截图 SHA 完全恢复，编辑区域残差 0；
24 次 rapid stamp 均改变实际 WorldHash，旧结果拒绝计数 0→23。
外部冲突反馈延迟 909 ms；保存/重开 RGB 平均差 0.001741，在既有阈值内，
World generation 1→2。该收据包含新 coverage helper 与 TCS/TES 修复，
不复用旧 shader 的编辑结果代替重验。

`artifacts/terrain-candidate-probe/SCREEN-RESULT.md` 随后将实际屏幕 ROI 的
6,156 个点映射到 coverage X517–520/Y653–657：全部在同页 `(4,5)`，
第五层权重均为 0，不能把其他定点的 top4 截断现象套用到该 ROI。
ID-aware 采样消除了 nearest 假换层的旧 normal 跳变；
真实原始/重建权重跨主导层等权线时，默认 detail normal 仍跳 13/31 byte，
相同位置关闭 detail/macro 后仅差 1/0 byte。
这提供了对该 ROI 若干边界的直接归因：`ids[0]` 的离散 detail 选择，
不是笼统的“所有坡面块都是同一原因”。
已据此安排连续覆盖权重下的 detail/macro 与 tiling 位移混合修复；
上述 finalshader01 仍是该后续修复之前的通过快照，需要按受影响范围重验。

最终加权 MF 冻结 SHA256 为
`40271255b210f7ff95d55528690b59b8ba24b060af5f9ffa1816013758858dd9`。
`weighted-detail-actual02/report.json` 的 136 个 GPU 查询通过，独立参考误差
1.49e-7，换序误差 1.19e-7，direct/composite 原始覆盖权重差 0，
位移误差 1.43e-9 m；旧 dominant 法线负控产生 0.0774 跳变。
主线程查看 `weighted-direct/direct-mountainside-beauty.png`，已定位左侧硬边
明显消除；新合成包的正常几何图仍单独验收，不以此 direct 结果替代。

`artifacts/terrain-editor-s4-worker2-finalshader02/editor-s4-report.json` 为此冻结
版本的实际编辑重验：failures/missingEvidence 均为空，四进程 exit0，无 validation
错误。Paint/Sculpt Undo/Redo 和 material revert 截图 SHA 完全恢复，
编辑区域残差 0；24 次 rapid stamp 改变 WorldHash，旧结果拒绝 0→23；
外部冲突延迟 218 ms。Save/Cook/reopen/reload 通过，
generation 1→2，保存重开 RGB 平均差 0.001568，在既有阈值内。

最终 MF 的 S1 重验：
`artifacts/terrain-composited-vt/s1-weighted-r8192-pair3/report.json`，
私有 Cook exit0（186.964 秒），18 次绘制正常完成，所有截图工作集已收敛，
原 union/grid 平均≤3、P95≤8 门槛全部通过，轮廓 XOR 全为 0。
远/中/近基础色 mean/P95 约为 2.03/7、0.87/3、0.79/3，
光照图约 1.39/5、0.62/2、0.58/2（精确值以 JSON 为准）。
主线程查看中景 direct/composite、近景 composite 与远景 composite，
未发现明显新增页缝、错层或方向翻转；这仍不冒充用户最终验收。
新产品 Cook/包和正常 LOD/TES 成品视图继续单独关联。

### 最终成品关闭收据

权威目录为 `artifacts/terrain-composited-vt/game-delivery-final/weighted-final/`，
汇总 `final-evidence.json`，身份 `final-package-sha256.json`。
前述 `final-filter-v2` 的初次 8K 包已完整移至同 topic `vt-h2`，
不覆盖历史失败、诊断或原七类负向收据。
新产品仍为 `vt-game`，导出 inventory 为 564 文件、5,118,970,251 bytes；
另有 manifest 111,892 bytes 和启动生成的十一份参数 GLSL 共 7,891 bytes，
最终目录实数为 576 文件、5,119,090,034 bytes；
exe SHA256 保持 `d9adce053b41089b69709067f27edcd7999cd962b5f973def2e3895eff5236f4`，
最终 manifest SHA256：
`0692e3a709d1a8d265c7de6b13aa74f03f3f37fdf72f3a92297149ff62006573`。

四份正式产品 Cook 正常退出；`identity-change.json` 逐面记录不同于旧包的
dependency/content hash，未复用旧身份。包导出、pristine verify、cold worker2、
错误版本/旧 filter 的 worker1/2 拒绝及恢复、官方 validator 均通过。
这是变更后有限重验，其他未变故障类别链接旧七类完整收据，不虚称全量重跑。
`verify-after-views-cost.log` 最后再次报告 `ok=true`。

主线程实际查看 `product-views` 的中/近/坡面三图：
已定位的左坡 dominant-detail 硬边与近景黑点消除，未发现新增 VT 页缝、
串层或法线方向翻转。正常 LOD0/1 选择及 TES cache 角色由 capture-results 记录，
不以宏存在代替画面，也不宣称遍历所有机位和 GPU。

最终 `product-cost/cost-results.json` passed、全部 checks=true：
目标 PID79548，双线程近景 1,800 帧，60 Hz paced，正常退出，无编译/cache miss。
30 个快照的 CPU p50/p95/p99 为 16.6685/20.87075/23.16791 ms，
GPU 为 0.0945/0.095/0.10352 ms；非无限速/逐帧 benchmark。
GPU 计时的 query 只覆盖当前报告的部分 passes，不代表完整地形/TES 帧成本。
启动加运行 CPU private 采样峰值 5,455,433,728 bytes，
同刻 WDDM dedicated+shared 峰值 4,320,878,592 bytes；
最后 30 秒分别为 4,028,907,520 / 3,306,508,288 bytes。
CPU/GPU 有效采样 133/42，尾窗 58/20。
cache/source payload 仍分别为 210,644,416 / 186,696,804 bytes；
包 manifest、包 shader cache、共享 shader cache 运行前后不变。
该记录承担最多四层 detail/TES 采样增加后的真实成本，不沿用旧单层结果。
进程/内存记录包含加权版本，但现有部分 pass 的 GPU query 不能独立证明
四层 TES 的精确耗时增量。

## 6. 交付边界

第一版使用非压缩页，不声称 BC 输出已实现。detail/macro/高度 mip3 等真实
实时消费者仍保留源数组，统计单独显示，不声称整个地形运行时只含 VT atlas。
旧场景通过显式 TR 开关继续使用旧路径；新产品默认合成路径不创建旧层物理 atlas。
碰撞与脚 IK 仍来自宏高度，微小 tiling 置换不是新的物理高度。
窗口最小化引出的已有 0×0 swapchain 恢复缺陷单列，不在本次地形工作中重写平台层。
