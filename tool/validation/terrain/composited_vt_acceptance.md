# Terrain composited VT 独立验收执行清单

日期：2026-11-19。准备基线：代码 `codex/terrain-openworld` / `5df9fbc`；
资源 `codex/terrain-openworld-assets` / `e665a40`。工作区有主线程未提交修改，
HEAD 不代表本次磁盘内容的完整版本；正式验收须重新记录 diff、二进制摘要与资源摘要。

本文件是验收操作说明，不是实现契约或通过报告。S0–S5 当前均为 **待执行/待证据**。
首轮只读核查文件、工具、设备和缓存配置；没有 configure、build、运行 renderer、
Cook、导出、编辑资产或运行模块测试。工具探测成功不能折算为功能通过。
后续由独立验收执行者在主线程交付阶段产物并交还构建/运行窗口后执行本清单，
记录失败并回报主线程；本轮准备结束后等待主线程继续。

## 0. S5 当前交接：fixture 已生成，height Cook 闭包仍有缺口

此节更新首轮准备快照；下面的“待入口/待证据”矩阵仍是阶段退出要求。
主线程正在串行构建/运行 `vl_renderer_backend_tests`、
`engine_launch_options_tests`、`game_export_tests`，记录于
`artifacts-composited-build-validation.log` 及主线程指定的测试日志。
独立验收没有重复这些任务，也没有使用共享 build 或启动 renderer。
producer GPU 验收脚本归 Ramanujan；这里仅执行无 renderer 的 fixture 几何 harness。

### 0.1 已实际生成的 fixture 与初始相机

资源场景：
`../VukanLearnResources/Generated/Validation/TerrainCompositedVT/SC_composited_vt_2x2.json`。
`build_composited_fixture.py` 最初生成场景、4 TR、4 base MI、4 detail MI，共 13 个 JSON；
后续默认套增加 legacy TR/scene 与 capture plan，共 19 个 JSON，作者隔离副本见 §0.5。
TR 采用 `enabled=true`、1024×1024、64 resident pages、8 MiB height budget，
Cook manifest 分别在 `Cook/tile_00|01|04|05/manifest.json`。
原始 tile/场景/config 保持原有文件内容。生成目录被资源仓库 Git 忽略，
脚本是可复现入口；重跑不会删除已有 Cook 包。

初始相机明确写 `look_at`：`worldBuilder.cpp::BuildCamera` 调用 `SetInitialLookAt`，
`worldCandidatePreparer.cpp::ConfigureInitialCamera` 因而保留作者机位。
只写 rotation 时会采用旧场景启动朝向世界原点的兼容行为，不能用于这组负坐标 tile。

| 参数 | 当前实际生成值 |
| --- | --- |
| position（cm） | `[-100800, 269590.5171, 230358.225146]` |
| look_at（cm） | `[-100800, 21221.848241, -100800]` |
| 水平 FOV | `65°`；引擎按水平角解释，不能当作垂直角 |
| near / far（cm） | `100 / 702203` |
| 地块 | 真实 00/01/04/05，保留原 X/Z placement 和 pageOrigin |

机位按四块地形的保守 AABB（含 tiling 位移幅度余量）拟合，
以 21:9 视口和 0.82 NDC 边界留白计算距离。独立 harness
`check_composited_fixture_camera.py` 用 Python 标准库解码真实 16-bit 灰度 PNG
（校验 CRC、处理 PNG filters），按 runtime margin/grid 约定检查全部
4×513×513 = **1,052,676** 个地形顶点，用 `sceneNode.cpp` 的 look_at/HFOV 约定投影：

- 4:3、16:9、21:9 下四块全部顶点进入视锥，near/far 裁剪失败均为 0。
- 16:9 合计 NDC 范围：X `[-0.458475, 0.475187]`，
  Y `[-0.247169, 0.616282]`；21:9 最大 Y 为 `0.808871`。
- 原始证据：`artifacts/terrain-composited-vt/S5-preflight/fixture-camera.json`，
  包含场景/四张高度图 SHA256、相机、每块实际高度/深度范围、各宽高比投影边界。
- 结论仅为**几何视锥检查通过**。尚无 renderer 截图，不能宣称已经验证 UI 面板遮挡、
  实际编辑视口尺寸、材质输出、背面裁剪、深度遮挡、Cook 或 GPU 正确性。

独立命令（不访问共享 build）：

```powershell
py -3 tool/validation/terrain/build_composited_fixture.py
py -3 tool/validation/terrain/check_composited_fixture_camera.py
```

主线程交付实际构建完成的 renderer 路径到 `$renderer` 后，首个实机动作：

```powershell
& $renderer --terrain-vt-cook `
  --initial-scene Generated/Validation/TerrainCompositedVT/SC_composited_vt_2x2.json `
  --exit-after-frames 2
```

`source/engine/launchOptions.cpp` 已有 `--terrain-vt-cook` 入口；
service 可链接/可运行及四份包完整生成仍由首轮实机证明。记录四个 surface 的
`[TerrainVT Cook]` pages/bytes/hash/path、退出码及 teardown。Cook 成功后，
以同一 fixture **不带 Cook flag** 冷启动，再获取视口截图与 VT stats；
两帧 Cook 启动本身不构成 Runtime 流式或打包验收。

### 0.2 height atlas：当前实现和 exporter 测试的真实覆盖范围

**S2/S5 缺口：独立常驻 tiling height payload 尚未形成已验证的发布/加载/导出闭环。**
初次源码核对如下；并行迁移中的最新变化见表后说明：

| 入口 | 当前事实 | 对验收的影响 |
| --- | --- | --- |
| `terrainCompositedService.cpp::Impl::Capture` | 捕获 `material->GetTexture("layerHeight")`；调用 `CreateTerrainHeightAtlas`，输入仍是完整层高度纹理 | 常驻 atlas 尚依赖完整 GPU height array |
| `terrainHeightAtlas.cpp::CreateTerrainHeightAtlas` | 要求源为 `R16Unorm`、具有 SourceMip；按 `RecordCopyImageRegions` 从源图复制到 atlas | 并非从 Cook 高度页读入/上传；不能宣称已消除完整高度源数组 |
| service Cook 分支 | Publish 的 channels 为 albedo/normal/ORM；另写 `<dependencyHash>.dependencies.json` | sidecar 的依赖记录不等于高度像素；不能由 hash 推导已存 payload |
| `terrainCompositedVTStore.cpp::ParseManifest` | v1 严格字段中没有独立 height payload/index/global-layer 映射描述 | exporter 不能替 Store 虚构尚无协议的 height 文件 |
| `TerrainCompositedExportsManifestAndContentAddressedBinary` 等 15 个 exporter 用例 | 合成 Store fixture 为单通道 `rgba8-unorm` 页，检查 `.vlcvt` 闭包/损坏/路径/版本/索引等 | 不包含 `layerHeight`、R16 height atlas Cook payload 或“去完整高度源后的冷启动”断言；即使全绿也不能证明 S2/S5 高度闭包 |
| fixture 的 base/detail MI | 仍引用真实 `T_terrain_kitedemo_height_array.json`，其 `sources` 为资源根相对 PNG 路径 | 现有递归闭包会沿 `Generated/` 前缀继续收集完整源纹理；这只维持当前消费依赖，不是完成高度 Cook 迁移 |

