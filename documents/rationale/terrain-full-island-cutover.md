# 完整飞马岛与唯一地形路径切换

日期：2026-11-20。执行记录，非实现契约。

用户要求完整 4×4 飞马岛使用新合成 VT，并将其作为后续地形标准，
旧直接基础层/层源 VT 不保留。此前 2×2 成品收据只证明此前范围，
不代替本次完整岛屿验收。

## 已落实的切换

- 默认场景改为
  `Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/SC_composited_vt_4x4.json`。
  16 个真实地块，独立基础/detail MI、作者高度/Paint 副本及 Cook 身份；
  保留海面与原环境，去除依赖 C# 的跑图角色。
- 各块 R8192、192 槽，保持已验收单块密度；World cache GPU admission
  为 1 GiB、CPU/上传暂存为 512 MiB。每帧 World 共享 4 MiB、16 页、
  2000 μs 后续调度额度，不按地块倍增。
- 删除旧层源页管理/预算/可见性/上传/反馈代码、旧材质采样分支、
  旧 atlas 绑定、旧控制台命令与专用测试。合成反馈环与 graph 资源改为明确名称。
- 保留新系统真实需要的作者覆盖数据、catalog、detail/macro 源数组、
  mip3 烘焙输入和宏高度碰撞；这些不是旧基础材质运行路径。
- TR 必须显式启用并指定包路径；材质中的退休参数/槽位也明确拒绝。
  导入统一输出新格式和 `renderReady=false`，并给出 Cook 命令。
  标准流程为保存作者输入→显式 Cook→加载 3D，不宣称自动 Cook。
- 架构门禁 `Terrain composited-only cutover` 禁止旧入口复活。
  旧数值图和收据仍属历史；旧运行对照驱动只保留明确拒绝入口，
  不再生成/执行旧场景。

## 当前数值与静态验证

- `artifacts/terrain-composited-vt/full-island-standard/bake-gpu/report.json`：
  实际 GPU 八种 bake 模式与全视距静态数学对照通过，材质四阶段编译链接通过。
- 同目录 `weighted-gpu/report.json`：136 查询通过，最大参考误差
  1.49e-7，位移误差 1.43e-9 m；这是独立 GPU oracle，不是第二条生产路径。
- `artifacts/terrain/full-island-standard/coverage-purehelper-20261120-215741/report.json`：
  136 查询通过，包括 bounded placement 和纯 helper；外地块权重为 0，
  不再包含退休页内过滤接口。
- terrain schema、导入/装配、Editor 资产页及 material schema 已有本轮回归。
  退休材质参数/atlas 拒绝相关 35 项测试通过。
- Full16 CPU 资产检查确认 16 TR、32 独立 MI、16 独立 Cook 路径，
  未发现退休/未知 MI 字段或缺引用。此时尚无完整 Cook/运行通过结论。

完整编译、16 块 Cook、全岛与近景截图、跨 tile/World 换代、包与成本收据
在后续追加；不以旧 2×2 包代替。

## 第一轮 Full16 实际收据

`artifacts/terrain-composited-vt/full16-standard/`：

- `cook.stdout.log` / `cook.exit.txt`：16 块均成功，每块 5,468 页，
  共 87,488 页、19,418,136,576 bytes 材质 payload，正常退出。
  Cook 使用短私有资源目录避免 Windows 路径长度问题，作者路径与包路径映射有记录。
- `overview/report.json`：两线程、零 shader miss、正常退出、无 validation 错误。
  实际统计 surfaces=16，LOD2 绘制 256 chunks；
  主线程查看 `overview/full-island-far.png`，确认完整岛屿新 VT 出图。
- 该轮 cache payload 为 842,577,664 bytes，source 为 186,696,804 bytes，
  七张共享源纹理；两者总 payload 1,029,274,468 bytes。
  cache 不超过 1 GiB；不把该数字当整个进程或驱动显存峰值。
- 第一轮 wall 约 254.02 秒，包含冷加载与 300 个限速帧，
  不保证 OS 磁盘缓存为冷，也不是纯帧耗时。

