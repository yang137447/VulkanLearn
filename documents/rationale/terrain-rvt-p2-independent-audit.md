# 地形 RVT P2 独立源码与 CPU 中审

状态：**PENDING（中审，不是阶段 PASS 或最终 FAIL）**。
日期：2026-11-21，Asia/Shanghai。Euclid 正在修改 Service/Runtime/预算，Cicero 正在修改 Baker；
以下是本次读取和 CPU 编译时的快照结论，后续修复须逐项复验。
本审计只写本文件，不修改实现，不使用 GPU，不构建或运行 `build-vt-release`。

依据为 [生产方案](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)
§5/P2、[P0 冻结](../../config/terrainRvtP0Frozen.json)
和 [P0 独立审核](terrain-rvt-p0-independent-audit.md)。
不重做 P0 统计，也不替代 Tesla 的 P1 审计。

## 可立即分派的具体问题

### A1：旧 revision 根页生产完成后，新 revision 根请求丢失（CPU 已复现）

位置：`source/terrain/terrainCompositedResidency.cpp` 的
`InvalidatePreview`、`Request`、`CompleteProduction`。

最小操作：

1. 创建无 Store 的 preview residency，generation=1、revision=0，四个物理槽。
2. 从 `ProductionRequests()` 取得根页并 `ReserveProduction(token, 0)`。
3. `InvalidatePreview(desc, 1, 1, {root.key})`。
4. 旧任务 `CompleteProduction(page, 1, true)`。
5. `BeginUploads(1)` 后 Abort 空事务，再查新生产请求。

实际：旧结果正确被拒绝，`Ready()==false`，但 `ProductionRequests()` 为空。
原因：失效处理清空 pending 后尝试 Request 根页；Request 看见同键 Uploading 槽就拒绝入队。
旧任务完成走 `current==false` 分支，只增加 discardedResults，不为当前身份重新 Request。
根页没有新的可派发请求。后续 feedback 可能再触发请求，但不能依赖它恢复缺失根页。

建议交 Euclid：旧任务退休后，按当前身份和 Needed/dirty 状态重新入队，
同时保留旧写入 epoch 的槽回收约束；增加“根页在途时 revision 改变”的回归。
本结论不依赖 GPU status 或 Baker 行为。

复现所用 Residency 源码 SHA256：
`8b2b9050e4fcd79fb4a2a451b9fda1e07eb51d07912c9981d7b634264d820d78`。

### A2：geometry CPU 准入尚未覆盖实际计划、源高度和构建峰值（源码已证遗漏）

读取时 `TerrainCompositedService::AdmitGeometry` 的 CPU quote 为：
`vertexBytes + indexBytes + dynamic vertex allocation + instance allocation + sizeof(RenderableObject)`。
这覆盖部分 mesh 持久数据和 host-visible 分配，不能据此宣称完整 terrain CPU 总账。

可核对的持有和分配链：

- `TerrainBuildPlan` 在 `source/terrain/terrainAssetTypes.h` 持有 decoded `HostImage heightmap`
  和 `TerrainLayerPaintData layerPaint`；仅计结构体不计像素/vector 容量。
- `TerrainPageManager::RegisterTerrain` 执行 `plans.push_back(plan)`；
  Service 的 `Surface::plan` 也复制计划；`TerrainCollisionInstance::plan` 可保留同一高度源。
  共享高度数据应按实际对象身份计一次，独立复制的描述、容器、字符串分别计费。
- `RendererTerrainLoader` 先 `BuildChunks` 生成一个 LOD 的完整 `chunks`，
  然后才逐 chunk `AdmitGeometry`。构建时 vertex/index/patchIndex CPU 数组已分配。
- `new RenderableObject(chunk.vertices, chunk.indices, ..., chunk.patchIndices)` 传入 const chunk
  的 lvalue，构造参数按值，发生复制；原 chunks 在该 LOD 循环结束前仍保留。
  patch 索引虽不长期保存在 RenderableObject，仍占构建及上传期间的 CPU 峰值。

读取时未见上述 plans/height pixels/paint arrays、批次 mesh 暂存及参数复制
对应的对象身份去重租约或构建前 scratch 准入。
这些占用也不能用仅用于 descriptor/driver 的 GPU metadata reserve 代替。
全文搜索当时没有 `retainedBuildPlans` 字面符号；这里审计的是实际计划持有链，
不是要求实现某个特定变量名。

建议交 Euclid：先列出 retained plans/height/paint/mesh construction 的分配清单，
在分配或接管前 reserve，按实际共享对象去重；
跨 World、编辑快照及取消路径持有租约至最后使用者释放。
staging 独立临时 scope 是已知正在进行的修改，本条不把该修改过程判为最终失败。
尚未独立测得这些遗漏的 Full16 字节数，不声称已经实测突破 3 GiB。

### A3：资源 phase 尚无业务 Active/Retiring 迁移调用（源码已证）

本次 `rg -n 'SetPhase\(' source` 仅命中：

- `TerrainRvtBudgetLease::SetPhase` 声明、实现及两个父租约传播。
- `TerrainCompositedProductionGpuResources::SetPhase` 声明、实现及内部 lease 循环。

未命中 World 发布、旧包退休或 Service/Runtime 的业务调用。
输入、物理池、页表、geometry 等多数按 Candidate reserve，
不能因此把 `byPhase` 视为真实 active/candidate/retiring 分布。
execution lease 直接以 Active 创建不代表这些资源完成迁移。
共享租约仍会保持 current 成本，不能把 phase 漏接扩大为已经证明提前释放预算。

建议交 Euclid：接入真实 World commit/retire 生命周期，检查 alias 保活时 phase 同步；
用旧包仍在途、候选失败、最后 alias 释放三条路径验证各 phase 与父 owner 统计。

### A4：feedback allocation 尚未形成实际持久准入闭环（源码问题，待修改后复核）

`RendererResourceLoadCoordinator` 在 `InitializeFrozenConfig/PrepareWorld` 前创建
`TerrainFeedbackGpuRing` 和 `TerrainCompositedFeedbackGpuRing`。
两构造器各自 CreateBuffer/MapMemory；读取时没有接收预算 owner 或保留预算租约。
Service workspace preflight 存在 `feedbackReserve = 1024 * 1024` 的局部算式，
但算式不是 actual requirements reserve，也不会随 ring/旧 World alias 退休。
descriptor/driver metadata 的 4 MiB 固定租约没有明确覆盖这些独立资源。

建议交 Euclid：明确 ring、feedback render targets 的所有者和账本归属，
对实际 requirements 与 mapped CPU 内存准入并绑定相同寿命；
若由另一已收费资源覆盖，提供对应租约、容量和非重复计算依据。
本轮未运行 GPU，不声称测得实际 allocation 大小。

## CPU 验证结果与已确认的正向事实

独立 g++ C++17/O0 构建，输出位于系统 TEMP：
`vl-rvt-p2-independent-audit.exe`。
没有写入仓库测试源码；两个附加测试通过编译器 stdin 输入。
直接编译当前 Budget、Scheduler、Residency、VT/Store/Production/Adjacency、
contentHash/atomicFile 及已有三个 CPU 测试文件，使用已有 `build` 的
GoogleTest 源码与 BLAKE3 静态库；未链接 renderer 或运行 Vulkan。

