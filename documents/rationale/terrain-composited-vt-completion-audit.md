# 地形合成 VT S0–S5 独立完成度审计

> **阅读入口（2026-11-20 最终闭环）：最终结论见 §23–26：本次约定的 S0–S5 已实现及验收范围全部完成，无剩余阻断。§19–22 记录 weighted 修复过程；更早章节是相应时间的历史快照。完整 GPU frame/TES 性能、任意设备或所有旧 legacy artifact 不在已证明结论之内。**

> 状态：非契约、可审计的完成度清单；不修改计划目标，不代表阶段验收通过。
> 审计日期：2026-11-20（Asia/Shanghai）。工作区：`codex/terrain-openworld`。
> HEAD：`5df9fbc0af122581260d614b3b9f35d8a4d8bacd`，大量实现尚未提交，HEAD 不是被审计内容的完整版本。
> 证据观察窗口截至约 11:38；其他执行者仍在追加包验收产物。以下是本次读到的事实，后续修复须另附收据。行号随并行修改可能漂移，定位时以所列函数名为准。

## 1. 口径与结论

对照 [生命周期计划](../plan/rendering/terrain-composited-vt-lifecycle-plan.md) §3–10 及 S0–S5
逐项核查。`proved` 仅表示所列边界内有源码或既有执行证据；`incomplete` 表示部分实现、
覆盖不足或有已知反例；`missing` 表示本次未找到该项所需实现/验收收据，不等价于断言它永远不存在。
模块证明不传递为 Cook→Store→上传→主视图→Editor→Package 的整链证明。

**总体：S0–S5 不能宣布完整完成。** 已有真实四 surface Cook、独立 GPU 合成验证、
单/双线程只读场景出图和 exporter 闭包实现；Editor 失败恢复、真实像素对照、性能阈值、
旧链路退出及真正可运行的 game 包仍未闭合。S1 尚无整场景画质退出证据，
不能因 S3 已接线而追认 S1 完成。

本次仅阅读文件、搜索源码、核对摘要并新增本文。未启动 renderer、未 Cook、未改资产、
未重跑已有模块测试；包运行结果来自 Maxwell 正在写入的证据。本文建议的复现均为后续动作，
不是本次已执行的测试。

## 2. 审查入口与证据边界

源码核查覆盖以下新实现及相应头文件/接线：

- 协议与 CPU：`terrainCompositedVT`、`terrainCompositedVTStore`、
  `terrainCompositedDependencies`、`terrainCompositedResidency`、
  `terrainCompositedFeedback`、`terrainHeightAtlasLayout`、`terrainHeightAtlasStore`。
- GPU 与服务：`terrainCompositedBakeGpu`、`terrainHeightAtlas`、
  `terrainCompositedService`、`terrainCompositedRuntime`，以及 terrain loader、
  resource coordinator、frame recorder、feedback ring/runtime。
- Shader：`terrainCompositedBake.comp`、`terrainCompositedEvaluation.glsl`、
  `mf_terrainCompositedVT.glsl`、`terrainCompositedFeedback.glsl`、
  `terrainHeightAtlas.glsl`、主地形 MF、feedback fragment 与两份材质声明。
- Editor/导出：作者 monitor、Paint/Sculpt 刷新、MI preview 通知、
  启动 Cook 开关、stats 入口、`terrainPackageClosure.h` 与 exporter 调用点；
  新 terrain 测试及 `tool/validation/terrain/` 的 fixture、GPU、feedback 和 acceptance harness。

主要证据根为 `artifacts/terrain-composited-vt/`（下表路径均相对此根）。
忽略目录中的复制 shader/运行根是运行快照，不能当作另一份当前实现。

| 证据 | 本次可确认的事实 | 不能推导的结论 |
| --- | --- | --- |
| `final-cook.log`、`final-cook.exit.txt` | 四 surface，各 92 页、20,419,584 bytes 材质 payload，退出 0；打印 content/dependency hash | 中断恢复、最大尺寸、峰值内存、运行失败注入已通过 |
| `independent-gpu-bake/report.json` | RTX 3080 上八类生产 compute 页验证及距离对照通过；距离组 colour 最大 1 byte、normal/ORM 0 byte，四阶段编译链接通过 | 完整场景主视图像素对照、TES 或 LOD 法线等价 |
| `feedback-address.log`、`feedback-protocol.log` | 16,335 地址表达式案例、540 mip 案例、57,344 地址案例及 metadata/编译检查有通过记录 | 实机 eviction、旧 World、失败上传与 fence 延迟压力通过 |
| `independent-readonly/run_20261119T150508Z_57eed261/independent-review.md`、`run.json`、六张 BMP | `main_editor.exe`，worker1/2 正常退出，近中远截图齐全，对应线程图片 hash 相同；末态 dirty/stale/readFailures 为 0 | game 包冷启动；有编辑工作负载的 dirty 收敛；逐像素 parity |
| 同上 | 每面 resident 最多 17/64；GPU 驻留为空的新进程启动，OS 文件缓存未清除 | 已施压 LRU/槽位复用；真正磁盘冷缓存 IO 吞吐 |
| `all-terrain-tests.log` | 486 项运行，483 通过、3 跳过 | 跳过的真实资产/符号链接项通过；483 项等于整链完成 |
| `all-renderer-tests.log`、`export-tests.log`、`launch-tests.log`、`editor-tests.log` | 分别 305 通过、58 通过/1 跳过、19 通过、1093 通过/5 跳过 | Editor 笔刷实机闭环或最终 game 包已通过 |
| `cpu-tests.log` 与后续 `all-terrain-tests.log` | 早期 odd-parent 审计项失败；当前 Parent 已用整数范围求 covering ancestor，后续全 terrain 日志无失败 | 旧只读二进制覆盖了新 Parent；该 review 明确记录源码比 exe 新 |
| `S5-preflight/*` | fixture、机位/输入配对、作者副本与分辨率变体有准备证据 | 几何视锥检查等于材质、页缝、斜坡和性能通过 |
| `S5-sidecar-harness/result.json` | 7 文件闭包、8 个负例拒绝，未启动 renderer | 已得到可运行 game 包 |
| `closure-preflight/result.json` | dev 导出资源闭包 332,964,625 bytes、28 Cook 文件检查通过 | 报告明确不是 game shader cache/运行验收 |
| `game-delivery-final/acceptance-results.json` | 最新读到 stage-warm/export/verify-pristine 均退出 0；package-cold 最新退出 1，先前 Maps 缺失轮退出 3221226505 | 包冷启动成功或 S5 完成 |

`normal-parity-formula-audit.json` 是 **CPU 源码公式反例，executedGpu=false**。
本次核对其七个源码摘要：meshBuilder、两个 normal 相关引擎 shader 及三个 MF 全部一致；
service 文件摘要已变化，当前 `Frames` 仍是单一细网格梯度生成（见 §5）。
因此保留为具体未解决风险，不能把历史 JSON 当作当前 GPU 实测。
既有运行报告的时间/摘要不是所有未提交源码与二进制等价的证明。

## 3. 分阶段逐项判定

| 阶段/退出项 | 状态 | 事实与欠缺 |
| --- | --- | --- |
| S0 资源分类、逻辑地址/内容身份、依赖键 | proved（源码范围） | 页键含 surface/mip/x/y；hash 记格式、generator、依赖；token 记 World/revision/hash/key；coverage/height sidecar 各有协议 |
| S0 权重优先级、local/global/source slice、支持范围 | proved（有限范围） | 两种权重来源显式映射；baker 拒绝多 sourceSet、镜像/非均匀缩放、未确认保留实时项及不支持项；不是全材质解释器 |
| S0 旧 feedback 地址修正 | proved（表达式/编译范围） | 既有地址 harness 通过；不把它扩大为旧路径全部 LOD、权重与实机场景一致 |
| S0 全资源 producer/owner/consumer/失败退休无阻塞 | incomplete | 服务刷新失败恢复、game 作者依赖、总预算与旧路径退出仍有阻塞（§4–7） |
| S1 多虚拟 mip、albedo/normal/ORM、border、颜色空间/粗滤波 | proved（实现/独立 GPU 范围） | 128+4、完整链至 1×1，有限子样本面积评价、normal 归一化/roughness RMS；RGBA8 三通道已固定，不把 Store 支持 BC 名字当成已压缩 Cook |
| S1 单地形固定页集与真实直接评价一致 | incomplete | compute harness 有公式对照；无完整 Cook→主视图配对通道差分和冻结阈值；有 LOD 法线反例 |
| S1 height 源 mip2、repeat border/global map | incomplete | layout/copy/Store/GPU Save/Load 和真实 Cook payload 存在；无 TES 实机 mip2 对照、跨高度页/切层/detail 对齐量化收据 |
| S1 斜坡/层界/平铺密度/斜视、每级 mip | missing（整场景退出证据） | 只读 review 明确近中景多为沙地、陡坡/层界覆盖不足；无逐 mip/页缝像素矩阵 |
| S2 依赖、完整索引/保底、版本和原子发布 | proved（实现/模块范围） | 当前 Store 是逐页流式临时文件写出、校验、最后原子替换 manifest；不是必须将整个页包驻留内存的旧实现 |
| S2 正式四 surface Cook /只读重开 | proved（该 fixture 范围） | final-cook 成功及独立只读运行存在；height/coverage/dependency 由 sidecar 随包交付 |
| S2 中断/损坏/缺页/版本不匹配整链 | incomplete | Store/exporter 负例已有；真实 Runtime 慢盘、缺页、baseline 错误、旧有效包保持没有联合执行证据 |
| S2 包体、成本、最大尺寸 | incomplete | 已知每面 92 页及包字节；没有带阈值的构建时长/峰值工作区/最大可用表面规模报告；旧 preflight 的尺寸限制判定可能落后于当前 4 GiB 上限 |
| S3 feedback→异步读页→上传/页表→父级 | proved（接线及只读 fixture 范围） | 源码按 fence 消费，Record/Commit/Abort 接到 frame；不再按整层 mip 请求合成页 |
| S3 贴地/远眺、两线程 | incomplete | 六张截图和 worker1/2 通过支持基本可运行；没有相机阶段归因的 actual sampled mip、快速转向移动压力矩阵 |
| S3 低预算、失败上传、eviction/epoch、World 换代 | incomplete | CPU 状态机和事务存在；64 slots 实测最高仅 17，无真正槽位压力或延迟完成后的 GPU 复用证据 |
| S4 revision、dirty 邻域/祖先、旧结果拒绝 | proved（源码范围） | SnapshotDirty 比较 Paint/高度快照，DirtyPages 包含 border/父 mip；回读 token 校验并计 staleBakeResults |
| S4 Paint/Sculpt/Undo/Redo、连续笔刷实机最终一致 | missing（整链证据） | 编辑入口已接，既有只读运行 dirty=0 并不覆盖一次编辑 |
| S4 材质改动、Save/重载/外部变化与未保存保护 | incomplete | MI 通知和异步作者 monitor 存在；材质改动未见调用 monitor MarkEdited，CheckBeforeRecook 未见生产调用；Save 后基线仍需明确的重载/重建生命周期 |
| S4 失败保旧页、预算下旧新过渡、可恢复性 | incomplete | 页槽机制支持 stale；服务层在成功候选建立前修改 live revision/输入，失败缺少事务回滚/重排队；§6 给出源码复现路径 |
| S4 小笔刷不全地形刷新 | incomplete | 合成页 dirty 可局部；服务仍全量扫描快照并重建/上传整个 placement coverage（以及高度改变后的 frame 图）；缺逐笔刷面积/页数/耗时证据及保守扩大的诊断 |
| S5 独立资源、多 tile 迁移 | proved（fixture 范围） | topic resources 下真实 2×2 已 Cook；不是当前默认 Pegasus 4×4 已迁移 |
| S5 必需 height payload、消费者核查 | incomplete | 独立 mip2 payload 已加载/导出；height mip3 和完整基础数组仍有消费者；不能直接删数组，也不能声称已脱离它们 |
| S5 删除不再消费的层源 VT/参数 | incomplete | 合成模式绕过旧物理 atlas 创建，但旧页表、管理器、feedback/参数兼容表面仍在；无最终移除清单或退出时点 |
| S5 契约/路由/测试映射 | incomplete | 部分实现契约和路由已写；有些“尚未成功 Cook/构建”的条目已落后于最新证据，不能将历史缺证据永久当作现状 |
| S5 game 包/默认路径/版本/画质及性能退出 | incomplete | 正式包静态验证已通过，首次独立运行异常；默认路径仍旧场景；无已冻结画质/性能/总内存阈值及对照结果 |

