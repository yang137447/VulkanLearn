# RVT P4 编辑事务独立 CPU 中审

状态：**PENDING**。日期：2026-11-21，Asia/Shanghai。
范围为稳定 Budget PhaseBatch、Runtime candidate joint switch、
ProductionGpu PhaseTransitionTicket。
审阅者未参与实现，仅写本文件，未修改源码/测试/预算/Frozen，未启动 GPU，
未使用 `build-vt-release`。
Service JointSwitch 实际整合仍开发中，本轮不以其编译中间态判最终失败。
本记录不重复 [输入 leaf 审计](terrain-rvt-input-ownership-independent-audit.md)。

## 可立即分派：Runtime false 后重新 Prepare 忽略新聚合 phase

最新状态：**该重试缺陷已由末节 Runtime R3 独立复验关闭**。以下保留原始失败快照。

**独立 CPU 已复现，建议交 Herschel 与 Euclid 对齐重试契约。**
位置：`source/render/terrain/terrainCompositedRuntime.cpp`
的 PrepareJointSwitch / CommitCandidate。

当前逻辑：

- PrepareJointSwitch 在 `jointSwitchPrepared` 为 true 时直接返回 true，
  不读取新的 additionalPhaseChanges。
- CommitCandidate 的 batch TryCommit 返回 false 时，不清除 prepared batch 或 prepared 标记。
- 所以旧聚合 target 过期后，即使调用方重新计算 leaf ticket 并传入新 changes，
  Runtime 仍缓存旧 batch，第二次提交继续使用旧 target。

独立用例按真实 Runtime CPU fixture 执行：

1. 当前 root 发布完成，owner Active，准备并发布候选 root（completedEpoch=4）。
2. 附加 input lease 的当前 phase 为 Active，PrepareJointSwitch 追加 Active target。
3. 模拟两次尝试之间的 phase 变化：附加 lease 前进到 Retiring。
4. 首次 CommitCandidate 返回 false，旧 active page table 保持原值。
5. 重新 PrepareJointSwitch，追加刷新后的 Retiring target；返回 true。
6. 第二次 CommitCandidate **仍返回 false**，新 target 没有进入 batch。

这不是要求回退 Retiring lease；新提交目标也是 Retiring，重新收集 batch 本应可行。
也不是把 Service 开发中行为当缺陷：当前公开 Runtime API 已能独立复现。
仅锁竞争、phase 未变时重用旧 batch 的既有测试通过，不能覆盖本例。
当前 CancelCandidate 会取消并退休候选，不能等价于“保留已就绪候选，仅重建提交 batch”。

建议：定义可重建的 prepared transaction 生命周期，失败后或显式重新 Prepare 时，
允许丢弃旧聚合 batch、保留 candidate pages 与 old current；
下一次从最新 leaf changes 重建同一个原子 budget batch。
重建及 MI 预绑定可在可抛异常的 Prepare 阶段完成；
CommitCandidate 继续保持 noexcept 和失败零切换。
本审阅不代修。

### 最小回归源码

可加入 `tool/terrain-tests/terrainCompositedRuntimeCandidateTests.cpp`
现有 fixture 所在的 `VL` 匿名 namespace。本轮通过 stdin 包含该测试冻结副本，
附加下列用例，未修改正式测试。

```cpp
TEST_F(TerrainCompositedRuntimeCandidateTest,
    IndependentReprepareAfterStaleAggregateMustRebuildBatch)
{
    PublishCurrent();
    owner->SetPhase(TerrainRvtResourcePhase::Active);
    auto extra = owner->TryReserve({100, 0, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Candidate);
    ASSERT_TRUE(extra);
    BeginCandidate();
    Produce(CandidateRequest(), 3);
    Record(3);
    runtime->Commit(4);
    auto oldTable = runtime->Binding("island").compositedPageTable;
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Active}}));
    extra->SetPhase(TerrainRvtResourcePhase::Retiring);
    EXPECT_FALSE(runtime->CommitCandidate());
    EXPECT_EQ(runtime->Binding("island").compositedPageTable, oldTable);
    ASSERT_TRUE(runtime->PrepareJointSwitch(4,
        {{*extra, TerrainRvtResourcePhase::Retiring}}));
    EXPECT_TRUE(runtime->CommitCandidate())
        << "reprepare ignored refreshed aggregate phase changes";
}
```

