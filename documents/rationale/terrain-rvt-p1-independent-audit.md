# RVT P1 独立中审与修复复核

日期：2026-11-21。工作树为 `terrain-openworld/VulkanLearn`，
验收者未参与 P1 实现；只写本审计与独立 P6 rationale，未改实现，未启动 GPU。
**P1 整阶段 pending，尚不放行。**
计划退出条件来自[生产计划](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)；
现存[合成 VT 契约](../contracts/terrain-composited-vt.md)仍有 V1 完整索引及全包扫描描述，
不能把这些历史行为当成 V2 的完成证明，整合后必须同步契约。

## 初审证据

- 原 CPU 日志为 165 项：162 通过、2 skip、1 fail；
  fail 为 `AbortedReadOnlySeedCanRetryZeroButPreviewCannotUseZero`，主线程已派 Euclid；
  本记录没有把开发中整合或暂时编译不一致裁为最终阶段失败。
- 原 exporter 为 72 项：71 通过、1 symlink skip；
  独立重跑相关 32 项：31 通过、1 skip。
- query 原二进制独立重跑 16/16；这是 CPU helper 证据。
- 独立重编 Store/Production/Adjacency：25 项，24 通过、1 symlink skip，
  真实 Windows junction 替换负例通过。日志在
  `D:/Temp/vl-p1-audit-20261121/store-tests.log`。

## 四项发现与当前复核

| 发现 | 原证据 | 修复复核状态 |
| --- | --- | --- |
| predictable atomic temp 的 hardlink 被 ofstream trunc 改坏旧输入 | 原 probe manifest 替换失败但旧输入覆盖 | 已独立重编原 probe，当前 CREATE_NEW error80；旧 manifest 与旧输入均保留。窄问题修复确认 |
| runtime MI 基准不一致 | Service 相对 manifest parent，exporter 相对 resource root；隔离 probe 查找失败 | 当前 exporter 改为 `Resolve(manifest.parent_path(), ...)` 并加入 material files。源码修复确认；Pascal 76 pass/1 skip 为执行侧结果，未冒称独立重跑 |
| Unicode 大小写 alias | ä/Ä 可同时通过 closure，但 NTFS equivalent 为 true | 当前 object path 只接受 ASCII 白名单，继续 ASCII case-fold 去重。源码及独立重编 Production 14/14 回归通过 |
| albedo 线性闭包获准而 Baker 需要 sRGB | 原 fixture rgba8-unorm 获准 | 当前要求 rgba8-srgb/bc7-srgb 及 Srgb transfer。源码修复确认；不将 schema 通过解释为实际 Baker dispatch 通过 |

修后独立编译并执行 `TerrainCompositedProductionTest` 共 14 项全通过，
包含线性 albedo 拒绝、两种 sRGB 格式、非 ASCII/ASCII case alias 拒绝、
Unicode surface/material 名保留；日志
`D:/Temp/vl-p1-audit-20261121/production-fixed.log`。

atomic 原 probe 完整源码：
`D:/Temp/vl-p1-audit-20261121/atomic_probe.cpp`，
SHA256 `3a2fc4ea0f3ecb2e90da8a3aeae68741a6d711d28e1ce5efcbfde61714839434`。
命令（工作目录为本代码工作树）：

```powershell
g++ -std=c++17 -O0 -DNOMINMAX -Isource D:/Temp/vl-p1-audit-20261121/atomic_probe.cpp source/shader/build/atomicFile.cpp -o D:/Temp/vl-p1-audit-20261121/atomic_probe_fixed.exe
D:/Temp/vl-p1-audit-20261121/atomic_probe_fixed.exe
```

原日志 `atomic-probe.log` 使用
`D:/Temp/vl-p1-audit-20261121/atomic-60048/world.json`、
`previous-input.bin`、`.vl-atomic.tmp.60048.0`。
修后 `atomic-fixed.log` 使用同根的 `atomic-48568`。
原 probe exit0 表示漏洞复现；修后 exit1 是其原漏洞谓词不再成立，
实际结果为 `old_manifest_retained=1 previous_input_overwritten=0`。
源码、二进制、日志及命令清单保留在上述短目录，没有覆盖原易受影响二进制。
Meitner handoff 在 `D:/Temp/vl-p1-atomic-fix-20261121/REVIEW-HANDOFF.md`；
其 20 项结果是 19 pass/1 existing symlink skip，10 新 atomic 用例实际执行。

