# 完整飞马岛标准地形切换：独立审计

> 状态：非契约、当前为 partial / pending。用户新范围是完整4×4岛屿、
> 唯一合成VT生产路径与后续标准地形；旧2×2闭环不代表本次完成。
> 日期：2026-11-20，22:32 Asia/Shanghai。
> 本轮只运行独立build中的CPU测试与静态检查，未启动renderer/Cook/headless GPU。
> 本文写集独立于主线程的 `terrain-full-island-cutover.md`。
>
> 22:53更新：源码/CPU最终结果及标准路径S4见§6–8；原§1–5为22:32阶段快照。
> Full16 Cook已由主线程报告全通过；新Reader/service/exporter版本的Runtime16、
> 交付包及成本仍等Maxwell原始收据，不沿用旧2×2完成结论。
>
> 23:14更新：新Reader Runtime16数值/收敛报告已独立核对（§9）；
> cross截图硬边仍可见，CPU中央共享edge/实际Cook定点核查见§10，
> 不以运行passed自动关闭当前截图门禁。

## 1. 当前逐项状态

| 项目 | 判定 | 当前证据与限制 |
| --- | --- | --- |
| 旧层源VT生产接线清理 | proved（源码范围） | 旧manager/visibility/mip-budget/层页上传/旧feedback编码及专用测试删除；RenderSystem/frame/coordinator旧调用移除；coverage/collision/catalog/真实源数组保留 |
| 合成feedback独立命名/协议 | proved（源码与CPU） | `terrainCompositedFeedbackGpu.*`、`TerrainCompositedFeedbackGpuRing`、graph四槽`terrainCompositedFeedback0..3`一致；五附件布局保留；尺寸常量迁至composite模块 |
| 强制TR与MI标准 | proved（本轮CPU范围） | 缺compositedVT、disabled、空package拒绝；退休MI参数/绑定拒绝的生产更改由schema owner完成；本轮terrain suite包含相应新规则 |
| 后续新建/导入producer链 | pending（整链） | 主线程已报告导入renderReady=false和显式Cook流程；本审计尚未取得新建/导入→保存→Cook→默认EditorReady的本次整链收据 |
| Full16作者数据布局/静态预算 | proved（输入/估算范围） | 实读16 TR、32 MI，8192/192、pageTilesPerSide8、grid512；global coverage占用1024/1024，7去重层、1 sourceSet；没有运行结论 |
| 完整目标构建 | 主线程已报告通过，独立CPU目标通过 | 不将主线程Release结果伪装成本审计另一次主目标构建；未触碰build-vt-release |
| Full16 Cook及默认EditorReady | pending | 等16面完整新hash、Cook退出/失败处理、默认编辑器初始世界与可编辑状态收据 |
| Runtime16移动/跨tile/World退休 | pending | 需新范围实机；不复用2×2运行或run03小fixture判整岛通过 |
| Full16包/冷启动/成本 | pending | 等最终岛屿成品，含inventory与实际目录计量、部分GPU计时限界 |
| 标准路径下S4实际编辑 | pending（Full16关联） | 既有weighted finalshader02是此前Near100m实际编辑证明；不替代本次标准schema/岛屿配置关联后的Editor退出证据 |

## 2. 本轮独立CPU验证

源与主线程相同worktree，使用独立build，未修改生产代码或测试断言：

- Terrain build：`artifacts/terrain-renderer-cleanup-cpu-build`；
  首次GTest发现阶段缺DLL搜索路径（0xc0000135），在本进程PATH加入已存在的
  Release/prebuilt DLL目录后重链成功。没有通过修改源码掩盖构建问题。
- Backend build：`D:/Temp/vl-vt-cleanup-cpu-20261120`。
  首个较长路径build的`.obj.d`写入失败；更短独立build成功。
  这两次基础设施失败日志保留，最终结果来自实际成功二进制。
- Terrain实际运行 **432 tests / 46 suites：429 passed、3 skipped、0 failed**，
  18.822秒。删除旧专用测试后不能继续声称原523项都运行了。
- Backend实际运行 **297 tests / 57 suites：297 passed、0 failed**。
  过滤器为
  `-TerrainLayerAtlasGpu.*:TerrainFeedbackReadback.*:BufferToImageRegions.*`，
  明确未运行这三组共8个headless Vulkan测试，以保持Maxwell独占GPU。
  不称backend全量305项已实跑；GTest自动发现只列举，不创建GPU工作负载。