后续收到 height Store owner 的协议同步：拟新增
`source/terrain/terrainHeightAtlasStore.h/.cpp`，提供只读 CPU 校验 helper，目标
kind=`terrain-tiling-height-atlas`、version=1，固定源 mip2/128+4 border/R16；
manifest 为材质 Store manifest 同目录的 `<dependencyHash>.height.json`，
binary 为 `<packageHash>.height.bin`，含 64-byte 头、R16 atlas 和 RGBA32F layer LUT。
service 已出现 Cook 调用 `SaveTerrainHeightAtlas`、只读分支调用 `LoadTerrainHeightAtlas`
的接线；核查时 helper 尚未落盘，尚无对应实机或 exporter 测试证据，不能把接线当通过。
helper API 最终落盘后 exporter 应复用它，补收集/校验 height manifest+binary，
避免再造一套 height parser；新增链接依赖由主线程按现有文件所有权集成。
完整 height array 仍有 mip3 消费者，独立 mip2 payload 接通后也不得直接删除。

当前 exporter-only 检查器验证 Store v1 manifest + content-addressed binary 的完整性；
它没有制造或隐式承诺 height payload。service 新写的
`<dependencyHash>.dependencies.json` 也不是 v1 manifest 中的文件引用，
当前 exporter 用例没有证明该 sidecar 被自动收集。应先明确它是发布时必需文件、
审计产物还是仅供 Editor/Cook 使用，再固定闭包规则，不能以复制整个目录代替协议。

后续完成 S2/S5 高度闭包至少需要：

1. 主线程/Store owner 固定高度源内容身份、源 mip2/逻辑尺寸、R16 页面/border、
   global-layer→height-content 映射与 payload 的持久化协议，并区分依赖元数据。
2. Cook 输出这些实际像素与索引；Runtime 用它们建立 height atlas。
   另行审计完整源 height array 的 mip3 等消费者，只有无消费者后才能移除；
   不能将“height atlas 已独立加载”表述成“全部高度源数组已可删除”。
3. exporter 收集协议明确引用的每份高度文件并登记 hash；
   新增高度文件缺失/损坏/版本/映射越界的拒绝测试，以及保留完整高度源时的兼容闭包测试。
4. 在隔离包中阻止读取作者高度数组，仅保留 Cook 高度 payload 后冷启动，
   验证 mip2/repeat border/TES detail 对齐、预算及有效高度；这才是高度迁移的退出证据。

本次 exporter 接入后的最新范围见 §0.3。阶段结论仍为：
**fixture 几何入镜已独立核查；S2、S5 未通过。**
主线程正在运行的测试结果由主线程日志负责，不在此复写为通过。

### 0.3 sidecar 闭包接入与独立 CPU harness

height CPU helper 已落盘，exporter 现在调用
`ReadTerrainHeightAtlasStore(path, budgetBytes)`，复制其返回的已校验
`data.packagePath`。存在 `hasHeight=true` 层时，高度 sidecar 必须存在；
TR 的 `heightBudgetBytes` 同步参与校验。完整高度源数组的 mip3 消费链保持不变。
这里没有另写 height JSON/LUT/repeat-border 解释器。

主 Store v1 schema 保持冻结；每个 surface 的 dependencyHash 锚定以下同目录文件：

| 产物 | 导出/包验证行为 |
| --- | --- |
| `<depHash>.dependencies.json` | 必需；严格字段、版本、排序/唯一依赖、root/path/mode/digest 与 canonical 总 hash 检查，hash 必须等于主 surface dependencyHash |
| `<depHash>.coverage.json` | 必需；8 字段、version1、dependencyHash、unsigned tiles/pageResolution/bytes、tiles 1..32、TR pageTilesPerSide 一致性 |
| `<hash>.coverage.bin` | 必需；hash basename、同目录、完整 bytes/hash、RGBA8 页表逐项验证 `(x,y,0,255)` identity；表+双 atlas 以 uint64 计算，最大 32 tile 输出受 129 MiB 文件预算限制 |
| `<depHash>.height.json` 与 `<hash>.height.bin` | 有 height 层时必需；有文件时也校验并收集。复用 CPU helper 校验版本、格式、尺寸、预算、header、hash、LUT 和 repeat border |

所有路径继续拒绝 traversal/绝对路径/符号链接，先校验源、复制至 exporter staging 后
再次校验；所有收集文件必须登记在 `game-export-manifest.json`。
`VerifyGamePackage` 同样检查 sidecar 与 binary 的语义和清单登记，
不能仅因文件恰好在导出目录里而通过。
dependency sidecar 当前检查的是协议及内容身份，不替代实际作者文件 digest 重算；
作者依赖新鲜度仍由 service 的 author validation 及正式只读启动验收证明。

`gameExporterTests.cpp` 现在共有 **28 个 TerrainComposited 用例**：
旧 15 个 Store 闭包用例已补上合法 dependency/coverage fixture，
新增 10 个覆盖 height/coverage/dependency payload 收集、缺失/损坏、预算、
版本/路径/极大尺寸、有效 hash 下的错误页表、依赖锚点和清单遗漏。
另有 3 个检查新的 `terrain-material-instance-static-json-v2`、
`terrain-material-static-json-v2` digest mode 与未知 v3 拒绝。
top dependency version/domain/hash 算法不变；exporter 只扩展 mode 白名单，
不重新解释 MI/M 的静态字段。旧 Cook 需要按新依赖语义重建。
这些修改尚未在主线程的共享测试目标上重新编译/执行；不沿用先前 15 用例的运行结果。

**CMake 集成项（用户后续明确授权只修改该 target）：**
已在 `tool/game-export/CMakeLists.txt` 的 `vl_game_export` 源列表加入
`source/terrain/terrainHeightAtlasStore.cpp` 和
`source/shader/build/atomicFile.cpp`。
仅将 helper 加入 `terrain_tests` 或 runtime source glob 不会自动满足 exporter 链接。
该 target 的链接仍只有 Json 和 BLAKE3，新增源/头为 CPU 文件 IO/校验，
没有引入 Vulkan、runtime 或 Editor target；独立 harness 也未链接 Vulkan。
下一轮由主线程串行重建 `game_export_tests`、`vl_export_game` 并运行新用例。

独立 harness 为 `tool/validation/terrain/sidecar_closure_harness.cpp`，
输出全部在 `artifacts/terrain-composited-vt/S5-sidecar-harness/`。
使用 GCC/G++ 直接编译本仓库源与 BLAKE3 portable C 源，未使用共享 build 的库或目标，
未 configure，未运行 renderer，也没有重复主线程模块测试。

原始证据 `S5-sidecar-harness/result.json`：

```json
{"closureFiles":7,"negativeCasesRejected":8,"rendererStarted":false,"sharedBuildUsed":false}
```

正例由真实 Store Publish 与 height helper Write API 生成，得到主 manifest/bin、
dependency JSON、coverage JSON/bin、height JSON/bin 共 7 个文件。
8 个拒绝场景为 tiles 溢出、coverage 路径穿越、coverage 版本错误、
有效 hash 但非 identity 页表、TR tiles 不匹配、height 预算不足、
dependency 身份篡改、height binary 损坏；恢复后正例再次通过。
首次 harness 在恢复损坏内容寻址包时触发了 helper 的禁止覆盖规则；
修正 harness 仅清理自身隔离坏文件后重跑，通过记录来自第二次完整执行。
另以独立 `g++ -fsyntax-only` 校验了 `gameExporter.cpp`，成功。

