# 地形合成材质 RVT：当前实现与验证边界

> 状态：**当前契约**。2026-11-21，terrain-openworld 工作树源码检查点。
> 当前为 V2 稀疏发布、gen3 按需生产与 World 共池实现；P3 性能、Full16 独立
> Game 和 P4 真实编辑尚无完整通过证据。不以接口、CPU 测试或历史 V1 收据代替验收。

CPU 宏高度、几何、Paint、coverage 与碰撞见 [terrain.md](terrain.md)。
基础材质唯一运行路径是合成页；旧直接基础层评价、层源 VT 和 V1 自动 fallback
均不受支持。动态 detail/macro/global 与独立 tiling 高度不是基础材质旁路。
操作见 [验收入口](../reference/rendering/terrain-composited-vt-acceptance.md)。

## 1. 可核对的源码边界

| 边界 | 源码 | 当前职责 |
| --- | --- | --- |
| 布局与身份 | `source/terrain/terrainCompositedVT.*`、`source/terrain/terrainCompositedIdentity.h` | 完整虚拟 mip 链、页键、内容摘要和 token |
| 稀疏发布 | `source/terrain/terrainCompositedVTStore.*` | V2 严格索引、实际输入/页读取与全闭包校验 |
| 生产输入 | `source/terrain/terrainCompositedProduction.*`、`source/render/resource/terrainCompositedProductionGpu.*` | 严格 CPU 协议、真实 bytes 冻结/恢复、GPU 输入所有者 |
| 显式邻接 | `source/terrain/terrainCompositedAdjacency.*`、`source/terrain/terrainCompositedAuthorRecipe.*` | World 集合、边区间与 UV 变换；不凭位置推断连接 |
| 驻留与调度 | `source/terrain/terrainCompositedResidency.*`、`source/terrain/terrainRvtScheduler.*`、`source/render/terrain/terrainCompositedRuntime.*` | 共池、缺页请求、生产预留、页表提交与 epoch 退休 |
| 总账/协调 | `source/terrain/terrainRvtBudget.*`、`source/render/resource/terrainCompositedService.*` | 单 World 子账、跨 World RVT 及外层 terrain 准入 |
| GPU 产页 | `source/render/resource/terrainCompositedBakeGpu.*` | World compute/padding、独立任务参数/descriptor、输出/status |
| 高度 | `source/render/resource/terrainHeightAtlas.*`、`source/terrain/terrainHeightAtlasStore.*` | 独立常驻源 mip2 R16 atlas、LUT 与高度 Store |
| 编辑依赖 | `source/terrain/terrainCompositedDirty.*`、`source/terrain/terrainCompositedRefresh.h`、`source/editor/terrain/terrainCompositedAuthorMonitor.*` | dirty/身份更新、作者监控；真实 P4 闭环未验收 |

## 2. TR、World 与身份

TR `compositedVT` 只接受 `enabled`、`packagePath`、`virtualResolution`、
`productionSourcePath`。必须显式 `enabled=true`、非空资源根相对 `packagePath`；
旧 `residentPages`、`heightBudgetBytes` 为未知字段，不能控制物理池。
虚拟分辨率可显式写 `[width,height]`，每边校验范围 128..8192。
作者预检不要求包已存在；这不表示未发布包可供 Game 绘制。

同一 World 所有 terrain 的 `packagePath` 必须选同一个 manifest，
surface 集合必须恰等于 `TerrainPlacementIdentity(plan)` 的集合。
Editor/Cook 另外要求所有 TR 指向同一个非空 `productionSourcePath`；
Game 非作者模式不读取配方。配方与运行时 manifest 是两个协议。

Game exporter 按场景 placement 身份选择 publication 中的 base/detail MI，
将包内 TR 的材质路径改为 manifest 父目录下归档 MI 的资源根相对路径，
并在改写前后调用原生 `TerrainAssetValidator`。原作者 MI 及其贴图不参与
这条 TR 依赖扫描；归档 MI 仍使用统一 resolver/材质 schema 校验。
包内 TR 去掉 `paintPath` 和 `productionSourcePath`，保留 layers、几何、
LOD、UV 和 placement 数据。几何/碰撞必需的高度文件以原字节复制到
`Runtime/Terrain/height_<digest>.<ext>`，TR 同步改写路径。
同 TR 多 placement 时派生独立 runtime TR，保留隐式 layer key 的原资产
身份，不任取第一个 surface。Store 生产对象（包括 RawAuthorSource）
仍完整随包并校验；这些改写和 CPU 校验不证明独立 Game 的文件访问或视觉验收。

