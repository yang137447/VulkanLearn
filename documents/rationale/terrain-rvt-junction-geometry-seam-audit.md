# junction_0_0 几何缝 CPU 定点诊断

2026-11-21。仅 CPU 取证，不修改生产代码、资产或配置，不启动 GPU。
本记录不是当前契约或最终视觉验收。

## 已证

实际读取 `p5/full-tour-tourr4-w2-smoke-r4/junction_0_0.bmp` 和该次 plan。
交点是 Landscape_00/01/04/05，世界 X=Z=−100800 cm，
宏高度 8923.415051666056 cm（camera plan 的 float64 值）。
实际日志相机为 (−100800,9723.415039,−99600)，forward=(0,−0.554700,−0.832050)。
不是中央 05/06/09/10。

独立新编译 CPU probe 调用当前生产 `TerrainMeshBuilder::BuildChunks`、
`TerrainHeightmap::MeshUvTransformFor/SampleHeightCm` 和 HostImage。
PNG 按生产解码规则转 R32_FLOAT。每块仅构造交点所在真实 chunk：
00=(3,3)、01=(0,3)、04=(3,0)、05=(0,0)，各取 LOD0/1/2。
完整 native 边界顶点在 artifacts 的 `native-vertices.json`。

四边交点附近 2520 cm 范围各取 1025 个同世界配对点；
高度和位置差全部 0，归一化 frame 最大分量差为
4.91e−6、1.20e−5、7.07e−6、4.93e−6。
每个 native 粗级边界环与同块 LOD0 的高度和法线差均为 0。
这排除该范围宏网格位置断开；不表示已读取真实运行 draw LOD。
当前实现没有 shader morph，粗级使用细边界环缝合。

**已证实现缺陷：TES coverage 输入坐标域不匹配。**
`mf_terrainLayerBlend.glsl::TerrainResolveTopLayers` 以局部
`context.texCoord` 直接采共用 32×32 coverage page table；
包内 production 的每块采样域却是 `coverageUvOffset + UV*coverageUvScale`，
scale=[0.25,0.25]，四块 offset 分别 [0,0]、[0.25,0]、
[0,0.25]、[0.25,0.25]。runtime service 直接绑定这组共用图，
TES reader 没有应用该变换，跨块 UV=1/0 读到全局图两端。
这不同于 producer 按 sampling 变换后的正确查询。

实读 tourr4 包内生产 image bytes、ID、权重、catalog、R16 height atlas 和 LUT，
按当前 TES 跳过 height blend 的公式复算：

| 边 | TES 七层权重最大差 | 位移最大差 cm | 可见配对点最大投影距离 px |
| --- | ---: | ---: | ---: |
| 00/01 | 0.945098039 | 3.284205222 | 1.419244425 |
| 04/05 | 1 | 3.227500599 | 0.818351153 |
| 00/04 | 0.992156863 | 2.819471313 | 1.350447066 |
| 01/05 | 1 | 2.501418432 | 1.303065190 |

同一组 CPU 点改用 production 声明的全局 coverage UV，
配对 UV、权重、标量位移差均为 0。此为 CPU 反事实，不是资产/源码整改。
位移频率为 `4*layer.uvScale*12`，七层都是整数；
先前基础 Rock 的 3.5 repeats/tile 不能用来解释本次 TES 相位差。
参数 radius=60m、nearGuard=200cm，本组采样距离在有效位移区内。

数值为 native float32 宏网格加 float64 TES 输入复算。
投影使用 plan 相机、HFOV65、1280×720；是配对边的投影距离量级，
不等于已经预测出裂口覆盖像素：未模拟实际整数细分节点、三角光栅、
MSAA resolve 和背景。不能用绝对距离混淆开口与覆盖重叠。
不能宣称该表完成屏幕黑线的因果证明。

## 身份与排除边界

checkout、tourr4、C009a、C008 的 TCS/TES、layer blend、
coverage 和 height-atlas 五个 shader 文件逐份 SHA256 相同。
输入清单、末尾复核在 `inputs.json`、`verification.json`；
源码以后可继续变化，应按收据解释本次证据。
包内 runtime detail MI 显式 TES=1，基础 MI 和 runtime TR 已读取。
实际 draw 是否处于 LOD0/已驻留必须另回读，不能以阈值125000cm推断代签。

page border byte oracle 的既有通过仅有限排除已验的页字节/边框错误。
rawComplete/noIdMiss 是反馈/驻留证据，不能排除 TES 输入取错世界区域，
也不能排除其他材质、深度或光照原因。此次没有完成作者 Paint 全岛复核，
不以中央旧审计替代本交点，也不把生产权重差称为作者 Paint 差。

