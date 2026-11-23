# RVT GPU 输入 leaf ownership 独立 CPU 中审

状态：**PENDING**。审阅日期：2026-11-21，Asia/Shanghai。
这是 P4 输入资源所有权 CPU 子项中审，不是 P4 或整个生产方案 PASS。
审阅者未参与实现；未修改实现、测试、预算或 Frozen，未使用 GPU，
未运行 Game/Cook，不使用 `build-vt-release`。
Service World COW 接线开发中，不以尚未完成的接线作最终失败。
本轮不重复 [P2 审计](terrain-rvt-p2-independent-audit.md)。

## 首要发现：预算 owner 同址重建可错误复用旧账 leaf

最新状态：**该缺陷已由末节 Token 整改独立复验关闭**。以下保留初次失败快照。

**可证门禁缺陷，独立 CPU 复现失败。**
位置：`source/render/resource/terrainCompositedProductionGpu.h`
的 `budgetIdentity` 与 `RestoreImages` reuse 判定。
当前只保存 `TerrainRvtBudgetOwner*`，通过 `prior->budgetIdentity == &budget`
判定同一 owner。预算 facade 析构后，leaf lease 可继续保留旧 state；
同址新建 facade 则具有不同账本，却能通过该裸地址比较。

保持同一个非空 WorldScope，使用 `std::optional<TerrainRvtBudgetOwner>`
先构建 owner/旧 snapshot，销毁 owner，再 emplace 同址新 owner，
以旧 snapshot 为 prior 调用 RestoreImages。实测：

```text
oldGpu=197668 newGpu=0 newUploads=0
```

旧叶的分配继续记在旧 state；新 owner 仅收 snapshot metadata，
实际复用的全部 GPU bytes 没有进入新账本。用例两条断言失败：
新 owner 应有 GPU 收费，且跨 owner lifetime 不应零上传复用。
现有 `DifferentWorldOrBudgetAndRetiringPriorCannotShare` 只测不同同时存活地址，
没有覆盖 facade 销毁后的地址复用。

建议交 Hubble：复用身份应绑定实际预算 state 或不可重用的 owner lifetime token，
不能以裸地址代替；修复不应改变 Frozen。
本审计未证明当前 Service 会重建同一个 WorldScope 下的 owner，
因此不扩大为“已观察到真实 Game 串账”；缺陷成立于公开 leaf restore API 的寿命边界。

### 直接纳回归的最小用例

放入 `tool/terrain-tests/terrainCompositedProductionLeafTests.cpp`，
复用当前 fixture 即可。本审计通过 stdin 包含冻结测试副本执行，未写测试文件。

```cpp
TEST_F(TerrainProductionLeaf, IndependentOwnerLifetimeReuseMustNotCrossLedger)
{
    std::optional<VL::TerrainRvtBudgetOwner> facade;
    facade.emplace(LeafCaps());
    auto first = Restore({}, scope, &*facade);
    first->SetPhase(Phase::Active);
    const auto beforeUploads = uploads;
    auto oldAddress = &*facade;
    facade.reset();
    facade.emplace(LeafCaps());
    ASSERT_EQ(oldAddress, &*facade);
    auto second = Restore(first, scope, &*facade);
    EXPECT_GT(facade->Statistics().current.gpuBytes, 0u);
    EXPECT_GT(uploads, beforeUploads);
    second.reset();
    first.reset();
    EXPECT_EQ(facade->Statistics().current, VL::TerrainRvtCost{});
}
```

## 独立构建身份与执行结果

先将 ProductionGpu.h/.cpp、两个测试 TU 及 fixture 复制到 TEMP，
独立编译优先包含冻结 header，避免并行 Service 修改干扰。
源目录与冻结副本的 h/cpp SHA256 在结束时仍一致：

| 文件 | SHA256 |
| --- | --- |
| terrainCompositedProductionGpu.h | `acd7f4044ece515e1c3a554ff696d6519ae1e50e3b1ec5babead2e2aa3a90fd1` |
| terrainCompositedProductionGpu.cpp | `590c88b23fcd0fe84f75582d4263e32a32a002c0813c3f428ab5fcf0e5fe9deb` |
| terrainCompositedProductionLeafTests.cpp 冻结副本 | `cfd726db9e6f1e6a1c464781225057bcec3006a5d3feda98086861e19af331cb` |

临时目录：
`C:/Users/PBE1A7~1.ADC/AppData/Local/Temp/vl-rvt-input-leaf-independent/`。
输出 `independent-tests.exe` 和 `independent-probes.exe`。

- **标准 CPU 45/45 PASS，8001 ms**。
  包含 120 revision chain、共享用途 phase、失败回滚、transfer chunk、
  source-map 与材料 alias 的既有用例。
