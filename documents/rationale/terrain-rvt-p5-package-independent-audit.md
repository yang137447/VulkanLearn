# P5 独立 Game 包 CPU 审阅：C008 / C009a / L009

日期：2026-11-21。性质：独立审阅记录，非实现契约。

**R9 reader 取证复验：第 10 节发现的 shader manifest / HashFile 两个漏 hook，
已由第 11 节实际生产源码 CPU 复验有限关闭；完整负证据资格仍未关闭，
R9 原生安装 worker1/2 readonly trace 尚未执行。**

**最新复验：L009 Local 双线程初始安装子项可关闭；C009a Coverage 初始安装及
readonly 子项的既有关闭结论保留，整 P5 仍 PENDING。**
Local 证据及其独立 R5 二进制/publication 身份见第 9 节；本轮不包含 Local readonly。
此前复验：C009a 闭包整改及三次实际初始安装烟测子项可关闭。
本轮仅 CPU 重算既有实际运行证据、只读查询恢复后的 ACL，没有重新启动 GPU 或修改安装包。
具体证据、身份和范围见第 8 节；第 1–7 节保留 C008 失败时的历史审阅，不倒签旧包。

**结论：旧 C008 为 NOT-READY / FAILURE，整 P5 不通过。**
文件身份、V2 Store、TR 改写和 native MI 语义的有限 CPU 检查可接受；
它们不能证明 native 全消费者闭包完整。实际 Quality compute 漏包已由静态核查和
另一路 worker2 实包烟测共同证实。Feedback graphics 的参数 include 在原始包中也未随带，
属于首次运行需要写安装目录的闭包缺口。不得让旧包继续 worker1 / readonly 重复同错，
不得通过删 cache 条目、修改旧包或放宽冻结预算消除证据。

## 1. 审阅范围与快照

工作树为 `D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn`；
实包为同 topic 下的 `p5g/C008`。本文中的 `p5/` 均指工作树的
`artifacts/terrain-rvt-production/p5/`，不是 topic 根下另一个目录。
源资源副本为 `../p5r/C008`，stage 为 `../p5s/C008`。

读取了 `p5/C008-HANDOFF.md`、`C008-package-audit-r1/receipt.json`、
`inventory.json`、`terrain-rewrite-byte-proof.json`、源资源清单、native Store /
negative / verify 收据，以及 exporter、TR loader、MI resolver、Store、
QualityFrame/QualityGpu/Feedback 的消费源码。未执行交接的 audit Python：
该脚本会删除/损坏原包输入，违背本次审阅的只读边界。

本审阅仅启动 CPU 哈希工具、已有 native Store validator、旧安装 verifier 和文档 gate；
没有启动 Game/GPU、ACL 操作、重新导出、构建、负例副本或测试全套。
唯一主动写入的交付文件为本文。用户资源、V1、原包和源码均未由本审阅修改。

审阅期间另一执行者完成了用户授权的 C008 worker2 烟测，导致包增加 generated include。
必须区分下面两个实测状态：

| 状态 | 文件数 / 总字节 | inventory SHA256 |
| --- | --- | --- |
| 本审阅开始时，原始 r1 实包；逐文件独立复算吻合 | 612 / 626,146,283 | `8333dab68092d9945a4c073abf7458b77adad49879854779a35619257821299e` |
| 外部 worker2 烟测后，本审阅再次复算 | 623 / 626,152,399 | `23d24630b4b641d61c853a9148a8f8b2fc7a928f624851bfc770608e7389d6da` |

后者相对原始清单新增 11 个文件、6,116 B；原始 612 文件没有修改或删除。
不能将烟测后生成的文件倒算为原始交付闭包。本文不恢复/清理这些外部运行成果。
源码也在并行修复；后续源码包含的修复不能倒签旧 exporter 或旧包。

## 2. 独立复算身份与 CPU 原生校验

清单算法：按 Python `sorted(Path.rglob("*"))` 顺序枚举普通文件，
每行 `{path: POSIX相对路径, bytes: 实际长度, sha256: 实际文件SHA256}`；
对 `json.dumps(rows, sort_keys=True, separators=(",", ":")).encode("utf-8")`
再取 SHA256。初始 612 行逐行等于 r1 inventory，不只是核对总数。
检查时未发现实包 symlink/junction。

| 对象 | 独立复算 SHA256 |
| --- | --- |
| `main_game.exe` | `ce9c1814d4813a94473193dc365fb235eeec318d6665e9b0a062ad35fee332d3` |
| `config/terrainRvtP0Frozen.json` | `43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd` |
| 冻结 `.sha256` 文件本身 | `84490352345b9565874d867873417910dc5ffd8973130ff103952431ccf5541d` |
| `resources/VT/775359b06fde29e5/manifest.json` | `4cbfbc0671bacee39cf478a34be517bba15863bbedad800755f14fe5b0a9143f` |
| `shader/spv/shader-build-cache.json` | `d6382b983c4ef3391fc401c64753e37ce0daf1925741a0d4c778b98a7ab1e48b` |
| `shader/runtime-shader.manifest.json` | `1b9c26fc6afe1b3a147ab6c5be0e93953714453910fbaf699d120c3c3f795e14` |
| `game-export-manifest.json` | `007f9d81b7e3d89b429b6c381b0ab8489680c7a5abc16db2f8bdddd54e7af9ef` |
| 本次调用的旧 `vl_export_game.exe` | `062a195958cb7a03cb70fac9a51d7e13bc33f441846ab633852e4473fc924e9f` |
| 本次调用的 `run006-native-store-validator.exe` | `2d8b945d0ad75b4d9b9ef7645c56ee42609dc7176c4598dfe651265dda8a7ef7` |

冻结 JSON 是原 bytes，sidecar 内记录原固定 hash；未“修正”其中历史 pending /
证据路径字段。实包 Game 与 stage、`core-native-r2-frozen/bin/main_game.exe`
均字节相同。独立核对 `core-native-r2-frozen/source-before.json` 的 1,073 个源码
副本，全部吻合；清单文件 SHA256 为
`e8a621f27a12c9060ec98ab1ce0fe637b71621b93392cac414028890e6dec6ee`。
QualityFrame、QualityGpu、ShaderCompilerApi 的当前文件与该冻结副本相同；
Service 当前文件已不同，运行行为核对采用冻结副本，不能用当前 P4 编辑成果代替旧二进制。

实际独立调用已有 validator，参数为实包 manifest 和 Windows `NUL` 报告目标；
该 helper 源码 `artifacts/terrain-rvt-production/validate-actual-publication.cpp`
确实调用 `TerrainCompositedVTStore::Open` 和 `ValidateAll()`，非仅检查 JSON。
退出 0，stdout SHA256
`684fe46176c55edf5fd01e3c436a41700fd9ac7bc4ca3f16f5f835bb3fd89697`。
实测 16 surfaces、448 pages、67 objects；对象 kind 0/1/2/3 分别为
29/22/4/12；对象总字节 390,874,396，page payload 99,434,496 B，
`.vlcvt` 99,434,512 B。manifest BLAKE3 为
`16c76701ab0f86244def67a82960ec9ee0be5deb6ba19b6b501ffef92103be5a`。

独立调用上述旧 exporter 的 `--verify-package <C008>` 同样退出 0，
stdout SHA256 为
`20de87a5f730f9f2763c5a1393a5e080de8ba183298eb01e48376bd1e9b662f1`。
导出收据列 611 文件 / 626,022,037 B，最后生成的 export manifest 自身
124,246 B 不在其中，故与物理 612 文件的差额可解释。
**这两个 exit0 是实测事实，但不足以签署完整包资格。**

## 3. 已证实阻断：Quality compute 消费者未进入交付闭包

按冻结 native 调用链检查，而非仅按 exporter required list 自证：

`TerrainCompositedQualityFrame::AfterFeedback` 创建自己的 ShaderCompiler /
PipelineFactory，初始化实包 `shader` 根，再构造 `TerrainCompositedQualityGpu`；
后者在冻结源码第 160 行调用
`CreateComputePipelineCandidate("terrain/terrainCompositedQuality")`。
`PipelineFactory` 经 `EnsureComputeStageCompiled` 创建 build request；
`ShaderCompiler::CreateComputeStageBuildRequestForShaderName` 在 cache 命中判定前
要求 `.comp` 实文件存在。cache 中有记录不能免除此读取。