## 最小 GPU 交接

CPU 计划已写入 artifacts 下 `gpu-handoff.json`；未创建私有 stage、
未生成新 shader/SPV、未调用 E003 或其他 GPU wrapper。
执行者从 tourr4 复制独立诊断 stage，镜头和 worker=2 保持不变：

1. A 保留正常 TES/位移，输出同 submission 的 depth、worldPosition、
   geometry-valid/placement ID、实际 draw chunk/LOD/拓扑。
2. B 仅四份 detail MI 增加 `u_layerDisplacementScale=0`，保留 TES 和 TCS，
   用相同输出隔离位移因果。该 shot 不作最终通过。

若 B 仍有 invalid-depth 裂口，才追加独立 TES-off/三角拓扑 shot；
关闭 TES 不能冒充最终无裂缝通过。不要只复拍 RGB 就给像素分类。
最终需要执行者修复坐标域根因并在正常 TES/位移下重新证明真实无缝。

复算脚本：[rvt_geometry_seam_audit.py](../../tool/validation/terrain/rvt_geometry_seam_audit.py)，
native probe：[rvt_geometry_seam_probe.cpp](../../tool/validation/terrain/rvt_geometry_seam_probe.cpp)。
输出目录：`artifacts/terrain-rvt-production/geometry-seam-audit/`。

## 授权根修实现检查点

用户随后授权本执行者实施根修，因此此节是**实现方自测**，不是独立验收。
生产源码只改 `mf_terrainLayerBlend.glsl` 一行：
`coverageUv = clamp(context.texCoord1, vec2(0.0), vec2(1.0))`。
Local 权重分支仍读局部 `context.texCoord` 并使用 local→global 映射；
独立 raw mip2 height atlas 与 fragment composited base evaluator 未改。
TCS/TES 模板、M_terrain schema、Runtime/Service 和资产均未修改。
源码检索确认此 helper 当前由 M_terrain.surface 和地形 base.tese 引用，
不存在被此次改成全局 UV 的通用非地形材质调用点。

新增 candidate 复算用**真实 native mesh UV1**，没有以 ideal UV 代替：

| 边 | 修前位移差 cm | 修后位移差 cm | 修后可见配对投影距离 px |
| --- | ---: | ---: | ---: |
| 00/01 | 3.284205222 | 0.004945018 | 0.002416493 |
| 04/05 | 3.227500599 | 0.038637736 | 0.002514646 |
| 00/04 | 2.819471313 | 0.005797101 | 0.003107474 |
| 01/05 | 2.501418432 | 0.008154735 | 0.003797465 |

**不是 scalar zero proof。** 之前为零的是 production affine UV 参考。
实际 mesh max 边有 `0.5/(128*32)` 内缩，内插后保留残差；
全岛48条有向内部边、每边513点的实际 UV1 coverage 查询，
最大七层权重差为 0.1257367387033399，不能宣称全岛权重连续。
16 TR 的 pageOrigin/pageTiles 与 publication sampling 逐份一致，
48 条 affine 对边、9 个 affine 交点相等；
这只证明映射和真实查询路径，不证明全岛宏几何、光栅或视觉通过。
回归另要求四条实际位移/投影差均比原始失败收据降低，不设置视觉通过阈值。

`--candidate` 输出独立保存在
`artifacts/terrain-rvt-production/geometry-seam-fix/`，原审计未覆盖。
实读包内 source 与新 source 的差异检查严格要求仅此一行。
新增 49,248 条 local/global UV 查询供 bounded helper 使用；helper 包含真实生产
mf blend，输出解析后的 IDs、weights、raw displacement 和 status。
GPU fixture 只编译和 SPIR-V 校验，未执行。
现有 native Local overlay 20 项 CPU 回归和 TES macro 6 项回归均通过。

CPU 私有 stage 从 tourr4 复制 shader/cache/config，再仅替换该一行源码；
包未导出的 shadow 测试源从 checkout 补齐，仅供编译测试。
stage 不含 Game exe/resources，不是可运行新包。
使用实际 MaterialShaderComposer/ShaderCompiler 新编译 Base TES0/1、
ShadowDepth TES0/1、LocalShadow TES0/1，以及 feedback DefaultLit/Unlit；
六组材质加两组 feedback 均通过 glslang 阶段链接。
阴影 Composer 当前不生成 TES 阶段，不能把 shadow TES=1 宏变体称为位移阴影实现。
实际 stage 源、cache、编译输出、日志和全部命令绑定在 `prepared.json`、
`shader-compile.json`。没有创建 Vulkan device 或启动 GPU。

