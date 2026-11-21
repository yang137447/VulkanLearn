# 地形 RVT：作者操作与验收入口

日期：2026-11-21。本文为操作说明，不是已交付声明。
当前行为见 [RVT 契约](../../contracts/terrain-composited-vt.md)、
[地形契约](../../contracts/terrain.md)；
退出条件见 [生产计划](../../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)，
阶段状态见 [执行台账](../../rationale/terrain-rvt-production-execution.md)。

**P3 性能、Full16 独立 Game、P4 真实编辑尚未通过。**
[P3 中审及追加复核](../../rationale/terrain-rvt-p3-independent-audit.md) 已独立接受
身份明确Top4候选的有限数值/validation-off性能子项；当前source-controlled promotion
仅保留World/WorldPadding及include，旧五个compute已删除。
Repeat保留完整评价域，Clamp裁剪证明不推广为Repeat等价。
真实10936页运行准备/执行不计为完成；有限候选通过不是整体P3或最终Game通过。
旧 `vt-game` 的 2×2 gen2 产品与旧 Full16 V1 全页包均属历史，
不能作为当前 V2/gen3 按需生产成品。新默认 fixture、Cook exit0、exporter verify、
根 Ready 或远景截图也不能单独表示完整交付。

## 1. 工作树与当前默认入口

- 代码：`D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn`。
- 资源：同级 `VukanLearnResources`，经 `config/config.json -> resourcePath` 访问，
  不修改共享 master 或重置本 topic 的现存 Height/Paint。
- 构建入口：`build-vt-release/bin`；执行前由集成 owner 锁定最新二进制/shader 快照。
- 当前 `config/config.json -> initScene` 仍是
  `Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json`。
  **未切换为新 RVTv2 默认**；旧 TR/包与新 Store 不兼容时不能靠 V1 fallback 启动。

新作者命名空间分别为
`RVTv2_Full4x4_R8192_Default_CoverageAtlas` 和
`RVTv2_Full4x4_R8192_Default_LocalLayerArray`，scene 都为其中的
`SC_composited_vt_4x4.json`。存在作者副本不等于已经 Cook/发布。
每 scene 有 16 个 1008 m 地块、每块 8192² 虚拟材质域；
画质验收不能用 100 m 缩尺或 2×2 fixture 替代。

## 2. 作者配方、保存和发布

每个 World 的 TR 共享 `packagePath`（V2 发布 manifest）和
`productionSourcePath`（`world.author.json` 作者配方）。
配方 kind 为 terrain-rvt-world-author/version 1，列全部稳定 surface 身份和显式邻接。
Full16 的正常网格有 24 条内部无向边/48 条有向关系、9 个交点；
不能用“位置相邻”替代配方。Game 不读取作者配方。

TR 不再写 `residentPages` 或 `heightBudgetBytes`；两字段被严格拒绝。
World 总预算唯一来源为未修改的 `config/terrainRvtP0Frozen.json`。
作者工具按当前 M_terrain 默认值省略冗余 MI 覆写；
Coverage mode0 默认时不重复写0，Local mode1 在与默认不同才显式写1。
不要为通过 MaterialValidator 放宽规则或恢复旧材质开关。

三个动作相互独立：

1. Save Heightmap / Save Paint 保存作者数据；保留现存 sidecar 或其真实缺席状态。
2. 预览刷新使用编辑快照；其成功不表示另一进程能读取未保存笔刷。
3. Cook 冻结真实生产输入、根/粗页及当前两 compute 编译身份，
   原子发布一个 World V2 manifest。源 bytes 变化需新 Cook，gen3 数字不够。

新资产生成入口（先只读预检，已有命名空间拒绝重复生成）：

```powershell
py -3 tool/validation/terrain/prepare_full16_v2_authors.py `
  --author-scene Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json `
  --output artifacts/terrain-rvt-production/full16-author-preflight-new.json
```

仅需新命名空间且 owner 放行时使用该工具的 `--generate`；
当前两个命名空间已有成果，不要重建覆盖。作者/sidecar/World2 检查点见
[P5 操作记录](../../../tool/validation/terrain/p5_checkpoint.md)，其中早期五阶段快照不适用于
当前 World+WorldPadding；使用最新 owner handoff。

## 3. 私有 Cook 与安装验证

以下为待 owner 在独占 GPU 窗口执行的模板，本轮文档任务未运行。
必须使用不存在的新 stage/resources/package 路径，不复用旧 `vt-game` 或改旧包。
先由集成 owner 确认当前二进制和 closure 一致，再按两种权重分别执行：

```powershell
$scene = 'Generated/Validation/TerrainCompositedVT/Variants/RVTv2_Full4x4_R8192_Default_CoverageAtlas/SC_composited_vt_4x4.json'
$stage = 'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\rvt-check-new\stage'
$resources = 'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\rvt-check-new\resources'
$package = 'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\rvt-check-new\game'
$binaries = (Resolve-Path build-vt-release/bin).Path
py -3 tool/validation/terrain/prepare_full_delivery_stage.py --scene $scene `
  --stage $stage --private-resources $resources --binary-directory $binaries --copy-inputs