旧 C008 cache 确有唯一 StageEntry：
`58aa8d4660f5f8b09cc8594c83da8b3e2e5ec19b5117ada780b998d122e35090`。
但它未进入 runtime shader manifest 的 82 个 retained artifact，
下面三个实文件均未交付：

| 缺项 | cache 声明的 BLAKE3 |
| --- | --- |
| `shader/glsl/terrain/terrainCompositedQuality.comp` | `138bd224dc0620cd1b5c6ad5c8fd5a7e0373074717871d872878df71b01d8d04` |
| `shader/spv/terrain/terrainCompositedQuality_comp.spv` | `d499ae46984017f9b0e48e58e1b71fb3ef8504fe78113eea436504623192c0f8` |
| `shader/spv/terrain/terrainCompositedQuality_comp.debug` | `8ae59acb23addd81ef3f01e8c26e9da603c8da5c154ab24b882f53724d2d0d44` |

该 StageEntry 的唯一 transitive dependency 为
`materialFunction/terrainCompositedFootprint.glsl`，实际存在且 BLAKE3
`0b3724b7b8b5a3fc9141a46da0e4ebd9f2ea950ef72171616f9eff16383b44a2`
吻合。故不能笼统声称所有 include 都漏了；缺主源码及两个产物已足以阻断。

外部真实运行证据为 `p5/C008-installed-live/worker2/`：
`execution.json` 绑定同一 ce9c… 二进制、worker2、外部 cwd、validation，
`result.json` 明确 `accepted=false`、`qualityAccepted=false`、captureCount=0。
本审阅读取时 `stderr.log` 为 20,165 B，SHA256
`227193871a1deb1b3d4e1d228d17ad931cb3a10c44dc123a2ee9b4c652aa32f1`，
109 行均重复报告缺少上述 Quality `.comp`。
该运行由另一执行者完成，本审阅没有使用 GPU。
即使进程 exit0、teardown 完整、启动 shader 统计 misses=0、validation errors=0，
也不能掩盖后续动态 Quality consumer 从未成功准备、没有 raw capture 的事实。

旧 verifier 只对其 runtime manifest 已列产物和源文件核摘要，
Store 验证覆盖 publication 输入/页；两者没有从 QualityFrame native 消费者反推
必须存在的 artifact 集合，因此接受了漏包。缺失 StageEntry 不在 retained 集合内，
旧 verifier 就没有检查其 source/SPV。这是已证实的验收盲区。

## 4. Feedback graphics 与其它 pipeline 闭包

冻结 QualityFrame 消费同次 terrainFeedback 的两张 winner 和最终 D32 depth；
它不创建旧 replay graphics pipeline。对应实包 graph 为 7 个颜色输出 + D32，
`terrainQualityWinner0/1` 是 128² RGBA32_UINT，depth 有 sampled usage。
该路径经 `MI_terrainFeedback.json -> shader/glsl/pass/M_terrainFeedback.json`
使用 native GraphicsPair：

- Unlit：`48de3810153df52e3d64d9d31fa130114e93d29d9773ea532a552c5eaa34a13d`。
- DefaultLit generic：`7664e3418abcb7cf546e13bcc73fbc5ed930a808a6967532287660ac0717c17e`。

两项均在 runtime manifest，四个输出各自齐全且实际 BLAKE3 吻合；
两项共享的 vertex/fragment 主源码及依赖摘要也吻合，但必须注明时间边界：
8 项依赖中的 `pass/generate/M_terrainFeedbackParamter.glsl`
**原始 612 文件 inventory 中不存在**，烟测后才生成。
其 BLAKE3 为 `805019c676bc17642dc250c30999cd7fb4eae3e0fe8496913cafdabf70b29d81`。
另外 7 项依赖和两个主源码原本随包，未发现第二处 Feedback SPV 丢失。

原始 runtime manifest 将以下四个缺失物理依赖标成 `generatedAtStartup`，
未放入 `sourceFiles`；实际启动会将它们写入安装目录：

| 原始缺失参数 include | 实际消费者 |
| --- | --- |
| `pass/generate/M_terrainFeedbackParamter.glsl` | 上述两个 Feedback GraphicsPair |
| `generate/M_terrainParamter.glsl` | M_terrain TES=1、TES=0 两个 MaterialGraphicsPair |
| `generate/M_waterParamter.glsl` | M_water MaterialGraphicsPair |
| `pass/generate/M_deferredLightingParamter.glsl` | deferredLighting 两个 GraphicsPair |

worker2 `package-after.json/result.json` 和独立清单差分显示共新增 11 个参数 include，
还包括 shadow/bloom/postProcess/sky/toneMapping。生成器
`MaterialParameterIncludeGenerator::CommitGeneratedIncludesIfChanged`
只在 bytes 不同/缺失时写文件；冷安装缺文件需要写盘，不能用 writable smoke
生成成功代替 readonly 安装通过。这是独立于 Quality compute 漏包的只读交付阻断；
本文没有实际执行 ACL，不能声称已经观察到 ACL 拒绝。
后续必须从全新导出包核全部启动生成候选，而不只是补上述四项 cache dependency。

整个原始 cache 有 86 项，stage 与包中 cache 逐项完全相同，未手删条目。
runtime manifest 保留 82 项，266 个已交付输出全部独立 BLAKE3 复算吻合。
cache 的其余 12 个未交付输出分属 Quality 两个、editorGrid 两变体共八个、
BakeSamplingDiagnostic 两个。后两类未见冻结 QualityFrame 调用，
不能将退役 editor/诊断消费者凭空当作本场景必须运行的 pipeline。
`terrainCompositedQualityVisible.glsl` 未随包，但当前 Quality compute
没有 include 它，冻结 QualityFrame 也没有旧 replay pipeline 调用，故不列作缺包。

两条实际生产管线 World / WorldPadding 各有唯一 StageEntry，
source/include/output 与 publication generator 对齐；分别核对主源码及依赖 6/1 项，
没有缺失或 hash 不符。实际 M_terrain TES=0/1 两变体也保留了各自全部输出。
对全部 retained artifact 的 source/dependency 并集检查，烟测后 163 个物理文件
BLAKE3 均吻合；其中上述四个文件不能追认为随包。
8 个 `__composed__/…` primary identity 是 Composer 内存生成身份，
不应误报成遗漏的磁盘作者文件。
上述核查证明特定消费者的物理闭包范围，不签署整个 native 运行的零 miss。

## 5. TR、宏高度和材料的有限接受项

独立从源 TR/实包 TR/scene/publication 对比，而非只读取 byte-proof 的 true：

- scene 与源资源 scene JSON 相同，16 个 terrain placement 对应准确的
  `terrain|Landscape_00` 至 `terrain|Landscape_15`，没有改名规避 surface 匹配。
- 每个 TR 删除 `paintPath`、`compositedVT.productionSourcePath`，
  base/detail MI 指向 manifest 父目录下对应 surface 的 `runtime/*.json`。
  32 个 MI 文件实际存在，与源 publication 的 archived MI SHA256 分别相同。
  未发现资源根 decoy、错误 runtime MI 路径或随原 terrain MI 的 textureSource 链。
- 16 张 geometry/collision macro height 移入 `Runtime/Terrain/height_<BLAKE3>.png`；
  每张实际 SHA256 等于其源 heightmap，没有解码重编码。
  仅消去 paint/recipe、替换 MI/manifest/height 路径后，完整 JSON 深比较相同，
  包括 geometry、layers、UV、LOD 等所有其它字段。
- 独立核 65 个原 height/paint/baseMI/detailMI/recipe 逻辑路径均未安装；
  资源树无 `Author` 目录文件、无 `.vlpaint`。源资源副本的 250 文件清单再次复算，
  与 `source-resource-before.json` 完全相同。

共享 TR 多 placement 的依据为 exporter `RuntimeTerrain` / `RewriteTerrainPlacements`
源码及既有定向回归。键为 `sourceTR + newline + surfaceId`，后续 placement 派生
`Runtime/Terrain/tr_<digest>.json`，按本 surface 绑定 archived MI，
补显式 `originalTR#layerName` 保持隐式 layer key；scene name / surfaceId 保持原值，
未发布 identity 拒绝。C008 本身是 16 个不同 TR，不能声称实包实测过共享 TR 派生。
本次未另造共享资产或复跑测试。