## 冻结身份及独立执行

先复制稳定 Budget.h/.cpp、Runtime.h/.cpp、ProductionGpu.h、相关测试到 TEMP：
`C:/Users/PBE1A7~1.ADC/AppData/Local/Temp/vl-rvt-p4-transaction-independent/`。
编译优先包含冻结目录，不依赖 Service 并行修改。

| 冻结文件 | SHA256 |
| --- | --- |
| terrainCompositedRuntime.cpp | `0e2c37e24bc9bc8366e068fccb0b8b04c6b17f6765b07dcb4ac573ff54777643` |
| terrainCompositedProductionGpu.h | `fbaaa13cca70b8bf7dfdcf24d798a159d4cdb478c51447c77734a4c53daa13c5` |
| terrainRvtBudget.cpp | `15e02bac0c290a49d0142a166cf106fc0a01dd82620e47594f99af2db18e7776` |

独立结果：

- Runtime + Budget + Residency 的 **79 项既有测试 PASS**。
  加上本轮新 regression 后总计 **80 项，79 PASS / 1 FAIL，28852 ms**；
  唯一失败是上述重新 Prepare 后 Commit 仍 false。
  未复跑交接中其余组合，因此不将实现者的 90 项计为本轮独立结果。
- ProductionGpu/leaf **62/62 PASS，8918 ms**。
  其中包含 ticket取消、锁竞争、并发 use 更新/销毁、
  Finalize在已锁预算state下执行不二次进入预算，以及共享用途 phase 汇总。
- 两个 exe 为 TEMP 下 `runtime-independent.exe`、
  `leaf-ticket-independent.exe`；不是直接执行交接二进制。

Runtime 编译真实 Runtime.cpp/Budget.cpp/Residency.cpp，
仅 backend/Texture 使用已有
`tool/terrain-tests/terrain-composited-runtime-private/` CPU double，
另链接实际 VT/Store/Production/Adjacency/HostImage/contentHash/atomicFile。
leaf suite 编译既有两个 CPU 测试 TU 和对应 CPU modules。
均为 g++ C++17/O0、pthread、`VL_FILE_IO_TEST_HOOKS`、
function/data sections、gc-sections、既有 BLAKE3 静态库和 GoogleTest 源。
测试替身不提交 Vulkan 队列；不能当作 GPU fence/barrier 实测。

精确复跑新负例：

```powershell
$auditExe = Join-Path $env:TEMP 'vl-rvt-p4-transaction-independent/runtime-independent.exe'
& $auditExe --gtest_color=no `
    --gtest_filter=TerrainCompositedRuntimeCandidateTest.IndependentReprepareAfterStaleAggregateMustRebuildBatch