这些证据只证明 CPU 文件协议与收集器基础逻辑；仍缺共享目标链接/完整 exporter
集成测试、真实 GPU Cook/readback、只读 Runtime 与完整 game 包冷启动证据。
S2/S5 的实机退出项继续保持待执行，不以 helper 或 harness 通过替代。

### 0.4 密度、原型上限与远/中/近 legacy 对照

**不能用远景截图判定 S1 通过，也不能宣称当前 1 km 地块已完成高精度近景。**
真实 tile 的边长为 100800 cm = 1008 m，并未缩成 100 m。
1024 虚拟分辨率只有约 1.016 texel/m；细分网格或常驻高度 atlas 不会提升合成颜色/
法线/ORM 的纹理分辨率。完整高度源仍在也不等于最终材质页具有相同近场细节。

| 夹具 | 边长 / virtualResolution | mip0 密度 | 完整 mip 页数 / 材质 payload | 当前 64 MiB 限制 |
| --- | --- | --- | --- | --- |
| 真实 2×2（每块） | 1008 m / 1024² | 0.984375 m/texel | 92 页 / 20,419,584 B ≈19.474 MiB | 可进入原型限额，未代表 Cook 实测通过 |
| 真实 2×2 的 2K 负例（每块） | 1008 m / 2048² | 0.4921875 m/texel | 348 页 / ≈73.661 MiB | 当前实现预期拒绝，不能绕过 |
| 独立 100 m 单地形 | 100 m / 1024² | 0.09765625 m/texel | 同为 92 页 / ≈19.474 MiB | 用于检查更高 texels/m 下的近场本质画质，不能替代真实 2×2 |

页字节按三通道 RGBA8（含 sRGB albedo）与 136×136 border 存储计：
`136*136*12=221952 B/page`。该估算仅为合成材质 payload；
Store header、coverage、height、dependency metadata、CPU 工作区和 resident GPU 缓存另计。
`terrainCompositedService.cpp` 当前 `CookPayloadLimit=64 MiB` 且完整页集入内存，
所以 2K 不因 `residentPages=64` 就能 Cook。`virtualResolution` 作者 schema 可接受更大
尺寸，不意味着此原型 Cook 已支持它。主线程须明示当前上限，或实现并验收流式 Cook
及 Store validation 的内存控制后再提高上限；只提高一个常量不能视为完成扩展。

生成脚本现支持 `--virtual-resolution`（128..8192 的二次幂）、
`--height-blend 0|1`、`--near-100m`。非默认参数写独立
`Variants/<Real2x2|Near100m>_R<resolution>_<mode>/`，
保留默认真实 2×2 的尺寸、placement 和资产路径。100 m 场景单独缩尺 tile00 的
TR size/宏高度尺度并移动到原点；复用原始高度/Paint/纹理，不改原始 TR。
分辨率超限仍可生成作者负例，输出 `withinCurrentCookLimit=false`，不伪造 Cook 文件。

已实际生成：默认 fixture、Real2x2 R1024 HeightBlend0/1、
Near100m R1024 HeightBlend0/1、Real2x2 R2048 HeightBlend0。
每套都有 composited scene、关闭 enabled 的 legacy TR/scene、以及
`capture_plan.json`；legacy 与对应 composited 的相机、base/detail MI、灯光、
placement、纹理、heightBlend 数值一致，只改变合成 VT enabled 和必要 TR 身份。
矩阵证据：`artifacts/terrain-composited-vt/S5-preflight/fixture-matrix.json`。
100 m 初始相机另由无 renderer harness 检查，证据为同目录 `near100m-camera.json`。

```powershell
py -3 tool/validation/terrain/build_composited_fixture.py --height-blend 0
py -3 tool/validation/terrain/build_composited_fixture.py --height-blend 1
py -3 tool/validation/terrain/build_composited_fixture.py --near-100m --height-blend 0
py -3 tool/validation/terrain/build_composited_fixture.py --near-100m --height-blend 1
py -3 tool/validation/terrain/build_composited_fixture.py --virtual-resolution 2048 --height-blend 0
```

真实 2×2 的三档固定机位（cm）如下；实际机器命令从每套 capture plan 读取。
near/mid 目标高度取真实 tile00 高度图中心像素，不使用理论高度上限：

| shot | position | look_at | 目标距离 |
| --- | --- | --- | --- |
| far | `[-100800,269590.5171,230358.225146]` | `[-100800,21221.848241,-100800]` | 全景 |
| mid | `[-151200,4015.406982,-146200]` | `[-151200,1515.406982,-151200]` | 约55.9 m |
| near | `[-151200,2315.406982,-150000]` | `[-151200,1515.406982,-151200]` | 约14.4 m |

shader `TerrainCompositedUsePage` 当前在 heightBlend>0 且距离<45 m 时走 direct fallback。
因此 HeightBlend1 的 near 图即使清晰，也不能当作 VT 近景画质证明；
HeightBlend0 的 near 才持续走全 VT，预期暴露约 1 m/texel 的模糊。
原默认 MI 未覆盖该参数时沿用 `M_terrain.json` 当前 default=3；
显式 0/1 是独立诊断模式，不静默替换用户原默认值。
每个模式必须与**相同 heightBlend 的 legacy** 对比，不将 0 与 1 互相当基准。

运行窗口交还后，对每套可 Cook 的模式执行：

1. 先真实 Cook composited scene，再以只读启动并等待至少120帧；
   固定 1280×720/实际 scene viewport、65° HFOV、MSAA、曝光、线程数、validation、
   UI/profiler 状态，记录两路完全相同的配置。
2. 使用分帧 `camera pose` 设置 far/mid/near，等待反馈/上传收敛，
   各拍一张，不在同批启动命令里排三条 screenshot。
   每张记录 mip/父页回退、队列/stale、是否使用 direct fallback。
3. 同一 exe 启动该套 legacy scene，重复相同机位/等待/截图；
   比较原始图和相同地表区域裁剪，分别报告材质细节、normal/粗糙度和接缝。
4. 真实 2×2 与独立 100 m 分开报告；近景 HeightBlend0 的退化必须明确展示，
   不用远景、direct fallback 或缩尺场景掩盖。2K 负例单独记录当前拒绝诊断，
   不与可运行模式混作画质通过。

**当前截图状态：尚未捕获。** 本轮保留“不得启动 renderer”的运行窗口限制，
只生成场景、计划和 CPU 几何证据。实际远/中/近及 legacy 成对截图、
用户对画质/可调分辨率/原型上限的最终确认，均为未完成验收项。

### 0.5 Paint/Sculpt/Save 的作者文件隔离

首次实机 Cook 前，已实际把四块真实 tile 的 heightmap/Paint 复制到资源根相对的：
`Generated/Validation/TerrainCompositedVT/Author/tile_00|01|04|05/`。
文件名保留原名；源文件旁的 `<file>.meta.json` 若存在也按原字节复制。
本轮四块原始 Paint sidecar 均未发现，因此没有伪造 sidecar。