runtime compute 导出清单包含 `terrainCompositedQuality.comp`，与 World/
WorldPadding 两个生产入口一起闭合真实 cache StageEntry 的主源、transitive
include、runtime SPV 和 debug reflection 输出。安装校验反向检查 required
compute 均有唯一记录且源/输出摘要一致。已发运每份 M_ 定义的参数 include
从私有 stage 逐份复制，并用原生 `MaterialParameterIncludeGenerator` 在内存
重算字节校验；不再因“启动可生成”而省略。生成器在 bytes 相同时不提交写入。
这项静态闭包校验不代替独立 Game 首跑的 inventory 差异与只读 ACL 实测。

作者配方严格顶层为 `kind,version,surfaces,adjacency`，固定
`kind="terrain-rvt-world-author"`、version 1。surface 身份与场景完全一致且无重复。
每条邻接为 `sourceSurfaceId,targetSurfaceId,sourceEdge,targetEdge,
sourceEdgeInterval,targetEdgeInterval,sourceUvToTargetUv`；
edge 为 minU/maxU/minV/maxV，UV 变换有六个数值，由 AdjacencyGraph 校验。
非连续摆放不自动登记邻接。

内页 128×128、每侧 border 4、存储页 136×136。虚拟 mip 每级
`max(1,floor(previous/2))` 到 1×1；矩形/奇数尺寸保留独立有效范围。
`TailMip()` 是首次每边至多一页，**根页是 `MipCount()-1` 的 `(0,0)`**。
Full16 R8192 每 surface 14 级，全虚拟域 87,488 页；
虚拟域页数不等于随包页数或常驻页数。

当前生成器常量来自 `source/terrain/terrainCompositedIdentity.h`：

```text
generatorVersion = 3
filterRule = mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3
encoding = gpu-rvt-adjacency-area4-rgba8-v3
```

BuildDesc 使用 BLAKE3 `terrain-composited-vt-v1` 域计算 ContentHash，
该 hash 域名字不是容器 V1 支持。生产闭包 hash 域为
`terrain-composited-production-v2`；surface 投影为
`terrain-composited-production-surface-v2`，覆盖邻接可达输入、静态参数与生成器身份。
动态 global/height atlas 等在完整闭包保留，基础页身份按实际消费者投影；
非零 height blend 的源高度与 displacement catalog 参与基础身份。
token 绑定 World generation、revision、contentHash 和完整页键。
**gen3 相同但 shader bytes 改变仍需新 Cook**，不能混用早期五阶段或 gen2 包。

## 3. Store V2：严格稀疏

`TerrainCompositedVTStore::Version=2`；Publish/Open 无 V1 兼容分支。
manifest 顶层恰为：

```text
kind,version,innerExtent,border,storedExtent,mipRule,filterRule,
package,packageBytes,packageHash,surfaces,pages,production,roots
```

固定 kind 为 terrain-composited-vt、布局 128/4/136、mipRule 为
floor-half-to-one、filterRule 为上述 gen3 规则。严格解析拒绝重复 JSON key、
超过 32 的深度、未知/缺失字段、非法整数及非规范摘要。
摘要为 64 位小写 BLAKE3-256 hex。

| 记录 | 字段与约束 |
| --- | --- |
| surfaces | `surfaceId,width,height,addressMode,dependencyHash,generatorVersion,encoding,channels,contentHash,tailMip`；无重复、与 production surface 集合及身份一致 |
| pages | `surfaceId,mip,x,y,offset,size,hash`；**只列实际 payload**，页键合法且无重复 |
| roots | 每 surface 恰一条末级 `(surfaceId,MipCount()-1,0,0)`，必须实际列入 pages |
| production | 严格 generator/objects/surfaces/adjacency/hash 子协议，不接受任意附加字段 |

合法未索引细页由布局推导，`HasPackagedPage` 返回 false；
`ReadPage` 对未索引页仍报错，不能通过猜测 IO 异常区分合法缺页。
包名恰为 `<packageHash>.vlcvt`，与 manifest 共址。16 字节包头为
ASCII `VLCVT002`、u32LE version=2、u32LE storedExtent=136。
页内部按 channels 顺序连接；offset 自 16 连续，size 等于 PageBytes，
无重叠、间隙或尾随数据。当前生产输出严格为
`rgba8-srgb,rgba8-unorm,rgba8-unorm`，每页 payload 221,952 B。
协议格式集合较宽不代表当前生产运行时可以选择 BC。