运行：`--gtest_brief=1`。**69 项，68 PASS，1 FAIL，测试运行 16.022 s**。
唯一失败为独立新增 `IndependentP2.RevisionChangesWhileRootProductionInFlight`，
断言为新 revision 根请求应存在，实际为空，即 A1。
新增的暂态生产失败后退避重试用例通过。
已有 Budget/Scheduler/Residency 67 项全部通过。

父 owner 第二次拒绝时先前 reserve 回滚、共享 lease 最后引用才归还、
跨 World/geometry 外层 owner、并发准入和 phase 单调性等已有 CPU 用例通过。
Budget 实现 charge 的 `accounted` 防止尚未正式入账的 charge 析构扣账；
其父租约成员仍会析构回滚，未将该路径判为缺陷。
Budget 编译快照 SHA256：
`1353a13e52c1a29ebdda8fff38eb46b27bb27bac9dcac6c32ca2a77e5a7f56c5`。

源码可见的共享并非仅接口声明：

- Runtime 只创建一组三通道 `sharedChannels`，每 surface binding 引用同一组；
  每 surface Residency 使用同一 `TerrainCompositedResidencyPool`。
- Runtime `RetainAllocation` 用 alias shared_ptr 把 Texture 与 lease 绑定；
  ProductionGpuResources 的 Image/SourceMap alias 保活整个输入 owner。
- pool/page table/geometry 使用查询 buffer/image requirements 的路径准入；
  不能将此正向事实扩展成全部 CPU/GPU 分配已经覆盖。
- Service 任务持有输入 owner/snapshot/纹理引用；完成路径先判断 epoch，再读 status，
  成功进入 produced，页表后续事务 Commit 才转 Resident。
  这只是源码路径证据，尚非真实 fence/visibility 的 GPU 证明。

## 尚未满足的阶段证据

**P2 仍为 PENDING。** Scheduler CPU 模型测试通过不能替代实际 Service/Runtime 集成：

| 必需项目 | 本轮结论 |
| --- | --- |
| 实际 Full16 共池竞争、512 槽含根/预留/退休 | 缺实际 GPU 证据，pending |
| 旧 epoch 页表引用、unmap 后槽复用 | CPU 局部覆盖，实际提交/旧帧证据 pending |
| 取消、非零 status、迟到 revision、提交失败 | A1 已复现；完整实际路径 pending |
| 同帧多页参数/descriptor/output 互不覆盖 | Baker 开发中，不能用四个 CPU slot 推断，pending |
| 长期 fairness、容量压力下无永久饥饿 | Scheduler 模型有测试，实际 Service 压力证据 pending |
| candidate/active/retiring 跨 World allocation/CPU 峰值 | 父 owner 算术通过；实际覆盖和峰值证据 pending |
| feedback/descriptor/host metadata 与 retained plans 全量准入 | A2/A4 待闭环及复验 |
| phase 与真实发布/退休同步 | A3 待接入及复验 |

未重新运行 P0，也未将开发中代码的编译中间状态判为缺陷。
后续 checkpoint 应保留本记录，再补充修复后的源码身份、最小复现和实际集成证据。

## 追加：可直接纳入的 A1 regression 与复现命令

建议测试位置：`tool/terrain-tests/terrainCompositedResidencyTests.cpp`，
在现有 `namespace VL` 中加入下面的普通 `TEST`，无需 fixture 或 Store。
本审计没有修改该测试文件。独立编译时用 `using namespace VL;`，
并将以下 C++ 文本作为 PowerShell `$rootRegression` 的 here-string 内容。

```cpp
#include <gtest/gtest.h>
#include "terrain/terrainCompositedResidency.h"

using namespace VL;

TEST(IndependentP2, RevisionChangesWhileRootProductionInFlight)
{
    TerrainCompositedVTBuildDesc desc;
    desc.surfaceId = "independent";
    desc.width = 256;
    desc.height = 128;
    desc.encoding = "cpu-audit";
    desc.channels = {TerrainCompositedVTFormat::RGBA8UNorm};
    TerrainCompositedResidencyBudget caps;
    caps.slots = 4;
    TerrainCompositedResidency residency({}, desc, 1, 0, caps);
    const auto requests = residency.ProductionRequests();
    ASSERT_EQ(requests.size(), 1u);
    const auto root = requests.front();
    const auto reserved = residency.ReserveProduction(root, 0);
    ASSERT_TRUE(reserved);
    residency.InvalidatePreview(desc, 1, 1, {root.key});
    EXPECT_FALSE(residency.CompleteProduction(*reserved, 1, true));
    const auto batch = residency.BeginUploads(1);
    residency.Abort(batch.id);
    EXPECT_FALSE(residency.Ready());
    const auto replacement = residency.ProductionRequests();
    ASSERT_EQ(replacement.size(), 1u);
    EXPECT_EQ(replacement.front().key, root.key);
    EXPECT_EQ(replacement.front().revision, 1u);
}
```

原失败用例在最后仅断言请求非空；上述移植版本进一步验证根页键及新 revision。
它检验 CPU requeue，不冒充实际 GPU stale publication/fence 验证。

在本 worktree 根目录，以 `$rootRegression` 保存上述源码后可直接执行：

```powershell
$auditExe = Join-Path $env:TEMP 'vl-rvt-p2-independent-audit.exe'
$cpuSources = @(
    'source/terrain/terrainRvtBudget.cpp'
    'source/terrain/terrainRvtScheduler.cpp'
    'source/terrain/terrainCompositedResidency.cpp'
    'source/terrain/terrainCompositedVT.cpp'
    'source/terrain/terrainCompositedVTStore.cpp'
    'source/terrain/terrainCompositedProduction.cpp'
    'source/terrain/terrainCompositedAdjacency.cpp'
    'source/shader/build/contentHash.cpp'
    'source/shader/build/atomicFile.cpp'
)
$rootRegression | & g++ -std=c++17 -O0 -pthread -DNOMINMAX `
    -Isource -Iextern/Json/include -Iextern/BLAKE3 `
    -Ibuild/_deps/googletest-src/googletest/include `
    -Ibuild/_deps/googletest-src/googletest `
    -x c++ - -x none @cpuSources `
    build/_deps/googletest-src/googletest/src/gtest-all.cc `
    build/_deps/googletest-src/googletest/src/gtest_main.cc `
    build/extern/BLAKE3/libblake3.a -o $auditExe