native MI 检查不是另一套自造宽松 schema：
exporter/verify 调用 `MaterialInstanceResolver::ResolveWithDefinition`，
再调用实际 `MaterialDescriptorSchema::Build/ValidateInstanceValues`。
32 个实包 MI 无非法空 textures/parameters override；
实际旧 verifier exit0 是这些安装文件通过该 CPU 路径的证据。
prebound textures 与 schema 校验是不同步骤，未验证 GPU descriptor / 画面。

## 6. 作者字节、绝对路径与证据边界

V2 的 12 个 kind=3 `RawAuthorSource` 归档字节仍为 Store 必需的哈希输入；
`ValidateInputs/ConsumeInput` 会读取包内 bytes 并核 digest。
这符合本次允许保留 archived bytes 的边界，不得误报为 runtime 打开原作者路径。
production 有 1,120 条 authorDependency 记录，包含原始逻辑路径，
不表示这些路径被 loader 实际尝试打开。
Game 的源输入从 publication 对象恢复，recipe 读取在冻结 Service
`IsAuthoringMode()` 分支内；archived MI 在 manifest 父目录解析，
通过 runtimeMaterialSources / preboundMaterialTextures 供 renderer 使用。
TR loader 的 paint 读取受可选 paintPath 控制，宏高度 decode 仍必需。

扫描实包所有 JSON 字符串发现 28 处工作区绝对路径，全部来自未改动的
`config/terrainRvtP0Frozen.json`：1 个 evidenceRoot、27 个 evidenceFiles 路径。
冻结 Service 的 `InitializeFrozenConfig` 消费 caps/targets/reserves 和本地 hash，
未发现运行时读取 evidenceRoot/evidenceFiles 的源码入口。
因此应记录“含历史绝对路径元数据”，不能等同“依赖工作区文件”，也不能擅改冻结 bytes。
其余 JSON 未发现工作区绝对路径依赖。二进制中的编译路径字符串不等于打开依赖。

**未采集 engine-controlled-asset-opens trace，不能证明运行完全没有外部原作者文件
读取或 fallback 尝试。** 静态 scan、源副本未改、实际文件清单均不能替代真实 open 观察。
本次未发现可证的 original-author 扫描链，但不签署 absence proof。

r1 的 Image/RawAuthorSource missing/corrupt 四负例及最后恢复 verify 的日志 hash
逐一核对吻合，均分别记录 exit1/最终 exit0；这是历史负例证据复核，
本次没有独立重演，更没有在原包删除输入。

## 7. 测试账与整 P5 资格

已有 exporter 全套首次 95 cases：90 pass、4 旧 fixture/assert 失败、
1 Windows symlink 权限 skip。修复四处测试假设后仅受影响 7 cases 复跑且全部 pass，
累计最终 94 pass / 1 skip / 0 未解决失败；**不是第二次全套运行**。
独立解析两个 XML，确认 95/7 case 数、4/0 failure、1/0 实际 skipped 节点；
两个文件 SHA256 分别为
`c32a0aa22cdaa9f1ccde7e1cb405351eec7193d7f2d55a1b1d9f2d9f9d512d21`、
`2c117d19156a319c2a8d8320a09bbd3691ad01fdadfef283b14525c7ac8db35d`。
本审阅不重复所有测试。测试通过未覆盖上面真实 native consumer 漏包。

整 P5 的后续资格至少仍需：

- 根修后的新目录重新 export，重新绑定 exporter、Game、shader/cache、
  publication、冻结 bytes 与全文件清单；不能给旧 C008 patch 或改 cache 消除缺项。
- 从 native 调用链核 Quality compute、Feedback graphics、生产双 compute、
  其它可达材质及所有启动生成 include，而非 required list 与自身输出互证。
- 新包实际 worker1/2 correctness、rootReady、有效 raw quality capture、
  interactive、零 shader miss，以及有写入拒绝探针的 readonly 安装。
- 两种权重模式、53 poses（16 块近景 + 24 内边 + 9 交点 + 4 外角）、
  未访问区域/移动/转头/瞬移、LOD/TES/ROI、根损坏恢复、World 换代退休证据。
- 冻结硬件和预算下的真实 perf / 生产与拷贝 / geometry 含 TES /
  CPU-GPU-host 总账峰值，以及完整原始样本；CPU 结构与 Pascal correctness smoke
  不代签正式性能。
- engine-controlled-asset-opens 的成功及失败尝试观察，区分安装输入、外部证据输出和
  原作者依赖；P4 实际编辑/保存/Cook/重开等未齐证据亦不能用历史测试替代。

本记录接受的范围仅限已逐项验证的 CPU 身份、Store 输入页完整性、TR 几何不变迁移、
archived MI 绑定与有限 native 语义。**C008 实际 consumer 完整性失败，P5 保持未通过。**

文档验证：`py -3 tool/validate-docs.py` 退出 0，报告 `documents layering OK`；
本文的 `git diff --check` 未报告空白错误。

## 8. C009a 整改及三次实际 installed 初始烟测独立复验

本节是后续独立复验，不修改第 1–7 节的 C008 判定。
实包为 `../p5g/C009a`；读取 `p5/C009a-closure/`、
`p5/C009a-installed-live/{worker2,worker1,readonly-worker2-r1}/` 和
`p5/C009a-review/`。本轮只写本文，不执行交接脚本、不变更 ACL、
不启动 Game/GPU，也不重新做未变 TR/height 的逐项语义验证。

### 8.1 当前实包身份与 native 验证

再次枚举实际物理文件，逐文件复算 SHA256/size，按第 2 节同一算法计算清单摘要：
**626 文件 / 626,209,598 B**，inventory SHA256 为
`3c7dff739acc34447c42cc76abaf1ab2549b1c56fb1dfcba78ce593f07249534`。
全部行与 `C009a-closure/inventory.json` 相同，也与三次运行各自
`package-before.json`、`package-after.json` 六份清单逐行相同。
不只是核对 result 的 unchanged 布尔值。

相对 C008 原始 612 文件清单，只新增 11 个参数 include、Quality 主源码及其
runtime/debug 两产物；仅两个交付 manifest 改变，没有删除其它文件。
因此不重复签署一遍未变 TR/height 的语义测试；它们的字节身份已由完整 inventory 覆盖。
C008 历史包继续 NOT-READY，未由本审阅改写、恢复或清理。

| C009a 身份 | 独立复算 SHA256 |
| --- | --- |
| `main_game.exe`，仍为 C008 core-native-r2 | `ce9c1814d4813a94473193dc365fb235eeec318d6665e9b0a062ad35fee332d3` |
| 冻结 JSON，仍为原 bytes | `43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd` |
| publication，仍为 C008 gen3 Coverage | `4cbfbc0671bacee39cf478a34be517bba15863bbedad800755f14fe5b0a9143f` |
| `game-export-manifest.json` | `02f61047b6718d9629fc1b17c1c73c84415cb15b920138fb37d589d4b000e7ed` |
| `shader/runtime-shader.manifest.json` | `4ef977056d2126779d031bb3d2bffaafe40821e78a7183de07a4b576651f2a3c` |
| 本次实际调用的新 exporter | `1113f28bc365b7af5a547c1eafd67851a60016b98e211e0aaf9ec474a5e952fb` |

包内 86 项 shader cache 与 C008 cache 字节完全相同，未手删条目。
源码/GLSL、公用 producer 及 publication 的证据边界仍绑定 C008 快照。
另独立读取并核 hash 的 R5 Game 为
`2cf3fca066073d1e173d74f3c746af5f79c93c8e1a50198786c428c9eb6ca8e6`，
与本包不同。C009a 三次 smoke **不是 R5/P4 编辑事务验收**；
不能因 Quality 模块对象或 GLSL 相同就将结果转签 R5。

本轮实际执行新 exporter 的 `--verify-package <C009a>`，退出 0，
stdout SHA256 为
`791928b7c7d1581a0d53f5981725d9a5053c9b55d4fa9ca8f3359d40c0d65a71`。
读取当前源码确认安装验证调用 native deterministic
`MaterialParameterIncludeGenerator::BuildGeneratedIncludeContent`，
将全部已安装 M_ 定义的生成 bytes 与实际 include 比较，并要求其位于 sourceFiles
身份清单；不是只核 closure receipt 的 true。
另实际运行第 2 节同一个 CPU Store validator，报告目标 `NUL`，退出 0，
仍为 16 surfaces / 448 pages / 67 objects；
stdout SHA256 为
`b98682cb8b7ba8e170e86e7dbb53eb9bcf4da6ac068ec7ca864a5460079ceaa8`。