Terrain三项跳过逐项为：

1. `TerrainCompositedDependenciesTest.RejectsCanonicalSymlinkEscape`：
   symlink creation unavailable / Function not implemented。
2. `TerrainCompositedRealAssets.CollectsPegasusAndInvalidatesEditedNestedShaderSnapshot`：
   未设置`VL_TERRAIN_DEPENDENCY_REAL_FIXTURE`，不能写成真实资产项通过。
3. `TerrainCompositedVTStoreTest.SymlinkPackageIsRejectedWhenPlatformAllowsCreation`：
   同类符号链接能力限制。

合成feedback未因删除旧测试失去覆盖：保留4项encoding、7项decode，
含floor/ceil配对、clear/tag/version/reserved、去重、完整buffer、
copiedEpoch和consume-once；Residency29项、World顶点队列/生命周期、
fallback/预算选择等CPU测试仍在本轮terrain432中。
Backend保留graph feedback输出映射、FrameEpochRing3项、
资源退休/提交边界等CPU验证。CPU证明不等于Full16实际GPU采样/退休已通过。

原始日志/机器输出：

```text
artifacts/terrain-renderer-cleanup-terrain.log
artifacts/terrain-renderer-cleanup-terrain.xml
artifacts/terrain-renderer-cleanup-terrain.exit.txt
artifacts/terrain-renderer-cleanup-backend.log
artifacts/terrain-renderer-cleanup-backend.xml
artifacts/terrain-renderer-cleanup-backend.exit.txt
artifacts/terrain-renderer-cleanup-tests-configure.log
artifacts/terrain-renderer-cleanup-tests-build.log
artifacts/terrain-renderer-cleanup-tests-rebuild.log
artifacts/terrain-renderer-cleanup-short-configure.log
artifacts/terrain-renderer-cleanup-backend-build.log
```

## 3. 源码清理与静态残留检查

`artifacts/terrain-renderer-cleanup-static-scan.log` 为空、rg退出1（无命中），
覆盖source/render、source/terrain、renderSystem、config、
terrain/backend测试中的：
`TerrainLayerPageManager/Feedback/Visibility/VT/Upload`、
旧对应文件名、`layerVTRequested/layerPageManager/layerUploadRing`、
旧requested capacity/force-mip接线。
此为明确字符串/调用范围检查，不声称全仓所有文档历史字样都清除。

保留的是合成构建/detail/TES真消费者，而非旧基础路径：

- pageManager/coverage ID与weight生成、上传及回读；
- World级layer registry/catalog/globalID→source slice；
- 源albedo/normal/ORM/height及其真实detail/macro和bake消费者；
- 合成四附件ring沿用覆盖页`TerrainFeedbackRingState`的fence/tag/epoch；
  不再依赖已删除的旧layerFeedback模块。

原layer source-set记录仍有“单atlas格式组”的过期解释/约束措辞；
当前是源数组格式/映射验证，已无层源物理atlas分配。此文案/未来多格式能力
不是当前Full16单sourceSet的阻断。本轮遵守冻结要求，不再修改production。

## 4. Full16预算估算：1GiB不等于总显存上限

机器计算见 `artifacts/terrain-renderer-cleanup-budget.json`，
可重放脚本为同名`.py`。输入实读Full4x4_R8192_Default全部16 TR/32 MI，
不由旧2×2文件数量乘算替代输入检查。以下是payload/准入估算，**不是运行测量**。

每面：128+4 border、136²×RGBA8三通道，pageBytes=221,952；
8192²完整14级、5468页，192个物理槽：

| 项 | bytes/面 | 16面 |
| --- | ---: | ---: |
| 三通道物理材质页 | 42,614,784 | 681,836,544 |
| GPU页表64²×14×RGBA32F | 917,504 | 14,680,064 |
| 独立coverage table/ID/weight | 8,388,864 | 134,221,824 |
| 常驻height+LUT估算 | 739,952 | 11,839,232 |
| Game服务payload合计 | 52,661,104 | **842,577,664（803.54MiB）** |
| Cook额外normal/tangent frame图513²×32 | 8,421,408 | 134,742,528 |
| Cook服务payload合计 | 61,082,512 | **977,320,192（932.05MiB）** |