if ($LASTEXITCODE -ne 0) { throw 'CPU regression build failed' }
& $auditExe --gtest_filter=IndependentP2.RevisionChangesWhileRootProductionInFlight
```

该命令只构建一个附加 regression；先前 69 项运行另外包含已有三个测试文件
`terrainRvtBudgetTests.cpp`、`terrainRvtSchedulerTests.cpp`、
`terrainCompositedResidencyTests.cpp` 和暂态重试附加用例。
无需 GPU，也不使用 `build-vt-release`。

## 追加：新生产事务只读检查

A1 整改正在进行：本轮后续读取已看到 `CompleteProduction` 的 stale 分支增加
`Request(page.token.key)`。这是源码修复迹象，尚未重新编译执行，不能将旧二进制
的结果改写为修复后 PASS。初审失败记录仍保留。

针对 Service `CompleteProduction → RecordProduction → Runtime::RecordUploads →
Service::Commit/Abort` 以及 Baker 输出屏障，本轮未新增可证状态缺陷。
具体核查范围：

- completion 以 task epoch 和 completedEpoch 筛选，再读取 coherent status；
  task snapshot/owner/retained refs 在 completion 后才释放。
- Record 预留空闲物理槽，Baker 结束有 compute-write → transfer-read，
  随后三通道直接 buffer→atlas copy，status 单独 copy 并建立 transfer-write → host-read。
- Runtime atlas/page-table 转换沿用 shader-read ↔ transfer-dst 屏障；
  当前通用 helper 的 shader stage 为 FragmentShader，和本次合成 atlas/page-table
  的 fragment 采样用途相符。本轮没有据“跨提交”三个字推断必然缺 barrier；
  若后续这些纹理增加 compute/TES 消费者，须重新核对 stage 覆盖。
- produced 页经下一次页表录制和 Commit 才成为 CPU Resident；
  Runtime 帧上传 ring 在覆写前检查对应 frame epoch。
- 帧录制器在 backend SubmitFrame 返回后 Commit，未成功时由事务 guard Abort；
  常规 OutOfDate/Suboptimal present 路径返回重建标记，不能据此声称必然误 Abort。
- Service RecordUploads 已把新生产任务的 recorded/epoch==0 纳入返回值，
  不能继续报告“只有生产、没有页表 copy 时返回 false”的旧代码问题。

这些是读取时的路径核对，未进行真实 status 注入、提交失败注入或同步验证。
`RecordProduction` 当前每次成功后 return，实际同帧多页隔离仍没有集成证据；
不能用新增槽数或 Scheduler CPU 测试证明它通过。
P2 保持 **PENDING**，等待整改复验及 Full16 实际竞争证据。

## 整改现态独立复验（2026-11-21 10:04 起）

本节取代初审条目对“当前是否仍存在”的判断；初审和失败快照保留作历史。
仍仅改本文件，未用 GPU，未运行 Cook 或 main，不接触 Frozen 和实现。

### 已关闭或收窄的原问题

| 原项 | 当前复验结论 |
| --- | --- |
| A1 stale root 请求丢失 | **已关闭该最小 CPU 缺陷**。新编译运行文中完整 regression，通过根页键及 revision=1 断言；不是运行旧 exe。 |
| A3 无业务 phase 调用 | **已关闭“没有调用”缺陷**。Cache CommitCandidate 对旧 terrain 回调 Retiring、对 active 回调 Active；RetireWorldLocalResources 也有 Retiring。coordinator 以 weak Service 回调连接 SetPhase，Service 同步 runtime owner 与 geometry owner。 |
| A4 feedback ring 未收费 | **两类 readback ring 的该缺陷已关闭（源码）**。创建移至 PrepareWorld 后，传入 BudgetOwner；构造先查询 TransferDst requirements，reserve GPU allocation、mapped CPU 和句柄元数据，再 CreateBuffer，lease 保存在 ring 中。 |
| A2 geometry/source/plans/暂存漏计 | **显著整改，部分关闭，不能沿用旧判断**。PrepareWorld 建 geometry owner 并按高度/paint 对象地址去重收费；descriptor 按四份预留。Loader 在 BuildChunks 前 reserve 整批 workspace，每 chunk 再 reserve 参数复制；geometry staging 是独立局部 lease，构造结束退出分支释放，持久 lease 随 RenderableObject 保留。剩余明确范围见 R2。 |

新增 owner SetPhase 对已有 charge（含 alias）迁移，后续 TryReserve 至少采用 owner 当前 phase；
对应 `WorldPhaseTransitionsIncludeRetainedAliasesAndFutureAllocations` 已独立执行通过。
父 owner 回滚测试也再次通过。phase 接线的源码关闭不代表完整 GPU 退休矩阵通过。
A4 此处仅关闭 ring；未把 render-graph feedback image 的账本归属一并宣称已验证。

### 本轮 CPU 结果与可复现身份

从本文件 C++ fenced block 提取 rootRegression，以前述独立 g++ 命令编译当前源码，
额外加入 `tool/terrain-tests/terrainRvtBudgetTests.cpp`。
新输出为系统 TEMP 的 `vl-rvt-p2-independent-recheck.exe`。
**15 tests / 2 suites，15 PASS，0 FAIL，运行 2 ms**：
文中 root regression 1 项，以及当前 Budget suite 14 项。
包含父 owner 第二次拒绝回滚、alias 保活、phase 迁移和未来分配继承、并发上限。
本轮没有声称重新执行之前全部 69 项。

可重跑命令差异：在文中 `@cpuSources` 后增加
`tool/terrain-tests/terrainRvtBudgetTests.cpp`，输出名改为上述 recheck，
执行 `--gtest_brief=1`。源码读取 SHA256：

| 文件 | SHA256 |
| --- | --- |
| terrainRvtBudget.cpp | `6166d4d9a95c3b71da606bc30dc301a802e3c9cf95aa661452a332fa1fae12ed` |
| terrainCompositedResidency.cpp | `c0c3198872e853eed6de83006dd6fb7097a88398a12c085dbd35aa94c04f499c` |
| terrainCompositedService.cpp（只读核查，未链接 CPU exe） | `d8a6b62512df8ff5ae77838ecef8986f207e0276928f064741b77fbad4237a9c` |

### R1：新增 charge 弱引用登记表随任务累计增长（CPU 已证）

位置：`source/terrain/terrainRvtBudget.cpp`：
每次成功 TryReserve 执行 `state->charges.push_back(charge)`；
charge 最后租约释放时只减统计，不移除该登记项；
过期项仅在该 owner 的 SetPhase 中擦除。
正常 active World 持续生产时，不会每个任务改变 World phase。
renderer-wide peak/outer parent 也会在每次向上传播 reserve 时登记，
但 Service phase 迁移只对 World owner 执行 SetPhase，并逐租约传播，
不会调用全局 parent owner 的 SetPhase 清理其登记表。

独立 CPU 白盒 probe（仅 stdin 编译，未修改实现）：
建立 peak 和 child owner，child 置 Active，
连续 10000 次 reserve `{gpu=0,cpu=0,slots=0,tasks=1}` 并在每次循环释放。
实测输出：

```text
liveAllocations=0 cpuCharged=0 worldRegistrySize=10000 worldRegistryCapacity=16384 peakRegistrySize=10000 weakEntryBytes=16
```

两个 vector 各自容量已占 262144 B，另有过期 weak_ptr 保留的控制块；
这些内存未在 current.cpuBytes 中收费。真实 Service 每个生产任务的 executionLease
正使用同类 reserve，因此有实际调用路径，不是单纯 API 假设。
这不是 GPU resource 泄漏，但会使预算自身 metadata 随累计生产数量增长，
不能作为有界 CPU 总账通过。
建议交 Euclid：在登记/回收路径做有界清理或可移除登记，
同时处理 long-lived parent owner，补充大量短命 lease 后 metadata 仍有界的回归。
不要通过每帧迁移 World phase 规避。

probe 为直接包含当前 budget 实现的单 TU：
预先包含标准 array/cstdint/memory/optional/vector/mutex/algorithm/stdexcept/iostream，
仅在包含 `terrain/terrainRvtBudget.h` 时以 `#define private public` 暴露 state，
随后取消宏并包含 `source/terrain/terrainRvtBudget.cpp`；
循环后读取 `world.state->charges.size()/capacity()` 和 parent 相同字段。
编译 `g++ -std=c++17 -O0 -pthread -Isource -I. -x c++ - -o <TEMP exe>`。
这只改变审计 TU 的可见性，未写任何生产文件。