manifest、包、输入采用 rooted/pinned 文件读取；拒绝根逃逸、链接祖先或文件替换，
检查句柄身份、大小和摘要。默认 limits 为 manifest 64 MiB、包/单输入/
输入总量各 1 TiB、1,000,000 页；这些不是运行时内存额度。

| 检查时机 | 实际检查 |
| --- | --- |
| `Open` | 严格 manifest/布局/索引/根记录、包大小与头、各输入路径/大小/身份；不同步扫描全部可选页 payload，也不在此完成全部输入 bytes 摘要 |
| `ReadInput` / GPU Restore | 消费前读取真实 bytes，核对 size/digest 及 payload 约束；损坏生产输入明确失败 |
| `ReadPage` | 重开包、核对当前大小/头、读取目标页并核对页 hash；不重算其他页/全包 hash |
| `ValidateAll` | 全部输入和随包页/包摘要；发布、exporter/安装 verify 使用此完整闭包边界 |

Publish 先验证输入，按选定页流式写暂存包、完整校验、内容寻址发布，
最后原子替换 manifest；失败不改变上一份 manifest。
没有全虚拟页必须存在的前提，也没有内容对象垃圾回收保证。
合法缺页、随包页读取失败与不可恢复输入损坏分别处理：
输入有效时无随包细页或读取失败可转生产；根索引缺失仍是协议错误。
损坏的已索引根 payload 的恢复/就绪顺序须由真实 Game 证据验证，
不能把安装 `ValidateAll` 拒绝等同 runtime 恢复结果。
当前 Service 报告 `cache=disabled`；没有已验收的可写细页磁盘缓存。

## 4. 真实生产闭包与作者依赖

闭包不仅是摘要：含 image bytes、shader source、compiled artifacts 和作者追踪 bytes；
对象记录 objectId、相对 manifest 父目录的 path、bytes、digest、kind 和 image 描述。
image 描述含 format/尺寸/layers/mips/transfer/address/filter。
Surface 记录 base/detail runtime MI 路径、weightSource、sourceSet、
localToWorld、localToGlobal、256 项 globalToSourceSlice、sampling、静态参数、
inputs 与 authorDependencies。runtime MI 也相对 manifest 父目录，
不从资源根同名 MI fallback。包内 MI 是动态参数权威，
Game 在作者纹理装载前注入生产纹理。

真实输入包括 coverage 表/ID/权重、local weights、catalog、源 albedo/normal/ORM、
height blend 源高度、局部 normal/tangent frame、height atlas/LUT 与动态 global 图。
frame 为 RGBA32F 有符号 local XYZ normal 和 tangent.xyzw，保留 mesh 插值基，
不是 `[0,1]` normal map。globalId→sourceSlice 必须显式恢复。
Baker 仍限定一组源数组、最多 16 surfaces、正 determinant/无 shear/镜像的
affine 均匀缩放 placement；不支持任意材质图或多 sourceSet。

当前 active Baker 创建 **两个** compute：`terrainCompositedBakeWorld` 与
`terrainCompositedBakeWorldPadding`。Service Freeze 当前筛选这两者的 primary sources、
传递 include 与所有编译输出角色，World.comp 为 computeSource。
WorldEvaluator/WorldTypes/Route 的真实 bytes 和 compilePolicy 进入身份。
source-controlled promotion 已删除旧 `terrainCompositedBake.comp`、
Address/BoundsInit/BoundsPrepare/Encode 五个 compute；当前生产入口仅 World、
WorldPadding 及其真实 include 闭包。SamplingDiagnostic/SourceFilterCandidate 属诊断/候选，
不代表运行时有另一个 evaluator fallback；最终包仍须绑定实际二进制与 artifact 身份。

`BuildTerrainCompositedDependencies` 与 Store 是独立协议，
记录 TR、Paint/可选 sidecar、MI/M 链、实际纹理容器及 GLSL 字面 include 摘要。
路径受 canonical resource/shader 根限制，不隐式搜索工作目录。
TR 投影去掉 packagePath 避免循环依赖；M_terrain 静态投影保留 UV/height blend/
weight mode 等基础输入，排除明确动态 detail/global/TES 数值。
`ValidateTerrainCompositedAuthors` 校验当前作者文件；
`ValidateTerrainCompositedManifestConfig` 校验配置而不访问作者文件。
Game 消费生产对象不免除发布前作者校验或消费前输入校验。