height估算前提：当前7层中5个hasHeight层、相同mip2布局与7-entry LUT，
从旧实际布局739,952 bytes/面推导；Full16 Cook sidecar实际值仍需核实，
不能宣称已测得16面高度显存。

当前Service GPU cap=1,073,741,824：
Game服务payload余量231,164,160 bytes，
Cook服务payload余量仅96,421,632 bytes。
每面TR的8MiB height预算先保守准入，随后按实际Capture再次核算；
没有发现16面本身触发该准入的确定错误。

### Source与通用覆盖页不在该Service cap之内

32份MI的layerAlbedo/Normal/Pbr/Height/WeightMaps、globalColour/globalMasks
各自都只有一个不同源路径，理论应加载成7个共享源Texture，而非16份全数组。
同sourceSet登记用shared_ptr身份验证共享，Service stats按Texture地址去重；
但native缓存/World替换是否实际复用仍须Runtime的sourceTextures/sourceBytes证明。
不要把“每个MI引用相同路径”直接写成实机显存去重已经通过。

参考此前7共享源payload=186,696,804 bytes（旧范围测量，非Full16新实测）。
此外coordinator仍有World全局4096² ID/weight双atlas及32²页表：
**134,221,824 bytes**，保留供coverage；Service自身另有16份独立coverage，
这两份有真实所有权，不能漏账或混称已经去重。

按上述源参考相加，Game服务+源+通用coverage约
**1,163,496,292 bytes（1.084GiB）**，已经超过1GiB cap；
这是cap仅限Service cache的设计范围，不是准入逻辑已超自己的cap。
该和还未含几何（Editor三槽顶点）、描述符、bake工作buffer、
Vulkan对齐、shadow/postprocess/环境图及driver。实际PID/WDDM峰值必须另测。

### CPU/staging、IO和队列隐藏倍增

- 每面CPU页payload保留4MiB，frame ring若为3，每面预留4MiB×3 staging；
  16面合计**256MiB**，不是全World只预留12MiB。
  当前512MiB cap能够覆盖静态准入，但还不是进程总内存cap。
- 最后一面Cook准入的声明工作区约325,373,024 bytes
  （前15面预留+当前索引/页工作区/覆盖frame输入），在512MiB内；
  JSON/STL、CPU高度/Paint、native纹理解码和已开Store索引不由该估算穷尽。
- 各Residency最多2个async read，Full16最多**32个并发文件读取**。
  每帧Service共享额度仍为4MiB/16页/2ms，按surface轮转，
  没有每面变成4MiB的上传倍增；时间只控制后续调度，不抢占正在执行的IO/GPU。
- 每面素材包1,213,633,552 bytes，16面合计**19,418,136,832 bytes**，
  只算材质包，不含作者源/coverage/height/历史版本/临时产物。
  单面4GiB限制不会被触发；磁盘总空间和打包临时副本不能按单面上限忽略。
- Store Open会全包hash并逐页校验；game登记路径当前会打开Store取得identity，
  随后正式登记再次Open。单进程启动的读取量不能只按baseline一页算，
  完整岛可能数十GiB校验IO；需记录冷启动耗时/读IO，不把慢启动等同shader miss。

### 全局页空间与设备/描述符

- 16×8×8=1024个虚拟coverage区域，当前pageOrigin恰好无重叠占满32×32；
  usable physical slots当前也是1024，没有旧fallback保留槽。
  **没有余量再塞第17块同规格terrain**，标准新建/追加World必须明确失败或另配布局，
  不能默默挤占。Full16本身没有发现越界/重叠。
- composite反馈支持256面、8192上限、mip0..13；每面192小于Runtime2048槽上限，
  当前7个global layer小于256，单surface只支持一个sourceSet，Full16符合。
  没发现生产中“最多16 surface”的硬限制。
- atlas是image array每面192层、extent136，不是16×192层合并成一张超限array；
  材质schema当前18个sampler，跨pass/stage实际active集合仍受设备limits约束。
  object资源按swapchain image和pipeline layout动态建descriptor pool，没有固定16面池；
  多chunk/LOD会增加pool/sets和强引用数量，需要验证无pool exhaustion/validation错误。

### World替换/编辑峰值

Service cap是每个World包的，不是跨World全局限制。旧World等待epoch退休时，
新World准备可能同时保有两份Game服务cache：
**1,685,155,328 bytes，仅Service payload**，
尚未含两份通用coverage、源缓存是否复用及几何/driver。
不能用1GiB cap证明换World峰值有界在1GiB。