### 8.2 消费者、Quality 和全部启动 include

新 runtime manifest 保留 83 项，较旧包多出真实 Quality StageEntry。
从 retained artifact 的 primary/dependency/output 身份逐个解析实包路径，
432 个物理 shader 文件的 BLAKE3 独立复算全部吻合；排除 Composer 内存生成的
`__composed__` 身份，没有将其误当成磁盘缺文件。
22 项 required compute 均有唯一 StageEntry、主源码、传递依赖及
runtime/debug 输出，实际 bytes 对应 cache 和 closure 收据。
Quality 缺失的三个文件现均随带，Footprint include 同时吻合。

没有只用 required list 自证：另从 ce9c… 对应冻结 source manifest 的
`.cpp/.h` 重新枚举 `CreateComputePipelineCandidate/CreateComputePipeline/
EnsureComputeStageCompiled` 的 literal 调用，得到 23 个调用位置、22 个不同 entry，
逐项找到唯一 cache StageEntry 且均在 retained manifest。
World / WorldPadding / Quality 的实际消费关系延续第 3–4 节；
Feedback native 两个 GraphicsPair 及全部 source/dependency/output 也已核对。
变量拼接 shader 名仍须由场景/graph/runtime 消费集合和真实运行约束，
该枚举不是完整程序所有动态路径的形式化证明。

实际安装有 11 个 M_ 定义和完整 11 个 native 参数 include；
逐项核 SHA256、BLAKE3 和 `sourceFiles` 收据。所有 include 与 C008 失败 smoke
实际新增的 11 文件字节完全相同，已在冷安装前随带。
它们既覆盖 cache 当前依赖的四项，也覆盖 shadow/bloom/sky 等其它启动生成候选。
native verify 的确定性重算进一步确认“已存在”不是随意占位文件。
三次实际运行清单完全未变，为启动没有再生成/改写文件提供了安装字节证据。
**C008 所指出的 Quality compute 和 generated include 两项交付阻断，
对 C009a 这个固定快照关闭。**

### 8.3 原始 capture、metadata、Service 和实际呈现绑定

本轮读取全部 **70 份** `.bin` 及对应 `.json`：
worker2 24 份、worker1 24 份、readonly-worker2-r1 22 份。
CPU 使用已检查实现的 TVQ1 decoder 逐样本重算，不使用 result 中的 accepted 作为输入。
检查 ABI v2 / 1,619,200 B、header/frame/pose/generation/submission/completed epoch、
surface dictionary、publication 的 16 个 layout/contentHash、真实 taps 的
page-table result 与 owner 的 surface/mip/page/revision/content identity，
并核 binary SHA256、metadata 和重算 counts。
所有 capture 的 duplicate/invalid/dropped/overflow/uninitialized header 均为零。

另外直接从 `stdout.log` 提取 CLOCK/STATE/PRESENT 原始 JSON，逐行等于 observations；
将每份 capture 的 epoch 和 counts 绑定至实际 Service STATE。
从首次 `loading_released` 的 `observationSubmissionEpoch` 反查 ready witness，
不采用文件名字符串排序选出的 `firstQualityWitness=capture-10` 代替真实首次 release。

| 实际运行 | 进程总时长 s | process→首次 release ms | release 引用 epoch / witness | capture 数 |
| --- | --- | --- | --- | --- |
| worker2，PID 67792 | 30.276486 | 24,048.8845 | 30 / `capture-6.bin` | 24 |
| worker1，PID 79228 | 28.995428 | 23,017.5741 | 29 / `capture-7.bin` | 24 |
| readonly worker2 r1，PID 77944 | 28.877020 | 23,170.8729 | 29 / `capture-6.bin` | 22 |

三个 release witness 均为 visible=3271、validAtRawTargetPlusOne=3267，
即约 **99.8777%**，effectiveMipBias=0；
unresolved/incorrectIdentity/rootOnlySamples/uninitializedDraws 均为 0。
对应 Service STATE 为 interactive，PRESENT 引用同 generation/pose/epoch，
完成时间及 release 时间均在原始 process-start 的 86,748 ms deadline 内。
没有用重置 deadline、只根就绪或无可见样本取代质量条件。
16 个 dictionary 身份正确不表示这个初始视角看到了全部 16 块；实际统计域是
128² feedback 可见样本及其全部 taps，非全屏所有 fragment，也非 53 poses。

上述三份首次 release witness 的 SHA256 依次为：

- worker2：`b2604fc5913730d40e9c1d93d55e92cbf1a93c4b82065b826c87af0273114396`。
- worker1：`c4951507d3c49083f7ae1edce6629a4d2bffe749d282898cda30437f3a8ac930`。
- readonly worker2 r1：`a7f55d4a01b63e61a90e6e4620d9ca918e9d7fcc05de4596a614c949892dff82`。

原始日志三次均有 `entries=86, artifacts=60, hits=60, misses=0, compiled=0,
shaderc=0, failed=0`，stderr 均为空，未发现 VUID / Validation Error。
execution 各自记 quit injection exit0 和 Game exit0，stdout 均有 Quit requested
及 EngineLoop teardown complete。运行从包外 cwd 发现实包 shader 根；
70 份 capture 与 metadata 均为 moduleOnly=false。
BMP 文件头、总长度与像素独立读取：三次 first-black 均全零，
first-ready 均为 1280×720 非黑图。这仅证明记录的黑屏到 ready 呈现边界，
不代签最终画面 ROI/接缝质量。

### 8.4 readonly 的实际保护、探针与精确恢复

没有重新 Protect/Restore 或重跑写探针。本轮对历史实际命令、helper 代码、
probe stdout、backup、restore 错误/成功日志和当前安全描述符分别核对：

- `readonly-worker2-r1/acl-command.json` 给出实际 bundled `pwsh.EXE`、
  `-NoProfile -NonInteractive -ExecutionPolicy Bypass -File readonly_game_acl.ps1`，
  Package 精确为 C009a、AllowedParent 为 `p5g`、Evidence 在包外、
  ResourceRoot 为 `p5r/C008`。wrapper 实际以该 argv 加
  `-Operation Protect` 调用；`finally` 中加 `-Operation Restore`，
  不是只设置 ReadOnly 属性。
- helper 限制精确包/父目录、与 workspace/resources/evidence 不交叠，
  保护前检查 READ_CONTROL / WRITE_DAC；向当前 actor SID 添加可继承 Write deny。
  真实 probe 使用随机新文件 `FileMode.CreateNew + FileAccess.Write`，
  和现有 `config/config.json` 的 `FileMode.Open + FileAccess.Write`；
  两项均捕获 UnauthorizedAccessException。
  `readonly-probe.json` 与 `acl-protect.stdout` 一致。
  `writeProbeExit=1` 是 helper 对两个拒绝结果的编码，Protect 进程本身成功，
  不能误写成两个独立子进程各 exit1。
- backup 记录原 owner 为 Administrators，owner SID `S-1-5-32-544`，
  actor SID 为 `S-1-5-21-4175325682-2436190947-2784970811-131127`。
  初次 Restore 的 `Set-Acl` 已移除 deny，但规范化了 DACL AI flag，
  `acl-restore.stderr` 的 “Restored ACL differs” 是真实失败，未被抹掉。
  后续 `acl-restore-raw-retry.log` 记录 raw `SetKernelObjectSecurity` DACL
  恢复后 exactSddlRestored；恢复重试没有再启动一次 Game。
- 本轮使用 `Get-Acl` 只读独立核当前根目录 Access/Owner/Group SDDL，
  与 backup 作区分大小写的精确比较，结果相同；owner SID 相同。
  另枚举 693 个后代（含目录）查询 ACL，当前 SID 的 Deny 规则为零，
  根目录对应 Deny 亦为零。未使用 `Set-Acl`，未写安装目录。