### R2：初始 plan reserve 已补，但编辑替换源快照的重叠 CPU 租约仍缺（源码已证范围）

当前 `planLeases` 只在 PrepareWorld 对初始 plans 计算并 reserve 一次，
高度/paint 以对象地址去重，各像素数组计一次。
不能继续说“source height 完全没有收费”。
但是 `RefreshPlacement` 把新 `TerrainBuildPlan`（包括新 heightmap/paint 强引用）
直接放入 `impl->updates`，此时旧 Surface::plan 仍保留旧数据；
后续 ConsumeUpdates `surface.plan = next`，该路径未追加/替换 planLeases。
`TerrainPageManager::UpdateHeightmap/UpdateLayerPaint` 也会持有替换指针。

因此至少在入队至替换期间，两份不同 HostImage/paint payload 可同时存活，
初始像素租约仍只对应一份；descriptor 的四倍 reserve 没有将像素乘四。
ConsumeUpdates 的局部 CPU limit 算式针对 localWeight/coverage/frame/staging，
没有按 queued 新 heightmap->data.capacity 或新 paint 数组收取额外源快照租约，
也没有向全局父 owner reserve 这部分重叠。
这是 A2 的收窄剩余项，非重复初审“没有 plan/source quote”的旧结论。

建议交 Euclid：在接管新源快照时按实际对象身份建立可共享租约，
覆盖 queued/candidate/active 及旧快照重叠，随最后持有者释放；
若已有其它层替新源收费，需给出该层实际租约及关联身份。
本轮未测该窗口 Full16 私有内存峰值，不声称已经突破冻结 cap。

### 阶段结论

**P2 仍 PENDING**。A1 最小复现已通过，A3 业务接线及 A4 ring admission 已关闭对应源码缺陷；
A2 按上述现态收窄。R1、R2 是本轮可分派的具体剩余问题。
Full16 GPU 竞争、旧 epoch、真实取消/status/提交失败、同帧多页、
长期 fairness 与跨 World 实际 peak 仍需授权 GPU 执行者提供证据。
本轮不审判正在修补的 main linker 或 Cook003 开发中状态。

## R1 附件：10000 次 shortLease 白盒复现完整源码与命令

以下是本轮已执行 probe 的完整版本。只读取登记表，不修改生产实现。
在 worktree 根目录直接运行 PowerShell；源码经 stdin 编译，输出仅在 TEMP。
这是当前缺陷的观测程序，exit 0 表示程序完成，**不表示有界性通过**。
修复后应以相同负载验证登记表大小及容量有界，并将其变为正式回归断言。
建议由实现者在 `tool/terrain-tests/terrainRvtBudgetTests.cpp` 纳入回归；
白盒宏仅用于此独立 probe，不建议直接加入正式测试 TU。

```powershell
$shortLeaseProbe = @'
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <mutex>
#include <algorithm>
#include <stdexcept>
#include <iostream>
#define private public
#include "terrain/terrainRvtBudget.h"
#undef private
#include "source/terrain/terrainRvtBudget.cpp"

int main()
{
    using namespace VL;
    TerrainRvtBudgetCaps caps{600, 500, 200, 1000, 800, 20, 4, 800, 800, 800};
    auto peak = std::make_shared<TerrainRvtBudgetOwner>(caps);
    TerrainRvtBudgetOwner world(caps, peak);
    world.SetPhase(TerrainRvtResourcePhase::Active);
    for (int iteration = 0; iteration < 10000; ++iteration)
    {
        auto lease = world.TryReserve({0, 0, 0, 1},
            TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
        if (!lease)
            return 2;
    }
    std::cout << "liveAllocations=" << world.Statistics().allocations
        << " cpuCharged=" << world.Statistics().current.cpuBytes
        << " worldRegistrySize=" << world.state->charges.size()
        << " worldRegistryCapacity=" << world.state->charges.capacity()
        << " peakRegistrySize=" << peak->state->charges.size()
        << " weakEntryBytes=" << sizeof(std::weak_ptr<TerrainRvtBudgetCharge>)
        << '\n';
    return 0;
}
'@
$probeExe = Join-Path $env:TEMP 'vl-rvt-p2-budget-registry-probe.exe'
$shortLeaseProbe | & g++ -std=c++17 -O0 -pthread -Isource -I. `
    -x c++ - -o $probeExe