```

## 已有正向证据及边界

### Prepare / Cancel / Commit

Budget PhaseBatch Prepare 收集与去重目标及父 charge，检查回退，
准入 metadata，创建 defer-lock 集合；不提前变更资源 phase。
TryCommit 以 try_lock 取得各 state，任何竞争或注入异常都先释放已取锁再 false；
全部条目验证后才统一修改统计/phase。
本轮标准测试覆盖两个 parent、取消、失败、并发ticket及零部分phase修改。

Runtime Prepare 仅建立下次 namespaces、retirement list 和同一个 budget batch；
CancelAfterPrepare 用例证实旧 table/old Active phase 不变。
成功 Commit 的预算批次先整体完成，然后执行预检过的
RetireAllNoThrow、maps swap和容器清理；既有 allocation-failure/no-allocation
测试通过。false路径保持旧 binding/table phase，未看到局部 map swap。
本轮新发现针对后续重建，不推翻首次 false 的零切换证据。

Service 的 inputs/MI 是否在 Runtime false 后仍是 old current，
以及“MI预绑定→false恢复→成功才改current”的实际接线，
不在这些 Runtime double 测试中；仍 pending 实连验证。

### leaf ticket 与锁

PrepareTransitions 不持 leaf lock 跨阶段；
TryLockLeavesNoThrow 按 allocationIdentity 顺序尝试锁，
失败/Cancel释放已取锁并清空本次 changes。
Finalize只写 local Use phase与phaseFloor，然后解锁；
没有再次reserve或调用 Budget lease SetPhase。
与已经合并执行的 Budget PhaseBatch 配合，避免二次收费/二次budget提交。

共享 leaf 目标由所有 live Use 加上待切换用途汇总；
新 Active use仍存在时，旧 use Retiring不会把共享资源退成Retiring。
真实被替换 leaf 可Retiring但由epoch Texture alias继续保活。
标准共享/失败候选/并发保活用例通过。

成功TryLock后的ticket确实持leaf锁，API不能自动判断“帧结束”；
调用方必须在当次尝试内Finalize或Cancel，false返回前不得把锁带到下帧。
62项测试证明局部Cancel/析构解锁，不证明Service每条return路径都已做到。

### metadata寿命

Budget PhaseBatch::Impl 的 metadata 声明在entries/states/locks之前；
leaf PhaseTransitionTicket 的 metadata也声明在所有transitions/leaves/locks之前。
逆序析构使锁先释放、强引用及容器随后销毁、metadata lease最后归还。
叶ticket默认move构造后锁所有权随vector迁移；
既有“moved locked ticket / 最后Use由ticket保活”用例通过。
本轮未发现这两个ticket容器的“metadata先返费、容器后销毁”字段顺序缺陷。
这不涵盖另由Euclid维护的Service transaction metadata容器。

## 阶段结论

本轮提供一个可证Runtime重试批次过期缺陷，需修复后独立复验。
未发现新的可证deadlock或首次Commit部分swap；不把未证情形泛化为已经安全。
P4仍须实际Editor跨surface编辑、undo、source-update fault、
取消/旧epoch/原子发布证据。当前无这些GPU证据，整P4不得PASS。

## Runtime R3 修复独立复验（2026-11-21 13:34）

**原“false 后重新 Prepare 仍使用旧聚合 batch”的缺陷已关闭，限本轮源码与 CPU 证据。**
本节取代前文对此缺陷仍开放的判断。
P4 仍 **PENDING**：Service 实际 MI/input 联合恢复与切换、
Editor 跨 surface/undo/source-update fault、GPU epoch/COW 仍未验收。

### 冻结身份与重编

复制当前 Runtime.h/.cpp、Budget.h/.cpp、三个 CPU 测试 TU 与 fixture 至
`C:/Users/PBE1A7~1.ADC/AppData/Local/Temp/vl-rvt-p4-runtime-r3-independent/`，
独立编译优先包含冻结文件。
使用真实 Runtime.cpp 和已有 fake backend/Texture，未链接旧 Service 对象。
没有执行交接 `artifacts/p4-runtime-reprepare-cpu/runtime-reprepare.xml` 对应旧 exe
来代替本次重编。

| 冻结文件 | SHA256 |
| --- | --- |
| terrainCompositedRuntime.h | `33f57b8a8c2cf07faeb550097b8b2bba4b3ea638ce555c8cd4365f36fd4b258a` |
| terrainCompositedRuntime.cpp | `f3e147af9107c98c4591d04fcb025b846fdcf3b33e0e1af78d476faa48984c96` |
| terrainRvtBudget.cpp | `15e02bac0c290a49d0142a166cf106fc0a01dd82620e47594f99af2db18e7776` |
| terrainCompositedRuntimeCandidateTests.cpp | `47b06d3d86a62e53ad604d0269583ef341e86c1bcc1f3df3f444199cb9bf1655` |

结束前重新计算当前 Runtime h/cpp 和 candidate tests，与冻结值一致。
构建源集合与前轮 Runtime CPU 组合相同，C++17/O0、pthread、
`VL_FILE_IO_TEST_HOOKS`，不使用 GPU 或 `build-vt-release`。

### 独立执行结果

**84/84 PASS，28800 ms**：
当前83项标准组合（原79项及R3四项，其中包括已纳入正式测试的原独立负例），
另加本轮1项10000次失败压力回归。
输出为 TEMP 下 `runtime-r3-independent.exe`。
没有把原独立负例重复计为第五个R3新增用例。

原负例现在实际断言：

- 第一次过期phase Commit false 后 prepared 状态已清；
- old table 不变、candidate table 不变，CandidateReady仍为true；
- 传新 additionalPhaseChanges 再Prepare后 Commit成功；
- current binding等于原先已准备好的candidate table。

其余R3新增测试覆盖：

- 显式CancelPreparedJointSwitch在禁分配下重复调用，
  old Active/current resident及candidate roots/binding都保留，可再次Prepare/Commit；
- 尚未Commit时重复Prepare确实采用新的additional changes；
- 重新Prepare遇无效target异常，旧prepared metadata释放，
  candidate保持就绪且可再次成功提交。

原锁竞争测试仍实际持peak parent mutex，
在释放blocker之前调用CommitCandidate并获得false，
证明该场景下false返回不等待metadata lease析构去重新取得同一parent锁；
随后phase/current binding未改变。
成功commit及显式cancel的既有no-allocation用例均通过。

### 新增10000次压力回归：延迟回收有界

本轮经stdin包含冻结Runtime candidate测试TU，附加
`IndependentDeferredTicketBoundedAcrossTenThousandFailedAttempts`，未改正式测试：

1. 发布当前root并令owner Active，生产并发布候选root，保存old/candidate table及baseline。
2. 每轮PrepareJointSwitch，注入phase batch首次try-lock失败，
   同时将runtime allocation countdown置零，
   调用CommitCandidate及幂等CancelPreparedJointSwitch。
3. 每轮断言Commit false、prepared状态为空、old/candidate table身份未变，
   candidate owner的CPU收费等于第一轮失败后的固定值。
4. 10000轮之后通过正常RecordUploads安全点回收，再Abort空录制；
   断言candidate owner current恢复完整baseline、候选仍ready。
5. 再次Prepare/Commit成功，current table切为原candidate table。

实测：

```text
attempts=10000 baselineCpu=8388864 retainedCpu=8390256 singleTicketCpuDelta=1392
```

该1392 B为本fixture中child owner统计的单票据inclusive CPU增量，
不是任意World ticket固定大小。失败现场保留这一份收费是有意延期，
不是要求当场返费；10000轮未累计更多ticket，
安全点回收后收费恢复baseline。
本轮没有把禁分配注入测试称为真实GPU提交失败。

复跑本轮exe：

```powershell
$auditExe = Join-Path $env:TEMP 'vl-rvt-p4-runtime-r3-independent/runtime-r3-independent.exe'
& $auditExe --gtest_color=no --gtest_brief=1
& $auditExe --gtest_color=no `
    --gtest_filter=TerrainCompositedRuntimeCandidateTest.IndependentReprepareAfterStaleAggregateMustRebuildBatch
& $auditExe --gtest_color=no `
    --gtest_filter=TerrainCompositedRuntimeCandidateTest.IndependentDeferredTicketBoundedAcrossTenThousandFailedAttempts