## 安全、身份与资源所有权边界

以下为首次中审快照；文件 API 的当前结论见本文末尾整改复核。
当时 ReadInput 在读取前后检查路径，包含 Windows reparse 检查，
并对实际 bytes 验 size/digest，不是只检查文件长度。
稳定 junction 替换负例通过；检查与打开不绑定同一 handle，
瞬时 parent junction 换入/换回 TOCTOU 未完成独立强测，保持 pending。
不把路径扫描前后检查称为竞争安全证明。
Store staged package 独占目录内再次以 trunc 打开 package.tmp 是另一窗口；
Meitner 正在修，未完成交接与独立复测，不扩展成已复现攻击。

whole closure hash 包含 shader 对象摘要和 generator/include/artifact 清单，
修改 compute digest 确认改变 hash；exporter 比较 shader cache 身份。
SurfaceContentHash 有意排除动态 MI 路径、global maps、TES 高度等基础页无关项，
不能误要求动态参数修改必重烘基础页。
Coverage 的 localLayerCount>0 当前由 Service 填充；未判定为确定缺陷，
仍需在真实双模式闭环证明合法输入可恢复。

Restore 源码先预留持久 input/sourceMap lease，再分配纹理；
workspace 在读 bytes/upload 前预留。Image/SourceMap alias 保活 owner，
成员销毁顺序支持先释放资源再释放预算。这是源码检查，不能替代设备实际账本证据。

## Rawls GPU 收据的独立只读核对

读取 `artifacts/terrain-rvt-production/p1-gpu/runs/run-002/report.json`，
重数为 3 allocation queries、58 roundtrips、278 mip/layer 子资源、
4 snapshots、21 rejects、2 lease cases；报告 status passed。
报告明确 `no Baker definition linked/tested; transfers and MakeBakeInput only`，
以及 `no World, Cook or performance claim`，opaque overhead 也是诊断预留。
因此接受为双模式 ProductionGpu 恢复/传输 helper 的执行侧证据，
不扩张为 Game/Full16/Baker dispatch/根页重建或 P0 总预算验收。
本验收者未重跑 GPU；该次中审时 GPU 窗口由 Cicero 独占，后续文件 API 复核时由 Rawls 占用。

## 仍未关闭的阶段门槛

1. 首次中审要求 Store staged package 修复后独立复测；本地 Windows 确定性标准用例的关闭范围见下文。
2. 无作者目录、不可访问工作区的真实离线 Game 双模式生产输入闭环；
   GPU helper 或合成 exporter fixture 不能代替。
3. 根页损坏但输入有效可重建、输入损坏明确失败、缺页和可丢弃缓存损坏的分类与就绪门控。
4. 整合 Service/Runtime/Baker 的实际产页、身份/status/fence 后发布及相关契约同步。

Full16、性能、视觉与整包巡游仍按各阶段退出条件验收；
本记录不把 P1 模块级收据或四项修复确认变成整阶段通过。

## 文件 API 最终整改独立复核（2026-11-21）

读取 Locke 的 `D:/Temp/vl-file-api-finish-20261121/RESULTS.md`、
最终 `ctest-verified.log/xml` 和五个源码/测试文件。
逐项复算 SHA256 与 `source-hashes.log` 全部相同：

| 文件 | SHA256 |
| --- | --- |
| atomicFile.cpp | `5a2cf4e258283f7ac4b0cbbffe331c40e4e72aaf2aba562de635055f301024fe` |
| atomicFile.h | `e97c58c3eb0ae2688e9ebf3a1411ba2bf2b278a471a9c77c240f56c62a2caeb4` |
| terrainCompositedVTStore.cpp | `b8910fe0d9dfc40451ca914edb4589b50447cf47ba232a69bbc69d25ca1de23a` |
| atomicFileTests.cpp | `94e0b4cdcabf60eb7677590f036ab8f7de0d6fdc4f91a5b50bf6b01bfffeb39d` |
| terrainCompositedV2StoreTests.cpp | `2e6ba9bb44f652ed79514b7fe7b1b51c9d55237e1ec3f562fc9dcb2d3f62f074` |

