# RVT P1 首次真实 Full16 发布独立审计

日期：2026-11-21，Asia/Shanghai。验收者未参与本次发布实现。
**结论：run006 actual publication 的 Store 结构、实际文件完整性子项通过；
已发布 native MI 合法性不通过；P1 整阶段 pending，不签整阶段 PASS。**

本记录依据[生产计划](../plan/rendering/terrain-runtime-virtual-texture-production-plan.md)、
[P0 独立冻结审计](terrain-rvt-p0-independent-audit.md)、
[P1 已有独立审计](terrain-rvt-p1-independent-audit.md)及实际文件。
不重复已有 121 pass / 3 skip 文件 API 审计，不将其计作本次新执行。
未启动 GPU、renderer、Cook 或 Game，未使用 `build-vt-release`，
未修改项目源码、资产、配置及实际 publication。仓库内本次只新增本文；
独立 CPU helper、报告及负例副本留在下述精确临时目录。

## 对象与身份

工作树：
`D:/YYBWorkSpace/GitHub/VulkanLearn-worktrees/terrain-openworld/VulkanLearn`。

证据目录：
`artifacts/terrain-rvt-production/cook-world-chunk-coverage-run006/`。

实际发布：
`../p5r/CoverageAtlas006-final/VT/775359b06fde29e5/manifest.json`。
核对的是这个实际文件及其同目录闭包，不以 stage-ready、计划中的包或另一轮报告代替。

| 身份 | 独立复算结果 |
| --- | --- |
| receipt.json SHA256 | `16829260f9c9474180361ff424089d29ebf3e05647b5475f75f4886bcfbb0089` |
| manifest SHA256 | `e3a1d8319e139c6697d54b43c73c3d9479f2f533d762c116ce993915d8c90cfd` |
| manifest BLAKE3 | `52389ff57f47148565d9e2c161b3c11967f0439e05f82747265c79d040498e0a` |
| package SHA256 | `9c1a5bcf9902e17e143683bdea25489edc8f5bc0ee89516b5e50bb1f5de17171` |
| package BLAKE3 / 内容地址文件名 | `b0ffb1d3a43350850328c5406ca3b394b9b2b69481b10657bb6fc86da4570f8d` |
| P0Frozen SHA256 | `43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd` |
| snapshot-006-final lock SHA256 | `ab0e7794af6a69a3952574ad08ac5ed9f4ca19e6339f6f0d9ef3dea6b1a72ce1` |
| run006-controlled-link.json SHA256 | `3140743b6193ddcfbaf4028e71ba570e29d5caa57d3ae2947b07c5c339dabb02` |
| 实际 editor SHA256 | `b187d147fa00a08ad09e26621baea13fbd58f968629c9fbbb437e99f8a462346` |
| stage game SHA256，仅核 bytes | `dea6a83b4ce30a103da26a3d4e533b327c9b2de9588cd46307f4b13d606852a5` |

上述摘要与收据、launch、stage 记录相符。实际 stage shader cache 的事后 SHA256 为
`fbdf36515c8f3bcb282b4a2d262f35c5c3f383d6d8d41743943e5057de73f925`，
与收据一致；事前值来自 launch 记录，本次没有重建历史时刻。
摘要证明文件内容绑定，不是运行正确性或第三方时间戳证明。

## Store 完整性已证明范围

独立遍历实际目录，并逐一复算 inventory 中全部 **100 文件、490,973,049 B**
的尺寸和 SHA256，差异 0，额外文件 0。规范 JSON inventory SHA256 为
`d54af526801efb685c1415db1ac4e2ad4b49065de3214d259a8c14cdd2570bac`，
与收据一致。receipt 引用的七项 artifact SHA256 全部一致。

独立复算整包和全部 448 个页面区间的 BLAKE3，差异 0。
另独立枚举预期 `(surfaceId,mip,x,y)`，与实际页键逐一比较：