保护调用记录的 helper SHA256 为 `a7dabaad…57b2099`；
含恢复补强的当前及 review snapshot helper 为
`8f02430539419229ebace846bea243113117794a423d35658362a99ff7d86b12`。
不能将后补脚本假充为最初 Protect 时的同一 bytes；
恢复可信度同时依据保存的错误/重试日志和本轮实际当前 SDDL/owner/全树 deny 查询。
历史命令未完整记录所有原生失败写尝试，本文不补造此类 trace。

`readonly-worker2`（r0）保留的是 Windows PowerShell Security 模块加载失败，
发生在 Get-Acl / ACL 变更之前，没有 backup / Game launch。
它既不算成功 readonly，也不算第四次 Game；本次有效实际 Game 恰为三次。

独立复算 `C009a-review/hash-bound-artifacts.json` 的 **212 个**文件记录，
bytes/SHA256 全部吻合；smoke receipt 及其 sidecar 的 SHA256 均为
`add0f7f6fde8d79ccd2ad6bd5e7b9168c3785f71770ecee695781a02b9c6c7f2`。
`C009a-closure/receipt.json` 的 awaiting-new-gpu-window / readonlyPassed=false
是导出时历史状态，后续三次 raw/runtime/ACL 证据补足了初始安装子项，
不需要改旧 receipt 或将 r0 伪计为实际运行。

### 8.5 本轮关闭项与保留缺项

**有限接受：C009a 固定 Coverage / C008 ce9c… 快照的完整已审消费者 shader
交付、全部 11 个启动 include、worker2/worker1 初始 Service 质量就绪、
readonly worker2 初始就绪、quit/teardown、零新增/改写安装字节和 ACL 精确恢复。**
这是既有真实运行证据的独立 CPU 复验，审阅者本轮未启 GPU。

**整 P5 仍 PENDING。** 尚缺 53 poses 全岛巡游/后续姿态收敛、LocalLayerArray
独立 installed Game 双模式覆盖、正式 steady/streaming perf 与 WDDM
时序/峰值及总账、fault/root recovery/World 换代退休、P4 真实编辑与保存/Cook/重开。
Pascal correctness smoke 及 27,396,128 B quality Workspace 观察不能替代冻结硬件
下的完整预算/性能验收，也不能转签 R5 编辑事务。
没有 native engine-controlled-opens 或 OS-wide trace，
原作者根也未在这三次运行中断开/拒绝，因此**不得宣称 no-external-author-opens**。
实际 NTFS 探针拒绝与零文件变化也不等于观测到了 Game 的全部失败写尝试。

## 9. L009 Local 双线程初始安装独立复验

日期：2026-11-21。本轮仅 CPU 读取并重算既有实包、日志及原始捕获，
没有启动 GPU/Game、重跑 exporter/native verifier、执行 ACL 或修改实现/安装包。
Coverage 的 ACL/readonly 证据不重复执行，也不转签给 Local。
第 8 节当时未齐的 Local 初始运行证据由本节补足，历史 C008 判定保持不变。

### 9.1 独立包及 source/publication 身份

实包 `../p5g/L009`，源资源 `../p5r/L009`。
逐文件 SHA256/size 及第 2 节同一清单算法独立复算结果：

| 对象 | 结果 |
| --- | --- |
| 当前 L009 实包 | 634 文件 / 743,561,689 B |
| 实包 inventory SHA256 | `8ad2c7149249f59eb7f0dcbb65532570f30b49550d46bc130b9221e7ee4d0e4e` |
| 当前源资源 | 258 文件 / 816,219,112 B |
| source inventory SHA256 | `4c3faca4ece7f6abe6d55bb3ba910d4996f799b3cf8448f3b829346c3646960e` |
| L009 Game，实际等于 R5 frozen binary | `2cf3fca066073d1e173d74f3c746af5f79c93c8e1a50198786c428c9eb6ca8e6` |
| Local publication SHA256 | `39ee6ad441f0668dbcbd28770df53c24d25fd06e74171c9bbed938e36cce404b` |
| export manifest SHA256 | `c73575a452cd3012090526e233c6697eac2afe868b07f6279e741529c88f415b` |
| 原始 P0Frozen JSON SHA256 | `43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd` |

当前实包清单逐行等于 `L009-package-audit/inventory.json`、
`L009-installed-review/package-final-inventory.json` 和两次实际运行的四份
`package-before/after.json`。源资源完整清单也逐行等于两次运行各自的
`source-resources-before/after.json`，不是只比较 result 布尔值。

`L009-installed-review/smoke-receipt.json` 独立复算 SHA256 为
`c1af3d2da385f406aab79293abbec00bee2406cc62ce38a2ca3695c1aaf6ab29`，
等于 sidecar。`hash-bound-artifacts.json` 为
`a88432c830b843ff0becff4fe935af3c0fe2b6ca7347074026a68e38439b11f0`，
其中 130 项文件的实际 bytes/SHA256 全部吻合。

Local manifest 位于 `resources/VT/17e02619cb09e6a1/manifest.json`，
与其源资源副本字节相同：16 surfaces、448 pages、**75 production objects**，
kind 0/1/2/3 为 45/22/4/4。16 surfaces 的 weightSource 均为 LocalLayerArray；
从每 surface 的 local-weight 输入引用反查对象，得到 **16 个不同的 R32 float**
对象，每个 515×515、7 layers、单 mip。全部对象 size 和 BLAKE3 与 manifest 相同，
包体 BLAKE3 也吻合。这里是 Local 的实际输入，不套用 Coverage 的 67 对象证明。
R5 Game 与 Coverage ce9c… 不同，Local publication 与 Coverage 4cbf… 也不同；
相同 producer shader/cache 不能把两份安装包或运行收据视为同一快照。

### 9.2 有限 CPU 闭包与 runtime 路径

L009 的 86 项 shader cache 与 Local source stage 字节相同，仍为
`d6382b983c4ef3391fc401c64753e37ce0daf1925741a0d4c778b98a7ab1e48b`。
83 个 retained artifact 的 primary/dependency/runtime/debug 输出和
`sourceFiles` 均从实际文件复算 BLAKE3；合并 75 production objects 与包体后，
共 515 个不同物理文件全部吻合。未将 `__composed__` 内存身份误当磁盘文件。
22 required compute 各有唯一 StageEntry，均 retained 且具备 runtime/debug 两角色；
包括 Quality 及 World/WorldPadding。Feedback graphics 的原生 cache 闭包亦在上述
实际文件检查中。11 个 M_ generated include 均存在，其 SHA256 与证明清单一致，
并纳入 sourceFiles 身份；未删 cache 条目。

独立沿 scene 的 16 个 terrain placement 对照实际 TR/publication：
TR 均无 paintPath / productionSourcePath，32 个 base/detail 路径均精确指向
manifest 父目录下本 surface 的 archived runtime MI，实文件存在且无 textures override；
原 MI、paint 和 recipe 逻辑文件未随包。宏高度路径位于 `Runtime/Terrain/height_*`。
资源树没有 Author 目录文件或 `.vlpaint` 文件。
这是静态 runtime 路径闭包核查，不声称观测了所有原作者路径打开尝试。
几何/高度语义沿用已有 CPU 证明，完整字节由本轮 inventory 覆盖，未无必要重跑测试。

历史导出/原生安装验证使用 SHA256 为
`1113f28bc365b7af5a547c1eafd67851a60016b98e211e0aaf9ec474a5e952fb`
的 exporter；实际 argv 和 exit0/exit0 位于 `L009-export/package-ownership.json`。
本轮复算 export.log / verify.log 的 hash 分别为
`43cd6ad26bb0b97735d37a6c2610590c90fc9a3050f90517c02bdd292966ce92`、
`848a8f1a59c9b49b0e63d8bfe1ba1c11777ba01819f95ae8bd1cba4ce989507c`，
均吻合。没有将此历史 native 运行写成本审阅再次执行；
native sparse MI、TR schema 和 deterministic generated include 验证的范围
沿用第 8 节已核实现，L009 的独立 CPU 文件核查和实际烟测另行补充。

### 9.3 两次实际运行的全部 raw/meta 与首次 ready