仅重跑标准 CPU 测试，不执行原漏洞程序、不改实现、不启 GPU；
没有手工清理任何临时目录，保留此前受策略限制的夹具及所有交接证据。
标准单测自行创建和释放其夹具。重跑命令：

```powershell
cmake --build D:/Temp/vl-file-api-finish-20261121/build --target shader_build_core_tests store_protocol_tests file_api_production_compile -j 2
ctest --test-dir D:/Temp/vl-file-api-finish-20261121/build --timeout 60 -V --output-junit D:/Temp/vl-p1-audit-20261121/file-api-independent.xml
```

独立 stdout 保存在 `D:/Temp/vl-p1-audit-20261121/file-api-independent.log`。
构建显示 `ninja: no work to do`，包含未定义测试钩子的两个生产 cpp 编译目标。
独立完整重跑 exit0，124 项发现，**121 pass、3 symlink skip、0 failure**，
耗时 45.28 秒；JUnit 的 124 testcase/3 skipped/0 failure 与文字日志一致。
Unicode、显式 extended/长路径、本机 UNC/extended UNC、瞬时 junction 拒绝均实际通过，
句柄回归记录 `before=88 after=88`。三个 skip 与交付报告完全相同，
没有把 skip 计入 pass。文档门禁运行通过。

### 已关闭的本地 Windows 可证问题

- 原 predictable temp 截断：`CREATE_NEW` 独占取得句柄后直接写入，
  预存普通文件、目录、hardlink 不截断且不删除；不使用 checkexists 后 ofstream。
- 临时路径替换：atomic 的发布和失败清理通过原创建句柄，
  Store 的 package 同样独占创建、流式写入、同句柄校验和发布。
  持有句柄时的 reopen、替换、父目录移动负例均纳入标准测试。
- 原 ReadInput 检查与读取分离：`RootedFileReader` 持有逐级祖先和文件句柄，
  从同一句柄获得属性、seek、读取和最终身份；Windows 不共享 WRITE/DELETE。
  瞬时 junction 换入后恢复及读取期间尝试改动文件/祖先的确定性负例得到覆盖。
- staging 期间目标变化：提交前检查目标存在状态、普通文件类型、volume/file ID、
  size 和 write time；初始不存在的 Windows 目标采用不覆盖 rename。
  同内容不同身份、目标新增、内容变化均拒绝候选。
- 原失败资源收尾缺证：不完整写、producer 失败、锁住目标、输入生产期间变化、
  staging 打开后异常均有旧 manifest 保留与拥有资源释放断言。
  Store 保留最终输入和已验证包的 reader 至 manifest 提交。
- 流式处理保持：逐页生产/写入，包 hash 使用 64 KiB buffer；
  大于 64 MiB 的包回归不是全包内存聚合路径。

上述关闭是文件 API 在已执行 Windows 标准用例范围内的关闭，不是全部平台、
任意调度或任意存储故障证明。目标身份检查后到 rename 之间没有 OS compare-and-swap，
不据此宣称“所有并发更新均不会丢失”。POSIX 分支未运行；
本机 UNC 不代表远端 SMB。磁盘写满、断电及系统级 flush/close 失败未制造，
部分写失败使用确定性钩子。Batch 仍逐文件提交，无跨文件回滚保证。
没有发现需要恢复原四项可证缺陷为未修的反证；这不增加上述接口的事务承诺。

### 当前阶段门槛

**P1 仍 pending，不签整阶段 PASS。** 文件 API 负例不能替代实际 Game：
仍需真实离线 Game、无作者目录的 CoverageAtlas/LocalLayerArray 双模式输入闭环，
根页损坏且生产输入有效的重建与就绪门控，以及生产输入损坏的明确失败分类。
Rawls 的分块输入 64 MiB 整合仍进行中，待最终实现和真实运行收据后再独立验收；
不将其当前开发状态裁为最终失败，也不以先前 GPU helper PASS 替代。
Service/Runtime/Baker 真实发布、status/fence/身份检查及当前契约同步仍按实际退出条件核对。