## 4. 主视图源数组、近场动态项与旧 VT

`mf_terrainLayerBlend.glsl:309` 用共享 `TerrainCompositedUsePage`：
strength=0 全距离读合成页；strength>0 且距离<45 m 走直接基础层评价，
45 m 及以上读冻结 fade=1 的页。共享评价器保留原 smoothstep 高度分数/归一化。
这符合计划 §4.3 “视角相关项显式保留独立实时项”的允许边界，且有距离 GPU 公式对照；
**不能仅因存在 fallback 判定违规，也不能因此缩减 S1/S5 的画质、资源和性能目标。**

当前 fallback 重算整个 top-4 albedo/normal/ORM，而不是只采一个小型动态修正。
`mf_terrainLayerBlend.glsl:356` 继续采完整三数组；`:374`、`:376`、`:405`
的 detail/macro 继续消费 albedo/normal；`:38` 在请求非 mip2 时读完整 height，
height-blend 请求 mip3。TES mip2 已可从独立 atlas 读入，不能替代 mip3。
`rendererTerrainLoader.cpp:188` 仍无条件要求四个完整数组，
`terrainCompositedService.cpp` 的 Capture 捕获并强持有四数组。只读 Capture 还上传独立
coverage table/ID/weights；fragment 在基础页分支之前就 ResolveTopLayers（`:296`）。
故“主视图完全不经源数组/覆盖权重”尚不成立。

源码已做部分旧资源收敛：`rendererTerrainLoader.cpp:302` 合成模式用源图占位旧 atlas
绑定，不创建旧层物理 atlas，不能误报为一定分配了两套物理层 atlas。
但 `rendererResourceLoadCoordinator.cpp:583` 仍创建层 GPU 资源/页表；
`rendererTerrainLoader.cpp:382` 附近仍填旧层参数。旧模式仍保留用于对照，
需要列明保留消费者和删除触发条件，而不是无条件清空兼容路径。

**unusedparams：** `M_terrain.json:34` 声明 `u_compositedAtlasTiles`，
service 的 Bind（本次末次定位 `:473`）每次写入，但非 generated 生产 GLSL 无读取；
当前寻址使用 array slot，不用二维 atlas tiles。此为可直接核对的未消费参数。
本次没有发现 `unusedparams` 工具收据，不虚构运行过该命令或 SPIR-V 反射结论。
应按实际 shader 消费/反射清理该参数，以及列出仍仅为 legacy 消费的参数与绑定。

## 5. 像素、法线与性能退出缺口

`terrainCompositedService.cpp:189` 的 Frames 用固定 gridResolution 和一步梯度生成局部
normal/tangent 图；`terrainMeshBuilder.cpp:199` 的实际几何法线则受 LOD gradientStep、
过渡带 fineNormalWeight 影响。合成 fragment 直接对缓存局部 XYZ 做 placement
inverse-transpose（MF `:322`），未使用当前 mesh LOD 的宏法线基。
这两条路径在起伏地形上不保证等价。

既有公式审计在接受的 identity/uniform3 摆放上得到 ridge_slope：
LOD2 neutral 最大约 14.277°，LOD3 neutral 最大约 64.388°、
oblique 最大约 48.879°。这些不是实际屏幕像素误差；
被 producer 拒绝的 anisotropic 案例也不算支持输入缺陷。
可重放定义在 `normal-parity-formula-audit.py`，本次未运行。
后续应先固定该法线差异的正确语义，再用真实生产路径拍摄 slope/ridge、LOD0..3、
neutral/oblique 法线、heightBlend=0/非零、45 m 两侧的配对通道图。

现有 GPU 1-byte 阈值只覆盖独立公式 harness，不能作为整幅主视图阈值；
六张 BMP 只有 worker 间 hash 对照，没有 direct/composited 的 pixel diff、
normal 角误差、ORM/颜色误差或页缝/斜视掩码统计。
`run_composited_acceptance.py:363` 本身明确 quality unverified。
2048/100 m 变体准备存在不代表已出图。应覆盖细分位移开关、mip2 源/atlas、
Paint 主导层变化和 detail 对齐，保留每组输入、Cook、shader/exe 的摘要。

只读 review 的 FPS 中位约 59.125/58.4195、CPU 16.9135/17.118 ms，
来自 validation 开启、60 Hz 限帧的 14/13 个诊断样本；
记录的 GPU total 0.070/0.0645 ms 不等于可靠的全场景 GPU 性能验收。
尚无 S1/S2 基准冻结的全画面误差、CPU/GPU p95/p99、IO、峰值显存和包体阈值。
阶段推进后补“看起来正常”或挑一个 FPS 不能替代计划 §10 的阈值。

预算已增加源图统计，但尚非统一总量限制：
`game-delivery-final/stage-warm.log:134` 报告
cacheBytes=93,380,032、sourceBytes=186,696,804、
accountedGpuPayloadBytes=280,076,836，超过标出的 worldGpuCap=268,435,456，
同时明确 `gpuBudgetScope=cache-excludes-source`。这是诚实的范围标注，
却不是“地形总显存预算已通过”。旧 coverage/层页表、退休重叠、分配对齐、
bake 工作区及几何/碰撞总量还应统一列账；cache admission peak=0 也不是实测峰值。

## 6. Editor 版本、失败与预算

已有正向设计：GT 排队、内部单调 nextEditRevision、快照 hash、回读 token 检查、
旧纹理按 epoch 保活、GPU preview 每次最多记录一页；不是每 stamp 调全局 waitIdle。
但这些不能替服务层的失败提交边界作证：

1. `terrainCompositedService.cpp:829` 在 coverage/frame 分配、生成及上传前更新 live
   revision/hash 并 `InvalidatePreview`；ConsumeUpdates 起点已 swap 出整个队列。
   在其后 BuildVirtualTexture/DeferredImage/Frames 失败时，异常进入
   `RecordUploads` 的 catch；`:1449` Abort 只 abort runtime 和清 pendingReadback，
   没有还原 surface revision/输入，也没有重排队此 batch。存在“版本已前移而新输入未完整建立”的源码路径。
2. ConsumeMaterials 的材质循环逐项替换 live sources（`:721`），`:746` Bind 后再 RefreshHeight。
   后者格式/预算/分配失败时，已替换数组和参数无整体 rollback。
   `:623` height replacement 预算只对当前 gpuBytes 加新布局做检查，
   但成功替换后未见同步调整 gpuBytes 的新旧差额；连续换不同大小高度源的账需核实。
3. `:897` 回读 status 非零直接 Require 抛异常，iterator 尚未 erase；
   没有该页的有限重试/退避/失败状态消费。若外层继续帧循环，同一结果会再次被检查；
   若外层终止，亦不满足“保持旧内容、诊断后可继续编辑”的预期。此为源码推导，未做 GPU 失败注入。
4. `:786` 预算不足仅 Requeue；未见该延期的年龄/拒绝原因与最长 stale 时间。
   RecordPreview 按 surfaces 固定顺序找到首个需求就 return，持续编辑首 surface
   时后续 surface 的公平性没有实机证据。各 surface 各持一个 runtime，
   单 runtime 时间/上传限额不能直接证明 service 跨所有 surface 的逐帧限额。
5. 作者 monitor 实现 dirty/conflict/强制复查；生产 MarkEdited 命中 Paint/Sculpt，
   MI preview 通知没有对应 monitor 调用。CheckBeforeRecook 只有定义/内部调用，
   未找到正式 Cook 的调用方。外部变化 Tick 只反馈“reload/Cook”，不是自动合并；
   不能把 monitor 模块测试写成材质未保存保护与 Cook UI 已闭合。