Editor候选会保留旧input/height到epoch完成；代码已将退休图计入部分检查，
Full16的Cook/Editor保留frame输入后头部空间比Game小。
连续笔刷、多个surface刷新、World reload需要实测收敛；
CPU元数据/队列不应把“预算延期”误报成页面内容已经更新。

## 5. 下一步具体实机退出证据（由GPU owner执行）

这些是完整岛新范围的实际闭环，不替用户再造一套旧2×2任务：

1. 16面完整Cook各5468页、新content/dependency hash与默认16 TR关联；
   记录构建时长、payload/sidecar/disk空间与峰值进程内存，不只看最后exit0。
2. 默认EditorReady：正常启动即16面，sourceTextures/sourceBytes去重、
   coverage1024注册/无重叠、无旧layerVT创建，近景LOD0/detail正常；
   新建/导入producer保存后显式Cook与加载流程按标准约束执行。
3. 全岛远眺→四角/中心交点/近景→跨tile快速移动或teleport，
   每面合理mip/父级/queued/reading/readFailures、停镜读取停止，
   仍保持World共享上传额度，不把CPU fallback计数称GPU逐像素证明。
4. 实际World替换并等待旧包退休：记录旧新重叠PID/WDDM峰值、
   generations、回读旧结果拒绝与最终队列/内存稳定；不靠增cap掩盖泄漏。
5. Full16新成品的cold/verify/必要变更负向恢复、默认场景与正常LOD/TES截图；
   inventory payload与运行generated额外文件分开记，成本计时列明query覆盖。
6. 标准shader/schema下S4：Paint/Sculpt/Undo/Redo、跨tile选择、Save/Cook/reopen
   关联新默认场景；已有Near100m回归可作为局部模块依据，不冒充整岛已跑。

**当前独立结论：清理源码与CPU回归通过；静态预算能容纳16面，但1GiB/512MiB并非
总进程或跨World峰值保证。Full16 Cook、默认Ready、移转/退休、包/成本及本次编辑关联
均待实际收据，不复用旧2×2“全部完成”结论。**

## 6. 源码/CPU阶段最终结果（22:53）

本轮有界Reader优化后，使用独立build的当前terrain二进制再跑一次完整suite：

| suite | 本轮实际结果 | 日志 |
| --- | --- | --- |
| terrain（清理后初次完整） | 432运行，429通过、3跳过 | `artifacts/terrain-renderer-cleanup-terrain.log` |
| terrain（含Reader新增4项后的最终完整） | **436运行，433通过、3跳过、0失败**，46 suites，20.507秒 | `artifacts/terrain-renderer-cleanup-terrain-final.log`、同名xml/exit.txt |
| renderer backend CPU | **297运行，297通过、0失败**，57 suites | `artifacts/terrain-renderer-cleanup-backend.log`、同名xml/exit.txt |
| Store单遍专项（此前已完成，不单独重复） | 29运行，28通过、1符号链接跳过 | `artifacts/terrain-store-single-pass-tests.log`、同名xml/exit.txt |

最终terrain的3项skip原因与§2相同；432→436是新增4项Store回归，
不是恢复已删除的旧层源VT专用测试。完整terrain自然包含Store项，
没有在最后再独立重复29项专项。Backend与本轮Reader修改无代码依赖变更，
采用本轮已经完成的297项CPU结果；仍明确排除8项headless Vulkan测试，
不触碰Maxwell独占GPU。

独立build使用：
`artifacts/terrain-renderer-cleanup-cpu-build`（terrain）、
`D:/Temp/vl-vt-cleanup-cpu-20261120`（backend）；
未使用rootbuild、build-vt-release构建或运行主程序。
日志/XML/exit均归档，两个完整suite退出码均0。

主线程另报告Reader+service+exporter最终Release三目标build exit0；
本审计不将其记成自己另跑的一次主目标构建。
截至当前，源码清理与CPU阶段没有已知失败或需要修fixture的剩余项。

## 7. Reader单遍/只读复用与导出强制出口

### Reader

`source/terrain/terrainCompositedVTStore.cpp::ValidatePackage` 已按record.offset
排序指针，从包头开始单次顺序读取；64KiB块同时更新page/package hasher，
逐页验证hash，结尾验证整个packageHash/EOF。
未改变schema、序列化、内容身份、范围/连续offset/长度/gap检查，
也没有懒验证或缓存外部可变文件。ReadPage仍独立校验所读页。