- 新 owner lifetime probe **FAIL**，如首项所述。
- 新 typed-use metadata observation **PASS（观测用例，不是预算合规断言）**：
  创建 10000 个同时保留的 PhaseUse，`chargedCpuDelta=0`，
  `PhaseUse::MetadataBytes()=168`。
- `*Independent*` filter 也匹配已有
  `ForkedUsePromotesIndependentlyOfRetiringOldRevision`，该项 PASS；
  所以 probe 运行显示总计 3 项、2 PASS/1 FAIL，不能误报为三个新增用例。

构建直接使用 C++17/O0、pthread、function/data sections 和 gc-sections；
两个 CPU 测试 TU，加当前 Production/Adjacency/Budget/VT、
HostImage/contentHash/atomicFile，GoogleTest 源与已有 BLAKE3 静态库。
没有链接 renderer backend 或执行 Vulkan。
主要命令形态（从 worktree 根目录）：

```powershell
$auditRoot = Join-Path $env:TEMP 'vl-rvt-input-leaf-independent'
$cpuSources = @(
    "$auditRoot/tests/terrainCompositedProductionLeafTests.cpp"
    "$auditRoot/tests/terrainCompositedProductionGpuTests.cpp"
    'source/terrain/terrainCompositedProduction.cpp'
    'source/terrain/terrainCompositedAdjacency.cpp'
    'source/terrain/terrainRvtBudget.cpp'
    'source/terrain/terrainCompositedVT.cpp'
    'source/resource/image/hostImage.cpp'
    'source/shader/build/contentHash.cpp'
    'source/shader/build/atomicFile.cpp'
)
& g++ -std=c++17 -O0 -pthread -DNOMINMAX -ffunction-sections -fdata-sections `
    "-I$auditRoot/source" "-I$auditRoot/tests" -Isource -Iextern/Json/include `
    -Iextern/VulkanSDK/include -Iextern/BLAKE3 `
    -Ibuild/_deps/googletest-src/googletest/include `
    -Ibuild/_deps/googletest-src/googletest @cpuSources `
    build/_deps/googletest-src/googletest/src/gtest-all.cc `
    build/_deps/googletest-src/googletest/src/gtest_main.cc `
    build/extern/BLAKE3/libblake3.a '-Wl,--gc-sections' `
    -o "$auditRoot/independent-tests.exe"
if ($LASTEXITCODE -ne 0) { throw 'Independent leaf CPU build failed' }
& "$auditRoot/independent-tests.exe" --gtest_brief=1 --gtest_color=no
```

复现新增 case 时，可在该测试 TU 中纳入上述用例，
或 stdin `#include "terrainCompositedProductionLeafTests.cpp"` 后附加用例，
代替 sources 中两个测试 TU，然后 filter 精确的新测试名。
本轮未把交接 `artifacts/production-leaf-build` 的历史日志当作独立执行结果。

## 源码与通过测试支持的结论

### Restore/Capture 与 raw kind

实际 GPU `Restore` 两个重载仍在 cpp，委托同一个 RestoreImages；
注入 uploader 调用原生 Upload 并记录真实 image/view/sampler。
CaptureGpuImage 保留源 Texture 强引用与显式 transfer lease。
`MaxTransferBytes` 仍为 1 MiB；Capture readback 和 Restore upload 都通过
VisitTransferChunks 循环，在复用 mapped staging 前同步完成该块提交。
两条路径不是改回整张 GPU staging，CPU payload/scratch 仍进 TransferCost。
这是源码保留与 CPU chunk/layout 测试证据，**没有新的实际 GPU roundtrip**。

RestoreImages 仅对 `kind == Image` 准入/恢复纹理；
closure.Validate 对 surface 输入要求对应 Image。
RawAuthorSource(kind=3) 可作为非图像对象校验/写盘，但不会被这两轮图像循环
创建或绑定为采样纹理。标准 CPU 非图像对象测试仍包含 kind3。

### alias 与共享

Image/SourceMap 返回 alias 指向 Leaf，不再 shared_from_this 保整个 closure；
标准测试证实 snapshot weak_ptr 到期、无关图像销毁，
只剩被 alias 引用的那一个 allocation。
生产任务使用 MakeBakeInput 的强 snapshot 仍有意保留完整输入集，
不能将这个显式在途 snapshot 误判为材料 alias 的 wholeclosure 回归。