if ($LASTEXITCODE -ne 0) { throw 'shortLease probe build failed' }
& $probeExe
```

注意：实现 `.cpp` 已由 stdin TU 直接包含，不要再单独添加到 g++ 源文件参数，
否则会重复定义。loop 每次迭代末尾都释放 lease，测量在最终一次释放之后；
parent owner 整段保活，不靠反复销毁 owner 掩盖累计增长。
本轮实测输出已在 R1 节保留。

本次仅补充复现附件，不增加检查范围，不代修，不新增测试结果。
P2 保持 **PENDING**，等待 R1/R2 整改及实际 Full16 等证据后再复审。

## R1 最终整改独立复核（2026-11-21 10:21）

**R1 已关闭，限本轮源码与 CPU 证据范围。P2 整阶段仍 PENDING。**
本节取代前文对 R1 仍开放的判断；R2 编辑新像素 overlap 未在本轮复验，
不据 R1 关闭推断 R2 已解决。Full16 GPU 等缺失证据仍保留。

读取交接目录
`C:/Users/PBE1A7~1.ADC/AppData/Local/Temp/vl-rvt-p2-budget-bounded/`
的 `handoff.txt`、`source-sha256.txt`、`shortlease-probe.log`，
但没有以交接中的 exe 或日志代替独立重跑。
直接从当前 worktree 源码重新编译；开跑前及结束后核对三个文件 SHA256，
均与交接一致：

| 文件 | SHA256 |
| --- | --- |
| source/terrain/terrainRvtBudget.h | `df5308fbc4878192ddb8dbd048a1b89cfe08d5933d06bae5048c79ad3400e7f0` |
| source/terrain/terrainRvtBudget.cpp | `f1971230ea68713a79520c6555f093bb94d25804a88366ab7ed67b0d5cff17f3` |
| tool/terrain-tests/terrainRvtBudgetTests.cpp | `a0fa60780951fe1c068526dcd0bd0238771a93c83ea616463c6cff5e7ed111cc` |

### 独立执行结果

| 执行内容 | 独立结果 |
| --- | --- |
| 原 R1 10000 次 shortLease，原 caps、原父子 owner、逐次最后引用释放 | 通过；child/parent intrusive head 都为空，CPU charge 与 metadata 全零 |
| 当前 Budget 标准 suite | **21/21 PASS**，144 ms |
| 当前 Budget + Scheduler + Residency 联合 suite | **77/77 PASS**，34453 ms |
| ConcurrentPhaseReserveAndFinalAliasRelease 重复 100 次 | **100/100 PASS**，命令 exit 0；合计 4000000 次成功 reserve/最后 alias 释放，另有 owner phase 与 alias phase 竞争 |

联合测试的 root integrity mismatch 日志来自既有损坏 payload 用例，
测试总体通过；没有启动 renderer/Vulkan/GPU。
本轮不是 sanitizer、形式化并发证明或 GPU 集成验收。

原白盒 probe 只将旧 vector 的观测字段适配为 intrusive head：
`world.state->charges == nullptr`、`peak->state->charges == nullptr`，
保留 10000 次相同 reserve/release 负载；metadata 通过公开 Statistics 读取。
独立输出：

```text
liveAllocations=0 cpuCharged=0 worldRegistryEmpty=1 worldMetadata=0 peakRegistryEmpty=1 peakMetadata=0 chargeBytes=160
```

独立输出文件均在系统 TEMP：
`vl-rvt-p2-r1-final-independent.exe`、
`vl-rvt-p2-r1-final-short-independent.exe`。
构建使用前文相同 C++17/O0 CPU 源集合和三个标准测试文件，
不加入早期附加测试，不使用交接 exe，不使用 `build-vt-release`。
运行命令：

```powershell
$auditExe = Join-Path $env:TEMP 'vl-rvt-p2-r1-final-independent.exe'
& $auditExe --gtest_color=no --gtest_brief=1
& $auditExe --gtest_filter=TerrainRvtBudgetTest.* --gtest_color=no --gtest_brief=1
& $auditExe --gtest_color=no `
    --gtest_filter=TerrainRvtBudgetTest.ConcurrentPhaseReserveAndFinalAliasRelease `
    --gtest_repeat=100 --gtest_brief=1
```

### 源码复核依据与边界

- registry 已换为 charge 自身 previous/next 指针，持久 owner 不再保存历史 weak vector；
  成功插入和最终释放摘链均在对应 state mutex 内，最后引用析构时 O(1) 摘链。
  本轮 probe 同时确认 parent 没有累计残留。
- 每 owner 最多 4096 个 active charge；满额拒绝及释放后重新准入由标准测试覆盖，
  不能把这个上限称为新增 GPU 物理槽容量。
- 每级把 256 B metadata 加入 accountedCost 的 CPU total/class/phase；
  向 parent 传递 inclusive accountedCost，parent 再收自身一级 metadata。
  `Cost()` 仍返回请求的 payload cost，这与 Statistics 的 inclusive 口径有意不同。
- allocate_shared 使用受限的 MetadataAllocator，按重绑定后真实分配类型的
  `count*sizeof(Value)` 检查不超过 256 B；这覆盖其请求的 charge/共享控制块 storage，
  不是只用 `sizeof(charge)` 猜测控制块。未将通用堆分配器隐藏 bookkeeping
  宣称为精确测得的字节数。
- reservation 在持 child 锁时向 parent 申请；SetPhase 也沿 child→parent 方向。
  owner SetPhase 在本 owner 锁内遍历 live 链，不再分配 phase 快照；
  不会反向锁 child。最终析构先在本 owner 锁内摘链/扣账，
  退出析构体释放该锁后，成员 parent leases 再析构。
  本轮没有发现这三条路径的锁顺序反转。
- phase promotion 保持单调；owner 后续 reserve 至少采用其当前 phase，
  parent 若已进一步退休，传播不强迫其回退。
  标准 phase/alias/并发测试通过。
- 第二 parent 拒绝时，尚未入账 child charge 不做错误扣账，
  已成功取得的首 parent lease 随 RAII 析构释放；
  10000 次第二 parent 拒绝、metadata total/class/parent cap、CPU 加法防溢出
  均由本轮独立运行的标准测试覆盖。

结论为原 R1 的“历史登记项与控制块无界积累且不收费”已修复，
本轮没有新增可证 Budget 缺陷。不扩大到 R2、Service 实际 Full16、
真实 GPU 旧 epoch/status/取消或跨 World allocation peak 的通过结论。
仅更新本审计文件，未修改实现、Frozen 或标准测试。

## R2 SourceSnapshots 与 Service 接线独立复核（2026-11-21 11:53）

**R2 的 Service 所持 CPU 像素快照重叠收费缺陷已关闭，限下述 CPU 测试及源码接线范围。**
不再沿用早期“仅初始 plan 收费、编辑新像素没有 lease”的判断。
Service 完整动态执行、真实 GPU COW/原子根发布与 Full16 竞争仍未证明，
P2 整阶段保持 **PENDING**。

读取
`artifacts/terrain-rvt-production/r2-clock-metadata-mi-r3-checkpoint.md`、
`r2-clock-metadata-mi-r3-service-handoff.json` 及 pixel worker handoff。
独立重新计算 requiredObjects 两个文件的 SHA256，均匹配交接：

- Service **object**：`08e94dda0ce2fb291a95e71bf96ac60b19102a9d18740ad489e646fb0d171539`。
- SourceSnapshots **object**：`e547861a9a675d652802c244cf60783a66c47c118c7f29027289e122d32310f7`。

对象哈希不冒充源文件哈希。源码读取身份：

| 源文件 | SHA256 |
| --- | --- |
| terrainCompositedService.cpp | `2a9a1213ac2e9d32fffd39f4085dc86bc03f80ce6764bbc42546cecd399e76c3` |
| terrainCompositedSourceSnapshots.h | `dd1e7f2e2aa1d203ff7e360c6023c5d4add12e585447f1ba806b0592d8b4bdc2` |
| terrainCompositedSourceSnapshots.cpp | `587a2bf256dd3d9ee9b9dec1484034b8adbfe2a132f2f8aa59bfc734c5b7bdaa` |
| terrainCompositedSourceSnapshotTests.cpp | `d51a882d76f5bd7e2e44d7c0799d78169a2ca09a3bea5d9de7b33a46d98220fe` |
| terrainCompositedSourceIntegrationTests.cpp | `43667857a7f792c7c2918b598f76de88bfcef9645256ae9253a39b15c71cdf89` |

### 实际独立 CPU 重编与运行

直接编译当前 registry/Budget、真实 TerrainPageManager、WeightMap/Heightmap/LayerPaint、
HostImage 和三个测试 TU。没有使用交接 exe 或预编译 Service object 代替测试。

**28/28 PASS，281 ms**：
SourceSnapshots 15、实际 PageManager alias integration 4、quality clock 9。
最后九项仅重跑交接组合，不据此宣称真实初始 Game deadline/GPU timing 已验收。
新增独立 stdin 用例
`IndependentR2.UnequalCapacityOverlapAndPerLeafFinalRelease` **1/1 PASS**：

- 高度/paint 两组源分别 reserve 1024 和 8192 的容量，size 刻意更小；
  新旧同时活着时精确验证
  `overlapCost == 2*oldCost + newPayloadCapacity - oldPayloadCapacity`。
- 同一 queued 再 Retain 成 candidate 不新增 leaf；四个 leaf 向 parent
  传播的差额为四份 Budget metadata。
- 只保留旧 height 最后 alias 时 live leaf 从 4 依次到 3、2；
  新 candidate 释放后 child/parent current 和 registry 均归零。

标准 suite 还实际覆盖：相同地址跨 placement/queue 去重、
等内容不同对象分开计费、单独 height/paint 替换、父拒绝及第二输入失败回滚、
最后 alias 跨 registry/owner facade 寿命、phase 单调、并发复用/最终释放、
**10000 次短编辑后 registry/child/steady/outer 无残留**。
真实 PageManager 测试覆盖首次准备 alias 释放后仍收费、新旧候选重叠、
相同像素地址但原控制块未收费时重新绑定收费 alias、反复替换只留当前源。

独立输出为 TEMP 下
`vl-rvt-p2-r2-independent.exe` 和 `vl-rvt-p2-r2-capacity-independent.exe`。
28 项构建/运行可从 worktree 根目录重放：

```powershell
$r2Exe = Join-Path $env:TEMP 'vl-rvt-p2-r2-independent.exe'
$r2Sources = @(
    'source/terrain/terrainRvtBudget.cpp'
    'source/terrain/terrainCompositedSourceSnapshots.cpp'
    'source/terrain/terrainPageManager.cpp'
    'source/terrain/terrainWeightMap.cpp'
    'source/terrain/terrainHeightmap.cpp'
    'source/terrain/terrainLayerPaint.cpp'
    'source/resource/image/hostImage.cpp'
    'tool/terrain-tests/terrainCompositedSourceSnapshotTests.cpp'
    'tool/terrain-tests/terrainCompositedSourceIntegrationTests.cpp'
    'tool/terrain-tests/terrainCompositedQualityClockTests.cpp'
)
& g++ -std=c++17 -O0 -pthread -DNOMINMAX -ffunction-sections -fdata-sections `
    -Isource -Iextern/Eigen3 -Iextern/Json/include `
    -Ibuild/_deps/googletest-src/googletest/include `
    -Ibuild/_deps/googletest-src/googletest @r2Sources `
    build/_deps/googletest-src/googletest/src/gtest-all.cc `
    build/_deps/googletest-src/googletest/src/gtest_main.cc `
    '-Wl,--gc-sections' -o $r2Exe