建议在隔离编辑场景按次注入：coverage/frame 创建失败、非法/超预算高度源、
非零 bake status、连续 A→B→C revision 并延迟 A 回读、两个 surface 连续笔刷、
Save 后重载及外部文件冲突。逐次保留旧页可见、revision/dirty/stale/readback 队列、
预算和最终像素；未取得收据前此项保持 incomplete。

## 7. 真正 Package 冷启动

最新 `game-delivery-final/acceptance-results.json` 已覆盖早先
`export-first-missing-pbr.log` 的失败：补齐 cache 后 export=0、verify-pristine=0。
不能继续把“缺 M_pbr cache”当作当前未修复结论。

首次 `package-cold` 为 **3221226505（0xC0000409）**；
已归档到 `package-cold-first-missing-maps.log`，末尾为未捕获 runtime_error：
`FileSystem.PathNotFound: Could not resolve runtime path. [Maps]`。
末次复查时执行者已继续修复该项：最新报告 `package-cold` 退出 **1**，
当前 `package-cold.log` 为 `EngineLoop init failed: Shader.LoadFailed`，
找不到包内 `shader/glsl/shadowLocal.vert`，之后正常 teardown。
因此 Maps 是历史复现，**当前最末次失败为 shadowLocal shader 闭包缺失**；两轮均未形成可用包。
`package-cold.command.json` 给出可重现命令：

```powershell
# 在最终导出包根运行；本审计没有执行此命令。
.\main_game.exe --exit-after-frames 180 --worker-thread-count 2 --no-dev-ui `
  --console-command-delay 60 --console-command "terrainvt stats"