默认真实 2×2、两个 heightBlend 变体、两个独立100m变体和2K负例，
所有 TR 的 `heightmapPath` / `paintPath` 已改指向 Author 副本。
legacy 配对场景同样使用这些副本。Sculpt/Save Heightmap、Paint/Save Paint 因而不会
经 TR 路径写回原 `Generated/HoudiniPegasusIsland`；大体积层纹理继续只读引用原 source。
复制路径纳入依赖摘要，这次调整使旧 Cook 不再匹配，必须重新 Cook。

生成器的重跑规则：

- 副本不存在：以原字节写入；存在且字节相同：不写文件。
- 存在且与源不同：报错并保留已编辑数据；不会静默覆盖。
- 用户显式传 `--reset`（别名 `--reset-author-copies`）才丢弃副本编辑，
  恢复原始作者 bytes。重新生成 TR/MI 不等于恢复作者高度/Paint。
- 未 Save 的编辑用 runtime 的 reload/discard 语义验证；
  已 Save 的副本恢复需要显式 reset，不能把普通 reload 当作撤销磁盘保存。

证据 `artifacts/terrain-composited-vt/S5-preflight/author-copy-check.json`：
8 个真实作者副本初始 SHA256/bytes 与源一致，6 套 fixture 路径检查通过。
另在 artifacts 下的隔离假文件夹具验证“编辑副本保留/默认报错/显式 reset 恢复”，
没有为测试破坏真实 Author 副本。此证据为首次编辑前快照，后续 Save 后 bytes
与原始源不同属于预期，不应被当作自动重置的理由。

### 0.6 独立 Release 交付 cache

用户已授权 `build-vt-release` 与主线程 Debug 并行 CPU 构建。
新 cache 采用 Release、prebuilt Release 依赖、C# OFF、validation ON、tests OFF，
显式复用 `build-merge` 的 WinLibs `c++.exe/gcc.exe/mingw32-make.exe`；
configure 已确认 GNU 16.2.0，deps-stamp OK。未改其他 build cache。

目标为 `main_editor main_game vl_export_game -j4`，
完整日志为 `artifacts/terrain-composited-vt/release-build.log`。
本节写入时构建进行中，最终产物/退出码由构建交接报告确认。
该授权只覆盖 CPU 构建；不启动产物、不触发 shader 编译/Cook，
实际实机验收仍等待主线程运行窗口。validation ON 的 Release 不与
无 validation 的帧率混作同一性能基线。

## 1. 依据、写集与执行边界

- 已读根 `AGENTS.md`、[routing](../../../documents/routing.md)、
  [testing](../../../documents/contracts/testing.md)、
  [文档分层](../../../documents/README.md)、根 README、
  [terrain 契约](../../../documents/contracts/terrain.md) 的相关数据流/编辑/VT 章节、
  [shader cache](../../../documents/contracts/shader-build-cache.md)、
  [GPU 计时](../../../documents/contracts/gpu-pass-timing.md) 和
  [完整生命周期计划](../../../documents/plan/rendering/terrain-composited-vt-lifecycle-plan.md)。
- 首轮新增文件仅本文件；后续用户已授权 exporter/tests/fixture 脚本和生成资产，见 §0。
  不修改用户 config 或其他执行者生产文件；后续仅获授权修改 `vl_game_export`
  target 的源列表，实际改动见 §0.3。
  未发现 `tool/` 下更深层 `AGENTS.md`。已有其他工作区修改归主线程，不恢复或覆盖。
- 复用现有同 topic 的两棵 worktree，不创建分支。资源目录是实际 Git worktree，
  `Get-Item` 未显示为 junction；当前 sibling 路径已满足 config 的相对寻址。
- 运行、shader 生成、Cook、导出和资产编辑也可能写盘；这些均留待主线程明确交接的窗口。
  不因使用不同 build 目录就并发启动程序：它们共享当前 worktree 的 `shader/spv/`、
  generated include 与资源目录。

交付前再次查看 status，已观察到其他执行者并行新增 `terrainCompositedVT*`、
`terrainCompositedVTStore*`、`terrainCompositedResidency*`、height atlas/evaluation 文件，
并修改 loader/validator/shader/tests；本文件的入口核查是准备时点快照，不是对这些
正在写入文件的最终评审。另已出现
`tool/validation/terrain/validate_feedback_address.py`：可用入口为
`py -3 tool/validation/terrain/validate_feedback_address.py`，附加 `--compile` 会调用
外部 shader 工具并写临时输出。脚本说明仅覆盖实际源码地址表达式、槽位重映射、
页边和导数控制流结构，不能证明渲染结果或 GPU 导数正确。本轮未运行此脚本。
独立验收接手时重新核查已完成实现及真实 CLI，不把文件出现等价于阶段通过。

## 2. 本机可执行条件：已核查事实

所有后续 PowerShell 命令从下列目录执行：

```powershell
Set-Location 'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn'
$assetRoot = (Resolve-Path '..\VukanLearnResources').Path
```

### 2.1 Build 与 config

以下是读取 `CMakeCache.txt` 和文件存在性的结果，不表示产物对应最新源码。
所有缓存的 generator 都是 `MinGW Makefiles`，home directory 指向本工作树。

| 目录 | 配置事实 | 已看到的主要产物 | 适用性与限制 |
| --- | --- | --- | --- |
| `build` | Debug，tests ON，C# OFF，editor ON，validation OFF，source deps | `main_editor.exe`、`main.exe`、`terrain_tests.exe`、`game_export_tests.exe`、`vl_export_game.exe` | CPU terrain 回归候选；不能直接用含 Echo blueprint 的默认岛屿场景做完整验收 |
| `build-merge` | Release，tests ON，C# ON，editor ON，validation ON，prebuilt deps | `main_editor.exe`、`main_game.exe`、`terrain_tests.exe` | 完整岛屿功能验收候选；`bin/vl_build_flavor.json` 当前为 editor，不能凭存在 `main_game.exe` 宣称可导出 game |
| `build-csharp` | Release，tests ON，C# ON，editor ON，validation ON，prebuilt deps | 本次未看到上述主要 exe | 缓存存在不代表完成构建 |
| `build-noval` | Release，tests OFF，C# ON，editor ON，validation OFF，Tracy ON，prebuilt deps | `main_editor.exe` | 性能候选；Tracy 开启须记入对照条件，不与无 profiler 结果直接混用 |
| `build-vscode/development` | RelWithDebInfo，tests OFF，C# OFF，validation OFF | `main.exe`、`vl_export_game.exe`，另有历史命名副本 | 优化旧路径对照候选；不把历史副本当本轮产物 |
| `build-vscode/release` | Release，tests OFF，C# OFF，validation OFF | `main.exe`、`vl_export_game.exe` | 同上；不是已确认的 game-only 包构建 |

`CMakePresets.json` 中 debug preset 默认开启 validation，但当前 `build` cache 为 OFF；
实测时以 cache + 启动覆盖 + layer 实际启用日志为准。性能用 `--no-validation`，
正确性用 `--validation`。验证层缺失时程序会降级，降级运行不能记为 validation 通过。

当前 `config/config.json`：`resourcePath=../VukanLearnResources`，
`initScene=Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json`，
1280×720、MSAA 2、workerThreadCount 2、UI enabled。该 config 已被主线程修改，
独立验收用启动参数指定场景/线程数，不自行覆盖它。

### 2.2 工具与设备