除首项 owner lifetime 问题外，prior 复用门禁包含：
非空且同控制块 WorldScope、相同预算地址、prior 非 Retiring；
逐叶 reusable 标志、lease 非 Retiring、array/2D view 类型，
objectId/path/kind/bytes/digest、两个 catalog digest，
format/extent/layers/mips/transfer/address/filter。
不同内容、新 sampler/view 类型或被标记修改的叶不复用；
源映射按实际 LUT 内容摘要匹配。新叶先全部准入，再执行读/上传。
标准测试验证旧 prior 在准入/上传失败后仍保有其分配。

### phase 与析构

每个 snapshot Use 和显式 PhaseUse 各自入叶内链；
leaf mutex 内汇总各 live use 的最早 phase，只有全部用途允许时才前进。
Active 共享 use 存在时，旧 revision Retiring 不会把底层 lease 误转 Retiring；
失败 Candidate use 析构不退休仍被 Active 使用的叶。
120 轮 revision 与 forked-use 测试支持这些局部性质。

普通 Texture alias 仅保证分配寿命，不自动注册 Active 用途；
需要用途语义的 World snapshot 必须保留 PhaseUse。
这个区别应在待完成的 Service 接线中验证，不能仅凭 Texture 强引用推断 phase 正确。

Leaf 的 lease 最先声明，UploadedImage 最后声明；
逆序析构使 UploadedImage 中 Texture 在 lease 释放前销毁。
标准 fake Texture deleter 在销毁时仍看到 GPU charge，
最后 alias 释放后收费归零。真实 Vulkan 退休仍依赖上层 epoch 保活。

## metadata 收费范围与接线门禁

snapshot metadata 在 closure 复制和 maps 建立前准入；
新 leaf 实际 image quote 加上 leaf/object 字符串/control-block 预留，
Budget 自身 metadata 另由既有 Budget 机制收费。
标准低 cap 及最后引用测试覆盖其回滚与归还。
这里的 128/256 B 控制块预留是显式估计，未使用受限 allocator 证明任意 STL
实现的精确 allocation 上界，不能称为驱动/堆实际高水位测量。

**PhaseUse::Retain/RetainImageUse/RetainSourceMapUse 自身没有 metadata lease 或数量上限。**
10000 个 live use 的独立观测确认账本不变。
标准 120 轮 leaf-only World 测试通过其测试侧 Revision::metadata
显式收费 `PhaseUse::MetadataBytes()`，因此该测试没有证明 API 内自动收费。
这是上层接线必须兑现的责任：
每份 World/COW/epoch用途容器在分配前 reserve 足够的 Use/control-block/map metadata，
并随最后用途保活；不能只有调用 MetadataBytes 而没有实际租约。
当前不因 Euclid 接线尚未完成判最终失败，也不将 metadata 完整收费标为已验收。

## 本轮结论

本轮给出一个可直接分派的 owner lifetime 复用门禁缺陷。
per-image alias、局部 phase 汇总、明确的析构顺序、失败回滚及既有 1 MiB
transfer 路径有正向源码/CPU证据；不能升级为 P4 PASS。
待办为修复并复验 owner identity、验证 Service World COW 的实际使用和 metadata 租约，
以及由 GPU 执行者提供新的 leaf roundtrip、真实旧帧/epoch/COW/原子发布证据。
整阶段 Full16 竞争等证据仍 pending。

文档门禁执行：`py -3 tool/validate-docs.py` exit 1，
唯一报错为 `documents/routing.md:52` 引用的
`source/render/pass/terrainQualityCapturePass.cpp` 当前不存在。
这是本轮未修改的路由/实现路径，不代修；本次新增文档的相对链接检查未报错。

## Token 整改独立复验（2026-11-21 12:45）

**原 budget facade 同址重建跨账复用缺陷已关闭，限本节源码与 CPU 证据。**
不再将前文负例作为当前开放缺陷。
P4 及完整 GPU-input ownership 仍为 **PENDING**：
Service PhaseUse metadata 真实 reserve/字段析构顺序仍在接线整改，
GPU 新 leaf roundtrip、真实 COW/epoch/原子发布证据仍缺。
本轮不代审或修改尚在变化的 Runtime/Service，也未扩大为 Budget phase-batch 验收。

### 独立源码身份与构建

先将当前 ProductionGpu.h/.cpp、Budget.h/.cpp、两个 CPU 测试 TU 和 fixture
复制到 TEMP 的 `vl-rvt-input-token-independent/`，
编译时优先包含冻结的 source/test 目录。
Budget 只使用本轮冻结的已落地 Token 接口，不等待后续 phase-batch 修改。
未以 `artifacts/production-leaf-build/accounting-domain-tests.xml`
或其 exe 代替独立运行。