### 发布身份的实算边界

只改消费者 helper，不在 World evaluator 的 generator include 闭包内。
使用原生 `TerrainCompositedProductionClosure::ContentHash/SurfaceContentHash`
把该 author shader 的依赖 digest 与 RawAuthorSource 对象改成新 bytes 的反事实：

- 完整 production hash 从
  `5a0265a470baca4eeeb7008e639636af69cade40bc42e5c4ac3967bcf184ab3f`
  变成 `67b8d60217305afedf1267e110675a83d0c738112efba2614ebb924362235a70`。
- 16 个 base SurfaceContentHash 全部不变：该函数清空 authorDependencies，
  按 generator 与基础评价输入投影，丢弃不被基础求值引用的 author shader 对象。
- M_terrain 静态 JSON 与 surface 文件本身未改，但递归 include 的
  mf shader 为 bytes 依赖；`ValidateTerrainCompositedAuthors` 仍会报旧 Cook stale。

该反事实只写独立数值收据，没有发布、改写或认可任何新 manifest。
base 页不需要因 kernel 变化重新数值烘焙，但完整 author/production 身份必须由
支持的 Cook/publication refresh/export 路径更新；不能绕过 stale 校验，
也不保证现有工具一定提供免烘焙刷新命令。由 Pascal 用匹配的新 cache 导出新包。
旧 C009a/tourr4 signed 包保持原样。

最新交接是 `geometry-seam-fix/handoff.json`：主执行者安排修前→修后同相机两张，
正常 TES/位移，附 depth/worldpos/valid ID 和实际 LOD。
旧 displacement-zero/TES-off 仅在归因仍不明确时追加；
未启动 GPU，本执行者不给最终视觉 PASS。

## 完整私有 stage 与真实修前/后两 shot

用户进一步授权从原完整 tourr4 复制独立诊断包，无需先等正式重新发布。
实际创建 `p5g/gb1`（before，全文件同原包）与 `p5g/ga1`（after）。
after 在相同 bin/resources/config/publication 上仅替换 mf blend 一行，
并在该完整 stage 中用原生 ShaderCompiler 重编 Base TES0/1 与 feedback，
没有移入测试用的其他材质或 shadow 源。完整 approved delta 23 个文件，
逐项旧/新大小和 SHA256 见 `full-stage-prepared.json`。
原 export manifest 仅作为复制来的历史基线，不认证这份诊断 shader/cache；
没有改写 author/full production hash 或绕过验证。正式发布义务仍保留。

两 shot 已于本轮串行实际执行，未与其他 live renderer 并行，
都是 worker=2、MSAA=2、正常 TES 宏和原位移倍率，镜头命令相同。
两进程 exitCode=0，实际截图 1280×720 BMP 完整，
日志未见 VUID 或初始化失败。启动 shader 都是 hits=60、misses=0、
compiled=0、shaderc=0；新 stage 的运行接受了先前原生编译的 cache。
日志与原图在 `geometry-seam-fix/ab-01/{before,after}/`。

**直接查看两张未经修图的实际截图：before 重现中央水平黑线，
after 正常 TES/位移下该黑线消失。** 这证明该单行修复及其共享 fragment/TES
消费者在此镜头产生实际可见改善，不将其扩大成全岛、全模式或逐像素几何验收。
共享 helper 同时影响 fragment detail 与 TES，单凭 RGB 两图不拆分两者的贡献。

截图之前和之后的 `terrainlod stats` 均明确报告：
00|3|3、01|0|3、04|3|0、05|0|0 全部 `level=0/3`；
不是用原始加载期 coarse-only residency 行或配置阈值推断实际 draw 级别。
没有关闭位移、TES 或改作者权重，没有改 budget。
已有 debugview 基础模式只包括材质/法线/光照等，本次没有实现或取得
raw depth/worldPosition/valid-placement ID 回读，因此不把色图称为 depth 裂口证明。

复跑命令（两张 GPU 图，需由主执行者协调空闲窗口，并指定新输出目录）：

```powershell
py -3 tool/validation/terrain/geometry_seam_ab.py --execute `
  --output artifacts/terrain-rvt-production/geometry-seam-fix/ab-02
```

准备工具为 [geometry_seam_ab.py](../../tool/validation/terrain/geometry_seam_ab.py)；
首次 `--prepare` 创建 after，`--prepare-before` 创建独立 before，
已存在的 stage 不自动覆盖。实际执行时会复核预备的 stage inventory。