| 项目 | 本轮核查结果 | 验收用途/尚未证明 |
| --- | --- | --- |
| GPU | `nvidia-smi`：RTX 3080，10240 MiB，驱动 621.01；`vulkaninfo --summary`：API 1.4.357 | 设备存在；尚未测各候选 VT 格式的 sampled/storage/transfer/filter 能力、显存预算与 R16 精度 |
| Vulkan SDK | `C:\VulkanSDK\1.4.363.0\Bin`；PATH 可解析 `glslangValidator`、`glslc`、`spirv-val`、`vulkaninfo` | 可做诊断；不能替代引擎的组合材质编译 |
| Validation | `vulkaninfo --summary` 列出 `VK_LAYER_KHRONOS_validation` 1.4.363 | 仍须正式运行日志证明实际启用；同步验证设置、VUID 和线程错误留证 |
| CMake/CTest/MinGW | PATH 可解析；cache 编译器在 WinGet WinLibs `mingw64/bin`，GCC 路径含 16.2.0 | 使用 cache 记录的工具，不换 MSVC/Ninja，不在已有缓存切 generator |
| RenderDoc | `C:\Program Files\RenderDocForMetaQuest\qrenderdoc.exe` 和 `renderdoccmd.exe` 存在，未在 PATH | 尚未验证该发行版对桌面 Vulkan 捕获是否可用；先做小场景捕获再依赖它检查上传/barrier/页表 |
| Nsight | PATH 未找到 `nsys`/`ncu`，已查 NVIDIA Program Files 目录未看到 Nsight 子目录 | 不能把 NVTX 或目录存在写成已具备 Nsight GPU 捕获；不阻塞 CPU/内建计时验收 |
| Tracy | `tool/tracy/tracy-profiler.exe` 存在 | `build-noval` 开启 Tracy；未连接采集，不代表已得到 GPU/IO 证据 |
| Python | `py -3` 为 3.14.0，`import numpy` 失败 | 无 numpy 的脚本不能直接照抄执行；`paint_manifest.py` 依赖 numpy，先选具备依赖的解释器 |
| .NET | PATH 未解析到 `dotnet`；主 checkout `artifacts/dotnet-sdk/dotnet.exe` 存在 | 需显式 SDK 路径及 `--list-sdks` 确认 .NET 11，未在本轮触发托管构建 |
| PowerShell | `pwsh` 可解析到 Codex bundled runtime | `console_inject.ps1` 需要它；保留独立注入进程，避免破坏宿主控制台句柄 |

核查时看到一个 `cc1plus.exe` 进程；未判断其归属，也未终止它。
这进一步要求由主线程串行管理构建窗口，而不是独立验收自行抢占。

### 2.3 如何可靠构建：仅交给主线程执行

1. 先停止同 worktree 的其他 build/renderer 写入，记录 `git status`、
   cache 配置和工具版本；确认没有目标 exe 被运行进程占用。
2. CPU terrain 最短路径为已配置的 `build`；只构建明确目标，使用 `-j 4`。
   不调用 preset 假定它等同于当前缓存。
3. 完整 16 tile 场景含 `blueprintInstance`；使用 C# ON 的 Release 构建。
   `build-merge`/`build-noval` 当前 `VULKANLEARN_DOTNET_EXECUTABLE` 指向
   `artifacts/dotnet-host/dotnet.cmd`。该 wrapper 的 build 分支直接转入
   `artifacts/build-managed.ps1`，不是标准 MSBuild 入口；不能把成功返回视为
   当前托管源码已用标准工具重建。
4. 主线程新增的 `tool/script-build/configure-csharp-build.ps1` 支持显式 SDK、
   BuildRoot。其默认 BuildRoot 在主 checkout，默认调用还会创建目录；
   本轮没有调用它，包括 `-PrintOnly`。交接时显式指定本 topic 的输出根。
5. prebuilt 前缀 `D:\YYBWorkSpace\VulkanLearn-deps\win64-mingw-16.2.0-ucrt`
   存在；stamp 校验是否通过只能由正式 build 证明。不要为通过验收绕过 gate。

主线程可按下列顺序使用已有入口；遇到非零退出立即停止并保存输出：

```powershell
cmake --build build --target terrain_tests game_export_tests -j 4
ctest --test-dir build -R '^(Terrain|Vegetation|GameExport)' --output-on-failure
```

这里的 regex 按 GTest 测试名匹配，不是 target 名。执行前先
`ctest --test-dir build -N -R '^(Terrain|Vegetation|GameExport)'` 确认非空，
并与各目标 `--gtest_list_tests` 对照；避免 0 tests 被当通过。
新合成 VT 测试注册后补入实际 suite 名，不能只跑旧 terrain 用例。

需要修复 C# 配置时，以下是待主线程执行的示例，不是已验证成功的构建记录：

```powershell
$env:VULKANLEARN_DEPS_ROOT = 'D:\YYBWorkSpace\VulkanLearn-deps'
powershell -NoProfile -ExecutionPolicy Bypass -File tool\script-build\configure-csharp-build.ps1 `
  -BuildDir build-merge -BuildType Release `
  -Dotnet 'D:\YYBWorkSpace\GitHub\VulkanLearn\artifacts\dotnet-sdk\dotnet.exe' `
  -BuildRoot 'D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VulkanLearn\artifacts\script-build\acceptance'
cmake --build build-merge --target main_editor main_game terrain_tests -j 4
```

不要通过 `CMAKE_BUILD_TYPE=Release` 推断验证层已关，也不要把 Debug 性能与 Release
直接比较。构建完成后记录 exe 的 SHA256、时间、cache、源码 diff 和托管产物版本。

## 3. 已有执行入口及证据范围

### 3.1 Shader 编译和校验

正式入口在 `source/shaderCompiler.cpp`，使用链接的 shaderc
`CompileGlslToSpv`，目标 Vulkan 1.4；启动先生成材质参数 include/组合源码，再编译。
`glslangValidator` 直接编译原始 `.surface.glsl` 不等同于真实材质编译。
`shader/spv/shader-build-cache.json` 是本 worktree 共享 manifest。

```powershell
build-merge\bin\main_editor.exe --initial-scene Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json `
  --shader-force-rebuild --validation --exit-after-frames 2
build-merge\bin\main_editor.exe --initial-scene Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json `
  --validation --exit-after-frames 60
```

首次强制编译允许 cache misses；第二次相同配置应为 cache hit。
两次都须记录退出码、编译错误、统计和 teardown；shader hit 不能证明 VT 画质。
需要 `spirv-val --target-env vulkan1.4 <实际SPV路径>` 时从新 manifest 取实际输出，
不凭 GLSL basename 猜输出名。新 Cook shader 的枚举/编译入口仍待交付。

### 3.2 运行、控制台、像素与性能

```powershell
build-merge\bin\main_editor.exe --initial-scene Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json `
  --worker-thread-count 2 --validation --exit-after-frames 600 `
  --console-command 'terrain stats' --console-command 'terrainlayer stats' `
  --console-command 'terrainfeedback' --console-command 'terrainchunk stats' `
  --console-command-delay 120