`TerrainCompositedProductionObjectKind::RawAuthorSource=3` 承载作者追踪原始对象，
与 Image=0、ShaderSource=1、ShaderArtifact=2 区分；严格 kind 解析允许到3，
非 image 对象的 image 描述为 null。Service 将尚无派生对象映射的作者输入保存为
`author-inputs/<raw BLAKE3>.bin`，并由 authorDependencies 的 productionObjectIds 绑定。
普通输入记录真实原始 bytes；absent 模式记录确定性的缺席标记，不伪造不存在的作者文件。
这些对象依旧经过 Store 输入 size/digest 校验，属于完整发布/导出闭包。

**raw digest 与作者投影 digest 分离**：对象 digest 校验实际归档 bytes；
作者依赖 digest 按 bytes/absent/TR静态投影/MI/M静态投影对应模式校验当前作者内容。
两者不能互换。作者监控基线绑定已验证 Store、surface 身份、依赖 mode/digest 与对象映射，
并实际 ReadInput 验证追踪对象。RawAuthorSource 不作为 generator shader 输入，
也不作为基础层采样输入；SurfaceContentHash 清除 authorDependencies 后只保留
基础绑定和生成器使用的对象，排除纯 RawAuthorSource 追踪对象。
完整 publication identity 仍涵盖这些对象，不能说它们不需要完整性校验。

## 5. GPU 页、双权重与独立高度

基础页只含 albedo、局部 XYZ normal、ORM，不含光照、detail/macro/global 或位移。
mip0 中心评价；粗 mip 服务请求每轴 4 子样本面积近似、按 footprint 选源 mip。
albedo 在线性空间混合再编码 sRGB，法线均值归一化、roughness RMS，
其余 ORM 算术平均。非零 height blend 要求 frozenHeightBlendFade=1 和源 R16 mip3。
邻接路由参与跨边界样本/粗 mip 积分；外边界与显式图决定 padding，
不以历史单 surface clamp 图片证明当前跨块正确。
[P3 中审及追加复核](../rationale/terrain-rvt-p3-independent-audit.md) 保留 Repeat129末页
反例和整改过程；当前 Repeat 评价完整136×136、padding分支跳过Repeat，
Clamp按valid+8裁剪。不能把 Clamp 支持域证明推广为 Repeat 裁剪等价。
Baker 在 pipeline 创建前执行 selected-device ValidateDeviceLimits。
身份明确的 Top4 候选有限数值/validation-off性能子项已有独立接受，
包含有限 Repeat129 回归；这是有限候选范围，不替代 source-controlled promotion 后
真实10936页、完整Game反馈/时序和冻结性能验收，P3整体仍pending。

- CoverageAtlas：同点 ID/权重读取，四邻域合并同 globalId、排序选 top4 后归一化；
  等权按 ID 排序。不能直接双线性过滤排名槽位。
- LocalLayerArray：真实冻结的 lossless R32_FLOAT 逐层 overlay 和 local→global 映射，
  生产权重不预先压平为 coverage top4。TES/detail 按当前 weight mode 解析原始 top4。
  Coverage 成功不代表 Local 已验收。

detail/macro 与 TES 使用 height-blend 前原始权重，层 UV/物理 displacement 保留。
TES requestedLod=2 只读常驻 R16 height atlas；未映射返回中性 0.5，
不 fallback 到源 mip2。基础 bake 非零 height blend 仍消费源 mip3，
不能声称源高度或全部源纹理已释放。中性值不证明高度就绪或置换通过。

height atlas 无 mip 链：源 mip2、128/4/136 repeat border，
R16 atlas/单行 RGBA32F LUT，层项 `(basePage,pagesX,mipWidth,mipHeight)`。
helper 以源 texel 坐标四点 texelFetch 双线性。
独立 Store 为 terrain-tiling-height-atlas version 1，内容寻址 `.height.bin`；
binary 为 64 B `VLTHA001` 头加 R16LE atlas/RGBA32F LE LUT，校验尺寸、hash、
有限整数映射、页范围和 border；payload 上限 32 MiB、manifest 64 KiB。
V2 也将真实 atlas/LUT 纳入 production objects 与统一输入预算。
其生命周期不依赖可淘汰的基础色细页；宏几何和 Jolt 仍来自宏高度。

## 6. World 共池、事务与冻结预算