| 项目 | 实际结果 |
| --- | --- |
| surfaces | 16，Landscape_00…15；8192×8192，gen3，三通道 raw RGBA8 |
| packaged pages | 448 个唯一键；mip4=256，mip5=64，mip6…13 各16 |
| roots | 每 surface 一个 mip13 / (0,0)，合计16，全部在包内 |
| page payload / package bytes | 99,434,496 / 99,434,512 |
| production objects | 66；Image=29、ShaderSource=21、ShaderArtifact=4、RawAuthorSource=12 |
| object payload bytes | 390,809,921 |
| author dependency records | 每 surface 70，合计1120；所有 productionObjectIds 均可解析 |
| adjacency | 48 条有向连接；协议被原生 closure Validate 接受，未做 GPU 边界质量证明 |

独立调用既有 `artifacts/terrain-rvt-production/validate-actual-publication.exe`，
对实际 manifest 执行 `Store::Open` 和 `ValidateAll`，exit0。
除 manifest 路径字符串外，报告与原 `store-validate-all.json` 完全一致。
原生 ValidateAll 验证全部66个对象的实际尺寸/摘要、页面和包摘要；
不是只相信收据上的布尔值。

**执行独立，Store 实现编译不独立。** 本轮复用既有支持 kind3 的原生 validator，
未从源码独立重编 Store/Production，也没有完整应用重链。
validator SHA256 为
`f8e8d2abd89e47a955a516b808edbda975cce7830b8860f7ad7bf9f825b379d3`。
独立 SHA256 由 Node 标准库计算；页面/整包另用既有 CPU BLAKE3 stdin helper 复算。
一次对全部输入进行额外 stdin-helper 扫描超时，没有把该未完成扫描计为通过；
66个对象的完整 BLAKE3 证据来自本轮实际执行的原生 ValidateAll。

## Author provenance、基础页身份与预览的边界

12个 RawAuthorSource 的 kind 为3、image=null；
generator 的 computeSource、includeSources、compiledArtifacts 不引用任何 kind3。
这是作者来源的实际归档及 provenance，不是基础页 generator。
shader kind 检查要求源码为 kind1、编译产物为 kind2，不能以新增 kind3 绕过。

整体 production hash 为
`5e890cbfc3f61c7e6b04698d24eab92fcb6ddc5322335e2575da79a9d04ac20a`。
`ContentHash` 与 `SurfaceContentHash` 负责不同身份：
后者在求基础页身份前清除 authorDependencies、替换 runtime MI 路径、
剔除基础页不消费的动态输入，并只保留实际使用的基础输入与 generator 对象。
不应因 author provenance metadata 改变就要求基础页 hash 改变。
本次原生 Open 重新验证了16个 surface 的 dependencyHash 与派生身份一致。
对 author dependency digest 的单独篡改被整体 production identity 检查拒绝。
这不等于从当前作者目录重算所有来源或证明作者到派生像素的转换正确。

“实际字节相同”有本次文件摘要与 Shader/MI 逐项验证；
仅看到同一 generatorVersion、同一路径或 preview label 不足以建立身份。
对象/surface 记录重排在副本上仍通过，证明该有限重排不改变可接受内容身份；
页面记录同时倒序则被 offset/overlap 规则拒绝，不能泛称任意 JSON 重排均合法。
本轮没有执行 Editor preview、迟到 revision、COW 或 status/fence 发布流程，
不以静态内容身份替代 generation/revision/task/epoch 的运行时身份验收。

发布源码顺序可见：校验候选 package 和输入，发布内容寻址包，再校验，
保留输入 reader 至最后 `WriteTextFileAtomically(manifest)`。
本次确认的是最终 manifest/package/input 的一致绑定；没有重演发布失败、
断电或并发更新，不将现存文件的一致性称为完整原子事务实测。
文件 API 的既有独立覆盖边界仍以原 P1 审计为准。

## 两个 active shader 的实际闭包