```

将线程数改为 1 重跑。至少包含一次正常退出，要求退出码 0、
`EngineLoop teardown complete`，无 Scene.LoadFailed、VUID、threading 错误。
仅启动 2 帧不足以覆盖 feedback/readback/退役窗口。

源码 `source/debugConsoleProcessCommand.cpp` 当前存在：

| 命令 | 当前用途 | 不得冒充的新链路证据 |
| --- | --- | --- |
| `terrain stats` / `terrain capacity N` / `terrain parity [tileX tileZ]` | 覆盖 ID/weight 页、容量、CPU/GPU bake 对照 | parity 不是合成 albedo/normal/ORM 或 height atlas parity |
| `terrainfeedback` | 现有 feedback 诊断 | 不是已验证的新 surface/mip 地址输出 |
| `terrainlayer stats` / `capacity N` / `preload` / `forcemip N` | 旧层源 VT；forcemip 为 -1 或 0..6 | 旧层页 capacity 是页数，不是新链路总 bytes 预算；preload 会掩盖流式缺陷 |
| `terrainlod stats` / `terrainchunk stats` / `terraincollision stats` | 几何与碰撞状态 | 不证明材质缓存状态 |
| `camera get` / `camera pose px py pz tx ty tz` | 固定相机与脚本化移动 | 每帧轨迹/采样时点仍须记录 |
| `loadworld <path>`、`screenshot <path>`、`gpustats`、`perf json`、`quit` | World 换代、截图、计时和干净退出 | 新版本计数器、故障注入尚无已确认入口 |

多次截图应通过 `. .\tool\validation\console_inject.ps1` 后
`Send-ConsoleLine -ProcessId <实际PID> -Line '<命令>'` 分帧发送。
`--console-command` 是一批延迟命令，多条 screenshot 会覆盖单个待处理路径。
进程必须是验收者自己启动并记录的 PID，不能注入主线程正在使用的程序。
如用 `Start-Process` 建立后台进程，指定 `-WindowStyle Hidden` 并保存 stdout/stderr。

性能命令选优化构建，示例：

```powershell
build-noval\bin\main_editor.exe --initial-scene Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json `
  --worker-thread-count 2 --no-validation --gpu-stats --exit-after-frames 1800 `
  --perf-jsonl artifacts/terrain-composited-vt/perf.jsonl