Runtime 只创建一组三通道 `sharedChannels` 和一个 ResidencyPool；
各 surface 有页表并引用同一物理池。Game 全局 coverage/catalog 从同一 production
owner 恢复，不是每 surface 稳态副本。输入快照、旧资源和任务由强引用/lease 保活。
池容量包含根、Reserved、Resident、Retiring，旧槽在 unmap 与引用 epoch 完成后复用。

Service 每次 RecordProduction 成功录制一页即返回，最多四个全局在途任务；
每任务独立参数/descriptor/output/status。三段 RGBA8 加 u32 status 共 221,956 B；
Record 建立 compute→transfer 可见性，三通道直接 GPU buffer→atlas，
只复制小型 status 到 host。完成 epoch 后读 status，status=0 且 token 当前才接受，
后续页表录制、提交成功 Commit 才成为 Resident；失败 Abort。
slot 数不证明同帧多页通过，Cook 同步 BakePageSync 不是运行性能证据。

冻结文件为 `config/terrainRvtP0Frozen.json`，实际 bytes SHA256：

```text
43fba0dd79a60a4a8b740598c7975d743ace9d32215adb35d8cab213e088e0dd
```

Service 校验固定 hash 与 `.sha256`，exporter 保留该身份。
JSON 内 independentReview=pending 的原始 bytes 未改；
[P0 独立审计](../rationale/terrain-rvt-p0-independent-audit.md) 是有限基线 PASS 收据，
不代表 P1–P5 通过。

| 账本 | 冻结上限 |
| --- | --- |
| 单 World RVT GPU | pool 224 MiB + input 768 MiB + workspace 32 MiB；合计 1 GiB |
| 单 World RVT CPU / pool / tasks | 512 MiB / 512 槽 / 4 全局在途任务 |
| 跨 World RVT 峰值 | GPU 2 GiB、CPU 1 GiB、物理槽 1024；任务仍 4 |
| 外层 terrain（含几何/host） | 稳态 GPU/CPU 各 2 GiB；Candidate+Active+Retiring 各 3 GiB；每 World 几何各 1 GiB |
| 生产/上传 | 生产每帧 1 页、221,956 B；上传每帧 16 页、4 MiB；上传环 2 槽 |

image/buffer 按实际 memoryRequirements.size（含对齐）准入，
descriptor/opaque driver 元数据显式 reserve；CPU 解码、队列、metadata、
host-visible staging/status 与保留输入属于总账。
`TerrainCompositedService::RecordProduction` 的唯一生产选页路径使用
`TerrainCompositedProductionPriority`：先过滤候选事务阶段、所属输入与
`inputPending`，再优先根页；非根页按正 mip 与 mip0 的 3:1 机会分配，
各类分别按 namespace 轮转。正 mip 通常粗级优先，每个 namespace 的
第八次正 mip 派工改用循环页键扫描，mip0 始终用循环页键扫描。
这两个机会比例是调度策略，不改变上述每帧生产页数或任务预算。
有限候选集合中预留失败的项只在本次调用内排除，继续尝试其它项；
实际记录成功后才推进游标，未记录的选择不耗轮次。记录后 Abort
仍保留已推进的公平轮次，页面回收与重试继续由 Runtime/Residency 处理。
selector 的 namespace 状态与游标由实际 CPU Workspace 租约收费；
提交游标只做已预备状态的 swap/标量更新，不进入 World joint commit。
这些策略与 CPU 回归不证明真实近景 10 秒画质或 CPU/GPU 性能通过。
Quality readback 在提交 epoch 已完成且 timestamp query 可读之后，
先把原始 mapped bytes 复制到该 collector 持有的 CPU vector，再由同一
raw ABI 解析器与导出器消费。额外 `Abi::Bytes` CPU 存储加入原有
Workspace quote，并在准入成功后分配；outstanding readback 持有原 storage，
阻止下一次 capture 覆写这份缓存。GPU workspace、采样频率与阈值不变。
cached heap 与 uncached CPU 代理的测量不等于实际 Vulkan mapped 性能，
仍须用新源码/二进制的实机 CPU interval、raw 身份与画质结果验证。
旧 World、candidate、在途与退休资源不能各自独立用满额度；
外层可拒绝候选保留旧 World。账本/phase 有源码，不等于全量 allocation/
真实峰值已独立验收，P2 中审缺项及复验保留。