读取 `p5/L009-installed-live/{worker2,worker1}/`，独立解码全部
**47 份** TVQ1 v2 raw：worker2 23 份、worker1 24 份。
每份 1,619,200 B；核 binary SHA256、metadata、frame/pose/generation/epoch、
已完成 submission、16 个 publication surface layout/contentHash，
逐个有效样本的全部 taps、resolved page 和 owner 的 surface/mip/page/
revision/content identity。重新计算 counts，与 observations 及原始
`RVT_QUALITY_STATE` 同 epoch 的实际 Service counts 相同。
全部 raw header 的 duplicate/invalid/dropped/overflow/uninitialized 为零，
capture 完整，moduleOnly=false，visibilityTest 为同次 feedback depth-tested MRT winner。

直接从 stdout 提取 CLOCK/STATE/PRESENT，与 observations 原始事件逐行相同；
从首次 `loading_released` 的 observation epoch 找 witness，
没有使用 receipt 的 accepted 作为解码前提。

| 实际 Local 运行 | 进程总时长 s | process→首次 ready ms | ready epoch / raw | 首次 ready 有效样本 |
| --- | --- | --- | --- | --- |
| worker2，PID 77884 | 31.009111 | 25,031.2093 | 29 / `capture-6.bin` | 3264/3271，99.7860% |
| worker1，PID 70740 | 31.103618 | 24,944.4503 | 32 / `capture-7.bin` | 3271/3271，100% |

worker2 首次 witness SHA256 为
`b0c011d356e3e8b350fe7175ede0bb0204f03d10795fdbb22560c108c8c9bfa4`；
worker1 为
`fd3e63c23ce483b7f24e9bfeaa03c3a4333e6e4727308c7fe300fbe0253b5ba4`。
二者 effectiveMipBias=0，unresolved/incorrectIdentity/rootOnlySamples/
uninitializedDraws 均为零，Service interactive 与 PRESENT 引用的
generation/pose/submission/completed epoch 对齐。
16 个 dictionary 身份经 Local publication 核对，不表示该初始视角覆盖全部地形。

两次原始日志各只有一个初始 CLOCK，均验证
`deadlineMonotonicMs - processStartedMonotonicMs = 86,748 ms`；
实际 ready 和 witness 完成均在原始期限内，未见迟到重置 deadline。
两次 execution 绑定同一 R5 Game、各自外部 cwd，quit injection 及 Game exit 均为 0；
stdout 有 Quit requested 和 EngineLoop teardown complete，stderr 均为空，
VUID / Validation Error 均为零。原始 shader 统计均为
`entries=86, artifacts=60, hits=60, misses=0, compiled=0, shaderc=0, failed=0`。
first-black/first-ready BMP 的头、字节长度和像素检查分别为全黑/非黑，
均 1280×720；这里只接受呈现边界，不将其等同全岛画面质量验收。

### 9.4 资格边界

**关闭 LocalLayerArray / L009 / R5 固定快照的 worker2、worker1
初始独立安装子项：实际 Service ready、已审 raw 身份/质量计数、
零 miss/compile、validation 0、quit/teardown、包及源资源清单零变化。**
因此两种权重模式都已有各自真实独立 Game 初始运行证据；
Coverage 使用 ce9c…，Local 使用 2cf3…，不能合并成同二进制全模式验收。

**Local 本轮未执行 ACL、不是 readonly 验收。** Coverage readonly 的第 8 节结论
保留，但不扩展到 Local。R5 初始运行通过也不等于 P4 编辑事务通过。
`L009-package-audit/receipt.json` 的 game-not-executed 是导出时历史状态，
由本节的后续实际 raw/Service 证据补足，不修改原 receipt。

**整 P5 保持 PENDING**：尚缺 53 poses 全岛巡游/接缝及后续机位收敛、
正式 steady/streaming 帧分布和 GPU timing、WDDM 时序/峰值/总账、
fault/root recovery/World 换代退休，以及 P4 实际编辑、保存/Cook/重开等证据。
这两次 Pascal 初始 correctness 结果不代替正式性能。
两次均未采 native engine-controlled-opens / OS-wide trace，
外部作者根未被断开或拒绝；**no-external-author-opens 仍未证**。
静态 TR stripped、完整哈希和零安装变化均不能代替该动态 absence proof。

## 10. R9 reader hooks 独立 CPU 取证子项审阅

日期：2026-11-21。审阅者未参与这组 reader hooks 实现。本轮只读生产源码、安装包及
交接证据，在全新私有目录运行 CPU helper / 编译独立反例，并追加本文；
未修改 production/CMake/预算/共享 build，未启动 Game/GPU，未触碰 Ptolemy 的 GPU 任务。

**结论：已审 reader hooks 的有限 CPU 行为可接受；完整 native reader 闭包仍有
两个可达漏 hook，不能据当前 R9 JSONL/footer 或 scope 布尔值签 no-author-opens。
同版本 R9 原生编链、重新导包及 worker1/2 readonly trace 尚未执行，整 P5 保持 PENDING。**
本节不撤销第 8–9 节按各自旧二进制已证实的初始安装子项。

### 10.1 实际版本与安装包闭包

读取 topic 下 `runtime-file-trace-evidence/r9-final-handoff.md`、
`r9-production-receipt.json`、`r9-production-syntax.json`、
`r9-jsonl-footer-verification.json` 及私有测试源码/日志。
production receipt SHA256 独立复算为
`3b6e112339832852fade8d40e3f11e9baebcf522ac1d7c45a534614d1fe97ce5`。
其列出的 19 个生产文件 SHA256 全部吻合；其中 18 个与私有 reader 副本逐字节相同，
logger 的差别为 traceStart 能力/pending 清单。公共 API 的冻结只固定接口身份，
不等于覆盖集合或运行证据通过。

重新复算三个包的实际文件清单：

| 包 / 二进制边界 | 文件数 / bytes | inventory SHA256 |
| --- | --- | --- |
| C009a，ce9c… core-r2 | 626 / 626,209,598 | `3c7dff739acc34447c42cc76abaf1ab2549b1c56fb1dfcba78ce593f07249534` |
| L009，2cf3… R5 | 634 / 743,561,689 | `8ad2c7149249f59eb7f0dcbb65532570f30b49550d46bc130b9221e7ee4d0e4e` |
| C010r8，R8 diagnostic | 626 / 626,942,340 | `c2ea7a3ebab841f5123091f0579601924711ab72738b0d1c05be228c1093cf15` |

C010r8 实测逐行等于 `p5/C010r8-package/inventory.json`，Game SHA256 为
`c6cb2334ad4c63093d1d9ebc5da439a540915d165f005c838b947ca5dad4a4ea`；
其 receipt 已明确是 R8 diagnostic 包，**不是已链接 R9 hooks 的包**。
旧 Coverage/Local 的 smoke 也不能倒签 R9 reader 运行。

三个实际 config 均 ui.enabled=false / scripts.enabled=false，
scene 类型均为 terrain×16、mesh×1、camera×1、directionalLight×1、environment×1。
16 TR 均已 strip paintPath 和 productionSourcePath。
mesh 实际指向 `Generated/HoudiniPegasusIsland/Water/SM_water_plane.json`，
环境为 `Common/Environments/sunset.exr`，不是 SM_cube 文件闭包。
无 animation/hair/blueprint/vegetation 场景对象。
当前 EngineLoop 的 UI Initialize/LoadFonts 受 uiSettings.enabled 控制；
ScriptRuntime.Initialize(false) 立即返回，不加载托管程序集。
这些排除只对该固定 config / 场景成立，改开 UI/scripts 或加载其它场景须重新审入口。

只读查询三个包全树及各自六级祖先的 Windows ReparsePoint 属性：
包树分别 694/702/694 项，均未发现 reparse。
这是审阅时刻的结构证据，不替代最终 trace 运行前后封闭性检查或抵御并发替换的证明。
未重新施加 ACL。

### 10.2 读取事件语义及已审入口