```

固定分辨率/MSAA/相机、预热窗口和 profiler 状态。`--perf-jsonl` 是每秒窗口统计，
不足以还原逐帧 p95/p99；计划要求的帧耗时分位数需增加逐帧采集或使用已验证 profiler。
GPU timestamp 会改变 pass 边界行为，计时开关也须一致。GPU 显存必须分别统计源数组、
材质 atlas、height atlas、页表、过渡槽位、几何/碰撞；`nvidia-smi` 总量只能辅助观察。

## 4. Fixture：真实数据与最小多 tile 集

资源根为
`D:\YYBWorkSpace\GitHub\VulkanLearn-worktrees\terrain-openworld\VukanLearnResources`。
现成可加载候选是 `Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json`：
静态读取为 16 terrain + camera + directionalLight + environment + mesh +
blueprintInstance，共 21 objects。默认 MI 为非 tess 的
`Generated/HoudiniPegasusIsland/MI_terrain_pegasus_island.json`；
旁边的 `MI_terrain_pegasus_island_tess.json` 仅“存在”不代表默认场景开启 tess。

最小边验收可用相邻 00/01 两块；完整最小多 tile 验收采用 00/01/04/05 的 2×2，
同时覆盖 X/Z 边界和四块交点。保留原有 placement 与 pageOrigin，不能把同一 TR 复制
四次当真实独立 tile。首轮尚无裁剪场景；现已按 §0 生成同资源分支下的最小 fixture，
实际资源相对位置为
`Generated/Validation/TerrainCompositedVT/SC_composited_vt_2x2.json`，
带必需中文 description、相机/灯光/environment，引用真实 tile 的隔离副本。
去掉 Echo/海面可消除最小用例对 C# 与额外透明材质的干扰；完整 4×4 仍保留真实依赖回归。

| tile | terrainPath（相对上述资源根） | 场景 X/Z（cm） | TR pageOrigin / 页边长 |
| --- | --- | --- | --- |
| 00 | `Generated/HoudiniPegasusIsland/tile_00/TR_terrain_houdini_island_tile_00.json` | -151200 / -151200 | [0,0] / 8 |
| 01 | `Generated/HoudiniPegasusIsland/tile_01/TR_terrain_houdini_island_tile_01.json` | -50400 / -151200 | [8,0] / 8 |
| 04 | `Generated/HoudiniPegasusIsland/tile_04/TR_terrain_houdini_island_tile_04.json` | -151200 / -50400 | [0,8] / 8 |
| 05 | `Generated/HoudiniPegasusIsland/tile_05/TR_terrain_houdini_island_tile_05.json` | -50400 / -50400 | [8,8] / 8 |

四块已逐一确认 `Source/heightmap.png`、TR 的
`Source/terrain_houdini_island_tile_XX_paint.vlpaint` 存在，七层定义；
对应 `<paint>.meta.json` 均未发现。不能宣称该集已有 VLP2 完整性证据。
`tile_layout.json` 记录 tileGrid 4、tileResolution 512、tileSamples 515、
tileSizeCm 100800；TR gridResolution 512、chunkQuads 128、lodLevels 3。
layout 中有绝对路径元数据，不能作为可移植 runtime 闭包的唯一索引。

共享源纹理描述符位于
`Generated/HoudiniPegasusIsland/tile_00/Textures/T_terrain_kitedemo_{albedo,normal,pbr,height,weights}_array.json`，
全局输入在 `Generated/HoudiniPegasusIsland/Global/T_global_{colour,masks}.json`。
height array 为七个 `Textures/Source/layer_XX_height16.png`、linear、repeat、
16 bit、mipmaps true。后续核查全部叶文件与内容摘要，不只核查 JSON 存在。
S1 的 mip2 等价性对照使用 tess MI 的隔离副本，不改默认作者资产。

非正方形/末页、非恒等物理映射、局部层顺序打乱、相同 TR 不同变换、不支持材质、
预算不足和包损坏 fixture 尚待新增。真实 2×2 不能替代这些定向用例。

## 5. S0–S5 独立验收矩阵

每行都必须留可重放输入、实际命令或操作序列、原始输出和结论。
“入口待实现”是阶段退出阻塞项，不得用相近旧命令代替，也不编造新 CLI。
阈值由 S1/S2 实测提案、主线程确认后冻结，最迟在 S3 开始前落盘。

### S0：语义和协议（待证据）

| ID | 可执行验收 | 现有入口/交付要求 | 风险与退出证据 |
| --- | --- | --- | --- |
| S0-01 | 对照计划 §3 逐资源核查 producer/owner/consumer、失效、发布、退休 | 阅读 loader、page manager、feedback runtime、editorHost；新协议 diff 审阅 | 每项有源码位置和失败策略；“设计了”不等于“实现了” |
| S0-02 | 构造 Paint、导入逐层权重、自动规则互相冲突输入，打乱局部/全局层序 | `terrain_tests` 的 TerrainWeightMap/Binding/LayerRegistry 等现有 suite 作基线；新语义测试待补 | 冻结优先级、sourceSet/slice/globalId 映射和不支持材质拒绝范围 |
| S0-03 | 固定随机/非恒等物理槽位，比较主视图与 feedback 解析的地址和层需求 | 新增 `validate_feedback_address.py` 作源码/CPU 检查；旧 `terrainfeedback`、`terrain parity` + GPU 捕获；地址读回断言待补 | 脚本未在本轮运行；旧 feedback 的虚拟/物理混淆必须先消除，不能用错误旧路径作金标准 |
| S0-04 | 对相同 TR 不同 placement/纹理/倍率/编码版本分别计算依赖键 | 稳定键测试和构建描述序列化入口待交付 | 相机移动不改内容键；实际影响评价的世界变换必须改键 |
| S0-05 | 盘点正常/失败状态及刷新矩阵，检查宏高度、tiling 高度、碰撞边界 | 协议/状态机测试待交付；现有 `terraincollision stats` 辅助 | 发布阻塞项清零，记录旧 LOD 与输出表面 LOD 的差异；否则 S0 不退出 |

### S1：单地形固定页原型（待证据）

| ID | 可执行验收 | 现有入口/交付要求 | 风险与退出证据 |
| --- | --- | --- | --- |
| S1-01 | 同快照、固定相机比较直接评价与合成 albedo/normal/ORM；斜坡、交界、平铺/旋转密度 | 原有 screenshot/camera；新固定页模式及通道读回待交付 | 在线性空间比较颜色、法线角误差及 ORM 各通道误差；记录 max/RMSE/分位与阈值，不只比较最终光照 |
| S1-02 | 非正方形、末页、每级 mip、跨页 border、外缘 clamp、斜视和虚拟三线性 | 新地址/过滤测试与固定页 fixture 待交付 | 内部 border 来自邻域，不能复制页边；粗 mip 必须面积过滤；记录别名/接缝像素误差 |
| S1-03 | 测格式能力与实际存储；候选 128+双侧4 border 检查块对齐 | `vulkaninfo` 设备探测 + 新格式测试；原型常量不得当定稿 | sampled/filter/storage/transfer 需求逐项成立；BC extent 对齐；各向异性上限实测冻结 |
| S1-04 | 原完整高度数组 mip2 与常驻 height atlas 对照，跨 repeat 首尾/内部页、层重映射 | tess MI 隔离副本、相同相机截图/顶点结果读回；新映射测试待交付 | R16 精度、源逻辑尺寸、TES/detail UV 和位移幅度/淡出一致；不得改成像素 VT mip |
| S1-05 | 重复高度源去重、Paint 切层、改倍率、换高度源，检查固定高度槽位 | 新计数器/映射读回待交付 | 不生成合成位移页；倍率变化不重烘源；常驻/过渡 bytes 全计费 |
| S1-06 | 固定页多次冷启动/热启动对照并归档测试集和阈值 | shader 入口、截图、测试清单 | 不使用复杂 LRU 掩盖评价差异；细分效果不能等价时先修订设计，不进入 S2 |

### S2：Cook 与只读包（待证据）

| ID | 可执行验收 | 现有入口/交付要求 | 风险与退出证据 |
| --- | --- | --- | --- |
| S2-01 | 从已保存快照 Cook 两次；预览与 Cook 同评价器，读取各 mip/通道/高度页对照 | 已有 `--terrain-vt-cook`，首次实机待执行，见 §0；独立 height payload 尚缺 | 比较解码语义/允许编码误差；不要求不同机器 GPU 逐位一致；预览编码不得误命中发布缓存 |
| S2-02 | 离线检查索引范围、摘要、页数、border、尾层、height 映射、格式/版本 | 新包 validator/测试入口待交付 | 越界/重叠/截断/错误层映射都拒绝；有效祖先完整 |
| S2-03 | 在临时输出中中断 Cook、强制构建失败，再加载旧 manifest | 验收者自己启动的 Cook 子进程 + 临时 fixture；故障点待交付 | 旧有效产物不变；临时文件不可见；manifest 最后原子发布 |
| S2-04 | 复制包后分别损坏数据、删细页、删尾页、改版本/依赖 hash | 新只读 loader；仅操作隔离包副本 | 无有效保底页明确失败；细页失败维持祖先并有限重试；禁止 runtime 现场补 Cook |
| S2-05 | 逐步扩大表面尺寸并测冷读、构建耗时/峰值 RAM/包体/压缩误差 | 新 Cook 统计 + OS/GPU 工具 | 记录最大已测尺寸，明确支持上限；S1/S2 的误差和预算阈值在 S3 前冻结 |

### S3：Runtime 流式闭环（待证据）

| ID | 可执行验收 | 现有入口/交付要求 | 风险与退出证据 |
| --- | --- | --- | --- |
| S3-01 | 清空仅运行时细页驻留、先加载粗层；贴地/远眺/快转/移动跨四块交点 | camera pose、screenshot、perf；新合成页需求/驻留读回待交付 | feedback 与主视图共用表面地址/导数换算；只请求可见邻域/祖先，不膨胀成整层 mip |
| S3-02 | 分别设置保底成本以下、等于保底+height、允许少量细页的预算 | 新 bytes/队列/上传预算配置待交付，不能用旧 `terrainlayer capacity` 充数 | 低于保底明确拒绝；正常预算不永久超额，过渡槽位也计入；缺细页始终查有效祖先 |
| S3-03 | 延迟上传、IO/解码失败、短暂空 feedback、重复需求和取消 | 可控故障/延迟入口待交付 | 有限队列/重试/退避；空反馈不误删保底；无无限请求循环；记录请求稳定/收敛窗口 |
| S3-04 | 单/双线程重复跑，捕获 upload→barrier→页表有效→draw→retire 顺序 | `--worker-thread-count 1/2 --validation`；RenderDoc 可用性先证实 | 无未上传映射、无 slot 提前复用；CPU submitted 与 GPU completed 不能混为一个状态；单次捕获不能代替压力测试 |
| S3-05 | 加载 A→B→A 时让旧 IO/上传晚完成，检查 generation 丢弃与 GPU 包退休 | `loadworld` 分帧注入 + 新 generation/epoch 日志待交付 | 旧结果不能污染新 World，旧任务不能解除新任务 pin；退休后显存回落且无引用失效 |
| S3-06 | 固定轨迹重复冷/热运行，统计 mip/回退/stale/IO/CPU/GPU/显存 | perf + 新 VT 机器统计待交付 | 当前统计不足以覆盖全部指标；以冻结阈值判断，记录超限与缺证，不能仅凭无崩溃通过 |

### S4：Editor 增量刷新（待证据）

| ID | 可执行验收 | 现有入口/交付要求 | 风险与退出证据 |
| --- | --- | --- | --- |
| S4-01 | 跨页/跨 tile 连续 Paint/Erase，小笔刷→Undo→Redo；延迟旧任务完成 | 现有 Landscape UI；新 revision/dirty/取消统计待交付 | 先扩滤波邻域再映射各 mip/border；旧任务不清新 dirty；小笔刷不能无诊断整地形刷新 |
| S4-02 | Sculpt 斜坡，比较几何、法线、材质依赖刷新范围并下次 Play 查碰撞 | Landscape Sculpt、`terraincollision stats` | 宏高度不重烘 tiling 源；碰撞保持“下次 Play 重建”，不宣称 Play 中 Sculpt 支持 |
| S4-03 | 改层纹理、UV/混合规则、height 源、位移倍率和 placement | TR/MI 编辑隔离副本 + 新 hash/dirty 输出 | 真实依赖才失效；重映射后 detail/TES 对齐；保守全失效必须有原因和面积统计 |
| S4-04 | 未保存编辑→尝试 Cook→保存→Cook→重载；另用冻结快照导出 | Save Heightmap/Save Paint 现有 UI；新 Cook 快照入口待交付 | Save 成功不等于 Cook；未保存快照和发布 hash 可辨认；普通 Cook 不能偷偷用未保存缓存 |
| S4-05 | 有未保存会话时外部改文件，读取失败/写失败后 Undo/重载 | 隔离文件副本、编辑器操作和故障入口 | 明确冲突处理，不覆盖未保存状态；保持旧有效页面；记录错误而非静默回到 layer0 |
| S4-06 | 在仅能容纳祖先和有限替换槽的预算下快速编辑，再等待稳定 | 新内容/驻留状态及队列统计待交付 | 先回退祖先再退休/上传；最终画面等于最新 revision；不得每 stamp 全局 idle |

### S5：资产迁移、导出与旧链路退出（待证据）

| ID | 可执行验收 | 现有入口/交付要求 | 风险与退出证据 |
| --- | --- | --- | --- |
| S5-01 | 先隔离真实 2×2，再同资源分支迁移完整 4×4，重跑冻结轨迹 | §4 fixture 和 §3 命令，新路径显式开关待交付 | 图片/内存/CPU/GPU/IO/包体同条件对照；记录资产格式和生成版本 |
| S5-02 | game-only Release 包冷启动，脱离作者资源根和构建工具 | §6 现有 exporter/validate 脚本 + 新 Cook 包闭包 | 不能回退读取开发目录；必需高度与尾层完整；缺包/版本不匹配明确失败 |
| S5-03 | 搜索全部 height/full source-array 消费者并逐项关闭旧资源 | 源码/descriptor/SPV reflection + GPU 资源捕获 | height atlas 生效后才能删除完整高度数组；detail/macro 仍需源数组单列，不虚报全部释放 |
| S5-04 | 显式新旧模式互斥，重复 World 切换/重载；删除不消费旧 VT 路径 | 新模式配置、资源清单与删除 diff 待交付 | 不混页表/包；旧层源 atlas/feedback/参数退出有证据；记录对照开关撤除时点 |
| S5-05 | 核查契约/routing/testing 与实现同步，完成模块/包/两线程回归 | `py -3 tool/validate-docs.py`、架构审计、实际新增 tests | 历史计划不得被复制成当前契约；全部必需证据齐全后再决定阶段通过 |

## 6. 现有导出闭包与适配检查

源码事实：`tool/game-export/gameExporter.cpp` 的 `FileCollector::Scan/Asset`
递归扫描 JSON，显式 asset 字段和 `Common/`、`Maps/`、`Generated/` 前缀字符串
进入资源闭包，JSON 再递归；二进制内容不会被扫描。当前 TR/texture 的完整资源前缀
因而可被发现，但不是所有新字段或二进制内的相对引用都天然支持。
`paintPath` 不会自动附带 `<paint>.meta.json`。新 Cook manifest 也不能仅靠
目录“正好在 Generated 下”就认为所有页块、索引与高度 payload 已导出。

适配应由主线程在生产实现中完成，独立验收检查：

1. scene/TR 明确引用当前 Cook manifest；manifest 闭包包含版本、依赖摘要、
   全部索引和随机访问页块、各表面尾层、常驻高度数据/映射/采样配置。
   如果内部只保存相对文件名/二进制 offset，exporter 必须有专用解析或明确文件列表。
2. 每个必需文件进入 `game-export-manifest.json` 的 size/hash；缺失、截断、
   错版本、路径越界/重名冲突应拒绝。现有 `--dynamic-assets <json-array-file>`
   可临时显式列出所有资源相对文件做闭包探针，不能替代最终自动闭包。
3. 现有 shader closure 使用 `shader/spv/shader-build-cache.json`，
   运行时 compute 白名单当前包含 `terrain/terrainPageBake.comp`。
   新运行时需要的 compute/material 输出必须加入实际枚举/manifest；
   Editor/Cook-only 构建 shader 是否进 game 要按消费者决定，不能照单全拷。
4. 保留 UI/runtime shader/flavor gate、MI→M JSON/`shaderEvaluation` 和反射输出，
   不能为新资源跳过现有完整性校验。旧完整层数组仅在无消费者后删。
5. 完整岛屿含 Echo，game 构建须包含匹配的 C# runtime/gameplay 闭包；
   最小无脚本 fixture 可先验证 VT，但不能替代完整岛屿包验收。

待主线程交付独立 `build-game`（当前未核实存在可用 game-only 缓存）后执行：

```powershell
cmake --build build-game --target main_game vl_export_game -j 4
build-game\bin\main_game.exe --initial-scene Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json `
  --shader-force-rebuild --exit-after-frames 2
build-game\bin\vl_export_game.exe --project-root . --flavor game `
  --initial-scene Generated/HoudiniPegasusIsland/SC_terrain_pegasus_4x4.json `
  --output artifacts/terrain-composited-vt/game-package
powershell -NoProfile -ExecutionPolicy Bypass -File tool\validate-game-package.ps1 `
  -PackagePath artifacts/terrain-composited-vt/game-package `
  -VerifyTool build-game\bin\vl_export_game.exe -SmokeFrames 600
```