if ($LASTEXITCODE -ne 0) { throw 'Independent R2 CPU build failed' }
& $r2Exe --gtest_color=no --gtest_brief=1
```

### Service 实际消费链：逐路径源码确认

这部分是 Service 源码审查，**没有把 registry/PageManager 测试写成完整 Service 动态测试**。
当前公开 Service 的初始化/注册依赖 renderer、PipelineFactory 及 GPU runtime；
本轮独立 CPU exe 不链接或构造该完整服务，也未伪造 backend 引用。

1. **首次接管**：PrepareWorld 永久 plan lump 只计 descriptor/容器，已移除 pixels；
   `preparedSourcePlans` 通过 registry.Retain 接管初始源。
   RegisterSurface 再 Retain 同对象，BindPlanSources 把 alias 交给真实 PageManager，
   surface 入表后擦除该 identity 的 preparation-only 引用。
2. **编辑入队**：RefreshPlacement 在替换 updates 前 Retain；
   reserve 失败不覆盖旧队列。QueueRefresh 在修改旧 receipt 状态前先 Retain 新输入；
   成功后才把同 surface 的旧未终态 receipt 标 stale 并释放其 inputs。
   新输入失败只给新 receipt Failed，不替换原 receipt 的输入。
3. **候选与消费**：Candidate 复制 `surface.plan` 的收费 alias；
   ConsumeUpdates 的 next 来自收费 update/refresh plan，写入 candidate.plan。
   PublishCandidate 先 BindPlanSources，再 Promote 至 resourcePhase，最后 move candidate；
   PageManager 两个 Update 函数实际覆盖 shared_ptr，即使 raw 像素地址相同也不跳过，
   因而可把原未收费控制块改绑到收费 alias。
4. **最后引用**：每 leaf 只持自身源对象、registry state 和租约，不持整个 Surface/World。
   最后 leaf 析构在 registry 锁内摘链，释放其 source 和 lease；
   父账通过同一租约寿命回收。retained aliases 不复制像素数组。
5. **终态 4096 receipt ledger**：RefreshCommand 唯一的源快照引用为 inputs；
   QueueRefresh 对入站 stale/failed、被新编辑 supersede 的旧 receipt 直接 reset；
   PrepareRefreshes 异常 Failed reset；
   UpdateRefreshes 对已有 Terminal 以及新 Published 都 reset。
   `changes/affected/status` 保存标识/矩形/诊断，不含 HostImage/paint 强引用。
   因此终态 ledger 并不因保留 receipt 而保留其旧 pixels。
   原 4096 条满账拒绝仍存在；本轮未把它误写为可无限编辑或自动回收 receipt。
   update/candidate 若仍在实际消费某版本可继续持其 alias，不等同于终态 receipt 泄漏。

SourceSnapshots quote 使用 `HostImage::data.capacity()`、
`weights.capacity()*sizeof(float)`、`opacity.capacity()`；
leaf/control-block 预留外加现有 Budget 每级 metadata。
同对象按 `(source.get(), kind)` 查找，内容相等不作为去重条件；
height 成功、paint 失败的临时 leaf 通过 RAII 回滚，不遗留 registry entry。
本轮未发现 R2 所审范围的新可证缺陷。

### 未关闭的边界

本次关闭的是 **Service 接管的不可变 CPU height/paint 快照及其队列/候选/PageManager
alias 重叠收费**。不据此证明所有外部编辑器/物理系统的原始源 owner 已统一收费，
也不宣称整个 terrain CPU allocation 清单已经最终验收。
receipt 各终态分支本轮为源码证据，没有运行完整 Service 的 4096 终态注入测试。

GPU per-image COW leaf、实际旧输入读者与 epoch 保活、候选根 status/fence、
原子页表切换/发布失败回滚，以及实际 Full16 竞争/fairness/跨 World peak
仍为 pending；不能用 28+1 CPU PASS 替代这些证据。
本轮仅更新指定 P2 audit，未改实现/测试/Frozen，未执行 GPU/Cook/Game，
未使用 `build-vt-release`，未重做 R1 API 审计。

## R9 Runtime/Residency DTO 与真实 slot watch 独立复核（2026-11-21）

**有限结论：本轮所审诊断/watch CPU 子项通过，未发现新增可证实现缺陷；
P2 整阶段仍 PENDING，未提供 GPU/真实 controller capability 通过结论。**
读取 `artifacts/terrain-rvt-production/r9-runtime-residency-owner-handoff.md`
和 `r9-owner-runtime-diagnostics-r2/receipt.json`。
未修改生产源码、正式测试、Frozen 或阈值，未运行 GPU/Game/Cook，
未使用 `build-vt-release`，未执行会读取该目录的交接构建脚本。

### 冻结身份与独立执行

四文件及 Budget、测试入口复制到系统 TEMP
`C:/Users/PBE1A7~1.ADC/AppData/Local/Temp/vl-rvt-r9-owner-independent/`。
实际编译冻结 Runtime/Residency/Budget 实现，
使用既有 private fake backend/Texture CPU边界，以及当前VT/Store/Production/
Adjacency/HostImage/contentHash/atomicFile与真实RuntimeFileTrace支持源码。
首轮链接漏加atomicFile新依赖RuntimeFileTrace而失败；
补入其真实cpp后重编成功，没有把该缺少支持TU的链接结果判成诊断模块缺陷。
未直接运行交接exe。

运行前后四文件SHA256与交接完全一致：

| 文件 | SHA256 |
| --- | --- |
| terrainCompositedRuntime.h | `762c7c60707aa014c1f402b4c099e368805991e70a975673d4502839619fd74a` |
| terrainCompositedRuntime.cpp | `ac9bd09b34f65b5c9f92018525f26a03d0bbcda3d55140f82f29cebca1d95867` |
| terrainCompositedResidency.h | `a8f7e3627ce233a1f0b098c94fab7d9e823a80e082b92fb98e0adfbf4bd27c8f` |
| terrainCompositedResidency.cpp | `507ba6612d0026d873526526a95865f443c138b32cfac1253e932f482dde4761` |

CPU输出 `independent-tests.exe`：

- 原定向7项加独立watch生命周期1项：**8/8 PASS，943 ms**。
- 原扩展23项加同一独立用例：**24/24 PASS，931 ms**。
- 两次运行有交集，不能相加为32个唯一测试。
- 扩展filter沿交接明确排除
  `NativeSlotDiagnosticsKeepOldDescriptorOwnerUntilLastReference`；
  该旧测试假设仅释放外部oldBinding就能清除Runtime自身Surface强引用，
  与真实CollectRetired边界不符。没有修改生产调度或该测试使其“通过”，
  也没有把filtered结果写成全部原始注册测试无条件通过。
  新R9RuntimeCollectRetired用例实际覆盖正确的释放边界。

定向入口包含WorldCandidateProgressTests，
后者包含RuntimeCandidateTests，只编译这个合并入口一次，
未重复链接其包含的测试TU。新独立用例经stdin加入，没有写正式测试文件。
本轮未独立重做O3原生对象编译；交接O3收据为实现者证据。

复跑命令：

```powershell
$auditExe = Join-Path $env:TEMP 'vl-rvt-r9-owner-independent/independent-tests.exe'
& $auditExe --gtest_color=no `
    '--gtest_filter=*R9*:*NativeFailureDiagnostics*:*Full16Selected216*:*IndependentWatch*'
& $auditExe --gtest_color=no --gtest_brief=1 `
    '--gtest_filter=-*NativeSlotDiagnosticsKeepOldDescriptorOwnerUntilLastReference'