新增CPU回归：

- 物理offset顺序与page key顺序不同的合法包可读，所有页bytes一致；
- packageHash仍正确时，错误pageHash拒绝；
- 所有pageHash正确时，错误packageHash拒绝（路径名同步到错误hash以排除路径误报）；
- interior gap及乱序manifest offsets拒绝。

Windows IO专项实测包6,806,544 bytes，
Open读取6,826,899 bytes（含manifest），满足一遍完整包加索引的上界，
不再满足旧两遍读取行为。该专项已包含于本次完整terrain回归。
主线程独立负责service复用首次validated shared_ptr Store，避免game只读Open两遍；
C++ reader复用/读取优化不改变Cook内容，不要求重新Cook16。
后续须用新binary重验Fullruntime启动时长，不能直接把旧254秒乘除当实测改进。

### 强制导出与包验证出口

本审计只读核对主线程修改的 `tool/game-export/gameExporter.cpp`：

- Asset解析terrain时，缺compositedVT、非object、缺/非bool enabled或false明确拒绝；
  必须解析packagePath并收集/验证派生包闭包。
- Scan进入compositedVT字段也明确拒绝缺/禁用配置，不允许仅复制作者TR而不带派生包。
- VerifyGamePackage对type=terrain缺compositedVT拒绝；
  存在compositedVT时要求显式enabled=true，再验证材质页/coverage/height/dependency闭包。

对应 `gameExporterTests.cpp` 新增缺配置、disabled拒绝以及修改inventory摘要后
仍不能绕过退休配置拒绝的测试，源码存在。
主线程报告rootbuild完整CLI **69项，68通过、1符号链接跳过**，
Release exporter已重链。本审计未重跑主线程占用的rootbuild，
也未用此前历史export日志冒充本次69项结果。
这关闭了“TR/Runtime强制新路径但export仍能输出无派生包”的出口缺口；
Full16新包本身仍须最终实际verify/cold收据。

## 8. 标准路径S4已通过；Full16最终运行仍pending

已独立读取：

```text
artifacts/terrain-editor-s4-worker2-standard-cutover01/editor-s4-report.json
artifacts/terrain-editor-s4-worker2-standard-cutover01/final-evidence-summary.json
artifacts/terrain-editor-s4-worker2-standard-cutover01/process.json
artifacts/terrain-editor-s4-worker2-standard-cutover01/lifecycle.json
```

failures=[]、missingEvidence=[]，实际CommandBus→EditorHost，
edit/bootstrapCook/savedCook/reopen四进程均exit0，
四份日志未记录validation/runtime错误。24个rapid stamp均实际改变World，
production staleBakeResults 0→23。
Paint/Sculpt/Undo/Redo、material revert的BMP hash精确复原，
编辑pixel residual为0；外部冲突反馈延迟1492ms、观察窗口2884ms。
保存独立Cook再reopen RGB mean差0.0015679253472222223，
World reopen generation1→reload generation2。

**判定：标准唯一生产路径的本轮S4局部编辑闭环proved。**
实际scene为私有 `Near100m_R1024_Default`，
不是整岛16块同时编辑；不将该收据伪装成Full16跨tile实跑。
它是切换后新执行的标准路径证据，不是简单引用以前weighted-finalshader02成功。
不因C++ Reader性能优化改变schema/GPU/作者内容而要求额外重跑该S4。

当前收敛状态：

| 阶段/门禁 | 结论 |
| --- | --- |
| 唯一路径源码清理、合成ring命名、强制TR/MI与export出口 | proved（源码/本轮CPU范围） |
| 当前完整terrain/backend CPU回归 | proved，实际计数436/297如上 |
| 标准producer新建/导入→作者保存→Cook/Ready流程 | 仍待本轮相应整链收据，不以schema接口存在代替 |
| Full16 Cook | 主线程已报告全pass；后续审最终16面metadata/内容身份，不重复Cook |
| 标准路径S4 | proved（本次小fixture实际编辑范围），Full16跨tile运行不由此推导 |
| 新Reader/service/exporter Fullruntime16移动/移转/退休 | pending，Maxwell已获GPU独占执行 |
| Full16默认EditorReady、最终包/cold/成本及运行截图 | pending，等待完整岛原始收据 |