前提为 Release、game-only marker、正确 C# 与资源版本、新 Cook 已完成。
新目录导出，不覆盖旧包。game cache 重建后不要插入 editor/Debug 启动再导出，
否则共享 shader manifest 的 compilePolicy/产物身份可能改变。
`validate-game-package.ps1` 现有成功只证明其当前检查范围；另须从包目录启动、
在不改名/不移动共享资源树的隔离环境中禁止访问开发资源和 Cook 工具，证明只读冷启动。
对隔离包副本执行删尾页/删高度/版本不匹配用例，保存明确错误及非零状态。

## 7. 证据交接与判定

建议证据根：`artifacts/terrain-composited-vt/<阶段>/<run-id>/`；
已创建的 S5 几何预检证据见 §0，正式 renderer/Cook 运行证据仍待交接。
每次运行记录代码/资源 HEAD 与 dirty diff、二进制 hash、cache、工具/驱动版本、
shader/Cook manifest hash、配置、完整参数/命令序列、线程数、相机轨迹、帧窗口、
退出码、stdout/stderr、validation 实际开关、原始截图/读回、统计与捕获文件。
证据目录被 gitignore 时，结论和关键数字仍需后续写入可审阅的验收记录。

判定只用：**待执行 / 待入口 / 缺证 / 失败 / 通过**。
无新计数器就记缺证；故障入口没有实现就记待入口；没有运行就记待执行。
禁止把编译成功、旧 CPU tests、旧覆盖页 parity、无崩溃或单张截图等价为 S0–S5 通过。

主线程下一次交接至少提供：

- 当前阶段与待验收 commit/diff；构建已结束且可独占运行的窗口、明确 exe。
- 新/旧路径选择方式、Cook/包 validator 的真实命令及格式版本。
- 不改生产逻辑即可设置的预算、统计与故障/延迟入口，及对应新增测试名。
- 隔离 fixture/资源版本、S1/S2 对照结果和拟冻结阈值。

收到后独立执行对应阶段、重现失败并给出证据路径；跨阶段退出条件缺一项时保留缺证，
不提前勾选后续阶段。本次交付止于可执行入口与验收准备，等待主线程继续。