```

### DTO 的只读性与实际字段口径

源码确认 ProductionDiagnostics/CandidateDiagnostics/SlotDiagnostics只查现有map/set/slot，
不调用CandidateReady、不修改candidateCompletedEpoch、不PollReads或启动任务。
CandidateDiagnostics直接遍历已有requiredPages或当前selection+root，
没有为统计构建中间vector；禁分配查询用例通过，
查询后CandidateBinding仍保持尚未调用准备流程时的readiness状态。

attempt、schedulingTick、retryAfterTick来自现有attempts/clock/retryAfter，
查询较大的completedEpoch不会推进调度时钟或读失败次数。
tokenCurrent及slotTokenMatches分别表达当前身份和实际槽reserved/published身份；
namespace surfaceId参与匹配，不用logicalId猜测槽身份。
同页键暂存多个旧/新reservation时，page视图不能替代实际
`SlotDiagnostics(page.slot, epoch)`与page.reservation核对。

`tablePublishEpoch`为Surface既有publishEpoch（包含upload的实际提交），
不是“最后任意table写入”，不能据此推断removal-only更新或GPU已经完成。
completedEpoch为调用方提供值，不是Runtime自己读取的GPU fence；
controller仍须传真实backend completed epoch。

### watch admission、退休与真实复用

默认pool仅空unique_ptr，无事件队列/ring/后台timer。
未arm时poll返回空；禁分配poll用例通过。
Arm在创建单个watch前reserve CPU Workspace；
已有watch、无效/free槽、预算拒绝都不改变reservation/调度，
第二次Arm拒绝，未累积第二个watch。
watch metadata声明在observation之前，
token字符串先析构，租约后归还。

实际CollectReusable的清槽条件保持：
`Retiring && retireEpoch <= completedEpoch && retirementBinding.expired()`。
armed watch只在这个既有边界读阻塞原因和释放epoch；
ReserveProduction及CPU upload两条分配路径在设置真实reservation之后ObserveReservation。
不为诊断提前清slot、不改token、不注入虚构reservation。
单watch只保存原始slot与一次替换reservation，
`replacementStillPresent`防止后续又被替换时把当前token冒认成首次替换。
它不是完整slot事件历史。

两个独立重跑用例分别验证：

- 两槽池真实占满后，retire/reference epoch和外部descriptor owner都可阻塞复用；
  owner释放且epoch足够后，原物理slot才被另一个真实reservation使用。
- 真实Runtime joint switch之后，RecordUploads安全点调用CollectRetired，
  先释放旧Surface自己的table binding/table lease；
  外部oldBinding仍在时watch记录owner阻塞。
  释放外部binding再经过实际collection，才观察清槽，
  之后ReserveProduction在该slot记录不同reservation及正确新token。
  不是仅在测试中销毁一个虚构weak owner。

### 新增watch回收用例与timeout边界

`IndependentWatchTenThousandCancelsAndDestructionReturnLease`：
发布当前root后，保存owner current；
连续10000次Arm该真实slot再Cancel，每次current都等于baseline；
最后再Arm但不手动Cancel，直接销毁Runtime，
child及peak parent的current均为零。
实际结果PASS，未发现watch本身保活或metadata租约泄漏。

Runtime API没有独立超时钟或自动timeout；
timeout的正确回收依赖controller显式调用CancelSlotReuseDiagnostics。
本轮验证了该Cancel路径及析构fallback，**没有动态执行controller timeout**，
也不把“API有Cancel”扩大为超时接线已经通过。
所有入口仍服从renderer owner串行访问约定；
本轮不宣称允许GT/RT无锁并发查询/Arm。

### 仍缺证据

Service/controller普通统一构建、真实World身份核对、arm/poll/cancel/timeout
capability注册、实际frame/completed-fence见证、Status/SlotReuse/Stale/Submit
GPU验收仍pending。
Full16Selected216 CPU模型通过不代表Full16实际GPU竞争通过。
本次仅关闭诊断/watch所审局部疑点，不升级P2或P4阶段结论。

## R12 私有生产优先级策略有限独立审阅（2026-11-21）

**有限接受排序缺陷的CPU反例及所提优先级/公平性方向；
未发现本轮私有selector有限测试范围的新可证算法缺陷。**
这不是Game/P3/P2整阶段PASS，亦不是当前Service整合后的验收。
Copernicus负责正常source-owned整合与coherent build，
本轮不修改生产代码，不把私有old-object overlay作为最终二进制，
不启动GPU、不使用共享build或`build-vt-release`。

读取`artifacts/terrain-rvt-production/r12-priority-private/PROPOSAL.md`、
`prioritySelector.h`及`run001/final-proposal-receipt.json`。
本节不重做R11实际两机位raw统计；
其中8keys/实际10秒帧数/quality consume耗时仍是既有原始证据的后续GPU复验背景，
不将CPU模拟帧数当墙钟时间。

### 独立重编结果

将当前Runtime/Residency/Budget h/cpp及私有selector、Runtime测试入口复制至
TEMP `C:/Users/PBE1A7~1.ADC/AppData/Local/Temp/vl-rvt-priority-independent/`，
优先包含冻结source和测试目录。
原私有tests.cpp仅在stdin将相对include改指向同一Runtime测试冻结副本；
追加三个独立测试，无生产/正式测试写入。
实际编译真实Runtime/Residency/Budget、VT/Store/Production/Adjacency、
HostImage/contentHash/atomicFile、RuntimeFileTrace及TracingIfstream，
使用既有private CPU backend/Texture边界、GoogleTest源码及BLAKE3静态库。
没有执行会读取共享build的私有build.py。

**9/9 PASS，1763 ms**：原6项全部通过，新增3项全部通过。
输出为TEMP下`tests.exe`。本轮为C++17/O0；
交接nativePriority O3对象属于实现者证据，不冒充本轮重编O3结果。

| 私有selector / 原测试文件 | SHA256 |
| --- | --- |
| prioritySelector.h | `814798a0003ced0b823f20f3a1d206f5edfd5ad9fe21a35f05a72c28b90434d7` |
| tests.cpp | `df6154151b7202d22806f67e7bf16538fdd1ca28ccc5c811bf9d81d05c5dab0f` |

Runtime.cpp、Residency.cpp仍为R9已冻结的
`ac9bd09b34f65b5c9f92018525f26a03d0bbcda3d55140f82f29cebca1d95867` /
`507ba6612d0026d873526526a95865f443c138b32cfac1253e932f482dde4761`。
P0 SHA256仍为
`43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd`，
没有调整冻结slots/caps/quality阈值。

### 实际Runtime CPU反例与通过边界

fixture是一个8192 surface、512槽、free reserve4、
已发布真实root、实际pending 200个mip0和4个mip1。
每模拟frame只完成一个真实Runtime reserve/complete/table Commit。
独立重现：

| 策略 | CPU页表/slot oracle结果 |
| --- | --- |
| 原pending次序 + Service全列表rotation | 60帧0/16384，0个mip1被生产，free仍大于400 |
| 仅mip降序但保留全列表rotation | 60帧8192/16384 |
| root/RR/3:1 quota/周期sweep提案 | 第5帧16384/16384；随后全部204个fine/coarse请求drain |

oracle不是仅断言“先选mip1”：它从真实CPU backend页表读slot，
核对Resident、实际publishedToken、publication epoch，
针对固定四象限16384样本检查target0或parent mip1可解析。
但这些仍是**合成可见样本及CPU lookup**，
不是实际GPU反馈分布、完整shader测量或frozen .99画质验收的镜像替代。

原六项还实际通过：

- 两surface各有coarse覆盖及fine进度；
- root在fine/coarse前，205 required pages的真实candidate最终Ready并joint Commit；
- 连续40次positive-mip新增请求时fine至少10次，
  持续等待的低positive mip1也通过周期sweep得到服务。

3:1、每8次coarse turn sweep是私有policy参数，
不属于P0资源预算，更不证明它们是最优参数。
有限已注册namespace和有限虚拟键空间下，独立class RR、
fine cyclic key bookmark及周期coarse sweep支持有限进度；
本轮没有证明任意无限namespace churn下的统一等待上界。
strict root-first也依赖真实有限World/root请求及先行eligibility门禁。

### 三个独立补充用例

1. `IndependentFailedTaskDoesNotSpendQuotaAndBackoffRemainsReal`：
   重复Pick不Commit，决策不变；
   对真实Runtime选中页Reserve后注入CompleteProduction失败，
   不调用CommitSuccessful。后续ProductionRequests实际过滤该backoff token，
   另一个合格coarse请求仍可选择，attempt=1且backoff>0。
   selector不读取或篡改attempt/retryAfter，不绕过原Residency条件。
2. `IndependentSixteenNamespaceCoarseAndFineQuotaProgress`：
   真实16namespace各一fine一coarse，32个成功dispatch后全部drain，
   每namespace两类均有进度，未见全局quota只服务第一个namespace。
3. `IndependentRejectedPickMustBeFilteredBeforeTryingAnotherRequest`：
   同一列表20次Pick且不Commit始终同一项；
   临时移除失败项后可选另一个不同token。
   这是**集成责任的阴性证据**，不是证明selector自己会跳过未准入项。

### 生产整合必须保留的门禁

`Pick`是只读，但其`states.find(std::string(chosen))`可能临时分配；
`CommitSuccessful`也可能分配namespace map节点/字符串。
本轮没有把它称为noexcept/no-allocation Commit。
它不能被直接放入已经成功GPU提交后的不可失败原子尾部，
也不能在预算/leaf锁持有下任意执行；由Service正常录制/调度边界处理。

未准入任务不能消耗quota/cursor，这是本轮正向确认的性质；
但在同一attempt重复原列表Pick会固定命中同一失败项。
Service必须先过滤inputPending、candidate phase/owner等已知不合格任务，
Reserve/prepare失败后用有界临时排除或等价fallback尝试其它合格任务，
不能每帧只撞同一不可准入token，否则仍可能阻止其它任务进度。
这项责任已在交接中声明，本轮不把尚在接线的Service判最终失败，
也不因私有六项通过就视为已经兑现。

推进state的边界应为实际成功Record；
失败准备、false reservation不推进，Abort语义须与已有Service提交路径对齐。
本轮CPU fixture在真实Complete/table Commit之后推进selector，
没有动态执行Service Record抛异常/Abort，所以该边界仍pending实连。

metadata lease先于map/字符串声明，payload先销毁后归还费用；
namespace状态数按实际poolCapacity有界，失去pending的namespace在成功dispatch后清理，
没有每page永久历史。
1024字节guard需与生产的实际namespace身份契约保持一致，
不能把私有guard/quote当作新增作者限制授权。

### 阶段结论

本轮独立支持“只改降序比较器不足，必须取消破坏优先级的whole-list rotate，
并保留fine与低positive-mip进度”的原理和CPU证据。
当前未发现需立即阻断私有提案的新实证bug；
失败任务fallback、Record/Abort推进、metadata准入及真实namespace身份
仍须检查source-owned Service整合。
下一次普通coherent build/source lock后的实际R13 GPU
必须重跑原机位、原10秒、`.99 target+1`门槛，
不能以5模拟frame或上述synthetic16384样本结果替代。
P2/P3和整个Game验收继续PENDING。