未发现源码/CPU阶段新的阻断；预算估算和进程/World退休限制仍按§4解释，
不会把“cap提高后可编译”当作整岛峰值内存及交付验收完成。
本审计后续只补实际Full16收据，不无依据新增门禁、不修改冻结production，
不占用renderer/GPU或主线程构建目录。

### 23:02补充：Full16实际TR依赖单项已通过

此前完整terrain suite中未设置环境变量而跳过的
`TerrainCompositedRealAssets.CollectsPegasusAndInvalidatesEditedNestedShaderSnapshot`
已单独补跑，**1运行、1通过、native exit0、26.697秒，XML已生成**。
不改写此前完整436项的“433通过/3跳过”原始统计，也没有全suite重跑。

实际fixture为
`Generated/Validation/TerrainCompositedVT/Variants/Full4x4_R8192_Default/TR_composited_tile_00.json`，
共70条实际依赖。原manifest hash：
`c5f7119f1cd80b2f3d835f7e3db724f037a9bd7811cde83173d20aaf7a5868d9`；
只修改测试私有nested shader快照后，作者校验拒绝旧摘要，
重建hash变为
`0e161930131dc3d404292d806e45cab5752f177a239389d8d272ec0da94cf4df`。
这是自定义测试buildConfig下的依赖模块身份，不冒充生产Cook contentHash。

可运行环境：上述独立terrain二进制，cwd为terrain-openworld代码根，
PATH前置 `build-vt-release/bin` 和
`D:/YYBWorkSpace/VulkanLearn-deps/win64-mingw-16.2.0-ucrt/Release/bin`，
`VL_TERRAIN_DEPENDENCY_REAL_FIXTURE`设为上述资源根相对TR。
该PATH只供已构建DLL加载，没有启动/使用共享Release构建任务。

原TR、MI、height、Paint与原nested GLSL前后SHA256全部一致；
测试仅写私有shader快照及已有测试约定的依赖证据JSON。
日志中原Paint缺sidecar兼容提示保留，不影响当前该项通过结论。
原始收据：

```text
artifacts/terrain-full-real-dependency.log
artifacts/terrain-full-real-dependency.xml
artifacts/terrain-full-real-dependency.exit.txt
artifacts/terrain-full-real-dependency-execution.json
artifacts/terrain-full-real-dependency-inputs-before.json
artifacts/terrain-full-real-dependency-inputs-after.json
```

## 9. 新Reader Fullruntime16：运行/收敛证据成立

选择读取
`artifacts/terrain-composited-vt/full16-standard/reader-final/runtime/report.json`，
没有整份倾倒274KiB报告。报告status=passed、所有checks=true、parserProblems=[]，
进程71576 exit0、no timeout、正常teardown。程序身份来自同目录上级
`binary-sha256.json`；本次读取实际fs/bin/main_game.exe核对为：
`3a9dbf5b7fe35c473a0316db40e050ff8bcf89cc6277b9dfc59df17ef32eb35c`，
与收据一致，不是旧Reader二进制。

far/mid/near/tile00_near/cross_tile_corner/move_across_island/teleport/reload/final_far
九段均有截图、settled=true。各段finalStats均为16个surface，
queued/reading/readFailures最终为0；generation1→reload generation2。
cacheBytes=842,577,664、sourceBytes=186,696,804、sourceTextures=7、
unknownSourceTextures=0，与§4静态Game服务payload估算一致，
确认本次源数组实际共享，而非每面重复完整数组。
服务cap仍不包含通用coverage/geometry/driver。

首次World commit162.931652秒、首次far截图170.981009秒、
第二次commit364.487839秒，总wall383.405765秒。
旧254.018567秒基线包含不同帧数/60Hz工作负载；
新报告本身已声明OS文件缓存未强制cold，
不能用这些不同区间直接宣称某个精确倍数的Reader性能提升。

本次进程CPU/GPU内存有效样本746/245，
peak working=3,429,851,136 bytes、private=10,460,635,136 bytes，
同刻dedicated+shared采样峰值=8,728,637,440 bytes。
这证实了之前“1GiB仅服务cap，不是全World/进程峰值”的边界，
不把该峰值武断称为泄漏，也不以cap一项代替实际记录。
GPU时间仍仅仪表化pass，不保证完整terrain/TES帧。