| 冻结文件 | SHA256 |
| --- | --- |
| terrainCompositedProductionGpu.h | `2324f88c823418b3c5d3362dc7df9b3be4eac082fc45b6f4bd801bf5938c882c` |
| terrainRvtBudget.h | `693169667da37d3979cf34d73d7b2c0fa6a661c3ca4350c0358b3483e7dbf5cb` |
| terrainRvtBudget.cpp | `81b129e5d7532c695345c43a5d230980e45c385d2ccd0428e3b9cc0f6d9fb59c` |
| terrainCompositedProductionLeafTests.cpp | `9f1f06ee284129b4e5c55cf2f2e0e1de731112c224c924c69937b5baa188b4eb` |

结束时前三个当前源文件与上述冻结值一致。
采用前文 CPU 构建源集合、C++17/O0，
将 Budget.cpp 也改为冻结副本，输出
`<TEMP>/vl-rvt-input-token-independent/independent-tests.exe`。
另以当前冻结头单独 `g++ -c` 编译 ProductionGpu.cpp，
输出 `independent-production.obj`，exit 0，无 GPU 启动。

### 实际执行结果

- **标准 CPU 50/50 PASS，9456 ms**。
- 对以下六项再以精确 filter 单独重跑，**6/6 PASS，507 ms**：
  同 owner 正向复用、原同址重建负例、
  accounting identity 的 facade/state 寿命、
  相同指针但不同 WorldScope 控制块、
  不同 alias 指针但同 WorldScope 控制块、
  无控制块 WorldScope alias。
- 原负例已纳入 `terrainCompositedProductionLeafTests.cpp:216`。
  本轮实际执行的是强化断言版本：
  同址新 owner 的 `current.gpuBytes == oldGpu`，
  uploads 增加，albedo allocationIdentity 不同；
  释放新旧 snapshots 后新账 current 全零。
  因此不是仅断言“新账非零”掩盖部分漏计。
- 同 owner/scope 正向例仍断言 reads、uploads、imageQueries 全不增加，
  image/view/sampler/allocationIdentity/actualCost 都相同；
  仅新增 snapshot metadata 收费。

精确复跑命令：

```powershell
$auditExe = Join-Path $env:TEMP 'vl-rvt-input-token-independent/independent-tests.exe'
& $auditExe --gtest_brief=1 --gtest_color=no
$cases = @(
    'SameVerifiedInputReusesActualAllocationWithoutReadQueryOrUpload'
    'IndependentOwnerLifetimeReuseMustNotCrossLedger'
    'AccountingIdentityFollowsLeaseStateRatherThanFacadeLifetime'
    'WorldScopesWithEqualPointersButDifferentControlBlocksCannotShare'
    'WorldScopeAliasPointersShareOnlyTheirCommonControlBlock'
    'NonOwningWorldScopeAliasesCannotEstablishSharedDomain'
)
$filter = ($cases | ForEach-Object { 'TerrainProductionLeaf.' + $_ }) -join ':'
& $auditExe "--gtest_filter=$filter" --gtest_color=no
```

### 门禁与寿命源码复核

`TerrainRvtBudgetOwner::IdentityToken()` 返回
`std::shared_ptr<const void>(state, state.get())`，
共享真实 BudgetState 控制块，不以 facade 地址或额外临时 token 猜测身份。
Production snapshot 强持该 token，leaf lease 也持有原账 state；
facade 销毁不会把旧 token 变成新 owner 的身份。
同址重建会产生另一个 state 控制块，双向 owner_before 比较不再相等。

WorldScope 与 Budget identity 两侧均要求 `use_count()>0`，
然后双向 owner_before 判断相同控制块：

- 非空 raw pointer、但由 empty shared_ptr alias 得到的 WorldScope，
  即使 `operator bool` 为 true，use_count 仍为 0，不能建立共享域。
- 相同 get()、不同控制块不共享。
- 同一控制块的不同 alias get() 可共享，这与所有权域而非指针值一致。

寿命测试实际验证：facade/snapshot 销毁后，仅剩单个 material leaf alias 时，
BudgetState 的 weak token 尚未过期；
最后材料 alias 释放后 token 过期。没有静态 token 或整个历史 closure
造成此测试下的永久保活。

本轮没有发现该修复范围的新可证缺陷。
之前 PhaseUse API 依赖外层 metadata reserve 的观察仍有效；
用户已说明的 Service metadata 字段末声明导致提前返费问题属于正在整改的接线，
本轮不以 leaf 的 50 PASS 宣称其已关闭。
仅更新本文件，未修改实现、测试、预算、Frozen，未使用 GPU 或 `build-vt-release`。

本次文档门禁 exit 1：routing.md:52 的
`source/render/pass/terrainQualityCapturePass.cpp` 及 routing.md:56 的
`shader/glsl/terrain/terrainCompositedBake.comp` 当前均不存在。
两处不在本次写入范围；未代修，审计文档链接未报错。