实际 stage cache 只有本轮使用的两条相关 StageEntry：
`terrainCompositedBakeWorld.comp` 和 `terrainCompositedBakeWorldPadding.comp`。
对照 cache 主源/传递依赖/输出 digest 与包对象，直接比较实际文件 bytes：

- World 主源及5个 include 分别对应 shader-29…34；
  debug/runtime 输出分别为 shader-35/36。
- Padding 主源对应 shader-37；debug/runtime 输出为 shader-38/39。
- 共11项实际 bytes 全部相等；7个源文件 SHA256 也均匹配冻结 snapshot 清单。

没有把额外 author-shader 文件或 RawAuthorSource 算作 active generator。
本检查确认 ShaderWorld + Padding 两 compute 的真实产物闭包；
不执行 shader，不证明设备准入、GPU 产页数值或性能。

## 确认问题：全部32份发布 MI 违反原生 sparse override 标准

16个 surface 各 base/detail 一份，合计32个 runtime MI：
均可从 **manifest.parent_path()** 解析到普通文件，
文件内容 BLAKE3 均等于 `runtime/<digest>.json` 的文件名。
Service 载入和 exporter 的 `TerrainPackageClosure::Open` 当前都使用这个基准；
不是 resource root 直接拼接 `runtime/...`。
所有 MI 引用 `shader/glsl/M_terrain.json`。
文件存在、内容地址正确和 Store ValidateAll 通过，不代表 MI 语义合法。

本轮原生 CPU 检查调用：
`MaterialAssetValidator::ValidateDefinition`、
`ValidateInstanceHeader`、`ValidateInstanceOverrides`；
M_ 来自实际 run006 stage，MI 来自原 publication。
**32/32 失败，0通过**，均报：

```text
Empty material instance textures object must be omitted: runtime/<digest>.json
```

具体标准不一致：检查时 Cook `archiveMaterial` 写出 `textures={}`，
而 `materialAssetValidator.cpp` 的 `ValidateSparseOverrideObject` 明确禁止
显式空的 textures/macros/parameters/renderStateOverrides 覆盖对象。
无纹理覆盖应省略 textures 字段；生产快照预绑定纹理不豁免 MI 的格式规则。
该失败发生在 `ValidateInstanceOverrides`，Header 检查只要求已出现的 textures 是 object。
本次不放宽 validator，也不修改发布件。

仅在 helper 内存中移除 textures 后再调用相同三个入口，**32/32通过**，
没有观察到被首个错误遮住的第二项这组校验错误。
这是定位诊断，不是修复后的冷包、实际材质加载、reflection 或 Game 闭包通过。
并行实现者可以继续根修和重新 Cook；本记录结论始终绑定上述旧 manifest 摘要。

MI helper 仅新编译临时调用程序，链接既有
`artifacts/terrain-rvt-production/p3-noether-variant/source_material_validation_materialAssetValidator.cpp.obj`，
未重编 validator 源码，未链接/创建 Vulkan 设备。
对象 SHA256 为 `fe4484ed47df7556bf532823fc9347748700088811422a78021514678d311eb4`；
已有 build.json 记录的 validator 源码摘要与本次源码实际摘要均为
`ea5ba561729e2e2aa58fd91f66341d144d3d11071752b43d6de35993fcfb5735`。
这是复用原生对象的执行证据，不冒称独立源码编译。

用户本轮报告：首次 Game 因旧 kind2-only parser 失败，换新对象后第二次真实 Game
因上述空 textures 失败，已交 Euclid 根修并重新生成冷包。
这两项 Game 进展是用户交接信息，本验收者没有运行 Game。
本轮32份实际 MI 的独立 CPU 失败与第二项报告一致；
目前没有有效的 Game input closure PASS。

## 副本负例与复现材料

精确新目录：
`D:/Temp/vl-p1-publication-audit-20261121-97a7267a981449eaa75ff98110b98545`。
`copy/` 是100文件、约491 MB的普通完整副本，未用 hardlink 共享可写对象。
未删除任何原始或已有审计目录。