if ($LASTEXITCODE -ne 0) { throw 'Stage failed' }
Push-Location $stage
try {
  & ./bin/main_editor.exe --terrain-vt-cook --initial-scene $scene --exit-after-frames 2
  if ($LASTEXITCODE -ne 0) { throw 'Cook failed' }
  & ./bin/main_game.exe --initial-scene $scene --shader-force-rebuild --exit-after-frames 2 --no-dev-ui
  if ($LASTEXITCODE -ne 0) { throw 'Game shader warm failed' }
  & ./bin/vl_export_game.exe --project-root $stage --binary-directory "$stage/bin" `
    --flavor game --initial-scene $scene --output $package
  if ($LASTEXITCODE -ne 0) { throw 'Export failed' }
  & ./bin/vl_export_game.exe --verify-package $package
  if ($LASTEXITCODE -ne 0) { throw 'Installation validation failed' }
} finally { Pop-Location }
```

LocalLayerArray 使用对应 scene 和另一组全新目录。实际收据还要记录各命令、退出码、
包 inventory/hash、P0Frozen hash、SPV/compilePolicy 与两 compute/include/output 摘要。
`--verify-package` 执行完整输入及随包页闭包校验，不证明 GPU 可运行或性能通过。
独立只读 Game 执行使用当前 `rvt_readonly_install.py` / `readonly_game_acl.ps1` 的
所有权收据及实际写入拒绝探针；不能用 ReadOnly 文件属性冒充 NTFS 写权限拒绝。
先看 `--help`，不要在未知或共享包上试 ACL。

Game 不依赖作者目录；所有生产/动态输入、宏几何/碰撞资源、base/detail MI、
shader/SPV 和冻结配置必须随包。raw RGBA8 基础页是当前编码，
P6 决策是不采用 BC5/BC7，不承诺压缩包体积或源资源全部释放。

## 4. 冻结门槛与证据采集

固定 SHA256：
`43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd`。
不得修改 JSON/sidecar、放大槽数、延长收敛时间或降低质量来通过。
条件为 RTX3080/621.01、3950X、Release MinGW、1280×720、MSAA2、
30 Hz、UI off、正式性能 worker2；正确性分别 worker1/2。
有限 P0 基线的审计边界见 [P0 独立记录](../../rationale/terrain-rvt-p0-independent-audit.md)。

关键门槛（完整字段以冻结文件为准）：

- 共池 512 槽，含 root/reserved/retiring；全局 4 在途、每帧最多产1页。
  单 World RVT GPU 1 GiB/CPU512 MiB；外层 terrain 含 candidate/retiring 峰值各3 GiB。
- 单页 production 4 ms、production+copy 6 ms、每帧 production+copy 6 ms；
  至少1000 raw帧，稳态 GPU pass sum p99≤4.618 ms、流式≤10.199 ms，
  geometry 含 TES p99≤3.754 ms。CPU墙钟 p95≤49.025/p99≤72.105 ms。
- World commit≤76.748 s、process→interactive≤86.748 s，
  pose/瞬移收敛≤10 s；有效 mip bias≤1、真实目标+bias ready fraction≥0.99，
  ready 时 unresolved/incorrectIdentity均为0。
- 完整进程 private peak≤8,194,532,966 B，
  同刻 WDDM dedicated+shared peak≤6,977,722,777 B；
  WDDM间隔≤2.5 s/至少20有效样本。不是精确瞬态GPU高水位。
- Full16随包粗页≤448、最细 mip4、强制根16；Game包≤1.5 GiB。
  这不是必须常驻448粗页，也不允许省略生产输入成本。

冷条件为新进程/新物理驻留，保留不可变输入、根/粗页与SPV；
不声称清空OS缓存。当前磁盘fine cache disabled，其删除/损坏case只能明确
not-applicable并交独立审阅，不能算执行通过；未访问区域实际GPU生产仍必验。

`terrainvt stats`、`terrainchunk stats`、`terrainlod stats` 是诊断。
根 Ready、queued/reading=0、CPU estimated histogram不代替实际采样identity/有效mip、
图像ROI、TES生效和真实interactive事件。当前quality使用原反馈7MRT真实winner：
两张128² RGBA32_UINT完整属性+同次final depth，单次compute解析，
不使用旧final-depth replay。两capture complete=true/dup0/validation0、
workspace约27.396 MB只是模块证明，不是interactive；缺必要事件/数据仍报告incomplete。
证据消费入口为 `rvt_delivery.py`、`run_rvt_case.py` 与
[证据协议](../../../tool/validation/terrain/rvt_evidence_protocol.md)，先检查其当前参数/快照绑定。

## 5. 全岛与真实编辑检查

每种权重、每种worker分别在独立包验证冷启动、未访问机位、移动/转头/瞬移、
World换代/退休。全岛覆盖16块近景、24内部边、9交点、4外角；
正常LOD/TES近场、坡面、基础色/局部法线/ORM与最终光照均留原图/机位/ROI。
根payload损坏须留原始loading→实际生产→publication/rootReady及未初始化draw=0；
根索引缺失/生产输入损坏属于协议/输入拒绝，不是合法缺页。

P4必须走真实CommandBus→EditorHost→同一生产链：
Paint/Sculpt、Undo/Redo、跨边dirty、静态材质改/恢复、外部冲突保留内存编辑、
Save→独立Cook→重开→World重载；旧revision GPU结果拒绝须有实际收据。
CPU dirty测试或历史S4 worker2不能代签本轮。

默认切换由主线程在以上证据及独立复核通过后操作：
保留旧默认/作者hash，选定新Coverage场景及真实TR/shared manifest，
更新config默认并重新无`--initial-scene`运行，验证实际加载身份一致。
本检查点不改config/资产、不宣称已切换。旧约19.42 GB V1页只在明确旧发布清单与
最终放行后清退；未知dirty/旧作者/历史日志不在删除范围。

历史资料保留在 [旧最终证据](../../rationale/terrain-composited-vt-final-evidence.md)、
[旧Full16记录](../../rationale/terrain-full-island-cutover.md)，不作为本轮最终结果。