| 消费者 | 实际入口 / 可接受的事件证据 |
| --- | --- |
| config / renderGraph | FileSystem.ReadJsonFile、CommonFunction 两配置读取；原 stream 打开成功与 JSON parse 成功分离 |
| scene / TR / detail MI | WorldLoader.Scene、TerrainAssetLoader.TR/DetailMI 的 TracingIfstream |
| MI / M / T / SM JSON | RendererMaterialLoader、MaterialInstanceResolver、参数生成器、TextureAssetLoader、MeshAssetLoader 的实际流打开 |
| macro height PNG / 普通 stb 图片 | signature 用标准流；stb 实际 fopen 的同一 FILE，经 `_fileno/_get_osfhandle/GetFinalPathNameByHandleW` 获取真实 final path |
| sunset EXR | trace enabled 时以实际 TracingIfstream 构建 OpenEXR StdIFStream，使用同一解码输入；off 保留原 filename 解码 |
| DDS / KTX2 | TextureIO.ReadWholeFile 实际标准流；内存 decoder 不虚构额外 file-open |
| water OBJ / 可能的 MTL | TracingAssimpIO 委托 DefaultIOSystem.Open，包含失败打开；公开 IOStream 无 native handle，记录 requestedOnly |
| SPV / 通用 binary / VT manifest、页及 production 对象 | CommonFunction.ReadFile 标准流；ReadBinaryFile / RootedFileReader 保留原安全句柄及校验 |
| height atlas / composited dependency bytes | TracingIfstream，不改原 parse/校验 |
| optional WAV | AudioClip.Wav；即使无破碎对象，音频设备可用时 AudioRuntime 仍预载四个 WAV，缺文件失败尝试亦须记录 |

三个包均未安装 WAV。不能因没有 glass/balloon 对象就把
`balloon_pop.wav / balloon_inflate.wav / glass_break.wav / gas_leak.wav`
四个可达失败尝试从取证集合删除。音频设备初始化失败则不进入预载，
最终 run 必须按实际音频状态解释。

FileSystem.ResolveRuntimePath 保留 absolute 分支及 resource → shader/spv → project
优先级；CommonFunction.Path 保留原三个 if/exists 的次序与重复次数，
首候选命中仍会对同一候选执行后两个原 exists。私有重跑确认 found=3、missing=3，
没有通过去重“优化”改变语义。Resolve/ResolveResult 只说明路径候选/解析，
不是打开事件，不能充当无作者读取证明。

`StreamReadOpen` 的 success 表示实际流打开，不表示 parse/解码成功；
actualAbsolutePath=null / pathBasis=requestedOnly，不能证明句柄最终未跳转。
stb 只有真实同一 FILE 的 final path 取得成功才记录 finalHandle；
获取失败时降为 stream 证据，不能伪造 Native 成功路径。
RootedFileReader 的 nativeReadOpen 本身也可能 actual=null，
必须连同其随后 readerValidation(finalHandle) 和原有身份/祖先校验解释；
不能按事件名字一律认为已有 final path。

EXR enabled 流适配、stb、WAV、OBJ+MTL 的 CPU 等价测试只覆盖当前 MinGW 分支。
若启用 STBI_WINDOWS_UTF8 / MSVC 宽字符分支，不能沿用此次 fopen hook 覆盖主张。
offline modelProfile 分支、Cook Store staged writer 回读不在当前 installed Game
闭包；若 trace/run 实际进入这些分支，排除即失效。

### 10.3 已证实的两个 reader 闭包缺口

**阻断负证据资格：** 当前生产以下两处仍直接使用未观察的 `std::ifstream`：

1. `source/shader/build/shaderBuildManifest.cpp` 的
   `ShaderBuildManifestStore::Load()`。ShaderCompiler.Initialize 调用
   manifestStore.Initialize → Load，真实读取 shader-build-cache.json；
   Quality 的独立 ShaderCompiler 初始化也可走这条链。
2. `source/shader/build/contentHash.cpp` 的 `ContentHasher::HashFile()`。
   ShaderCompilerCache 的 TryUseCachedArtifact 在非 frozen sourceSnapshot 分支
   用它读取实际 GLSL dependency；ShaderCompilerApi 的源/依赖/输出身份核对也调用它。
   这不是 shaderc 编译 include 回调，**zero compile 不会排除此读取**。

因此不能把交接中“SPV及其他通用binary 已 hook”外推成所有 shader/cache 身份读取
均已观察。即便目标始终包内，未记录的读取仍使“全部尝试均在记录内”这个前提不成立；
scope=package 或 footer incomplete=false 不会自动发现漏 hook。

为确认不是只靠搜索命中，本轮在全新私有目录编译当前生产
runtimeFileTrace / contentHash / shaderBuildManifest / atomicFile 源码，
只链接已有 BLAKE3 静态库，读取实际 C009a：

```text
trace initialized                    records=2
HashFile(Footprint.glsl) succeeded    records=2
manifest.Initialize loaded86entries  records=2
ReadBinaryFile(config.json) succeeded records=4
final incomplete=false; traceEnd present
```

Footprint 的实际 BLAKE3 与包 cache 一致，manifest 得到 86 artifacts，
证明两次真实读取成功但没有新增事件；随后 Rooted reader 的两事件作为正对照。
这构成“footer 完整但 reader 覆盖仍不完整”的直接 CPU 反例。
本轮不改这两处实现，不签 reader 全闭包 PASS。

### 10.4 私有 CPU 重跑、footer 与成本

独立私有证据目录为 topic 下
`runtime-file-trace-evidence/independent-r9-reader-cpu-c29c8e866452459e9381ba2ecd74fcbf`。
11 项已有 R9 reader helper 在每项全新 fixture/trace 路径重跑，**11/11 pass**；
实际 C009a OBJ/EXR 和 16-bit height 精度测试只读原包。
初次独立启动缺 DLL，退出 `0xC0000135`，未执行测试；失败目录
`independent-r9-reader-cpu-937f9a05f01341ad98fcb261915db548` 保留。
随后仅给子进程 PATH 指定包内 DLL / MinGW runtime 后全项成功，
没有把首次装载失败计作测试 pass，也没启动 Game。

两个 default-off 测试不生成 trace，其余九份 JSONL 逐行重新解析：
首行 traceStart；正常末行 traceEnd；故障项末行 traceIncomplete /
incomplete=true；逐份验证 recordsBeforeTerminal 和前缀实际 byte 总长。
原始九份 `r9-jsonl-footer-verification.json` 所指 JSONL 也独立复算吻合。
成功私有 results.json SHA256 为
`5f3e90b876f7f443475b55ade0410812a8960e5165d02fc54344b86c8a218e32`。
生产反例源码 SHA256 为
`12952b27ccea6dd6c60f53e0508abd996d96b838ab4d30d617934f26b7471a80`，
exe 为
`996b2c9a4edc17781c9a8ba36940167779d34f8e6e6afc2444ffead74320d940`；
源码、编译日志、运行日志和 gap-trace.jsonl 全留在同一成功私有目录。
19 个生产文件在本轮检查结束仍与交接 hash 相同。

CostQuote 为 CPU **2,179,432 B** / GPU **0**：
fixed 82,280 B（State 82,272 + 两个 helper atomic 共 8 B）加
2,097,152 B workspace。16 KiB record / 64 KiB FILE buffer 已在 fixed 内，
不重复计账。workspace 是保守预留，不是 allocator peak；
knownStringHeapCapacityBytes 只核已知 root/output 字符串，不是全部 heap。
源码可见一次 process lease 的 outer-owner 接线及失败 MarkIncomplete；
交接时该集成仍 pending，**本轮未验证最终同版本链接、实际 reserve 生命周期或峰值**。
早于 Service 的 config 读取仍需在最早可用 owner 补齐一次成本；
跨 World/retiring 不得重复绕开全局上限。CPU trace 采样也不覆盖初始化、
所有 helper/native 查询和完整 frame 开销。

### 10.5 最小仍需的原生负证据资格

当前只接受表中已审入口的有限 CPU 语义和可检测的 observer 故障行为。
后续至少需要以下可审结果，不能单给 bool scope/fullCoverage：

- 先闭合第 10.3 节两个实际 reader 缺口，或明确缩小取证问题且不再主张完整
  engine-controlled-asset-opens；对最终 reachable calls 再核覆盖，不能拿该旧
  11 测试集合当作全集。
- 正常 R9 同版本编链并绑定 source / bin / shader / cache / publication，
  导出全新包；C010r8 不是 R9 版本证明。保留构建/链接与 inventory，不 patch 旧包。
- 实际 worker1/2、readonly 安装取证，与初始 config 读取至正常退出全程绑定。
  外部 cwd 启动后 application 会切至 exe 包目录，不能宣称全程 cwd 在外部。
  包内无 reparse、安装输入闭包封闭且 readonly、before/after inventory 与 ACL
  恢复应有实际证据；requestedOnly 的范围据此受约束，而非伪装成句柄 final path。