```

### 修复机制源码核对与关闭边界

PrepareJointSwitch每次先CancelPreparedJointSwitch并在Prepare安全阶段
回收abandoned票据，再从最新candidate和additionalPhaseChanges创建新budget batch，
不再因旧prepared标记直接返回true。
CommitCandidate任一false路径调用CancelPreparedJointSwitch。
该noexcept helper清空prepared maps/list/flag，
把旧batch移动到唯一abandoned槽；不取消candidate namespace、不退休candidate pages，
不在失败现场析构仍持metadata lease的旧batch。

下一次Prepare或CollectRetired回收abandoned槽；
正常状态路径在创建新prepared前已清该槽，
所以不存在每次false积累一份历史票据的容器。
移入abandoned后原optional仅剩moved-from batch，
reset不触发旧metadata parent扣账锁。
本轮锁竞争和压力测试与这个机制一致。

本轮未发现修复范围内的新可证race/deadlock或partial swap。
本次不重新评价稳定Budget/LeafAPI，也不对Service新private布局做旧对象链接。
仅更新本事务审计，不修改实现/测试/预算/Frozen，不提供GPU通过结论。

## E006 Coverage / E007-local 实际单次 Paint→Undo 独立验收（2026-11-21）

**有限实际子项 PASS：两种权重模式各一次真实 Paint→Undo 的发布、
World descriptor revision推进、当前输入恢复及完成采样身份核对通过。**
本结论来自既有实际运行原始证据的独立只读复算，
本审阅者没有重跑 GPU，也未参与 Service author fix / Local authored identity fix。
**整 P4 仍 PENDING**：本节不是 full27、Save、Shader、World reload、
worker1 或 lit visual/performance gate 验收。
没有接触 Peirce 正在执行的 full27 路线或共享构建。

### 收据身份与 ABI 闭包

| 对象（均相对 worktree 父目录） | 实际 SHA256 |
| --- | --- |
| p4e/E006/evidence/e006-final-receipt.json | `1431a69e52f0372dbba63caf9f583f24a41c5464725e076c141d2bb10c5cac2e` |
| p4e/e006-final-checkpoint.json | `842b16cdc237ac14adcb15ff40c1c902983a209c8fb071f61271ea0852354982` |
| p4e/E007-local/evidence/e007-local-final-receipt.json | `26cc14e16e787863ff907cde05cf4a45cc8a9645dff56838b54368af586b21c0` |
| p4e/e007-local-final-checkpoint.json | `aa44e67d0133002cfea0a07b1e2cbacea4f8a51f4bdc4428db403d4c0afa2b3f` |

独立复核两份 core：

- `artifacts/terrain-rvt-production/core-native-r9-owner-fix-final-v2/checkpoint.json`：
  `b7f71acfbee7a30143cbb6a62cbdec539c68241e18a141a3d42bba2b5247db4a`。
- `artifacts/terrain-rvt-production/core-native-r10-local-author/checkpoint.json`：
  `dd973e1d9fc9bef61ec48406deb49670a494c0ee164f814e776ce7899e750480`。

各 core 的1107份source-before/source-after逐项相等，
并重新读取其冻结source文件逐份哈希，无差异；
各37项build-input before/after相等。
checkpoint中的R9共311项、R10共321项binary/library/object/shader文件
逐项重算均匹配，包含实际main_game和main_editor。
R9→R10这1107份源清单只有Service.cpp变化，
没有以改validator/扩Frozen cap的方式使Local通过。

适配器B006/B007的build-lock哈希分别为
`9077767dfa710e4c5487b4acca2d89fae3b4ecdbd06fb3c8f0d69154270fa543` /
`a329bc3b14ef09344f4be5354cb085f1c7dac3a3eb2b758030040923d893e443`。
各30份链接库按实际commands指向的
`p4e/R9v2-inputs` / `p4-r10-local-editor-adapter-handoff`
目录重新哈希，无差异，不是误按B006/B007自身目录寻找库。
build-lock无overlaidObjects，绑定各自coreCheckpoint及完整库清单。
实际运行的private editor_s4.exe分别为：

- Coverage：`149841ca24a1fe78c7a1705549f3b7466eba4fba4fb02f261d163915025553fe`。
- Local：`719b12a40adc306ae39c7f6a296b81163f26bd9d7304166164abe12400a221c4`。

它们与各receipt/build-lock一致；两次均worker2、validation开启、exit0、无timeout。
这是已有native双main/full ABI构建与运行身份复核，不是本轮再次构建或执行mains。
R9 checkpoint的buildLog相对路径按checkpoint目录直解不命中，
实际日志在`artifacts/terrain-rvt-production/r9-owner-fix-v2-build.log`存在；
本轮读到实际日志，不以该路径拼写问题否定已核对的二进制闭包。

### 原始 JSONL、发布与 fence

两次各10份outputFileHashes重新计算均匹配，
包括原始editor.log、runtime-events.jsonl、三张BMP及report。
直接解析JSONL，sequence连续，没有用receipt复制出的event替代原始行；
receipt引用的gate/joint/quality/measurement四行均逐对象等于对应原始sequence。

| 实际run | joint revisions / descriptor revisions | Paint lastOldDraw→draw/completed | Undo lastOldDraw→draw/completed |
| --- | --- | --- | --- |
| E006 Coverage | 1→2 / 10→11 | 244→249/249 | 475→479/479 |
| E007-local | 1→2 / 10→11 | 273→275/275 | 528→532/532 |

四个gate分别满足：
joint commit在前，匹配quality state、capture_completed随后，published-gate最后；
quality与capture的World/pose/submission/completed身份一致，
drawSubmissionEpoch等于submissionEpoch，
completedEpoch达到draw epoch，且两者严格晚于lastOldDrawEpoch。
对应质量行均incorrectIdentity/unresolved/rootOnly为0，
visible与validAtRawTargetPlusOne均为10259，interactive=true。
这些是单次发布后实际读回身份与可用性证据，不是完整视觉误差gate。

### lifecycle、held inputs 与 Undo

| 原始证据 | E006 | E007-local |
| --- | ---: | ---: |
| capture_submitted / capture_completed | 233 / 233 | 268 / 268 |
| aborted / pending | 0 / 0 | 0 / 0 |
| footer sequence | 1332 | 1520 |
| recordsBeforeTerminal | 1331 | 1519 |
| 原始footer前字节数 | 774191 | 890728 |
| oldSnapshotIntact观测行 | 63 | 77 |

每个captureHandle唯一；逐对检查submitted/completed的
World、pose、frame、submissionEpoch、drawSubmissionEpoch相等，
completed在submitted之后且完成epoch不小于提交epoch。
不是仅对比累计计数。
footer的bytesBeforeTerminal与原始文件末行之前实际字节总数相等，
incomplete=false。原始日志VUID-及VK_ERROR_DEVICE_LOST命中均0，
receipt validationErrors/runtime errors为空，正常退出。

两次初始inputIdentity同为
`e3d71f28b308c257581ff3d35a63c9c738da0f5ffaaa83ec65a68fd1af24ad07`，
Paint为
`8007024be0d86c73391b85794cba4775b3ed815c8f93374e87422b9529da2e40`，
Undo恢复初始identity。
适配器冻结源码InputDigest实际哈希paint.weights、opacity与height.data，
每次诊断tick重算所持旧input并在变异时失败退出；
published-gate另核对当前input与真实Service ticket observation一致，
且Undo必须等于baseline。
因此oldSnapshotIntact不是单纯无条件输出的布尔证明；
检查在输出前执行。本轮重算raw观测行全true并有成功outcome。
该结论限上述输入字节，不扩展成所有图像、descriptor、GPU旧owner全量无泄漏证明。

重新哈希原始资源inventory：
Coverage C010的250份、Local L010的258份均无差异；
Local同时符合original-integrity-after的258 files verified。
没有为验收修改原作者资源或把输入hash更新成运行结果。

### BMP与视觉结论边界

三张BMP各run均为1280×720 top-down、24 bit、2764854 B；
直接解析offset/rowstride/height并检查像素数据完整，hash与receipt相符。
按全部像素字节独立复算相对baseline平均绝对差：

| 模式 | Paint | Undo |
| --- | ---: | ---: |
| Coverage | 0.19791124131944443 | 0.19673828125 |
| Local | 0.19813404224537037 | 0.19692057291666668 |

与receipt一致，但**未把这些差值当lit visual gate或Undo逐像素相等证明**。
BMP writer没有独立记录submission epoch；
只能确认BMP在各Published/quality门禁后完整落盘，不能把它直接贴上
前述原生capture的精确GPU epoch。原生capture身份与BMP时序证据分别保留。
本节不宣称标准lit ROI、光照误差、normal/ORM逐通道或性能门槛通过。

### author/runtime分离与Local identity的必要独立CPU复核

从冻结R10 Service源文件提取未改写的实际AuthorMaterialState函数，
经stdin以真实MaterialInstanceStateSnapshot结构独立编译一个CPU用例，
不链接完整Service/backend。
**1/1 PASS**：

- author snapshot去除compositedPageTable等runtime texture，
  即使held author snapshot仍存活，旧table leaf最后引用可正常销毁；
- parameters保持不变；
- Local runtime overlay被替换为实际authored texture及authored asset identity，
  cache identity不冒用旧runtime key；
- 用户真正换成另一个authored texture时保留该新texture/identity/cache，
  不被先前author覆盖。

输出位于TEMP：`vl-rvt-p4-author-split-independent.exe`。
这是冻结实际函数体与真实snapshot类型的局部回归，不冒充完整Service动态测试。
初始一次提取遇到参数默认`{}`被误认函数体而编译失败，
改为定位独立函数体起始后重编通过，未改生产函数。

另独立重编真实Runtime/Residency精确池反例
`E005Exact94Plus216Plus198LiveSnapshotStopsUndoUntilSafeRelease`，
**1/1 PASS，933 ms**，观察：

```text
activeInitial=94 paintCurrent=216 undoCurrent=198 required=216
roots=16 queued=18 free=4 retiring=94 slots=512
release remaining=18 publishedRevision=2 heldInputUnchanged=true
liveDescriptorBlocked=true capsUnchanged=true gpuExecuted=false
```

证明原先runtime table owner混入held author snapshot确可在冻结512槽中阻塞Undo，
但仅释放author副本仍有descriptor owner时不会强行复用；
descriptor最后引用及真实collection边界结束后18页才完成。
held author input原字节持续不变。
本轮未增加slots或改任何阈值；CPU输出
`<TEMP>/vl-rvt-r9-owner-independent/paint-undo-exact512-independent.exe`。

Service实际修复源码路径核对：
注册/候选copy/Material intent均使用不含runtime绑定的author state；
Local初次publication在Authoring分支读取实际MI声明的weights identity，
恢复authored texture与CPU输入；相同identity可共享已恢复作者输入。
runtime材质仍绑定生成overlay，author state恢复真正作者绑定；
实际新作者替换不被剥除。
raw CPU importedWeights通过CpuAllocation租约alias持有，
本轮未发现新的可证CPU owner泄漏；未据此宣称整个进程长期内存峰值通过。

R9→R10只有Service源变更、validator源hash不变；没有放宽validator，
没有更改生成kernel、Frozen 512槽或既有格式来掩盖Local identity问题。
本轮没有参与上述实现，也未代修。

### 最终有限结论

独立接受 **Coverage worker2一次Paint→Undo** 和
**Local worker2一次Paint→Undo** 的真实发布/恢复子项。
原E005/E006-local失败证据仍保留，未改写为曾经成功。
未发现本节实际证据中的新可证阻断问题。
整P4继续等待full27及Save/Shader/Worldreload/worker1等路线，
本结论不覆盖正在执行的Coverage full27，也不把单项成功升级为P4 PASS。