shader 显式虚拟 floor/ceil mip 采样、最多 8 主轴 tap、各级重算 UV 父页地址；
物理 atlas 只读 LOD0，不生成整图物理 mip。
四张 RGBA8 feedback 编码两个目标 mip，带 frame tag/copy epoch；
当前反馈 pass 的完整配置为7个颜色输出加D32 final depth，其中新增
`terrainQualityWinner0/1` 两张128×128 RGBA32_UINT保存完整winner属性，
基础四张反馈不因此变成另一次绘制或可见性判定。
工作集压力最多按冻结 target+1 报有效目标和 budgetInsufficient；
队列空、CPU estimated fallback 或根 Ready 不等于真实可见画质就绪。

## 7. 验证边界

显式原生 lit 诊断通过 `native_terrain_lit_capture_game` 单独入口注册，
仅在 `VULKANLEARN_BUILD_TERRAIN_CAPTURE_VALIDATION=ON` 时加入构建图；
启动要求显式 TES 与 lit case。Frame 在已有 Geometry 捕获之前提供
`DiagnosticReferencePoolOwner()`，并在实际 `RecordPass` 后调用 source-owned
可选 lit 接口。普通 Game 的 null-hook 路径跳过该回调，不读取 case、
创建参考池或申请额外诊断设备能力。`ITerrainCaptureHooks` 基接口保持原样。
参考 owner 使用原 steady caps，父级仍为实际 peak RVT 与 outer terrain；
参考 atlas、ROI target、descriptor 与 readback 继续按实际需求准入，
不能因诊断需要提高 P0 或走未收费 fallback。

注册和原生编译不等于 lit 视觉通过。实际 ROI tap/slot/format/capacity
足迹、独立原始 GPU 全通道参考页、固定独立 mask、candidate replay 对实际
最终画面的校准，以及同一 epoch 的 lit/depth readback 仍须分别提供证据。

本检查点只执行文档 gate 和只读源码核对，未启动 GPU。
P0 PASS、P6 不采用 BC5/BC7 的决策有独立记录；
P1/P2 中审仍 pending，P3 性能/Full16 Game/P4 编辑未完整通过。
详见 [测试入口](testing.md) 和 [执行台账](../rationale/terrain-rvt-production-execution.md)。

当前 quality 运行链使用原 terrainFeedback 的深度测试 **7MRT winner**，
不再以 final-depth visible replay 重绘：两张128² RGBA32_UINT共524,288 B裸图payload，
保存UV2、主轴UV2、targetMip、bias、surfaceAndTaps、depth bits/valid；
这是裸像素算术，不是全部allocation/workspace。
一次实际反馈绘制同时产生最终winner属性与final D32深度；
quality compute单次导入两张winner/final depth，校验valid、surface和精确depth bits，
然后解析页表/owner身份并copy结果，完成epoch后消费。
winner覆盖自然服从同次绘制的实际depth winner，不用另一轮primitive竞争计数代替。

`terrainQualityCapturePass.cpp` 已删除；保留的 QualityReplay helper、
QualityVisible shader、ABI v2 raw duplicate appendix及旧深度replay记录是历史/兼容材料，
不是当前生产调用链。当前帧模块已有 BeginFrame/AfterFeedback/Submitted 接线，
`VULKANLEARN_RVT_QUALITY_DIR` 控制raw导出目的地，不代表仅有导出接口。
Service stats 的 `qualityReadiness=unverified` 字样不能单独证明完整链未接线或已通过。

本轮两份实际capture的complete=true、duplicate=0、validation错误0、
workspace约27.396 MB只构成winner模块证明；不等于99%可见目标画质、
interactive、Full16Game或整体P3 PASS。当前真实10936页运行准备/执行中的状态
不写为已验收；完整画质、时序、成本和双worker/双权重证据仍须分别核对。
待最终证据包括当前二进制/两 active compute 闭包绑定、双权重离线 Game worker1/2、
根损坏恢复、全岛边/交点及 TES、真实 GPU timing/总账峰值、
Editor Paint/Sculpt/Undo/Redo/保存/Cook/重开。
这些是当前缺失证据，不是对未实现功能的承诺。

旧 2×2 weighted/V1 Full16、S1 ROI、S3/S4、高度 oracle 收据保留在
[历史最终证据](../rationale/terrain-composited-vt-final-evidence.md)、
[旧全岛记录](../rationale/terrain-full-island-cutover.md)；
只证明各自源码/包快照，不能据其“通过”改写本轮 V2/gen3 状态。