调查该加载成本发现只读服务重复 Open 同一 Store、Store 验证重复读取包。
修正为一次 Open 并复用已验证对象；Store 按物理 offset 顺序读取，
同批字节同时更新逐页和整包 hash。保留 header、严格 schema、
长度/offset/gap/overlap/EOF 与所有 hash 校验，不使用懒验证或可变文件全局缓存。
该修复不改协议、序列化、Cook 身份或页面数据，不需重 Cook。
`terrain-store-single-pass-tests.log`：28 通过、1 symlink 条件跳过；
实测约 6.8 MB 包的读取字节包含 manifest 后仍为单遍。
后续运行和包使用重链后的新 Reader，原 254 秒收据仅作基线。

## 唯一路径回归

- `artifacts/terrain-editor-s4-worker2-standard-cutover01/editor-s4-report.json`：
  failures/missingEvidence 均为空；bootstrap Cook、编辑、saved Cook、
  reopen/reload 四进程 exit0，无 validation/08114/DeviceLost。
  Undo/Redo、material revert 截图 SHA 精确恢复；24 次 rapid stamp 改变 WorldHash，
  旧结果拒绝 0→23；保存重开 RGB 平均差 0.001568，World generation 1→2。
- `game_export_tests` 69 项：68 通过、1 symlink 条件跳过。
  导出前拒绝缺少/关闭新配置；即使重算包 inventory，包内关闭/缺失配置仍拒绝。
- Blender producer→split→import 契约已同步；四项回归及 importer self-test 通过。
  producer manifest 检查为静态，split/import 为真实 CPU 执行，未运行 Blender。

全岛多机位、位移/瞬移/World 换代、默认作者工作树发布、
最终包与新 Reader 成本仍以随后收据为准。

## 新 Reader 全岛运行与作者工作树发布

`source-publication.json` / `source-readiness.json` 已记录真实派生数据回到 topic
资源工作树：16 manifest、112 文件、19,584,863,344 bytes，
52 个生成器输出身份一致。包路径统一使用短 `VT/<id>/manifest.json`；
作者输入不变、包路径从内容身份中规范排除，未篡改包摘要或重新 Cook。
代码工作树的默认完整岛场景已具备作者和包闭包。

`reader-final/runtime/report.json` 为最终 Reader binary 的实际运行：
status=passed，所有 checks=true、parserProblems 为空，正常退出与 teardown，
无 shader 编译、无 validation/runtime 错误。
覆盖远/中/近、另一地块近景、四地块交点、跨岛移动、瞬移、
实际 World reload 及返回全景；各段 16 表面的工作集收敛，
缓存账本未超过 1 GiB，generation 随重载推进。
主线程实际查看 near、mid 与 cross_tile_corner，跨地块局部作者层边界
仍由独立审阅解释，不把 exit0 当作任意像素无边界的保证。

进程 PID71576，首个 World commit 约 162.93 秒，首 far 图约 170.98 秒，
第二次 World commit 约 364.49 秒。此前 254.02 秒是另一整次运行口径，
不当作同负载的严格 A/B benchmark，也不保证 OS 磁盘缓存为冷。
新 Reader 保存完整内容校验，因此非压缩全岛加载仍有明显成本。

该运行采集 746 个 CPU 内存样本、245 个 WDDM 样本，无缺失通道。
包括启动、移动和新旧 World 重叠的 CPU private 峰值约 10,460,635,136 bytes，
同刻 dedicated+shared 采样峰值 8,728,637,440 bytes。
这些包含源纹理、驱动与退休重叠，是目标 PID 的采样估计，不是精确瞬态高水位。
30 Hz paced 的 38 个 CPU 快照 p50/p95/p99 为 33.47/43.82/82.54 ms，
含节拍与等待；GPU query 只覆盖报告中的部分 passes，不是完整地形帧耗时。

最终独立包/冷启动、退休配置与预算拒绝和恢复仍待最终包收据关闭，
不复用此前 2×2 包大小、性能或二进制摘要。