- raw JSONL 保留成功和失败尝试，校验唯一 start、终止 footer、完整记录/byte计数、
  error/domain、最终 Stats.enabled=true / incomplete=false；Flush/退出写失败、
  record cap、缺尾、路径/observer 溢出均不得接受。单有 traceEnd 不证明没有遗漏入口。
- RootScope 只是注册最长前缀分类；逐条核实际绝对路径/操作/reader，
  包外 sourceauthor/cache/project 不能因换个分类名字被放行。
  optional WAV 失败尝试、Rooted readerValidation、stb finalHandle、EXR/Assimp
  requestedOnly 必须按不同强度解释；UI/scripts/Paint 排除要绑定实际 config/TR。
- 正式 trace 成本纳入冻结总账；真实私有内存/帧成本另测。
  Shader 编译第三方读取未覆盖时必须以实际 zero compile 及 readonly cache 限定，
  但这不免除第 10.3 节 warm-cache 自有读取。

即便上述闭合，结论也仅可表述为明确闭包下的引擎控制读取范围；
不能称 OS-wide/kernel trace。当前仍没有 R9 安装运行的完整负证据，
**no-external-author-opens 未证，整 P5 不放行**；
53 poses、正式性能/WDDM、fault、P4 等其它缺项继续保留。

## 11. R9 warm-cache 两 TU 漏 reader 整改独立复验

日期：2026-11-21。后续复验，第 10.3 节保留为整改前事实。
本轮只读生产源码及 C009a，在全新私有目录编译/运行 CPU 工具；
未改 production、CMake、包或预算，未调用共享 build、Game/GPU、ACL。

**结论：第 10.3 节两个具体漏洞有限关闭。**
这不是 metadata/resolve probe：已再次实际读取 C009a 的同一 include、
加载实际 86 项 shader cache，观察到两次原来缺失的真实 StreamReadOpen。
公共 API/报价边界和 requestedOnly 限制不变，完整 R9 native trace / P5 资格仍 pending。

### 11.1 生产身份与变更范围

读取 `../runtime-file-trace-evidence/r9-warm-cache-fix/receipt.json`，
其 SHA256 为
`b4445d4d5c5484ef7f0904fd0c2a7dda0a0376be6569135f67a56946ca120af1`。
独立复算两个当前生产 TU，均等于该 receipt：

| 生产 TU | 整改后 SHA256 |
| --- | --- |
| `source/shader/build/contentHash.cpp` | `12637b4870c48a5578364102092836711953875f5e39e6da5cc955d85be1c902` |
| `source/shader/build/shaderBuildManifest.cpp` | `ce842933f2f75bbda2b4e56834b0c946606029ac01fbc80461b15970a3a6c2ce` |

源码核对：两个原 ifstream 构造分别改为 TracingIfstream，owner 为
`ContentHasher.HashFile` / `ShaderBuildManifestStore.Load`。
原 binary/in 模式、读取/hash/parse、缓存失效与错误路径保持；
不把 JSON parse 成功与实际文件打开成功混为一谈。
没有另开探测句柄代替真实读取。
前轮 19 文件在复验前后均与其原 production receipt hash 相同，
公共头和 CostQuote API 未改，不能将本轮加法修复误写为改造全部 reader。

### 11.2 原反例实际重跑与原生编译

新私有目录为 topic 下
`runtime-file-trace-evidence/independent-r9-warm-recheck-20261121-001`。
沿用原反例的实际调用顺序与同一 C009a 输入，
只将整改后 probe 的接受条件由“reader 未增加事件”改为
Hash +1 / manifest +1 / Rooted +2；保留原工具/旧 binary 不覆盖。

本轮独立编译链接当前生产 runtimeFileTrace、tracingIfstream、
contentHash、shaderBuildManifest、atomicFile，链接已有 BLAKE3 库，
使用 `g++ -std=c++17 -Wall -Wextra -Werror`，exit0、无告警。
命令和完整输出在该目录 `build-command.json` / `build.log`。
没有调用普通工程共享构建或修改生产源码。

| 本轮实际重跑 | 起始 | HashFile 后 | manifest Load 后 | Rooted read 后 | 取证覆盖判断 |
| --- | --- | --- | --- | --- | --- |
| 原始整改前 binary | 2 | 2 | 2 | 4 | 两 reader 仍漏，FAIL |
| 当前生产源码新编译 binary | 2 | 3 | 4 | 6 | 两 reader 均新增真实事件，PASS |

旧 gap 工具的 exit0 表示其“旧漏洞存在”断言成功，不是覆盖通过；
新工具 exit0 才表示上述增量条件通过。
两次均实际返回 86 artifacts，读取 config 1004 B，
同一 Footprint.glsl 的实际 BLAKE3 均为
`0b3724b7b8b5a3fc9141a46da0e4ebd9f2ea950ef72171616f9eff16383b44a2`，
未因修改输入或用内存占位值消除反例。

新 raw JSONL 含 owner=ContentHasher.HashFile 的实际 include 打开和
owner=ShaderBuildManifestStore.Load 的实际 shader-build-cache.json 打开；
均为 event=streamReadOpen、success=true、operation=read、
requestedAbsolutePath 精确落在 C009a 安装路径。
actualAbsolutePath=null / pathBasis=requestedOnly，**没有升级成同句柄 final-path 证据**。
Rooted reader 的原 native open + readerValidation 正对照继续存在。

独立逐行解析 raw/footer：新 trace 总计 7 行，
末行为 traceEnd、recordsBeforeTerminal=6、bytesBeforeTerminal=3384，
与实际前缀行数/字节精确相等，incomplete=false；
costQuoteCpuBytes=2,179,432，GPU cost=0。
不能由这份小型 CPU trace 外推最终 Game 全闭包没有漏读。

### 11.3 四项 CPU 回归及证据绑定

检查 helper 源码后，在独立全新 fixture/trace 路径实际重跑
`warm_trace_after.exe` 的四项 CPU 测试，**4/4 pass**：

- 实包两个 reader 每次各增加一次真实 open，digest/cache 行为不变。
- trace 默认 off 时 hash/cache 等价，记录数为零，不生成 trace 文件。
- HashFile 缺文件时保留原异常，同时记录 success=false 的真实 StreamReadOpen。
- corrupt manifest 已打开时记录 success=true，原 parse 失效仍为 manifest corrupt，
  不伪称读取或 parse 全部成功。

九次/十一次测试的旧计数不在本轮重复累加；本轮是两版原反例加四个定向回归。
回归所用 helper SHA256
`0545499a4201f84058d5f6910c77d7af2802ab04a3e274230961d044b086b5a1`
与整改收据相同。所有生成的正常 trace 都独立校验 footer 计数/bytes、
traceEnd/incomplete=false；default-off 不生成 trace。

私有证据 SHA256：

| 文件 | SHA256 |
| --- | --- |
| 新编译 `reader-gap-probe-fixed-gate.exe` | `17f363b20fc52a9b4e296571813be034faf6e19d041c25a250e38f6afe095a0e` |
| `production-after.jsonl` | `8a967b1f4ecf309d6a15ed186ac21fc7a3ab56e6c3506430233ad9c5f38385af` |
| `independent-counterexample-results.json` | `0e1b56423a832211eb88f51b50aa416f6d67c8c12be0db31a4518a1be35775d1` |
| `independent-four-tests.json` | `8974b8948885069e2b4718b1dc6a1310b1bea2b0239f69ded673c65ab0fd823a` |

### 11.4 关闭范围与仍待原生证据

**接受：HashFile 和 ShaderBuildManifestStore.Load 的两个已知漏 hook，
在上述固定生产 hash 下已由真实读取及失败/parse 反例有限关闭。**
第 10 节的完整 reader 覆盖不能靠 bool scope / Resolve /
requestedOnly / footer 单独自证的边界继续成立。

仍需 Euclid 正常同版本 R9 source/bin 编链、重新导出新包，
再进行实际 worker1/2 readonly raw trace、封闭无 reparse 包、
最终 Stats/footer 完整性、路径分类逐条审核及预算租约/成本核验。
本轮没有执行这些原生运行，也未替换任何既有包二进制；
**no-external-author-opens 和 whole P5 仍 PENDING**。
Coverage/Local 已证初始子项保持原范围，53 poses、正式性能/WDDM、
fault、P4 缺项不因两个 CPU reader 修复而关闭。