```

该次运行已使用包内 exe 和包根 cwd，不能用源码树的 stage-warm 成功替代。
日志还报四个音频缺失（有 procedural fallback，不能由此直接认定它是崩溃原因）。
需要修复包启动必需路径/shader 闭包，重新取得包内主视图、terrainvt stats、正常 teardown/退出码、
未调用 Cook/编译工具、没有借用绝对作者根的证据；由 Maxwell 继续独占执行。

还有独立的“精简发布”实现缺口：`RegisterSurface :1273` 不区分 Editor/game，
始终调用 `BuildTerrainCompositedDependencies`，后者读取 TR/高度/Paint、MI/M、纹理源、
GLSL 入口/include。`ValidateTerrainCompositedManifestConfig` 虽存在，
service 没有调用它。因而即使修复 Maps 并使保留完整作者闭包的包启动成功，
也不能证明无构建依赖的只读消费路径成立。应区分 Runtime 必需的几何/实时项输入
和仅供基础页构建的依赖，采用已存 sidecar 的配置/身份校验并验证受限包读取集合。

## 8. 给主线程的三个最高优先级可执行缺口

1. **P1：交付可独立冷启动的 game 包。** 按 §7 最新失败收据补 `shadowLocal.vert` 的必需闭包，保留 Maps 历史回归，
   然后在同一隔离包重验 worker2/正常退出、截图与 VT stats。
   接着消除 service `:1273` 对构建专用作者文件的无条件重摘要，
   审核完整源数组/源 shader 的实际运行消费者。静态 verify=0 和 stage-warm=0 都不是完成条件。
2. **P1：补 Editor 服务级失败事务和恢复。** 把 `ConsumeUpdates :829`、
   `ConsumeMaterials :721` 的 live 版本/纹理发布移到候选成功后，
   或提供完整 rollback/requeue；消费并限次处理 `CompleteReadbacks :897` 的失败结果，
   修高度替换计账并暴露预算延期。按 §6 失败矩阵验证旧页可见和最终最新版本收敛，
   再覆盖 Paint/Sculpt/Undo/Redo、MI、Save/重载、外部冲突，不以 monitor/Residency 单测替代。
3. **P1：解决法线反例并完成像素/性能退出矩阵。** 从 `Frames :189` 对比
   `terrainMeshBuilder :199` 的 LOD normal 开始，复现 §5 的 ridge/LOD2–3 差异；
   固定保留动态 height-blend 的对照条件，量化 45 m、页缝、height mip2/TES/detail。
   落盘 direct/composited 通道差分及冻结阈值、无节流性能分位数、包含源数组的峰值总内存。
   依据消费者清单清理 `u_compositedAtlasTiles` 和已退役旧 VT 资源/参数，
   明确默认场景迁移与旧路径退出时点，才能关闭 S1/S3/S5。

上述三项不缩减 S0–S5，也不授权本审计修改生产源码或占用 renderer。
每项关闭须追加具体版本、复现输入、执行命令和原始结果；本清单保留本次观察事实。

## 9. 第二次独立审计：当前结论与版本边界

日期：2026-11-20，约 17:05（Asia/Shanghai）。本轮继续只读审查生产源码和既有报告，
只追加本文；没有启动 renderer、Cook、export、修改生产代码/资源或重复运行已有测试。
以计划最新 §12–13 明确的实施语义及原 S0–S5 范围为准，不增加质量指标，不提高容忍阈值，
也不把任意设备/输入的全部故障恢复升级为本轮退出前提。

**当前结论：核心实现与 S1 数值画质、S3 压力闭环、S4 组合编辑闭环已有新证据；
旧审计的三项 P1 不能原样续报。最终 filter v2/R8192/192 的实包验收仍 pending，
正常 LOD/TES 产品的最终人工查看亦应由对应产品证据关闭。**
本轮没有确认新的生产 P1。不能因新包尚未完成而否认已完成的其他阶段，也不能用历史
filter v1/R4096 包通过替代 filter v2 最终包。

审计期间有新的进度：用户发起时主线程正在 Cook；本次实际读到
`artifacts/terrain-composited-vt/product-filter-v2-r8192-cook.exit.txt = 0`，
同名 log 四个 surface 均完整发布、正常 teardown，已不是“Cook 仍未完成”。
`game-delivery-final/final-filter-v2-preflight/acceptance-results.json`
仍为 `Prepared only`、`rendererAuthorizedByInvocation=false`；这份文件只证明预检，
没有被提升为实包通过。
收尾复查时 `Game/final-filter-v2/` 已由执行者建立，stage-warm 退出 0，
export 已开始但尚无退出收据；其报告顶层仍为 `Prepared only`。
因此正式验收已启动，状态仍 pending，不等待或占用其 renderer 窗口。

以下统一缩写：

- `VT/` = `artifacts/terrain-composited-vt/`。
- `S1/` = `VT/independent-gpu-bake/s1-r8192-budget192-run/`。
- `S3/` = `artifacts/terrain-runtime-stress-run03/`。
- `S4/` = `artifacts/terrain-editor-s4-final/`，原始 worker 报告在同级
  `terrain-editor-s4-worker2-monitorfix01/`、`terrain-editor-s4-worker1-monitorfix01/`。
- `Game/` = `VT/game-delivery-final/`。

源码为持续修改中的同一 worktree；报告自带的 binary/source/resource 摘要是各自执行快照。
S1、S3、S4 和最终产品不是同一次运行，以下不将它们伪装成同一 exe 的全链测试。
RTX 3080/当前 Windows 驱动的实测不外推为任意显卡；producer 只支持已固定的
正 determinant、uniform scale、无 shear/镜像的 placement，其他配置明确拒绝，
不是待本任务补实现的通用变换能力。

## 10. 旧缺口逐项销项及当前 S0–S5 矩阵

| 原审计项 | 当前事实/证据 | 当前处理 |
| --- | --- | --- |
| 包因 Maps、shadowLocal 等闭包失败 | `Game/final-r4096-04/acceptance-results.json` 已有成功 cold、负向与官方 validator；`DELIVERY_README.md` 明确它是 filter v1 历史包 | 旧失败已修，不再报当前 P1；新 filter v2 包单独 pending |
| game 无条件重读构建专用作者 GLSL | `terrainCompositedService.cpp::RegisterSurface` 现在按 `GlobalAuthoringMode` 分流；game 读 packaged dependency sidecar，调用 `ValidateTerrainCompositedManifestConfig` 和 hash 校验 | 源码缺口已关闭；新产品包执行收据待补 |
| Editor 先改 live revision 后构建、失败掉队列 | `Candidate/PublishCandidate`、`ConsumeMaterials/ConsumeUpdates` 先准备候选，成功前不移动 live runtime；catch 保留旧状态并有界重试 | 旧源码推断已失效；不能继续引用旧行号认定未修 |
| 非零 bake status 反复抛同一个结果 | `CompleteReadbacks` 消费并 erase 失败回读、保留 dirty、增加失败计数，`RecordPreview` 在失败三次后停止 | 旧无限失败路径已修；不要求重新跑任意故障组合 |
| 高度替换预算未更新、各 surface 无共享额度 | `PublishCandidate` 更新 input/height 差额；`RefreshHeight` 计退休重叠；服务以一个 `TerrainCompositedFrameUploadBudget` 遍历并轮转 surface | 原源码缺口关闭 |
| Paint 旧快照可原地变动、descriptor 提前失效 | Paint 按 revision 发布不可变副本；每个 swapchain image 的 descriptor 包强持纹理直到相应 fence；S4 worker1/2 无 08114/DeviceLost | 有生产修复和实际 GPU 回归，不停留在 CPU 单测证明 |
| `u_compositedAtlasTiles` 无消费者 | 当前 M 声明/生产 shader/Bind 已删除；只在依赖摘要的旧字段排除白名单残留 | 参数清理已完成；白名单兼容项不等于 GPU 未用资源 |
| 全视距基础层仍近场 fallback | 新产品与 direct 夹具显式 `u_heightBlendDistanceFade=0`；默认 1 仅为旧作者语义保留 | 静态产品全视距合成已实现；不把兼容 fallback 算成产品成果 |
| LOD 法线反例、无真实像素门禁 | 计划 §13 固定 canonical 局部法线，材质误差比较固定 same LOD0/noTES；S1 的真实图像门禁通过 | 历史 64° 不能混入同几何材质缓存误差；正常 LOD/TES 外观查看保留原退出范围 |

| 阶段/原计划要求 | 当前判定 | 具体依据/边界 |
| --- | --- | --- |
| S0 分类、producer/owner/consumer、地址、版本、失败与退休 | proved（当前协议/支持范围） | CPU 布局/hash/token、三类 sidecar、candidate/epoch 规则及契约/路由已接；uniform scale 之外拒绝已固定 |
| S0 权重来源/局部全局映射/旧 feedback 对照 | proved | LocalLayerArray 与 Paint 用同一 R32_FLOAT 覆盖结果，producer/fragment 共用；既有地址/协议验证与当前 feedback/filter 数学记录存在 |
| S1 多 mip、border、颜色空间、normal/ORM、粗滤波 | proved（bounded GPU + 真实图像） | mip0 center、coarse 4×4、8 tap center-border；GPU/CPU oracle 和真实 18 次 renderer 原 ROI 门禁通过，详见 §11 |
| S1 常驻 mip2、repeat、层映射及源/atlas 数据 | proved（规定数学采样语义） | `canonical-helper-bounded-final`、`canonical-helper-real-v1` 的实际 helper/独立 double bilinear 对照和 atlas 存储校验；旧硬件舍入差异保留作诊断 |
| S1 斜坡/层界/真实平铺密度材质对照 | proved（same LOD0/noTES） | 真实 2×2、8192²、192 slots；三个机位 base/beauty 的 union 与有效网格全部过原阈值；主线程已查看中近配对图 |
| S1 正常 LOD/TES/detail 最终外观 | pending（原产品退出项） | S1 报告明确 noTES/LOD0；计划 §13 明确“不替代正常 LOD/TES 场景人工查看”。这不否定 mip2 数学对照，也不新增要求跨 LOD 像素相等 |
| S2 格式/依赖/原子包、随机页/保底、失败路径 | proved（源码和已有执行范围） | Store/height/coverage/dependency 协议、原子发布、旧 filter 拒绝；历史包负向与本轮 Core/export CPU 回归。通道选择为 RGBA8，不宣称生产 BC 压缩已完成 |
| S2 新产品完整 Cook/尺寸/包体 | proved（R8192 四 surface） | `product-filter-v2-r8192-cook.log` 四份各 5468 页、1,213,633,536 bytes payload；exit 0，filter v2 元数据已被离线预检接受 |
| S2 最终新包只读消费 | pending（合并至 S5 包验收） | 不以旧 v1 冷启动替代；不用新 pending 项倒退全部 Store/Cook 结论 |
| S3 页需求、预算/父级、读页取消、上传/退休 | proved（源码+CPU+实机压力组合） | run03 baseline64/budget16，selected 保护和祖先粗化、尾段 reads 停止、两次 World reload；29 Residency CPU 用例补充 epoch/token 边界 |
| S3 两线程/近远/快速移动转向/低预算/内存与耗时 | proved（报告限定的平台/采样范围） | run03 status=passed，两个 variant failures/missingEvidence 均 []，详见 §12；不是每帧高水位或任意设备证明 |
| S4 Paint/Sculpt/Undo/Redo/材质/Save重开/外部冲突 | proved（最终组合范围） | final-record + 两个 monitorfix 报告；真实 CommandBus→EditorHost→GPU→Save→Cook→reopen/reload |
| S4 旧任务不能回写、局部 dirty、最新版本收敛 | proved（组合范围） | worker2 实测 staleBakeResults 0→1；worker1 无竞争窗口保留 N/A；编辑像素恢复/队列收敛与 CPU late-token 回归共同支持，详见 §13 |
| S5 独立多 tile、完整 height payload/实时源消费者 | proved（实现/旧包闭环及新 Cook） | topic resources 真实四 tile；mip2 sidecar 独立交付；detail/macro 与兼容 mip3 的源数组单独统计、不提前删实际消费者 |
| S5 旧链收敛/无消费者参数、显式比较开关 | proved（计划 §12 当前迁移边界） | 合成路径不创建旧层源物理 atlas、不发起旧初始层页上传；删除 unused 参数；旧场景仍消费的 shader/合法 descriptor 页表保留，不能误报成需要删除全部 legacy 实现 |
| S5 新默认产品、包版本与最终交付 | pending | 场景已明确 R8192_Default/192/filter v2；新 Cook 完成，`final-filter-v2-preflight` 仅 Prepared；等待 Maxwell 新包证明和最终交付索引 |
| 文档/路由当前同步 | proved（已有更新），pending（证据文字收尾） | 核心规则已反映 v2/候选/readonly game；§14 列出的旧验收措辞应在收尾更新，不再当新缺陷 |

## 11. S1：保留原门禁后的实际通过范围

当前生产入口核查：

- `TerrainCompositedService::Bake/RecordPreview` 均设
  `areaSamplesPerAxis = mip == 0 ? 1 : 4`；`TerrainCompositedBakeGpu::Validate`
  仅允许 mip0 单中心，粗 mip 单中心明确拒绝。
- filterRule=`mip0-center-coarse-area-local-normal-v2`、generator=2、
  encoding=`gpu-center-mip0-area4-coarse-rgba8-height-fade1-v2`。
  容器 version/header/hash domain 保持 v1，不把“容器 v1”误读为“旧滤波包”。
- 主视图/feedback 共用足迹规则，最多 8 tap；中心页 border 优先，
  超出有效 border/无中心映射才单独解析 tap。缺页请求没有为双线性扩成九页。

`VT/center-border-8final-v2/report.json`：RTX 3080、9 次 dispatch、
每次 18,496 samples，masks 31/17/18/20/24/16/1/0 均 passed；
通道最大误差 1 byte、法线最大约 0.366°，
actualMip/validity/footprint errors 均 0；
单中心驻留与全驻留的最大差 0 byte。原阈值仍是 2 bytes、1°、
resident-vs-all 1 byte。它证明 sampler/地址 oracle 的有限范围，
不单独当作完整 renderer 验收。

`independent-gpu-bake/canonical-helper-bounded-final/report.json` 的 material/height
均 passed，height 最大 0.003489 R16 units；真实源
`canonical-helper-real-v1/report.json` 的 atlas 存储/数学过滤判据 passed。
独立 double 源双线性为 gate，旧 hardware sampler 的 repeat seam 差异只是诊断，
没有通过增大 2 R16 units 门槛消除旧失败。实际生产 helper、源 mip2、
tiling/amplitude 均有身份记录。
`center-filter-v2-ready/prepared.json` 则只是 CPU/shader 准备（gpuDispatched=false），
本轮没有把这份准备文件误记作 GPU passed；真实出图与生产 Cook 由下述证据承担。

`S1/report.json` 的 18 次 renderer 分别为三个机位 × mask/base/beauty × direct/composite，
均正常退出、无检测到的 validation/runtime 错误。每次 composite 截图前连续三次 stats
中 queued/reading/cpuBytes/dirty/stale/readFailures 均为零，未将未收敛图用于质量通过。
原门槛仍为 visible-terrain-union 和所有 ≥64 terrain pixels 的 4×4 网格 ROI：
meanAbsRgb8≤3、P95≤8，轮廓 XOR=0；不是降低到全画面平均或剔除较差区域。

| 机位/通道 | ROI 数 | 所有 ROI 中最大 meanAbsRgb8 | 所有 ROI 中最大 P95 | 结果 |
| --- | ---: | ---: | ---: | --- |
| far base | 7 | 2.262973 | 8 | proved |
| far beauty | 7 | 2.096422 | 7 | proved |
| mid base | 17 | 1.088073 | 4 | proved |
| mid beauty | 17 | 0.796146 | 3 | proved |
| near base | 17 | 1.162581 | 4 | proved |
| near beauty | 17 | 0.862870 | 3 | proved |

报告机器状态仍写 `screened-pass-awaiting-human-review`，主线程后续
view_image 中近 direct/composite 无明显接缝、错层或法线方向异常的审阅已由用户及
计划 §13 记录；两类证据合读，不因旧机器状态文字否认后续人工审阅。
同样，不把有限截图升级为任意几何 LOD/TES 的逐像素相等。
历史 R4096/filter v1、R8192/64 slots 的失败保留，当前选择 8192/192 仍受
256 MiB World cache admission，非无限增显存。源数组成本另列，不能用 cache cap
冒充总进程 GPU cap。

## 12. S3：压力实测与 Residency 边界已补齐

`S3/report.json` status=passed，workerCount=2；
baseline64 与 budget16 都 process exit=0、failures=[]、missingEvidence=[]。
完整原始日志、截图、perf JSONL 和 PID 内存采样归档于该 run 的 variant。

- 两组均含 near/far/mid、快速转向移动、teleport、两次 LoadWorld、final_near；
  generation 1→2→3，每阶段尾部队列收敛，readsStarted 尾段增量八个阶段全为 0。
- baseline64 peakResident=64、peakQueued=36；budget16 peakResident=16、
  peakQueued=8、peakBudgetMipBias=2，实际进入了预算压力，不再是旧审计的 17/64 情况。
- 源码 `ConsumeFeedback` 在 slots−1 内选择/祖先粗化，清理不再需要的 pending/ready，
  旧异步结果丢弃；BeginUploads 不选择当前 selected 页和 baseline 为 victim。
  共享 frame budget 和 epoch Commit/Abort 接线仍在。
- `VT/residency-budget-tests.log` 是 29 项 Residency CPU 通过；
  `VT/streaming-lifecycle-tests.log` 是 46 项通过。它们补充 late token、
  retirement 和 chunk/顶点队列生命周期，不替代上述 renderer 压力证据。

原 run 的卡顿阈值 500 ms、frameLimit=12 Hz，没有过滤 reload 帧来制造通过。
baseline CPU sampled p95/p99=91.0807/91.9237 ms，budget16=91.34975/93.8875 ms，
均无超阈值 sample。GPU 表为 sampled engine pass 数据，不冒充整设备吞吐。
性能是每秒采样快照，不是逐帧 percentile；PID/WDDM 是进程采样近似，不是瞬态精确峰值。
这些是证据解释边界，不新增“必须证明任意瞬态”的退出要求。

两组同一采样时刻 dedicated+shared 峰值分别 4,669,231,104 /
4,817,174,528 bytes，包含源纹理/driver，不再只有 atlas payload；
CPU working/private、Cook 与 runtime 的 PID/采样覆盖也在 report。
因此原审计“完全没有带源数组的进程内存/分位数记录”已关闭。
run03 是有限压力 fixture，最终 R8192 产品包的运行/成本记录仍由最终交付收据关联，
不将该内存数值当作新产品包峰值。

## 13. S4：按最终组合证据关闭，不抹掉 worker1 的 N/A

`S4/final-record.md` 的结论已核对两份实际 JSON：
worker2 failures=[]、missingEvidence=[]；
worker1 failures=[]，missingEvidence 只保留未观察到 obsolete completion 的窗口，
不改写它的原 checker exit 3 为全通过。

两者实际完成 14 个编辑阶段及保存/重开/重载；bootstrap Cook、edit、saved Cook、
reopen 进程退出均 0，无验证层错误、08114 或 DeviceLost。
CommandBus→EditorHost 的真实动作和 GPU 图片支持：

- 24 Paint/Erase stamp 每次真实改变 WorldHash；不是只增加 revision。
- Paint/Sculpt Undo/Redo 在编辑 pixel mask 上残差 0，配对 BMP hash 精确恢复；
  Paint mask=55,740 pixels，Sculpt mask=53,669 pixels，非稀释全帧均值。
- material `u_uvScale` preview 改变图像，revert 恢复 rapid 的 hash；
  外部写入后 host feedback 新鲜变为 unsaved conflict，延迟 992/929 ms，
  没有复用先前提示冒充本次观察。
- Save→独立 Cook→新进程 reopen→实际 LoadWorld，generation 1→2，
  persisted Paint/R16/material 与输出满足原容差。
  保存重开的 mean RGB 差为 0.0021484375，未声称量化后 byte-identical。
- worker2 在 `editor.log:2683` 的 `EDITOR_S4_SAMPLE rapid 10` 观察到
  production staleBakeResults 0→1（service revision25/sourceRevision12）；
  worker1 未出现竞态窗口，与 worker2 + CPU late-token 证据组合判定旧任务拒绝。
- peak dirty=20/92，而不是全表面重烘；工作集 stale/queued/reading/readbacks、
  editor/material pending 最终 0；其余不驻留 dirty 页仍按需，不虚称所有虚拟页 GPU 内容已更新。

源码再次确认 immutable Paint 发布、descriptor 纹理强持、候选发布与失败结果消费，
以及 Sculpt 的 runtime `TerrainVertexUpdateQueue` /三槽动态顶点/World 资源包所有权。
这使旧审计的“必须先补服务失败事务”和“正常编辑闭环缺失”不再成立。
未执行的 arbitrary texture/rule replacement、跨 tile 故障矩阵等仍依原记录解释，
不将“任意输入/所有设备失效都要 renderer 实测”新增为 S4 P1。
已有 minimize 0×0 平台恢复问题不归本 VT 目标，不要求借此重写平台。

## 14. 剩余 pending、game 脚本覆盖与文档收尾

**最高优先级真实剩余项：取得最终 filter v2/R8192/192 的实包收据。**
截至本轮读到的证据，该项是 pending，尚无新失败，不沿用历史 Maps/shadowLocal、
缺 cache、R4096 质量或预算追尾来宣称新包失败。

新产品 Cook 四 surface 每面 5468 页、1,213,633,536 bytes payload，
总材质 payload=4,854,534,144 bytes，未含四个 16-byte 包头/sidecar/实时源资产；
不能把它写成旧 R4096 的 1372 页或用旧包总大小代替。
`Game/final-filter-v2-preflight/acceptance-results.json` 记录四面的
filter/generator/encoding/resolution/resident/manifest hash/pageCount/packageBytes，
例如 tile_00 manifest SHA256 为
`713838d990847be1f34a531341a983e27018bfd8a390bad1985555500159d0d9`。
预检的原 metadata 路径 260 字符、重定位上界 195 字符，也不是新的长路径失败。

本轮已读正式 `tool/validation/terrain/accept_game_delivery.py`：

1. 默认 scene 为 Real2x2_R8192_Default、8192/192，
   在 renderer/export 前核对 filter v2/generator2/encoding v2；
   metadata 来自四份实际 manifest，不再硬编码 R4096 页数。
2. stage worker2 smoke→新 game export→pristine verify→包根 worker2 180 帧 cold，
   相对 `resourcePath=resources`、截图与 stats、零 shader miss/正常 teardown。
3. 七类真实变异：corrupt page、missing page binary、missing height manifest/binary、
   VT/height version 错误、wrong-filter-rule。每项 verify 和包内 worker1/2，
   runtime 必须 exit1+明确 init failure+正常 teardown；crash/timeout 不算预期通过。
4. finally 恢复原 bytes、SHA256 校对并逐项 verify；最后官方 package validator 和
   shared shader-cache 前后摘要相同；唯一 run-id 目录保留原始日志。

脚本覆盖足以承接该发布退出项，无须为审计另外引入 renderer 或重跑已完成测试。
七类变异选首个 surface 的代表文件，其他表面的全包完整性由正式 verify 覆盖；
它不是每个 tile 的穷举 fault matrix。旧 `Game/accept_delivery.py`、
`record_final_snapshot.py`、`REPLAY.md` 的 R4096 固定值只属历史，
本轮应调用正式 tool 脚本 `--runid final-filter-v2`，不能混用。
`--reuse-package` 是显式恢复选项；本次新产品应由新 export 收据证明，
不让旧 v1 包借其通过。脚本 `run()` 记录启动摘要不自动证明源与 binary 一致，
最终 proof 应保留本轮冻结 binary/marker/GLSL/Cook 身份关联，现有准备流程已为此留入口。

**原质量范围尚待产品证据关联的事项：** 正常 LOD/TES/detail 产品最终人工查看
（计划 §13 已明确保留），以及最终 R8192 包的运行/交付成本记录。
S1 已通过的 sameLOD0/noTES 阈值不能放宽，也不要求正常不同 LOD 之间满足同一个逐像素门禁。
可由主线程随后提交的产品查看/包 proof 关闭，不要求为了本清单再设计一组验证。
本轮没有新增“任意平台”“所有故障”“每像素 epoch 可见”等替代原 scope 的门禁。

文档收尾有过期证据文字，属应更新的索引，不是复活旧代码阻断：

- `documents/contracts/terrain-composited-vt.md` §6 仍称 GPU Save/Load
  “只有单 TU 编译、无实机回读加载对照”；后文与当前 helper/既有包证据已更具体。
- 同文 §9 仍笼统列“无 base-only 像素比较、无预算饥饿/高速镜头、无进程内存”等，
  现应分写 S1 ROI、S3 run03、S4 组合已证明的范围和最终新包 pending。
- §9 的 final-r4096-04 应明确历史 filter v1；不能把其成功更新成当前 filter v2 成品通过。
- `Game/REPLAY.md` 开头及旧入口仍以 R4096 为最终；以正式 tool 的 R8192/v2 默认值、
  `R8192_READY.md`、最新预检及后续 `final-filter-v2` 收据为当前交付索引。

**本轮交接：** 已证的核心/数值/压力/编辑项保持 proved；仅把上述真实产品退出项留 pending。
主线程提供最后 package proof 后在本文追加其路径、binary/Cook 身份、运行退出和原质量
查看结论即可更新，不删除历史失败，不重跑整套既有验证来制造“最后一次全绿”。

## 15. 最终包与产品补充收据独立复审（17:53）

2026-11-20，约 17:53（Asia/Shanghai）。本轮只更新本文；没有运行 renderer、
Cook、export、validator 或既有测试。直接阅读最终 JSON/原始日志，核对包内四个
身份文件的实际大小与 SHA256，并通过 view_image 独立查看 mid/near/mountainside 三张 PNG。
主线程同步指出的矩形明暗边界与黑点纳入当前判定；**本轮不宣布正常 LOD/TES 视觉门禁全绿。**

本节所有证据路径根为
`artifacts/terrain-composited-vt/game-delivery-final/final-filter-v2/`，
对应成品为 topic 下 `../vt-game`。
三类收据的 scene 均为
`Generated/Validation/TerrainCompositedVT/Variants/Real2x2_R8192_Default/SC_composited_vt_2x2.json`，
包、product-views、product-cost 的 exe 摘要一致：
`d9adce053b41089b69709067f27edcd7999cd962b5f973def2e3895eff5236f4`。
这是新 filter v2/R8192/192 产品收据，不是旧 filter v1/R4096 历史包。

## 16. 已关闭：最终 game 包只读消费、负向与恢复

`acceptance-results.json` 的状态为
`Package delivery checks passed; does not certify all S0-S5 gates`。
该措辞正确：它只关闭包功能门禁，不替代产品视觉审阅。

本次独立核对结果：

| 项目 | 原始收据 | 判定 |
| --- | --- | --- |
| 新包导出、pristine verify | export、verify-pristine 均退出 0 | proved |
| 包内冷启动 | package-cold 退出 0，正常 teardown、无 shader miss；原始日志有截图保存和实际 LOD2 draw | proved |
| 必需资源闭包 | 四 surface、四材质包、四 height manifest，包内 `resourcePath=resources` | proved |
| 七类故障拒绝 | corrupt-page、missing-page-package、missing-height、missing-height-binary、wrong-version、wrong-height-version、wrong-filter-rule | proved（代表性文件变异范围） |
| 每类 worker1/2 失败行为 | 本次逐一检查全部 14 份 runtime 日志：exit1、明确 init failure、正常 teardown，未检出 Validation Error/VUID/THREADING | proved；未将 crash/timeout 算预期通过 |
| 每类恢复 | 七个 restored=true、七次 verify-restored 退出 0，最终 verify-restored 退出 0 | proved |
| 官方 validator | official-validator.exit.txt=0，日志明确 passed all checks | proved |
| 补充视图/成本后的包完整性 | verify-final-after-supplements.exit.txt=0，日志 `{"ok":true,"verified":"../vt-game"}` | proved |
| 共享 cache 保持 | before/after 均 `88619ea621d16b64acdca30099f25d115165ee2a18b03c8cb56341acce8b7a55` | proved（该执行窗口） |

`final-package-sha256.json` 本次末读使用 `relativePath/bytes/sha256` 格式；
按其路径读取当前成品核对，下面四个文件的大小/hash 全部匹配：

- `main_game.exe`：73,058,834 bytes，SHA256 如 §15。
- `game-export-manifest.json`：111,708 bytes，
  `ef4341de7916c9e3dabe1955b31053ee2efaad0fccdd7078217960d499cd0627`。
- `shader/runtime-shader.manifest.json`：110,550 bytes，
  `510e94c097c73467ae2aec9de557839d75b2d15d6bf1f0c81d158a948543656a`。
- `vl_build_flavor.json`：53 bytes，
  `29511d73d92f130e13241de7a846d383950053a6f220ea944bdd0bce3603c0ad`。

包 payload 全量完整性依据上述既有正式 verify/validator 收据；
本审计没有重哈希 5 GB 全包或重新运行验证器。
据此关闭 §10 的“S2 最终新包只读消费”和 §14 的“等待新实包功能收据”。
历史 Maps/shadowLocal/cache/长路径失败不再是当前包阻断。

## 17. 已关闭：新产品成本记录；视觉捕获成立但审阅未通过

### 17.1 产品成本收据

`product-cost/cost-results.json` status=passed，全部 checks=true；
进程 PID86012，exit0、no timeout、正常 teardown、无记录到的 runtime/validation 错误。
本次核对 `cost.log` 的实际 SHA256 与 JSON 中
`622a22d6c3bfc402bc6c55a8d76f3275cd02e3a0e87b3e4c0371a30a0d12285e` 一致。

| 量 | 收据值 | 解释边界 |
| --- | ---: | --- |
| 包总大小/文件数 | 5,118,836,628 bytes / 563 | 新产品，不能沿用旧 v1 包大小 |
| CPU timing 样本 | 31 | 60 Hz paced，包含 pacing/present 等待；不是无节流吞吐 |
| CPU p50/p95/p99 | 16.690 / 17.150 / 21.7244 ms | wall-clock snapshot 的统计，不声称每帧上界 |
| GPU p50/p95/p99 | 0.097 / 0.112 / 0.1134 ms | engine perf 报告量，不冒充整设备完整 GPU 成本 |
| 全进程 CPU working/private 峰值 | 908,906,496 / 5,478,002,688 bytes | 含启动与近景工作负载 |
| 同时刻 dedicated+shared 采样峰值 | 4,968,931,328 bytes | PID/WDDM 近似，包含源资源和 driver，非瞬态精确高水位 |
| 最后 30 秒 dedicated+shared 采样峰值 | 3,306,500,096 bytes | steady near-view 的有限采样窗口 |
| CPU/GPU 内存有效样本 | 137 / 44 | tail 有效样本 56 / 20 |
| Terrain cache/source payload | 210,644,416 / 186,696,804 bytes | source 明确单列，cache 受 268,435,456-byte cap |
| Terrain accounted payload | 397,341,220 bytes | 不能再次加到已包含它们的 WDDM 进程总量上 |

包 manifest、包 shader cache、共享 shader cache 在成本运行前后不变，
无编译、零 shader miss；四 surface 尾态 queued/reading/dirty/stale/readFailures 为零。
`gpuHelperExitCode=1` 未被隐藏：GPU JSONL 末项为进程退出后
`Target PID WDDM counter unavailable`，前面已有 44 个有效样本，末个有效样本时间
09:24:21Z，随后 09:24:22Z 不再有目标 PID counter。收据没有把无效项算成有效采样，
这不是 renderer 退出失败或成本证据缺失，不新增一次重复采样要求。

据此关闭 §14 的“最终 R8192 产品运行/交付成本记录 pending”。
没有将本次成本记录改成新的宽松性能门槛；此前 S3 的既定压力门槛和 S1 的原 ROI
门槛仍分别成立，不能混为一个全平台性能保证。

### 17.2 正常 LOD/TES 捕获的有效性

`product-views/capture-results.json` 与三份 command/log 确认：

- 使用包内同一 exe，正常 LOD selection，无 fixed LOD override；
  detail MI 宏 `VL_TERRAIN_TESSELLATION=1`，包中含 runtime/debug tessControl/tessEvaluation。
- authored displacementScale=1、tessRadius=60 m、tessMax=8、targetPixels=4、
  heightBlendStrength=3、distanceFade=0。
- mid/near 实际 drawnByLevel 为 LOD0:46、LOD1:18；
  mountainside 为 LOD0:39、LOD1:25。三个进程均 exit0、正常 teardown，
  无原始日志中可见的 shader miss/validation/runtime 失败。

这些足以证明本轮捕获不再是 S1 的固定 LOD0/noTES 对照。
但宏/输出角色/LOD0 draw 并非 GPU tess primitive 计数，收据本身已明确该边界。
本次不为此新增 primitive-counter 门禁。

### 17.3 独立看图发现：产品视觉退出仍 pending

本审计实际查看 `mid.png`、`near.png`、`mountainside.png`：

- mountainside 左坡约 **x110–280、y200–550** 存在可见矩形/阶梯形明暗边界，
  边界与周围坡面连续渐变不同；可以确认图中有该视觉疑点。
- near/mid 地面散布细小黑点；当前单张 PNG 无法区分材质输入、几何/TES、
  原 direct 路径已有现象或合成页/过滤路径新增问题。
- 不能仅凭无 validation 错误、capture exit0、S1 同几何 ROI 通过，
  就将上述正常产品图像判为“无接缝/无错层”；也不能仅凭矩形形状就确定是 VT 页缝。

用户/主线程已明确交由 Maxwell 做 **相同 pose、正常 LOD/TES 的最小 direct/composite
对照**。本轮不再发起 renderer，不新增第二组测试。
在对照和来源结论未落盘前，本项保留 **pending：有明确视觉疑点，来源未确认**。
若 direct 同样存在，只能证明该现象不由合成路径新增，仍需明确记录其现有来源/
对本计划原质量退出的影响；不能把“两个都相同”未经审阅自动写成最终产品视觉通过。
若 composite 独有，则须定位修复，并按受影响范围恢复原产品视觉门禁，
不放宽已经固定的 S1 数值阈值。

## 18. 当前阶段结论与唯一剩余交接

| 阶段 | 当前结论 | 本次新增影响 |
| --- | --- | --- |
| S0 | proved（已固定的协议/支持范围） | 无新增阻断 |
| S1 | 数值/过滤/高度/真实 sameLOD0-noTES 对照 proved；正常产品视觉退出 pending | 三张产品图已捕获并独立审阅，发现明确疑点，不能提前关闭 |
| S2 | proved（本次既定实现与新产品只读包范围） | 最终 filter v2 包 cold、负向、恢复、validator 补齐 |
| S3 | proved（run03+既有 CPU/源码及有限平台范围） | 新产品包运行/成本收据补齐，不外推任意平台 |
| S4 | proved（最终 worker1/2 组合范围） | 不因产品视觉待查而倒退已闭合的编辑动作/版本证据 |
| S5 | 包交付功能、资源闭包、预算/成本记录 proved；最终质量退出 pending | 等待同 pose direct/composite 视觉来源结论；不能宣布全部 S0–S5 完成 |

**当前没有需要再次补跑的包功能/成本门禁，也没有确认的新生产 P1；
唯一实际阻断是已观察到的产品视觉疑点尚未完成归因与原质量退出审阅。**
下一份所需收据就是 Maxwell 正在做的最小对照及结论，包含对应 pose/正常 LOD/TES、
direct/composite 输出和来源解释。确认其是否属于合成路径新增问题后，更新本节的
S1/S5 pending；不要求重跑全部已通过 suite，也不要求解决不相关的 minimize 0×0 平台问题。

主线程负责 acceptance/final-evidence/plan/testing 的收尾索引；本审计只更新本文，
不将其他代理尚在核实的结果预写成通过，不删除历史失败或视觉疑点。

## 19. 已定位缺陷及冻结修复（2026-11-20 19:53）

本轮结束边界按用户明确授权固定为：修复已定位缺陷，并在同 pose 恢复原左侧 ROI 的
正常新 composite 视图；不继续追逐整张旧 legacy 场景的任意块状 artifact。
不放宽 S1 原质量门槛，不新增任意坡面/平台/驱动的全覆盖要求。
本次只追加本文，不修改冻结源码、资产或他人写集，不运行 renderer 或已有测试。

### 19.1 几何细分边界：TCS 与宏面一致性修复

TCS quad outer 原先按周长边顺序直接填入，和 quad tessellation outer 编号不符；
已修为 `[edge30,edge01,edge12,edge23]`，使共享物理边获得同一边因子。
Base TES 的 worldPosition、previousClipPosition、ctx.clipPosition 已采用
terrainMeshBuilder 固定 p1–p3 对角的分片权重；normal/UV/tangent 保留原双线性，
近场层高度仍独立采 mip2 并按物理幅度/距离 fade 位移。

`tool/validation/terrain/validate_tess_macro_surface.py` 的六项既有数值回归通过，
覆盖平面/非共面、对角线/共享边、退化 patch 和投影一致性，TES SPIR-V 编译/验证通过。
这是评价点数学证明，不是 quad tessellator 任意三角连边完全贴合原宏面的保证。
`slope-diagnosis-fixed/` 实景反映 near 黑点减少而坡面块仍在，
shadow 图全白；因此宏面偏差不能被宣称为剩余坡面明暗块的唯一根因。

### 19.2 Coverage ID 与权重错配：共享 ID-aware 双线性修复

原 nearest ID + 排名槽 weight linear 会把不同 texel 的不同层身份混合到同一槽。
冻结 helper 对四个虚拟 texel 各自解析页表，读取同 texel 的 ID/weight，按 global ID
合并、选 top4 并归一化。legacy 用 PageClamped 防外国 placement，
composite/bake 使用独立完整表；缺 footprint 返回 status，
fragment 中性输出、TES 零权重、bake 拒绝发布。
fragment base 导数前置，neutral return 放在隐式采样之后，避免新增非一致早退导数问题。

`independent-gpu-bake/coverage-id-aware-actual07/report.json` 是本轮 helper 数值通过收据，
此前 actual06 的 136 queries、placement/legacy/missing、foreignWeight=0、
独立 double oracle 和负控记录保留。helper 实景 `coverage-direct/` 仍有块，
所以“修了错槽就必定解决整个 ROI”已被否定，未据此继续 Cook/宣布交付。

### 19.3 Dominant detail/macro/TES：改为原始覆盖权重的四层求和

CPU 实证已收紧到真实屏幕，不再使用无关 `(639,87)` 示例解释 ROI：
`artifacts/terrain-candidate-probe/SCREEN-RESULT.md` 记录真实相机/mesh/TRS 射线，
ROI 的 6156 个采样点落在 coverage X517..520、Y653..657，同一 page(4,5)，
第五层权重均为0、Paint opacity均为1。**此 ROI 不支持 top4 截断或候选裁剪归因。**

原 Paint 有连续但陡峭的 layer0/1 过渡，coverage ID-aware 权重也连续；
`ids[0]` 硬选 detail 法线却把排名交换变成整层切换。
实际同像素 witness：
`(231,200)→(232,200)` 及 `(131,410)→(132,410)` 的 ID-aware 法线 RGB 最大跳变
13/31 byte，关闭 detail/macro 后1/0 byte；raw/aware 等权线与该位置吻合。
这证明了本轮目标缺陷，不外推为全坡所有多边形的唯一解释。

冻结 MF 将归一化后、height-blend 前的 `coverageWeights` 单独保留：
detail albedo/world normal、macro albedo 和 TES 层高度分别按同一 `(ID,coverageWeight)`
集合求和，最多四层；不再按第一名选择整套 detail。
fragment 使用入口无条件导数与 textureGrad，catalog 显式 LOD0；
TES 仍显式 mip2、每层原 detail UV/物理幅度，normal 最后归一化。
源码独立复核未发现排名置换、原始权重拷贝时点、UV/TES 或新控制流导数的具体阻断。

## 20. actualweighted02 数值与同 pose 实景改善

冻结 MF SHA256：
`40271255b210f7ff95d55528690b59b8ba24b060af5f9ffa1816013758858dd9`。
本次读取当前源码 hash 与
`artifacts/terrain-composited-vt/independent-gpu-bake/weighted-detail-actual02/report.json`
完全一致。报告 passed=true、failures=[]，RTX3080 实际 GPU dispatch/readback：

| 数值项 | 测量 |
| --- | ---: |
| queries | 136 |
| 最大独立参考误差 | 1.4876667e-7 |
| rank permutation 最大误差 | 1.1920929e-7 |
| 原始 coverage 最大误差 | 1.1920929e-8 |
| height-blend 对基础结果的变化 | 0.1084745（确认基础变化未污染原始 detail 权重） |
| 显式梯度响应 | 0.0250000 |
| direct/composite 原始权重结果最大差 | 0 |
| rank crossing 最大 step | 8.046627e-7，门槛2e-5 |
| TES 位移最大误差 | 1.4305115e-9 m |
| 旧 dominant normal jump 负控 | 0.0773843 |

`game-delivery-final/final-filter-v2/weighted-direct/report.json`：
真实正常 LOD/TES 的 near/mountainside default direct 与 no-detail/macro 诊断均 exit0，
无额外诊断 shader，私有作者资源已恢复、生产包未改；default direct shader miss 为空。
这是新 shader 的 direct 实景回归，不是新 Cook/composite 包验收。
主线程已实际查看 default mountainside beauty，原左 ROI 硬边明显消除。

本次核对其 `pixel-jumps.json` 的同位置结果：

| 相邻像素 | weighted 前 normal 最大 RGB 跳变 | weighted 后 default normal | weighted 后 default beauty |
| --- | ---: | ---: | ---: |
| (225,200)→(226,200) | 3 | 1 | 1 |
| (231,200)→(232,200) | 13 | 2 | 2 |
| (131,410)→(132,410) | 31 | 2 | 0 |

据此，本轮已定位的 dominant-detail 缺陷具备数值和同 pose direct 改善证据。
没有用两边相同、无验证层错误或模块测试来自动关闭最终 composite 视觉门禁。
此前 S1 数值/历史包功能/成本收据仍是原快照事实，不删除，
但不能证明这次实际改变 detail/TES 语义的新快照完成。

## 21. 当前 pending 与结束边界

| 当前项 | 状态 | 后续收据 |
| --- | --- | --- |
| 三组已定位修复与 weighted 数值 | proved（上述源码/数值范围） | 源码冻结，不再扩写 |
| 原左 ROI default direct 同 pose 改善 | proved（主线程实际看图及像素收据） | 不追整张旧 legacy 任意块 |
| 新 Cook | proved（最终核对见 §23） | 冻结 coverage/helper/MF/TCS/TES、四面新 hash 与实际包已关联 |
| S1 最终新快照原质量门槛 | proved（20:17 独立核对，见 §22） | 新 weighted S1 原 sameLOD0/noTES union/有效grid mean≤3、P95≤8、silhouette XOR=0 |
| 正常新 composite 同 pose 结束视图 | proved（最终独立看图见 §24） | 原左 ROI 恢复，normal/detail/TES 语义与已授权修复对应 |
| 新质量包 | proved（最终收据见 §23–25） | 新 Cook/源 shader/二进制身份关联、cold/闭包/本轮选定负向恢复与正式验收通过 |
| S4 finalshader02 | proved（最终报告已独立核对，见下） | `artifacts/terrain-editor-s4-worker2-finalshader02/editor-s4-report.json` 与 `final-evidence-summary.json` |

S4 此次不能复用 monitorfix01 的通过结论：weighted detail/macro/TES 实际改变了
编辑后的输出与高度，必须关联新 shader 的 Paint/Sculpt/Undo/Redo/材质/保存重开结果。
19:53 阶段审计时尚无最终报告；随后已读到 finalshader02 完整收据并关闭本项。
该项没有复用 monitorfix01，也没有为本次核对重跑。

### S4 finalshader02 关闭收据

`artifacts/terrain-editor-s4-worker2-finalshader02/editor-s4-report.json` 的
failures=[]、missingEvidence=[]；`final-evidence-summary.json` 明确 staged MF SHA256
为 `40271255b210f7ff95d55528690b59b8ba24b060af5f9ffa1816013758858dd9`，
对应本轮实际 weighted detail/macro/TES 冻结版本。

- 14 个编辑阶段全部到达并完成，edit/bootstrap Cook/saved Cook/reopen 四进程均 exit0；
  四份日志的记录 validation/runtime 错误为空。
- baseline=Paint Undo、Paint=Paint Redo=Sculpt Undo、Sculpt=Sculpt Redo、
  rapid=material revert 的 BMP SHA256 精确复原；编辑 pixel mask 残差为0，
  Paint/Sculpt mask 分别为53,761/53,657 pixels。
- 24 个 rapid stamp 均改变实际 World；production staleBakeResults 0→23，
  不以合成计数或未观察到竞争窗口替代旧任务拒绝证据。
- 外部冲突产生后实际反馈延迟218 ms，观察窗口2431 ms；
  保存/独立 Cook/新进程 reopen 后 RGB mean差为0.0015679253472222223，
  没有声称持久化量化后字节完全相等。
- reopen generation1、实际 World reload generation2；reopen/reload BMP hash相同。

据此，S4 在本次最终 shader 的既定编辑闭环范围内恢复 proved。
主线程另已报告523项 terrain 回归中520通过、3跳过；这里不重跑，
也不把它代替上面的实际 EditorHost/GPU/持久化收据。

本轮独立审阅后续仅核对上述最终收据与正常新 composite 结束图，
不会再扩大到整张 legacy 场景任意 artifact，也不运行 renderer、不修改冻结生产代码。
完整 S0–S5 新快照结论仍 pending，等待上述已有任务完成；
其中剩余包/质量 pending 是因为实现变更后的身份和实际行为需要重验，
不是复活已修复的历史失败。

## 22. Weighted S1 原数值质量门禁关闭（20:17）

独立读取 `artifacts/terrain-composited-vt/s1-weighted-r8192-pair3/report.json`：
status=`screened-pass-awaiting-human-review`，exe SHA256
`a2dae74171f3d83df443892572965f048efc56d08eeac88c782d6dd2ed5216bd`。
这是新的 weighted shader 私有 Cook/图像对照收据，不沿用此前 S1 图像或旧产品 Cook。

- privateCookRuns：186.9638793秒、exit0、无 timeout，validation/runtime 错误为空；
  log SHA256=`bc2f640797f74c9c68d08a5a5e9e2523352af2d24715ff9c67681b6bc6f08e5b`。
- 18 次 renderer 均 exit0、timeout=false、validation/runtime 错误为空。
  workingSetSettledForAllCaptures=true；composite 截图前连续三次所需状态收敛。
- 原 ROI 定义保持：visible-terrain-union 加每个至少64地形像素的4×4 screen-grid；
  meanAbsRgb8≤3、P95≤8，不以全画面平均替代局部门槛。
- far/mid/near 的 silhouette XOR 均为0；六组通道的 screenedPassed、
  qualityEvidenceValid 全为true。

| 机位 | base union mean / P95 | beauty union mean / P95 | base 全部ROI最差 mean / P95 | beauty 全部ROI最差 mean / P95 |
| --- | --- | --- | --- | --- |
| far | 2.033609 / 7 | 1.394663 / 5 | 2.210822 / 8 | 2.072633 / 7 |
| mid | 0.865458 / 3 | 0.619759 / 2 | 1.083368 / 4 | 0.790938 / 3 |
| near | 0.788691 / 3 | 0.578496 / 2 | 1.178426 / 4 | 0.870284 / 3 |

far每通道7个ROI、mid/near各17个ROI，全部过原门槛；没有放宽最差ROI的P95=8。
主线程随后已查看 mid pair、near composite、far composite，无新增接缝/错层。
机器报告保留的 human-review pending 字面值不抹去后续已发生的主线程查看，
但 sameLOD0/noTES 的数值结论仍不冒充正常 LOD/TES 最终产品视图。

**据此关闭 S1 新快照原数值质量 pending。** private Cook 成功同时补齐本次 S1
夹具的构建证据；它不自动关闭最终交付产品 Cook 的身份关联与包验收。
S4 finalshader02 继续 proved。当前只等待最终产品新 Cook/新包的完整身份与功能收据、
正常 LOD/TES 新 composite 结束图和相应产品成本，再决定 S5 关闭。
不重跑 suite、不运行 renderer、不修改冻结源码，不再扩大旧 legacy 全坡 artifact 调查。

## 23. Weighted 最终 Cook/包闭环独立核对（20:39）

最终证据根：
`artifacts/terrain-composited-vt/game-delivery-final/weighted-final/`。
本轮直接读取 final-evidence、identity-change、package-checks、product-views、
product-cost、原始日志及当前成品的小型 manifest/身份文件；没有重新启动 renderer、
Cook、validator、既有 suite，也没有修改生产源码或成品。

`product-cook.exit.txt=0`；`identity-change.json` 四面的 dependencyHash/contentHash
均不同于旧包。逐面读取当前 `../vt-game` 的 TR→包 manifest，确认 new hash 与该表一致，
每面5468页、1,213,633,552 bytes（含16-byte头），dependency/coverage/height 三类
sidecar 齐全。依赖清单含 terrainCoverage 和最终 MF。
冻结快照列出的 MF、coverage、bake、TCS、TES 五个当前源码 SHA256 全部匹配；
MF仍为 `40271255b210f7ff95d55528690b59b8ba24b060af5f9ffa1816013758858dd9`。

`package-checks/acceptance-results.json` 状态为
`Changed-snapshot package checks passed; selected negatives only; prior receipt linked`：

- stage-warm、新 export、verify-pristine、package-cold 均exit0；
  cold零shader miss、正常teardown。
- 本轮仅执行 **wrong-version、wrong-filter-rule 两类** verify及worker1/2拒绝。
  四份runtime日志独立检查均有明确init failure、正常teardown、exit1，
  未检出VUID/THREADING/Validation Error；两类恢复及后续verify均exit0。
- 官方validator退出0；`verify-after-views-cost.log` 为
  `{"ok":true,"verified":"../vt-game"}`，退出0。
- priorReceipt指向 `final-filter-v2/acceptance-results.json` 的七类完整负向历史，
  其记录SHA256与实际文件匹配。本轮**没有宣称七类完整重跑**。
  已有七类协议/缺失/损坏覆盖与本轮变更后的选定负向、冷启动、正式verify组合形成证据。
- shared cache前后均为
  `88619ea621d16b64acdca30099f25d115165ee2a18b03c8cb56341acce8b7a55`。

当前成品：
`D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/terrain-openworld/vt-game`。
按 `final-package-sha256.json` 对当前四个身份文件实际核对大小/hash，全部一致：

| 文件 | bytes | SHA256 |
| --- | ---: | --- |
| main_game.exe | 73,058,834 | d9adce053b41089b69709067f27edcd7999cd962b5f973def2e3895eff5236f4 |
| game-export-manifest.json | 111,892 | 0692e3a709d1a8d265c7de6b13aa74f03f3f37fdf72f3a92297149ff62006573 |
| shader/runtime-shader.manifest.json | 110,701 | a798e993263cb92b2efa332a7061b795ded373a2f60f54c80499bd792643a2f8 |
| vl_build_flavor.json | 53 | 29511d73d92f130e13241de7a846d383950053a6f220ea944bdd0bce3603c0ad |

Shader源码/缓存变化而native exe摘要保持不变是本次交付的真实身份，
不能只比exe就认为新shader未生效；新依赖hash、runtime shader manifest、
冻结源码、增量cache与新Cook/实图共同关联本轮产物。

## 24. 正常 LOD/TES 新 composite 视觉门禁关闭

本审计已实际通过 view_image 分别查看 weighted-final/product-views 下
**mid.png、near.png、mountainside.png**，不是复用旧包图或仅阅读报告：

- 原左坡 ROI 的矩形硬边明显消除，先前 near/mid 可见黑点未再观察到；
  未发现新增VT页缝、错层或与已定位缺陷相同的断裂。
- mountainside 同原pose、正常LOD/TES下的变化符合 weighted 修复目标；
  当前composite图坡面也比旧legacy对照平滑。此判断不扩展为清除全图每个旧artifact。
- 三份原始日志均正常teardown，无shader miss或检测到的validation/runtime失败。
  capture-results三机位exit0；mid/near实际LOD0:46、LOD1:18，
  mountainside实际LOD0:39、LOD1:25，无fixed LOD override。
- detail MI tessellation宏/包内TCS/TES产物存在，
  displacementScale=1、tessRadius=60m、tessMax=8、targetPixels=4、
  heightBlendStrength=3、distanceFade=0。没有用关闭detail或位移的消融图作为最终产品图。

**关闭本次正常 LOD/TES 视觉pending。** 此为用户指定机位/原ROI恢复边界内的人工审阅，
与§22原sameLOD0/noTES数值质量门禁相互补充；不声称跨任意LOD逐像素相同，
不新增整张旧legacy任意块清零要求，也不把之前“两边都有块”作为通过理由。

## 25. 新产品成本及精确文件计量

weighted-final/product-cost/cost-results.json status=passed，全部checks=true；
进程exit0、无timeout/报告错误、正常teardown、无编译、零shader miss。
本次核对原cost.log实际hash与report一致，包manifest、包shader cache、
shared cache前后保持。四面stats与完整payload ledger存在。

### Inventory payload 与实际目录分别计量

独立读取当前 `game-export-manifest.json.files` 并枚举磁盘（20:39），结果如下：

| 计量范围 | 文件数 | bytes |
| --- | ---: | ---: |
| export inventory payload（files数组合计） | **564** | **5,118,970,251** |
| inventory外：game-export-manifest.json本身 | 1 | 111,892 |
| inventory外：运行生成GLSL参数include | 11 | 7,891 |
| 当前整个产品目录 `Get-ChildItem -Recurse -File` | **576** | **5,119,090,034** |

差额12文件、119,783 bytes与manifest+11个generated文件精确对应。
generated位于 shader/glsl/generate 及 shader/glsl/pass/generate，
verify允许这些启动产物且最终已ok；它们不是缺失或被篡改的inventory payload。
**不删除generated、不删除或改写新包。** final-evidence/cost里的packageBytes/files
应解释为inventory payload，不能称当前整个目录只有564文件。

### 成本观测范围

| 量 | 本轮记录 |
| --- | --- |
| CPU timing | 30 snapshots，p50/p95/p99=16.6685/20.87075/23.16791ms |
| CPU working/private采样峰值 | 912,384,000 / 5,455,433,728 bytes |
| PID dedicated+shared同刻峰值 | 4,320,878,592 bytes |
| 最后30秒 dedicated+shared峰值 | 3,306,508,288 bytes |
| 有效内存样本 | CPU133、GPU42；tail CPU58、GPU20 |
| Terrain cache/source payload | 210,644,416 / 186,696,804 bytes |
| Terrain accounted payload | 397,341,220 bytes，cache cap=268,435,456 |

CPU数据为60Hz paced、含pacing/present等待的wall-clock snapshots，
内存为目标PID/WDDM采样近似、含源资源/driver，不是瞬态精确高水位。
源数组计量明确保留，不将source再加到已经包含它的PID总量上。

**GPU时间必须限界：** 记录的gpuTotalMs p50/p95/p99=
0.0945/0.095/0.10352ms，实际query覆盖shadowCascade0..3、
localShadow0..7，lightCulling为null；不覆盖完整Base/terrain/TES帧。
final-evidence已标记 `wholeTerrainFrameProven=false`。
因此该数据只称“已计时pass的部分GPU耗时”，**不得宣称完整frame、
地形或tessellation性能已量化通过**。本次不据此补造吞吐结论，也不在已约定的
有界成本/压力验收之外新增全帧计时门禁；这是完成结论的明确技术限制。

## 26. 最终 S0–S5 矩阵与结论

| 阶段 | 最终判定 | 最终证据边界 |
| --- | --- | --- |
| S0 语义/资源协议/版本与所有权 | **proved / 完成** | 已固定支持范围、ID-aware输入、内容/驻留身份、候选发布与epoch退休；非均匀/镜像等不支持输入明确拒绝 |
| S1 合成/height/质量原门槛 | **proved / 完成** | actual07、weighted-detail-actual02、height数学oracle；weighted pair3新Cook+18图原ROI数值通过；正常LOD/TES三图原ROI恢复已独立查看 |
| S2 Cook/只读存储/新包消费 | **proved / 完成** | 最终四面新dependency/content hash、5468页/面、sidecar闭包、新包cold与正式verify；协议失败的既有完整负向+本轮变更选定负向 |
| S3 Runtime闭环/预算/World换代 | **proved / 完成** | run03两线程64/16压力、selected保护/祖先粗化/读取停止/两次reload、Residency29及生命周期回归；最终产品成本记录补齐 |
| S4 最终shader编辑生命周期 | **proved / 完成** | finalshader02最终MF实际动作、旧任务0→23拒绝、Undo/Redo/revert精确恢复、保存Cook/重开与World换代，无fail/missing |
| S5 多tile迁移/旧链收敛/交付 | **proved / 完成** | topic资源四tile R8192/192新Cook与包，独立height、实际实时源消费者留账，unused参数清理/旧物理atlas绕过，新包功能/视觉/有界成本及身份核对完成 |

**最终结论：在本次用户固定的实现范围、原质量门槛、有限平台与结束机位条件下，
S0–S5 全部完成，本审计没有剩余pending或具体阻断。**
不依赖删除失败历史、不放宽ROI阈值、不把模块测试替代整链，
也没有新增未经授权的全坡/全设备清零要求。

完成不意味着任意设备、任意作者输入、全故障注入或所有旧legacy artifact已验证；
尤其完整GPU frame/TES性能没有被本次部分query证明。上述限制保留，
不混入“全部已实现范围完成”之外的能力承诺。
主线程继续负责其他文档索引收尾；本审计写集仅本文，不修改冻结生产代码、
运行生成文件或最终成品，也不重复执行任何已完成suite。