**当前判定：新Reader Runtime16加载/移动/跨tile请求收敛与World换代运行proved；
截图质量需独立审阅，见下。** 运行报告不是最终Full16包功能收据，
也不是默认EditorReady或完整producer新建链验收。

## 10. 四图独立查看及中央共享edge有界CPU核查

本审计实际view_image查看reader-final/runtime的
`far.png`、`near.png`、`mid.png`、`cross_tile_corner.png`：

- far完整岛屿及海面可见；near正常近景呈现，没有据此作全像素质量保证。
- mid存在局部块状边界；cross底部约x500/y550有明确草/岩视觉硬边。
  **不能写成“exit0且16面settled，所以画面无缝通过”。**

cross实际相机为position(0,24279.7641,5000)、look_at(0,21779.7641,0)，
HFOV65、1280×720。其中央相邻placement为05/06/09/10。
本轮私有CPU脚本
`artifacts/terrain-candidate-probe/full16_shared_edges.py`
读取这四份实际Full16 TR/原作者height/Paint，不修改输入、不运行GPU。
原始报告/日志：
`artifacts/terrain-full16-shared-edges.json`、同名`.log`/`.exit.txt`。

### 共享作者edge

每对edge检查513个对应宏网格顶点与同UV的原Paint双线性七层权重：

| 共享edge | 高度最大差cm | 七层原Paint最大绝对差 | 层key顺序 |
| --- | ---: | ---: | --- |
| 05→06，X方向 | 0 | 0.00917636 | 一致 |
| 09→10，X方向 | 0 | 0.01060859 | 一致 |
| 05→09，Z方向 | 0 | 0.00634325 | 一致 |
| 06→10，Z方向 | 0 | 0.00946536 | 一致 |

Paint opacity差仅double舍入量；原作者edge并非逐bit等权，
但没有大幅跨层错配或宏高度断裂证据。
该检查只覆盖中央四条edge，未扩成全岛任意artifact调查。

### cross硬边附近实际定点与Cook数据

CPU按实际tile TRS和固定三角宏面，用相机射线求首个交点；
射线区间按X/Z网格线及固定对角线的穿越距离分段，每段高度线性求根，
不是拿先前tile05的无关coverage点替代。
不含厘米级TES tiling位移，不能称GPU逐fragment世界坐标回读。

| 屏幕点 | 宏世界X/Z cm | placement | surfaceUV | coverage texel |
| --- | --- | --- | --- | --- |
| (499,549) | -664.305 / 1152.262 | 09 | 0.99340967 / 0.01143117 | (1017,11) |
| (500,550) | -658.952 / 1158.021 | 09 | 0.99346278 / 0.01148830 | (1017,11) |
| (501,551) | -653.609 / 1163.769 | 09 | 0.99351579 / 0.01154533 | (1017,11) |

三个点原Paint七层均为 `[0,0,0,1,0,0,0]`，opacity均为1，第五层为0。
读取实际新Cook coverage sidecar/binary：
surfaceId=`terrain|Landscape_09`，
虚拟page(7,0)→mapping(7,0,0,255)，ID=(3,0,0,0)、weight=(255,0,0,0)；
dependency config的worldLayerMapping=(0,1,2,3,4,5,6)。
**这些点没有top4大权重丢弃、错误页映射或错误placement身份的直接证据。**

屏幕(639,550)/(640,550)宏射线分别落在09/10：
09 coverage(1023,10)、10 coverage(0,10)，对应Cook surfaceId各自正确、
两侧主层均ID3，原Paint也接近/等于layer3=1。
这补充实际跨X=0 placement边界地址核对，不把x500的内部点误称tile交线。
x500宏命中距共享X=0约659cm，距Z=0约1158cm；
厘米位移有求交误差，仍不能据“相机叫cross_tile_corner”就把该像素自动归为tile seam。

**已证界限：** 中央作者edge高度连续、层身份顺序一致，
指定cross内部点和真正跨X边界点的原Paint/实际Cook身份没有串层证据。
**未证：** 可见草/岩硬边的最终来源。这里原Paint纯layer3，
既不能泛归为top4，也不能凭CPU覆盖身份正确宣称最终像素采样必然正确。
当前截图门禁保持pending，交主线程/视觉owner决定该具体硬边的受影响材质/
几何/合成页范围；本审计不继续扩展全图artifact追逐，不修改production。