| 本轮副本检查 | 结果 |
| --- | --- |
| 原样 copy Open/ValidateAll | exit0 |
| author-raw-54 等尺寸翻转1 byte | exit1，`VT production input integrity mismatch` |
| Landscape_00 mip13 根页等尺寸翻转1 byte | exit1，`VT page integrity mismatch` |
| 恢复这两项原字节 | exit0 |
| 将 kind3 对象指定为 computeSource | exit1，`shader object kind mismatch` |
| 单独篡改 author dependency digest | exit1，`production identity mismatch` |
| 仅重排对象和 surface 记录 | exit0 |
| 同时倒序页面索引 | exit1，`Invalid VT page offset/size/overlap` |

本轮结束再次重算原 publication 全部100文件的尺寸/SHA256，差异0。
负例证明 Store 对实际坏字节的检测，**不是 runtime 根页重建或坏输入失败分类实跑**。

临时 `actual-store.json` 是实际原包独立 CPU 结果，
SHA256 `6ba6b0db11ad07784c4f856161f4f8b034bc5e3f566f4cd60f7fb4e2fe6b7db5`。
`mi-native-results.json` 列出全部32个精确路径、角色与两次检查结果，
SHA256 `dd93d468f55c2edb5e1ee3e5cf354909542482d23c22596a967497467c46af3b`。
`mi_audit.cpp` / `mi_audit.exe` SHA256 分别为
`e74d79334373bb56cd41bb3a57b3618dcb764d55ecb06782a0261ca269e863b5` /
`037ee2d6b9fb7448d9066e5245b3fe37edb780fa67f2f65daeb8d72a235292cb`。
另保留 `additional-cpu-checks.json`、`final-identity-check.json`；
前者的页面倒序失败不被后者的有限重排通过覆盖。

## Readiness、性能及阶段剩余门槛

收据明确 `qualityAccepted=false`、`performanceAccepted=false`、
`gameExecuted=false`、`localCookExecuted=false`。
原 stderr 是 `RVT_QUALITY_INCOMPLETE: Graph feedback depth was not created with sampled usage`；
stdout 的 `world_frame_arrived` 不是 accepted/interactive 就绪证明。
`readyForDiagnosticLaunch=true` 只代表诊断可启动。
本次不把 diag exit0、根页齐备或 Store ValidateAll 解释为 readiness accepted。

当前 P3 数值/性能 known failure 留在
[P3 独立审计](terrain-rvt-p3-independent-audit.md)及执行证据中，
不因为它们把这里已经证明的 P1 Store 结构判为错误，
也不因结构通过消除 P3 问题或性能 false。

P1 继续 pending，至少还需：

1. 修复 MI 标准不一致后重新生成实际冷包，重新绑定 manifest、对象、MI、
   native validator、shader 与冻结配置身份；新包不能继承旧摘要的子项结论。
2. 实际 runtime 根页损坏但生产输入有效时的重建、提交及 readiness 门控；
   输入损坏时明确失败分类。CPU 检出坏字节不能代替这两种运行时行为。
3. CoverageAtlas / LocalLayerArray 两种权重的独立离线 Game：
   无作者目录、无工作区路径依赖、真实加载及未访问区域生产；
   当前没有有效 Game closure PASS，Local 也未在本轮执行。
4. 预览与发布、revision/COW、status/fence 及真实 World 替换的完整执行证据；
   本次有限静态身份验证不关闭这些门槛。

本记录只接受 run006 的已列 Store/实际文件/Shader 子项，
并明确记录32份发布 MI 的具体阻断；不批准整阶段或最终交付。

落盘后执行 `py -3 tool/validate-docs.py`：exit0，149条路由、
94条生成规则、720个树内相对链接通过；224个出仓链接仅提示，
未将它们记为本轮已验证。
