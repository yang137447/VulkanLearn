# TressFX 资产导入

> 状态：**当前契约**（CPU 导入、静态 ribbon 与 CPU/GPU guide 模拟路径已落地）。本文件只描述当前源码与验证覆盖的行为。
> 根点骨骼蒙皮（`simulation.skinning`）已落地，只支持单个源角色 + 单个根骨矩阵；
> VSP、SDF 碰撞、逐帧碰撞网格蒙皮属于后续阶段，当前不存在。

## 实现边界

`source/hair/` 的 `vl_hair_assets` 是纯 CPU 静态库，供根工程 `main`、
`vl_tfx_inspect` 和 `hair_asset_tests` 链接；不依赖 UE、Vulkan、World 或材质系统。
该库提供 `.tfx` 解析、静态几何准备和参考 solver `VL::Hair::GuideSimulation`，
没有 `.tfxbone` / `.tfxmesh` 解析或 follow 生成。GPU 模拟 pass
`VL::Hair::HairSimulationPass` 依赖 `PipelineFactory` 与 Vulkan backend，因此不进入
该静态库，而是单独编进 `main`；静态运行时接入仍位于独立的网格 importer 和引擎顶点工厂，
没有 `GR_*` 描述符或新的 groom 场景对象。

迁移依据是 AMD 分支 `TressFX5-5.4.4`，revision
`61f8dd9b51b88d41937ef8d73618aac778642f41`：
`TressFXTFXFileHeader.h`、`TressFXAsset.cpp`、`Tools/Maya2022/TressFX_Exporter.py`。
来源及派生算法的 MIT 许可保留在
`source/hair/THIRD_PARTY_NOTICES.md`。

## 字节与数据契约

`ParseTfx` 消费内存字节，`LoadTfx` 从 `std::filesystem::path` 读同一格式。
均返回 `RuntimeResult<TfxAsset>`，失败包含 code/message/sourcePath/sourceNode；
不发布部分解析结果。

- little-endian、160-byte header；前 32 bytes 是 version、guide 数、每根点数和五个 offset。
- TressFX 5.0 Maya exporter 实际写 `version=4.0`。继承源 loader 的有限 `version>=4`
  判定，仅解释已知布局；reserved 和尾部扩展字节不消费，不声称理解新版本扩展。
- 点数仅接受 4、8、16、32、64；guide 数必须大于 0。
- position 必需，每点一个 float4；strandUV 为每 guide 一个 float2；
  vertexUV 为每点一个 float2；strandThickness 为每 guide 一个 float；
  vertexColor 为每点一个 float4。
- offset 是文件起点的绝对 byte offset，非零项必须在 header 后、8-byte 对齐、
  不越界、互不重叠。可选项 offset=0 产生空数组，不从 header 误读属性。
- 所有属性必须有限；逆质量与 thickness 不得为负。UV 和颜色不钳制到 `[0,1]`。
- 输入预算为 512 MiB、4,194,304 个点；在 payload 分配前检查数量和字节范围。

xyz、UV 和宽度保留导出值；position.w 是逆质量，根部前两点通常为 0，
其他点通常为 1，可由 exporter 额外固定 tip。解析器不补 compute 线程组、
不增补 guide、不换轴、不改单位、不翻转 UV，不把逆质量当齐次坐标。
TFX 不包含足以恢复导出轴向与单位的元数据，不能从文件名推测它们。
bounds 仅由 xyz 求 min/max，不包含 ribbon 宽度。

## 静态几何

`PrepareStrandGeometry` 只接受成功解析、未被修改的 `TfxAsset`。
它拥有新增几何约束的校验：相邻点不能重合，段长必须可用 float 表示，
内点的相邻方向不能精确折返。失败不发布部分数据。

- 每点 tangent 为 float4，w=0；端点用最近段的 root→tip 单位方向。
- 内点使用相邻**单位**方向之和再归一化，保留 TressFX 的非等距采样语义。
- 长度、方向仅使用 xyz，double 中间计算避免有限 float 坐标差值或平方溢出。
- restLengths 为每点到下一点距离，tip 为 0 占位；maxRestLength 是全局最大值。
- 每点左右两个逻辑 ribbon 顶点；每段索引为
  `(2i,2i+1,2i+2)`、`(2i+2,2i+1,2i+3)`，不同 guide 不连接。
- 这里只生成切线、rest length 和拓扑；不生成相机朝向顶点、thickness taper
  或 GPU buffer，不把 CPU 派生成功当作渲染验证。

## 工具与验证

```powershell
cmake --build build --target vl_tfx_inspect hair_asset_tests material_schema_tests -j8
ctest --test-dir build -R "^(TfxLoader|TfxFileTest|StrandGeometry|TressfxDescriptor|TressfxImport|TressfxMaterial)\." --output-on-failure
.\build\bin\vl_tfx_inspect.exe path\to\hair.tfx
```

检查工具只读输入，stdout 输出 JSON：文件版本、guide/点数、bounds、
属性存在性、ribbon 顶点/索引数、maxRestLength。失败向 stderr 输出诊断，
exit 1；Windows 入口使用宽字符路径。不会创建场景、修改资源或写入导出文件。

`tool/hair-tests/` 独立构造 exporter 布局字节，覆盖可选属性、绝对 offset、
输入预算、截断/重叠/非有限值拒绝、坐标与逆质量保留、中文文件路径、
切线、rest length 和 ribbon 不跨 guide。测试通过根 `vulkanlearn_add_gtest`
逐项注册；不新增 runtime test 标志。合成夹具不代表女性 Maya 资产已完成导出。

### 离线 SDF 生成

`source/hair/sdfBuilder.{h,cpp}` 是纯 CPU 的离线 SDF 生成器，编入
`vl_hair_assets` 并链接到 `vl_build_sdf`；不启动引擎、不创建 Vulkan 资源，
运行时 solver/shader 目前也不消费它。距离单位与输入一致（身体资产是厘米），
负值在网格内部、正值在外部。

```powershell
cmake --build build --target vl_build_sdf -j8
.\build\bin\vl_build_sdf.exe --input <body.obj> --output <field.sdf> `
    [--cell-size 1] [--band 4] [--max-voxels 4194304] `
    [--region x0,y0,z0,x1,y1,z1] [--threads 16] [--sample x,y,z]...
```

- OBJ 只读 `v` / `f`：多边形扇形三角化，忽略 `vt` / `vn`；支持负数相对索引，
  缺失/越界索引、非有限坐标、无面文件和退化域都以 exit 1 和明确消息拒绝。
- 不传 `--region` 时域是网格 AABB 每侧外扩 `--band`。传 `--region` 时域是
  该盒每侧外扩 `--band`，再向外取整到整格（`dims = ceil(extent/cell)`,
  `origin = region.min - band`）；零/负体积、与网格 AABB 完全不相交、或超过
  `--max-voxels` 的区域直接失败，错误消息给出实际体素数。`--cell-size` 默认 1 cm。
- 体素数超过 `--max-voxels` 时直接失败并给出所需数量，不会静默放粗格距。
- 最近点查询先建三角形 AABB 层次包围盒（BVH，叶子 8 个三角形），符号使用
  角度加权伪法线：面内部用面法线，边上用相邻面法线之和，顶点用入射面法线按
  该顶点内角加权求和。不使用单一面法线或射线奇偶性。
- 结果按 `clamp(distance, -band, +band)` 截断。体素顺序固定（x 最快、z 最慢），
  BVH 构建/遍历顺序也固定。
- `--threads N` 默认 `min(硬件线程数, 16)`，0/1 为单线程。多线程按 **z 切片**
  划分：每个体素只由一个 worker 计算，只写自己的槽位，切片边界不改变任何
  单点距离/符号数学。因此线程数、切片划分和调度都不影响输出字节；模块测试
  用 1 vs 4 线程比较完整序列化字节，正式资产也用不同线程数和重复运行核对
  SHA-256。没有跨线程归约。
- `SdfField::mesh` 报告顶点数、三角形数、开放边数（只被一个三角形使用的无向边）、
  非流形边数（被三个及以上三角形共享）和退化三角形数。
- 文件为 little-endian、64-byte header：magic `VLSDF\0\0\0`、`version=1`、
  `dims[3]`、`origin[3]`（最小角）、`cellSize`、`band`、`payloadFormat=0`
  （float32）、`payloadBytes`、payload 的 FNV-1a 32 checksum、reserved。
  体素 `(x,y,z)` 的中心是 `origin + cellSize * (index + 0.5)`。读取端拒绝
  截断/多余字节、尺寸或 checksum 不符、非有限 header 和越界距离。
- 采样 `SampleSdf` 为三线性插值；域外返回 `+band`。跨 band 边界的插值是截断场
  的插值，会低估真实距离。
- `tool/hair-tests/sdfBuilderTests.cpp` 覆盖解析球/盒距离、近边与近顶点的符号、
  锐尖顶点伪法线、开网格开放边计数、字节确定性、1 vs 4 线程并行字节一致、
  写读往返及截断/损坏拒绝。这些是合成夹具；正式身体资产只在离线生成时需要，
  不进入模块测试。

#### 头发碰撞场 `female_head.sdf`

资源仓库里的头部场用下面这条命令生成（从本仓库根目录执行；输出到资源仓库，
不进入本仓库）：

```powershell
.\build\bin\vl_build_sdf.exe `
    --input ..\VukanLearnResources\Maps\SC_tressfx_female\Source\female_body.obj `
    --output ..\VukanLearnResources\Maps\SC_tressfx_female\Collision\female_head.sdf `
    --region -17,127,-20,19,179,19 --cell-size 0.4 --band 4 --threads 16
```

- 区域覆盖 `.tfx` 发丝范围（x [-14.4, 16.5]、y [129.8, 176.2]、z [-17.6, 16.1]）
  并每侧留约 2 cm 余量；域再外扩 band 4 cm，所以发丝所在区域全部在有效带内。
- 分辨率：0.4 cm（110×150×118 = 1,947,000 体素、7,788,064 B / 7.43 MiB）。
  同一区域在 band 4 下 0.35 cm 需要 2,925,720 体素 / 11.16 MiB，超过约 8 MiB
  预算；0.39 cm 也需要 2,105,642 体素，仍略超 2M，因此取 0.4 cm。
- 生成耗时（Debug MinGW，单次）：16 线程 116–117 s，8 线程 188 s；输出均为
  SHA-256 `025583F09A728417C9CAD6D5F917AEBD23A211E2AD6F3AB072285242FFCE01A2`。
  体素中心距离与暴力最近点一致；三线性插值在头部/面部表面顶点上的绝对误差
  （真实距离 0）中位数 0.048 cm、p95 0.124 cm；旧的 1 cm 全身场为 0.096 / 0.234 cm。
- 开放边界：整个身体网格有 11,781 条开放边，其中 3,696 条（480 个边界连通分量）
  位于该区域内。按位置可辨认出的主要特征：左右眼窝各 72 条边的闭环（y≈163、
  z≈9）、嘴/鼻/耳/眼睑一带 20–49 条边的多个小环（y≈152–157），以及颈部
  （y≈146–150，110+64 条边）、胸部（y≈137–146，180 条边）、肩/臂（69–82 条边）
  和上背（78 条边）的壳体接缝；其余 378 个分量只有 3–4 条边，是细小孔洞/裂缝。
  伪法线符号只在闭合流形上被证明；在头部场里，头发可以触及的口、眼、颈部和
  肩部接缝附近，符号不应被当作可信结论。本场没有做补洞或孔洞掩码，运行时
  采样器应当把开放边界附近的结果当作弱约束。距离本身（最近点幅值）不受
  开边界影响。
- 该文件目前还没有运行时消费者：仓库里没有 SDF 采样、GPU 上传、descriptor 或
  solver 接入；本节的字段只是离线生成的资源。

#### 离线碰撞代理 `female_body_proxy.obj`

TressFX 的碰撞 SDF 本应由一个小型 `tfxmesh` 逐帧重建。本仓库没有 `.tfxmesh`，
渲染身体网格有 455,071 个三角形，逐帧体素化不现实。`source/hair/collisionProxy.{h,cpp}`
因此提供纯 CPU 的离线顶点聚类：把真实身体网格压成低模三角形汤，保持输入网格的
局部空间和厘米单位，输出类型与 `vl_build_sdf` 的输入相同。它编入 `vl_hair_assets`，
由 `vl_build_collision_proxy` 驱动，不启动引擎、不创建 Vulkan 资源。

算法与规则：

- 区域过滤：三个顶点都落在 `--region` 盒内（含边界）的三角形才保留；跨边界的
  三角形整块丢弃，所以裁剪处必然有开放边，报告不补洞。
- 聚类：格原点固定在世界原点，cell = `floor(p / cell-size)`；只聚类被保留三角形
  引用到的顶点。簇按顶点首次出现顺序编号，中心用 double 累加后除以计数再收紧到
  float。输出保留输入绕序。
- 剔除：两个或三个顶点落入同一簇、聚类后面积为零的三角形被丢弃；顶点集合相同
  （忽略顺序和绕序）的重复三角形只保留首次出现。输出按三角形首次引用顺序压缩
  顶点，未被使用的簇被删除。
- 报告：输入/输出顶点与三角形数、聚类数、被区域拒绝的三角形数、退化/重复三角形数、
  输出包围盒，以及输出网格的开放边/非流形边数。拓扑计数由 `CountMeshTopology`
  提供，与 `vl_build_sdf` 的报告规则相同（只被一个三角形使用的无向边 = 开放边，
  被三个及以上共享 = 非流形边）。
- 确定性：单线程、固定遍历顺序、double 累加；哈希表只用于查找，不参与输出排序；
  OBJ 写 9 位有效数字且不带时间戳。相同输入、参数和输出文件名生成逐字节相同的
  文件；正式资产两次运行的 SHA-256 都是
  `3AC48E927BFB157DAE12DC93BCCF4C76450C6016C87CBC4CF8CFFFE7482BC8C7`。
- 失败边界：OBJ 无顶点/无面、索引越界、坐标非有限、区域零体积或与网格 AABB
  完全不相交、区域拒绝全部三角形、聚类后无三角形、输出低于 `--min-triangles`
  都以 exit 1 和明确消息拒绝，不发布部分文件。

正式代理的再生命令（从本仓库根目录执行；输出到资源仓库）：

```powershell
.\build\bin\vl_build_collision_proxy.exe `
    --input ..\VukanLearnResources\Maps\SC_tressfx_female\Source\female_body.obj `
    --output ..\VukanLearnResources\Maps\SC_tressfx_female\Collision\female_body_proxy.obj `
    --region -17,127,-20,19,179,19 --cell-size 1.6 --min-triangles 3000
```

- 输入：235,490 顶点、229,329 个多边形；`v`/`f` 读取后扇形三角化为 455,071 个
  三角形。区域拒绝 331,129 个三角形。
- 输出：2,572 顶点、7,391 三角形、2,578 个聚类（压缩后 6 个簇未被使用）；丢弃
  115,300 个退化、1,251 个重复三角形；208,457 B。输出包围盒 x [-16.903, 18.898]、
  y [127.168, 174.920]、z [-11.984, 15.021]，完整落在区域内；1.6 cm 格距使三角形数
  落在 3k–8k 目标内。
- 格距权衡（同一区域）：1.0 cm → 16,959 三角形；1.2 cm → 12,554；1.5 cm → 8,528；
  1.6 cm → 7,391；2.0 cm → 4,753。用已有 0.4 cm `female_head.sdf` 在代理顶点、
  三角形中心和边中点采样 32,136 个点，|距离| 的中位数/p95/最大值为
  1.0 cm: 0.033/0.153/0.424，1.2 cm: 0.042/0.181/0.629，1.5 cm: 0.055/0.243/0.722，
  1.6 cm: 0.061/0.266/0.716，2.0 cm: 0.074/0.316/0.819（厘米；SDF 自身在表面上的
  插值误差约 0.048/0.124 cm）。1.2 cm 用多约 70% 的三角形只换到约 0.02 cm 的中位
  改善，1.6 cm 是 10k 以内较合适的折中。
- 拓扑：输出有 522 条开放边和 2,092 条非流形边。开放边中约 190 条距离裁剪面小于
  2 cm，其余来自原始眼窝/嘴/耳/颈部孔洞和聚类折叠；非流形边集中在眼窝（y≈163、
  z≈9）、耳（y≈157）、嘴/鼻和颈/胸褶皱（y≈130–150），只有约 169 条距离裁剪面
  小于 2 cm。裁剪不是非流形边的主要来源，跨凹陷合并不同表面薄层才是。伪法线符号
  只在闭合流形上被证明；在这些特征和裁剪边界附近，符号应作为弱约束，距离幅值不受
  影响。代理没有补洞、没有跨特征保护，也没有逐顶点法线。
- 运行时逐帧 SDF 管线不存在：仓库里没有逐帧代理蒙皮、SDF 重建、GPU 采样、
  descriptor 或 solver 接入，`vl_build_collision_proxy` 只产出离线 OBJ。

## 静态运行时路径

场景仍用 `type: "mesh"`、`modelPath: ".../Meshes/SM_*.json"`。描述符为：

```json
{
    "name": "SM_guides",
    "type": "tressfx",
    "modelDataPath": "Maps/SC_probe/Source/hair.tfx",
    "strandRadius": 0.05,
    "materialSlots": [
        {"name": "hair", "materialInstancePath": "Maps/SC_probe/Materials/MI_hair.json"}
    ]
}
```

- 必须为 `.tfx`、一个材质槽、有限且 float 可表示的正半径；拒绝 importOptions 与
  顶层 `skinning`（骨骼蒙皮声明在 `simulation.skinning`，见下文）。
  半径是模型局部长度，场景长度仍按厘米作者；没有隐式导出单位转换。
- `TressfxModelImporter` 走 `MeshAssetLoader` 的正式策略选择，输出一个 `guides` section，
  源材质槽为 `hair`，按既有顺序映射到描述符唯一槽。派生流预算为 512 MiB。
- 每点上传两个相同的中心线 position；tangent.xyz 是根→梢方向，w=1。
  location 7 在工厂前为 `(有符号半径, 根梢0..1)`。可选 strandThickness 是半径乘数，
  不自动 taper；几何 bounds 扩展最大半径。逆质量不进入渲染顶点。
- UV0 优先用 vertexUV，其次 strandUV，均缺失时为零。顶点颜色原值保留，缺失时为白色。
  探针材质只用 tint，不把这些导入属性解释成未经确认的颜色空间或 Hair 光照。
- `TressFXStrands` 工厂由 `VL_TRESSFX_STRANDS=1` 选择，进入 shader 编译请求身份。
  工厂在 GPU 用相机统一 ViewForward 与变换后的切线叉积展开，之后才执行材质求值；
  与视线平行时用相机横轴投影定义横向。输出 UV1 为 ribbon 横向/根梢坐标。
- TressFX section 使用共享 ribbon Vertex/Index 拓扑，并为每个对象保留一份中心线
  SSBO（Object Set 2 binding 3）。启用 simulation 时，CPU backend 由游戏线程在帧
  边界运行参考 solver，GPU backend 由 compute dispatch 在每帧 pass 循环之前写同一
  SSBO；graphics `TressFXStrands` vertex factory 从该 SSBO 读取中心线并完成相机
  朝向 ribbon 展开。未启用 simulation 时，SSBO 仍由静态中心线初始化，保证
  descriptor 合同稳定。模拟 buffer、dispatch 与同步见下节。
- 仅开放有 shaderEvaluation、Unlit 或 DefaultLit、双面、AlphaBlendWriteDepth 的材质，
  拒绝 WPO、蒙皮以及其他 ShadingModel/路径。DefaultLit 只提供简单非金属 PBR 预览，
  不恢复 Hair ID 或 Marschner 闭包；普通模型与发丝材质互配在资源加载时报错。
  离线加载明确拒绝，因为离线三角形路径不执行相机朝向工厂。
- `hair/M_strandProbe` 与 `hair/M_strandPbr` 使用 AlphaBlendWriteDepth，进入既有
  forwardTransparent；前者保留粉色 Unlit 几何诊断，后者以 `u_tintColor`、
  `u_roughness`、`u_specular` 提供 DefaultLit 简单 PBR 预览。
  `u_edgeFeather` 控制两侧各自的 UV1 渐隐宽度（默认 0.15，0 为硬边，最大 0.5），
  tint alpha 为 0..1 的整体保留概率。Base Pass 按像素坐标和 provoking vertex 点索引
  的固定散列随机裁剪；丢弃样本不写深度/选中标记，保留样本以 alpha=1 写入，
  避免重复乘覆盖率。种子经 location 8 的独立 flat varying 传递，不改共享材质上下文。
  这是 VL 的静态预览近似，有空间噪声和相机移动时的闪烁，不是源引擎覆盖率算法。
  没有 Marschner、像素面积覆盖积分、时域累积、MSAA/A2C、像素最小半径、
  OIT、发丝投影或自阴影；不恢复 Hair ID，不写 GBuffer。
- 内容浏览器把 tressfx 归为 Mesh；标准 mesh API 支持既有字段读改保存，
  Scene/BP 的模型引用接受该类型。`strandRadius` 不在标准修改能力表中，
  描述符半径修改属于能力表外的源码编辑。

`tool/hair/generate_static_probe.py --resource-root <isolatedRoot>` 创建
`Generated/Validation/SC_tressfx_static_probe/`（存在即拒绝），包含合成 TFX、
SM/MI 和两实例场景；正式 Common pass 资产由调用方提供。
它不改 config、不启动引擎，也不是 runtime test。生成场景说明其夸大半径、
Unlit 与非女性资产边界。

## Guide 模拟 backend

`simulation.backend` 是描述符作者开关：缺省 `"cpu"`，`"gpu"` 选择 compute 路径；
其他值由 `MeshAssetValidator::BuildLoadPlan` 拒绝，错误信息带 SM 资产路径。
backend 写入模型缓存身份，同一个 `.tfx` 的 cpu/gpu 描述符不共享缓存条目。
`simulation.enabled = false` 仍是静态中心线回退，两种 backend 都不分配模拟 buffer、
不构造 solver、也不 dispatch。

### 作者字段

`simulation` 对象的字段、默认值与范围（校验失败时错误信息带 SM 资产路径）：

| 字段 | 默认值 | 范围 | 含义 |
| --- | --- | --- | --- |
| `enabled` | 无对象时 false；对象内缺省 true | bool | false 为静态中心线回退。 |
| `gravity` | `[0,-980,0]` | 3 个有限数 | 重力加速度，厘米/秒²；直接作为积分力，不再套用 TressFX 的 `GravityMagnitude*(0,0,-1)`。 |
| `wind` | `[0,0,0]` | 3 个有限数 | **平均**风加速度，厘米/秒²；Host 四路填入 TressFX 的 `Wind0..3`。 |
| `damping` | 0.995 | `[0,1]` | 60 Hz 下每步保留的速度比例，运行时映射为 `DampingCoeff = -ln(damping)`。 |
| `iterations` | 3 | `[1,16]` | LengthConstraintsWind pass 的偶数/奇数边约束迭代次数。 |
| `rootFixedPoints` | 2 | `[0,32]` | 每根 guide 从根部起固定的点数，即 TressFX 的 `NumVerticesFromRootNoSimulation`。 |
| `maxDt` | 0.033 | `(0,0.25]` | 单帧 dt 上限；`dt = clamp(frameDelta, 0, maxDt)` 直接成为本帧唯一 solver step。 |
| `bendStiffness` | 0.3 | `[0,1]` | 映射到 LocalShape pass 的逐顶点 `Stiffness` 标量；pass 内再乘 0.5（TressFX 公式）。0 关闭。 |
| `windGust` | 0.5 | `[0,1]` | 阵风幅度；`gust = 1 + windGust*sin(2π f t + guide*0.6180339)`，乘在四风混合结果上。 |
| `windFrequency` | 0.5 | `[0,10]` | 阵风频率 Hz；0 时相位固定。 |
| `globalShapeStiffness` | 0 | `[0,1]` | TressFX Integration 的 global shape 强度；0 关闭，既有资产外观不变。 |
| `globalShapeEffectiveRange` | 0 | `[0,1]` | 作用范围占每根 strand 顶点数的比例；0 关闭。 |
| `vspCoefficient` | 0 | `[0,1]` | TressFX VSP 基础系数；0 表示关闭，Host 不录制 VSP dispatch。 |
| `vspAccelThresholdMax` | 5000 | `(0,1e7]` | 伪加速度阈值，厘米/秒²；超过时强制 VspCoeff = 1。 |
| `backend` | `"cpu"` | `"cpu"` / `"gpu"` | 参考 solver 或 compute dispatch。 |
| `skinning` | 无 | `{source, rootBone}` | 可选；把根点改为跟随场景内源角色的单个骨骼。缺省即历史行为。 |

固定步字段已删除：`fixedStep` 与 `maxSubsteps` 不再被解析，出现即在资产边界报错
（错误消息指向 `maxDt`），避免旧资产静默失去效果。`maxDt` 仍留在模型缓存身份中。

### 碰撞胶囊

`collision` 是 SM 描述符的顶层对象，与 `simulation` 同级；所有坐标和半径都是
**资产局部空间厘米**，与 positions、gravity、wind 使用同一坐标系：

```json
"collision": {
    "capsules": [
        {"a": [0.0, 168.0, 0.5], "b": [0.0, 168.0, 0.5], "radius": 8.5},
        {"a": [0.0, 149.0, 0.5], "b": [0.0, 159.0, 0.5], "radius": 6.5}
    ]
}
```

- `capsules` 缺省或为空数组表示无碰撞，Host 不录制 Collision pass。
- 数量上限为 8（`VL::Hair::TressfxMaxColliders`，GPU 侧同名常量），因为当前
  作者规模只需头、颈、肩背少量代理；更多细节应转向 SDF，而不是继续堆胶囊。
- `a == b` 表示球。`a`、`b` 必须是 3 个有限数，`radius` 必须有限且 > 0；
  未知键、缺失键、非 3 元向量、超过 8 个胶囊都按现有错误风格拒绝，消息带 SM
  资产路径。
- 每个 collider 的值都进入 `modelCacheKey`；修改胶囊会使模型缓存失效。
- 当前运行时没有 `.tfxmesh` / SDF 碰撞：资源导出只有 `.tfx` guides、身体 OBJ 和蒙皮
  FBX，`tfxLoader` 也没有 `.tfxmesh` 解析器。离线 `vl_build_sdf` 能从身体 OBJ 生成
  SDF 文件（见“离线 SDF 生成”），但仓库里还没有运行时 SDF 采样、GPU 上传或
  descriptor 接入。

### 根点骨骼蒙皮

`simulation.skinning` 把每根 guide 的根前缀从“钉在 rest”改为“跟随同一场景里的
一个源角色骨骼”：

```json
"simulation": {
    "backend": "gpu",
    "rootFixedPoints": 2,
    "skinning": {
        "source": "FemaleCharacter",
        "rootBone": "Head_M"
    }
}
```

- `source` 是同一场景内的 `mesh` 对象名（`sceneObjectIdentity`），`rootBone` 是该
  对象**目标骨架**（`skinning.skeleton` 指向的 SK）里的关节名，大小写敏感。两块
  都必须是存在且非空的字符串；未知键、缺失键都在 `MeshAssetValidator::BuildLoadPlan`
  拒绝，错误消息带 SM 资产路径。两者都进入 `modelCacheKey`，改来源或骨名会得到
  不同 renderable。
- 只允许 `backend = "gpu"`。姿态来自渲染线程的冻结 `RenderScene`/`SkinPose`；
  CPU 参考 solver 在游戏线程运行且没有 pose，所以 `skinning` 与 `cpu`（包括缺省
  backend）组合在加载期硬错误，而不是运行时退回 rest pin。
- 解析发生在 World 构建期的 `RendererResourceLoadCoordinator`：等场景内所有 mesh
  renderable 创建完成后，按对象名找源对象、取其 `SkeletalPlayback::Asset()` 的目标
  SK、用 `ResolveTressfxRootJoint` 得到关节下标，再把
  `{sourceSceneObject, rootBone, jointIndex}` 存到发丝 `RenderableObject` 上。
  没有第二套场景注册表、GR 描述符或新的场景对象类型。
- 硬错误（消息同时带对象名/骨名与发丝 SM 资产路径，绝不静默回退）：源对象不存在、
  源对象没有蒙皮骨架（`Asset().HasSkeleton()` 为 false）、根骨不在目标 SK 中。
- 共享对象空间：骨骼矩阵把源角色的绑定空间映射到**源对象的模型空间**，而发丝仍按
  自己的 model 绘制。因此两个场景对象的 `model` 矩阵必须一致（逐元素容差 `1e-4`）；
  不一致在构建期报错，不把错位的根点写进 SSBO。当前正式关卡两者都为 identity。
- 运行时 `HairSimulationPass` 在冻结快照的 `drawPackets` 里按 `sceneObjectIdentity`
  找源角色 draw，读取 `skinPose->matrices[jointIndex]`。pose 或 draw 本帧不可用时
  保留上一帧矩阵并只报告一次（stderr），不销毁 pass、descriptor 或整帧。
- 矩阵每帧写入 std140 params UBO 的 `skinning`（mat4，offset 160，整个 UBO 224 bytes；
  C++ `static_assert` 固定大小、对齐与偏移）。没有声明 `skinning` 时上传单位矩阵。
- 不可移动点（`local < rootFixedPoints` 或逆质量 <= 0）每步写
  `InitialPos = M * rest`；这是 TressFX Integration 的规则。可移动点不消费 M，
  只在 global shape 开启时把 `InitialPos` 当作回拉目标。
- 同一矩阵的上 3×3 旋转部分在 Host 分解为四元数 `skinningQuat`（offset 144），
  作为 TressFX `SkinningQuat` 供 LocalShape pass 使用。VSP/局部形状需要的是
  bind→pose 的**增量**旋转；`SkinPose::matrices[jointIndex]` 正是这个增量矩阵
  （与顶点着色器同一用法），不是骨骼的绝对变换。
- TressFX 5.0 映射：源实现从 `GuidesBoneIndexBuffer`（逐 strand 的
  `FBoneIndexData`）在积分 pass 里蒙皮 guide，并用 `NumVerticesFromRootNoSimulation`
  标出“蒙皮但不模拟”的根前缀；本实现只有**单个根骨矩阵**，没有逐顶点多骨权重，
  也没有 `.tfxbone` 导入。`VSP`/`LocalShape` 的 `SkinningQuat` 因此是整根 strand
  共用的同一个根骨旋转，而不是每根 strand 的根三角帧旋转。
- 碰撞胶囊仍是作者绑定空间的静态代理，不随头部/身体蒙皮移动；运行时 SDF 重建和
  逐帧碰撞网格蒙皮都不存在。

### GPU 路径

GPU backend 的位置 buffer 放在 device-local：current/previous/prevPrev/rest/
restLengths/strandLevel 都用 `eStorageBuffer | eTransferDst` 分配，创建时经 staging
buffer + `copyBuffer` 上传一次初值；每帧 compute 与 vertex shader 的读写不再落在
可回读的主机堆上。CPU backend 的 current 仍是 `eHostVisible | eHostCoherent`，
因为游戏线程逐帧映射写入。params UBO 每帧由主机改写，两种 backend 都保持
host-visible/coherent。

每个 tressfx 对象有下列 buffer：

- current positions：既有 `tressfxPositionBuffer`，vec4（xyz = 位置，**w = 可移动标记**，
  1 可移动 / 0 不可移动），同时绑定在 graphics Object Set 2 binding 3；vertex factory
  只读 xyz，不受 w 变化影响。
- previous positions：vec4，Verlet 上一帧位置；w 与 current 同步。
- previous-previous positions：vec4，TressFX 的 `GuidesDeformedPrevPrevPositionBuffer`，
  供 strand-level 伪加速度与下一帧历史使用。
- rest positions：vec4.xyz = 静止中心线，w = 导入逆质量；Integration pass 用它判断
  不可移动点（根前缀或逆质量 <= 0）。
- rest lengths：每点一个 float，第 i 项是 i→i+1 的静止段长，末项 0；创建时上传一次。
- strand level data：每根 strand 3 个 vec4（48 字节）：
  `VspQuat`、`VspTranslation.xyz + VspCoeff.w`、`SkinningQuat`。
- 每个 frame-in-flight 槽一份 std140 params UBO（224 bytes），布局：
  gravity(0)、Wind0..3(16/32/48/64)、controls(80)、shape(96)、motion(112)、
  counts(128)、skinningQuat(144)、skinning(160)。
- 静态 collider SSBO（Collision pass binding 7）：创建时上传一次，每个 collider
  32 字节（`GuideColliderGpu`：a.xyz + a.w=radius、b.xyz），固定 8 槽；
  `counts.w` 决定 pass 实际读取数量。空列表也保留零填充 buffer。

buffer 由 `RenderableObject` 拥有并随 World 资源包退休；`HairSimulationPass` 只拥有
6 个 compute pipeline、每个 pass 的 descriptor pool、逐对象 per-pass descriptor set
缓存和逐对象 `simulationTime`。

### Pass 顺序

`HairSimulationPass` 严格按 TressFX 5.0 的 order 录制。前五个 pass 与
`artifacts/tressfx-shaders/` 的源 .usf 一一对应；第六个 Collision 是 VL 追加：

| # | TressFX 源 | VL shader | dispatch 粒度 | 读取 → 写入 |
| --- | --- | --- | --- | --- |
| 1 | `TressFXSimulationIntegration.usf` → Integration + GlobalShapeConstraints | `tressfxSimIntegration.comp` | 每顶点，`ceil(pointCount/64)` | rest/current/previous/prevPrev → current/previous/prevPrev |
| 2 | `TressFXSimulationStrandLevel.usf` → CalculateStrandLevelData | `tressfxSimStrandLevel.comp` | 每 strand，`ceil(guideCount/64)` | current/previous/prevPrev → strandLevel |
| 3 | `TressFXSimulationVSP.usf` → VelocityShockPropagation | `tressfxSimVsp.comp` | 每顶点，`ceil(pointCount/64)`；仅当 `vspCoefficient > 0` | strandLevel/current/previous → current/previous |
| 4 | `TressFXSimulationLocalShapeConstraints.usf` | `tressfxSimLocalShape.comp` | 每 strand；strand 内串行 | rest/strandLevel/current → current |
| 5 | `TressFXSimulationLengthConstraintsWind.usf` | `tressfxSimLengthWind.comp` | 每 strand；strand 内串行 | restLengths/current/previous → current/previous |
| 6 | 无（VL 追加） | `tressfxSimCollision.comp` | 每 strand；仅当 `colliderCount > 0` | colliders/current → current |

所有 shader 的 `local_size_x = 64`；每根 strand 的 pass 在 invocation 内按 root→tip
串行读写，等价于源实现 per-strand 线程的 Gauss-Seidel 顺序。TressFX 用
groupshared 把同组多根 strand 交错存放、再用 `GroupMemoryBarrierWithGroupSync`
同步偶数/奇数边约束；VL 改为 strand 独立串行，**不保留跨 strand 并行**，但约束的
分组与顺序不变：每个 `iteration` 先处理偶数边 `(0,1),(2,3),…`，再处理奇数边
`(1,2),(3,4),…`。每组内部边不共享顶点，因此组内串行与并行结果相同。

各 pass 语义（与源 shader 的公式一致）：

1. **Integration + GlobalShape**：`InitialPos = M * rest`（源实现还会乘 0.01 转米，
   VL 厘米不缩放）。不可移动点写 `current = (InitialPos, 0)`；可移动点做
   `current + exp(-DampingCoeff*dt*60)*(current-old) + gravity*dt²`，其中
   `DampingCoeff = -ln(damping)`，`gravity` 取资产向量。然后若
   `GlobalShapeStiffness > 0 && GlobalShapeEffectiveRange > 0 && local < range*N`，
   `temp += stiffness*(InitialPos - temp)`。最后按 `prevPrev ← prev`、`prev ← current`、
   `current ← temp` 更新三个历史 buffer。
2. **StrandLevel**：取前两个点的 `prevPrev/prev/current`；
   `u = normalize(old1-old0)`、`v = normalize(new1-new0)`、
   `q = QuatFromTwoUnitVectors(u,v)`、`trans = new0 - q*old0`；
   `Accel = |new1 - 2*old1 + oldOld1|`，超过 `vspAccelThresholdMax` 时
   VspCoeff = 1，否则 VspCoeff = `vspCoefficient`。同时写
   `SkinningQuat = skinningQuat`。即使 VSP 关闭此 pass 也执行（LocalShape 需要它）。
3. **VSP**：跳过根前缀；`coeff = VspCoeff`，当 `coeff < 0.9` 时乘逐顶点
   VSP stiffness，然后对 current 和 previous 同时做
   `(1-coeff)*p + coeff*(q*p + trans)`。同时混合 previous 是为了不引入虚假速度。
4. **LocalShape**：从 `LocalVertexIndex = rootFixedPoints - 1` 到 N-2 串行。用
   `SkinningQuat` 旋转 rest 三点得到 bind-pose；`rotGlobal` 把 bind 段
   `(i-1→i)` 转到当前段方向，再把 bind 段 `(i→i+1)` 旋转得到 `i+1` 目标；
   `del = 0.5*Stiffness[i]*(target - pos_{i+1})`，i 和 i+1 可移动时分别 `-=` / `+=`。
   VL 用 `Stiffness[i] = bendStiffness`（作者标量填充整条曲线），因此弯曲强度
   语义保留、但乘以 TressFX 公式里的 0.5。
5. **LengthConstraintsWind**：
   - 风：`w = a*Wind0 + (1-a)*Wind1 + a*Wind2 + (1-a)*Wind3`，
     `a = (strand % 20)/20`；再乘 VL 的阵风 `gust`。对
     `rootFixedPoints <= local < N-1`，`v = pos_i - pos_{i+1}`，
     `Force = -cross(cross(v,w),v)`，`pos_i += Force*dt²`。
   - 长度：`ApplyDistanceConstraint` 用可移动组合的 multiplier（0.5/0.5、1/0、0/1、0/0），
     目标长度来自 rest length buffer；偶数边后奇数边，迭代 `iterations` 次。
   - 速度 clamp：`delta = pos - prev`，`|delta| > 30 cm` 时
     `delta *= 30²/|delta|²`，`prev = pos - delta`；只重写历史，不改当前位置。
     源实现的分母是速度平方，因此结果速度是 `30²/|delta|` 而不是严格 30 cm；
     VL 逐字保留这个 quirk。
6. **Collision**（VL 追加，TressFX 5.0 guide shader 没有胶囊路径）：按作者顺序对
   每个胶囊做位置投影，只改 current；不可移动点跳过；`a == b` 退化为球。

同步：

- dispatch 前 barrier 的 src scope 为
  `TRANSFER_WRITE | VERTEX_SHADER(READ) | COMPUTE_SHADER(READ/WRITE)`，dst 为
  `VERTEX_SHADER(READ/WRITE) | COMPUTE_SHADER(READ/WRITE)`。它覆盖 device-local
  buffer 的一次性 staging 上传、上一帧 vertex shader 读取 current、以及上一帧
  compute 的读写。
- **每个相邻 pass 之间**插入 `COMPUTE_SHADER(READ/WRITE)` →
  `COMPUTE_SHADER(READ/WRITE)` 的 memory barrier。Vulkan 不会为同一命令缓冲里的
  连续 dispatch 自动建立内存依赖；barrier 同时提供执行依赖。pass 1 之前由
  dispatch 前 barrier 覆盖。
- dispatch 后 barrier 为 `COMPUTE_SHADER(WRITE)` →
  `VERTEX_SHADER(READ) | COMPUTE_SHADER(READ/WRITE)`，使本帧写入对本帧顶点读取和
  下一帧 dispatch 都可见。
- 同一 graphics queue、命令缓冲提交顺序固定，因此这些 barrier 足够；params UBO
  按 frame-in-flight 分槽，`BeginFrame` 等该槽 fence 回收后才由主机写入。
- 每个 pass 的 descriptor set layout 不同（各 shader 只声明自己用到的 binding），
  因此 per-object 缓存按 `[pass][frameSlot]` 保存 6×2 个 set；每个 pass 有独立的
  descriptor pool 链。set 里的 buffer 句柄每帧校验，buffer 被替换时重建。

`simulation.enabled = true` 且 backend = gpu 时，CPU 不构造 `GuideSimulation`，
`RendererResourceCache::AdvanceTressfxSimulations` 跳过该对象。tressfx 顶点 buffer
回到普通 device-local 上传路径，不再逐帧 memcpy：vertex factory 只消费 SSBO 中心线，
顶点位置不参与展开。

### 变量步长与阵风时间

两种 backend 使用 TressFX 的变量步长：

```text
dt = clamp(frameDelta, 0, maxDt)
if (dt <= 0) 本帧不推进
Step(dt)                      // 一帧恰好一个 solver step
simulationTime += dt          // 阵风相位用 frame-start t
```

没有 accumulator，也没有固定子步重复 dispatch。`maxDt` 是唯一的稳定性边界；
帧率降低时每步更大，行为随帧率变化是 TressFX 的刻意语义。GPU 端在
`HairSimulationPass` 中逐对象保存 `simulationTime` 并录制一次 6-pass 序列，params
只写一次。`RecordHairSimulation` 仍每帧在所有图形 pass 之前调用一次；delta time
使用 `exposureDeltaSeconds`（渲染帧墙钟，clamp 到 `[1/240, 0.1]`）；CPU backend
使用游戏线程的 `simulationDelta`。两者的 delta 来源不同是当前已知差异，且两种
backend 都不消费游戏暂停/time-scale（接入前就如此）。

### CPU 参考 solver

`VL::Hair::GuideSimulation` 按同样的 pass 顺序逐 strand 串行执行：
Integration+GlobalShape、StrandLevel、VSP、LocalShape、Length/Wind、Collision。
纯数学放在 `source/hair/tressfxSolverMath.h`，GPU shader 与 CPU solver 共用同一组
公式约定。CPU solver 没有渲染线程的 `SkinPose`，因此 `InitialPos` 直接取 rest
（等价于单位蒙皮矩阵），且只能运行未声明 `skinning` 的资产；其余分支与 GPU 一致。

CPU/GPU 的一致性是“相同操作顺序”，不是逐位相等：Eigen 与 GLSL 的
`sqrt`/`sin` 舍入、编译器 FMA 融合和指令调度都可能产生末位差异；frame delta
来源也不同。没有 CPU 读回或结果交叉校验。

模拟状态是 per mesh asset 的：渲染器按 mesh key 共享同一个 `RenderableObject`，因此
同一 `SM_*` 的多个场景 placement 共用一份位置 buffer、strandLevel 和 simulation
time。作者 `gravity`/`wind` 在资产局部空间求解，旋转 placement 不会旋转重力；
当前没有 per-instance 或 world-space simulation。

### 单位、忠实度与显式差异

TressFX 源实现在**米**制下工作：rest 位置乘 0.01、速度 clamp 0.3 m。VL 全链路使用
**厘米**，因此所有 0.01 与 0.3 m 都省略/换成 30 cm，位置、半径、重力和风速的数值
保持厘米语义。这个换算不是对所有公式都无损：

- **风的三叉积**带 `|v|²` 量纲。源实现把米制未归一化的段向量代入，VL 若照搬到厘米
  会把风力放大约 `10^6`，直接把发丝吹散。VL 先把段方向归一化再代入同一
  `-cross(cross(v,w),v)`，得到 `-w` 在段法平面上的分量（cm/s²），保持源实现的
  推动方向与作者风速的物理量级。
- **速度 clamp** 的 `delta *= 30²/|delta|²` 是源实现的既有 quirk，VL 保留。
- **重力方向**：源实现是 `GravityMagnitude*(0,0,-1)`（Z-up）；VL 直接使用资产的
  `gravity` 向量（Y-up）。这不是通用换轴，只是不再注入源引擎的固定方向。
- **阵风** `gust` 是 VL 在四风混合结果上的作者化扩展，TressFX 源实现没有这一项。
- **VSP 逐顶点 stiffness**：TressFX 用最多 64 项的作者曲线
  `VSPStiffness`；VL 没有逐顶点曲线作者面，用从根前缀（1）到梢部（0）的线性衰减
  近似，并且只在 `VspCoeff < 0.9` 时参与（与源实现同一条件）。只移植非 MT 的
  `CalculateStrandLevelData`：MT 变体里 `Accel < VSPAccelThresholdMin` 时把系数
  设为 0.6 的分支没有进入当前资产面。
- **LocalShape 起点**：源实现从 `NumVerticesFromRootNoSimulation - 1` 开始；该值
  为 0 时无符号下溢使循环不执行，为 1 时会读取 `GlobalVertexIndex - 1`（首根
  strand 越界）。VL 保留 0 时跳过，1 时从 local 1 开始；作者资产使用 >= 2。
- **单骨骼蒙皮**：只有根骨矩阵与根骨旋转，没有 `.tfxbone` 逐顶点多骨权重，也没有
  逐帧碰撞网格蒙皮或运行时 SDF 重建。
- **串行约束**：见 pass 顺序一节的替换说明。
- **ResetPositions**：TressFX 的 `ResetPositions` 全局复位开关没有接入。buffer 在创建
  时以 rest 初始化，等价于一次复位；运行中没有 teleport/reset 路径，pass/World
  重建会重新上传初值。
- **MT/blend-shape 变体**：只移植单骨骼
  `IntegrationAndGlobalShapeConstraints`/`CalculateStrandLevelData` 路径；
  TressFX 的 blend-shape 蒙皮变体依赖逐顶点根三角数据，不在当前资产面内。

显式限制：碰撞只有作者解析胶囊，没有 `.tfxmesh`/SDF、follow hair、深度阴影、OIT、
LOD；胶囊接触是纯位置投影，不修改 previous positions，因此没有摩擦、没有碰撞
速度冲量，接触略黏且可能抖动；重叠胶囊的最终位置取决于作者顺序。VSP 与 global
shape 默认关闭，只在资产显式作者时启用；`vspCoefficient > 0` 时 Host 才录制 VSP
dispatch。没有 per-instance 或 world-space simulation。

## Female 正式静态关卡

正式入口为资源仓库的 `Maps/SC_tressfx_female/SC_tressfx_female.json`。
`Meshes/`、`Materials/`、`Source/` 同属该地图，正式引用不经过 `Generated/`。
源资产来自上述 AMD revision 的 `Asset/Maya2018/Female_Outside/Female_Outside.zip`，
不是合成 guide；关卡依然是静态几何审阅，不是完整 UE 画面或 Hair 光照复现。

`tool/hair/import_female_scene.py` 是此归档的受限离线提取器，依赖 NumPy/SciPy：

- 只接受 `Female_Outside.ma` SHA256
  `ee1e944273f4b27004e65b2915117f281a9b585ed757f1e0a7f40f27eb71ed65`，
  按源声明的 CP936 解码，并要求厘米单位。不是通用 Maya importer，不执行 MEL。
- 以完整 DAG 路径区分重名 guide。主组 `xgCurvesFromGuides1` 为 **40,334** 根；
  另组 `xgCurvesFromGuides2` 的 308 根不导入，不按短名覆盖或去重。
- 接受源文件中的二次、非有理、开放夹持曲线，补齐 Maya 省略的首尾 knot，
  用 `scipy.interpolate.BSpline` 求值，每根均匀参数采样 32 点。要求零起点参数域，
  与本源文件的 exporter 路径一致；校验端点、有限值及 float32 后的相邻点非重合。
- 所选节点及祖先的 TRS/剪切/轴旋转须为恒等；保留厘米/Y-up，不执行 UE 的换轴，
  不把非零 pivot 当平移。TFX 为 1,290,688 点、2,581,376 个逻辑顶点、
  7,502,124 索引，前两点逆质量为 0。没有 UV、颜色、骨骼权重或 follow 数据。
- 角色和眼球来自保存的 `BodyModelShapeOrig` / `yanjingShapeOrig`，
  分别为 235,490 / 2,118 顶点、229,329 / 2,100 原始多边形。
  转换器按 Maya 有符号边引用还原并校验闭环，OBJ 交给既有 Assimp 三角化与生成平滑法线；
  不求值蒙皮、约束或修改器，不把 Orig 输入几何称为 Maya 最终绑定输出。
- 源包只有 `.ma`，两个外部皮肤/眼睛贴图路径未解析。角色采用明确作者的中性 PBR，
  头发可切换粉色 Unlit 探针或简单 DefaultLit PBR 预览，局部半径 0.012 cm；
  两者都不冒充原材质或物理测量半径。
- 已存在的地图目录直接拒绝，exit 1，不覆盖作者改动。归档/输出摘要、依赖版本、
  曲线规格、bounds 和缺失纹理记录到
  `Generated/Import/SC_tressfx_female/import-report.json`；
  来源、输入摘要和 AMD MIT 许可保留在正式 `Source/ATTRIBUTION.md`。
  `.tfx` 与 OBJ 一样由资源仓库 Git LFS 管理。

`tool/hair/run_female.ps1` 从引擎根目录按配置的资源根启动该关卡；
默认开启编辑器，`-NoDeveloperUi` 关闭编辑器工作区。
默认使用 `build-vscode/development/bin/main.exe` 的优化预览构建；
先运行 `cmake --preset development` 和 `cmake --build --preset development`。
`-BuildDirectory build` 显式使用 Debug 目录，启动时打印实际可执行文件路径。
进程级设置 `tonemap 3`、`bloom strength 0` 与 60 Hz 帧率上限，不改共享 config。
正式相机位置 `[50,165,210]`、目标 `[0,151,0]`、水平 FOV 38°、EV100 0；
共用 `Common/Environments/UE_AssetsPreview/EpicQuadPanorama_Gray.hdr`，
强度为 1，另有 3 lux 的作者方向光。这些是 VL 审阅配置，不是恢复的 UE 参数。

```powershell
# 使用安装了可用 NumPy/SciPy 的标准 CPython；本机 py -3 的 free-threaded 版本不兼容已装扩展。
& 'C:\Program Files\Python314\python.exe' tool/hair/import_female_scene.py `
    --reference-root D:\YYBWorkSpace\GitHub\UnrealEngine-TressFX5 `
    --resource-root ..\VukanLearnResources
pwsh -File tool/hair/run_female.ps1
```

## Female 原骨架与蒙皮关卡

同一 `.ma` 含真实 deformation joints 和 skinCluster；静态 OBJ 路径没有导出它们，
不能把原文件说成“没有骨架绑定”。`extract_female_skin.py` 为上述固定 SHA256 的
受限离线提取器，读取保存的 Orig 几何、joint `.bps`、skinCluster `.pm/.gm/.wl`
和 joint 连接，不执行 Maya、MEL、约束、blendshape 或完整 deformer graph。
行向量矩阵转置为列向量，再按父级世界绑定矩阵恢复局部 rest。

- **105 个关节**：104 个源 deformation joints，加一个恒等 `FemaleRoot`。
  绑定矩阵与 inverse bind 独立核对；身体 55 个声明影响骨，单点最多 4 个正权重；
  眼球 2 个影响骨，单点 1 个权重。保存全部正权重，不截断或重新归一化。
- 固定源身体有 12 个微小负残差，最小约 `-2.16e-9`；离线丢弃并逐条审计。
  scalp proxy 的 15,226 顶点、55 影响骨和单点最多 5 权重也校验，但因无 UV
  不作为可见身体层导出。
- 身体保留源 UV0；眼球把源 UV1 放入运行时 UV0。身体 15,136 个面没有作者 UV，
  显式填 `(0,0)` 并记入审计，不把占位 UV 视为贴图修复。UV 缝拆点保持完整蒙皮身份。
- `vl_female_fbx` 使用 Assimp FBX exporter，实际改轴到厘米/Z-up FBX，
  再经生产 `AssimpSkeletalImporter` 回读。导出关闭按位置合点，离线分块为
  3 个 body section 和 1 个 eyes section，不扩大运行时每 section 顶点预算。
  总计 459,143 三角形；法线由工具生成平滑法线，不是 Maya 作者法线。
- 回读逐点核对完整位置、UV 与按骨名标识的全部权重，并核对关节名、父链和 rest。
  固定资产最大绑定蒙皮残差约 `0.0000462 cm`，float 权重和误差不超过 `4.48e-8`。
  SK 清单使用回读值，同时独立对照 Maya rest，不能用自生成清单掩盖导出错误。

```powershell
& 'C:\Program Files\Python314\python.exe' tool/hair/extract_female_skin.py `
    --archive D:\YYBWorkSpace\GitHub\UnrealEngine-TressFX5\Asset\Maya2018\Female_Outside\Female_Outside.zip `
    --output ..\VukanLearnResources\Generated\Import\SC_tressfx_female\skin-v1\female-bind.json
cmake --build build --target vl_female_fbx -j8
.\build\bin\vl_female_fbx.exe `
    ..\VukanLearnResources\Generated\Import\SC_tressfx_female\skin-v1\female-bind.json `
    ..\VukanLearnResources\Maps\SC_tressfx_female\Source\Female_Outside_Skinned.fbx `
    ..\VukanLearnResources\Maps\SC_tressfx_female\Skeletons\SK_Female_Outside.json
& 'C:\Program Files\Python314\python.exe' tool/hair/build_female_skinned_scene.py `
    --resource-root ..\VukanLearnResources `
    --source-resource-root D:\YYBWorkSpace\GitHub\VukanLearnResources
```

提取器、FBX 转换器和关卡生成器拒绝已有输出；转换失败可能留下待检查的 FBX。
交换 JSON 与 `fbx-roundtrip-report.json` / `package-report.json` 位于
`Generated/Import/SC_tressfx_female/skin-v1/`；正式 FBX、SK、SM、MI、retarget profile
和 Blueprint 均位于 `Maps/SC_tressfx_female/`。
蒙皮 FBX 的来源、转换限制及 AMD MIT 许可单独保存在
`Source/Female_Outside_Skinned.ATTRIBUTION.md`；静态导出的说明继续归原 `ATTRIBUTION.md`。

`RT_Female_From_GASP.json` 显式映射 24 个 root/pelvis/躯干/头/四肢关节。
GASP 仅提供源 skeleton 与 Idle/Walk；源资产仍归 `Maps/SC_gasp/`，
生成器只复制实际引用的依赖，冲突内容拒绝覆盖。
动画预览同时复制 `SC_model_preview` 场景及其背板 SM、MI 和 OBJ；共用 HDRI 由资源根提供。
运行时目标网格保留自身骨长、局部平移和缩放；只传递根位移及映射骨的模型空间旋转差。
未映射的手指和 twist 保持局部校准参考姿态并继承父骨动作；不传递 pelvis 平移，
没有 IK、足底接触修正或完整 GASP Motion Matching 控制器。

`calibrate_female_retarget.py` 为这对正式 SK 生成 `targetPose` 的 14 个旋转偏移，
双侧肩带、上臂、前臂、手腕、大腿、小腿与踝部。局部参考按 `bindLocal * offset`，
父先于子地对齐骨段，手腕另用中指/食指/小指的掌面框架，不能仅用骨段推断 twist。
源使用 GASP bind，不更改原始 SK、权重或 inverse bind。
Female 穿高跟鞋，踝参考保留 Female 原模型空间朝向，仅抵消腿链校准的父级影响；
不把其脚尖强行扳到 GASP 平底脚。两侧 foot→toe 的约 34.9° 方向差保留为鞋型差异，
不纳入其余校准骨段的方向残差。该策略不代表着地或脚滑已解决。

工具 `--resource-root <root> --output <new.json>` 拒绝覆盖已有 profile/report。
`build_female_skinned_scene.py` 调用同一校准函数，包报告保留校准量与输出 SHA256。
正式 RT 的 `preview` 显式引用 GASP/Female 两个 SK；
内容浏览器搜索 `RT_Female_From_GASP` 并双击打开 Retarget 只读标签，
`targetModelPath` 指向正式 `SM_female_skinned`，Animation 内直接并排查看 GASP/Female
真实蒙皮，切 Idle/Walk、播放/暂停或拖秒时间轴；环绕/平移/缩放可检查实际鞋底。
Reference Pose 保留 Bind/Calibrated、正/侧/顶视、选中局部轴与映射测量。具体语义见
[UI 组件体系](ui-component-system.md#retarget-参考姿态标签)。

正式关卡与启动参数：

| 关卡 | `run_female.ps1` 参数 | 行为 |
| --- | --- | --- |
| `SC_tressfx_female_bind.json` | `-BindPose` | 原始绑定姿态对照 |
| `SC_tressfx_female_skinned.json` | `-Skinned` | 普通 mesh 自动重定向 Idle；模型预览可切 Walk |
| `SC_tressfx_female_skinned_blueprint.json` | `-Skinned -Blueprint` | 点击 Play，C# 请求原地 Walk |
| `SC_tressfx_female_hair_animated.json` | 无（直接 `--initial-scene`） | 普通蒙皮角色 + 根点跟随 `Head_M` 的 GPU 发丝 |

三个新关卡的相机为 `[250,120,420]`、目标 `[0,90,0]`、水平 FOV 38°、EV100=0；
环境和方向光复用静态版的审阅设置。身体与眼球共用一个蒙皮 mesh component，
各用中性 PBR 材质；蓝图源码属于资源根的 `Blueprints/`，
`cmake --build build --target main vl_gameplay -j8` 构建原生和 Gameplay。
蓝图入口要求构建时启用 C#；宿主默认 Editing，只有 Play 后才推进其动画。
旧静态版保持独立。

`SC_tressfx_female_hair_animated.json` 是根点蒙皮关卡：普通 `mesh` 对象
`FemaleCharacter`（`SM_female_skinned.json`，目标 SK 为 `SK_Female_Outside`，
Idle 来自 GASP 重定向）+ `Female_hair_skinned`（`SM_female_hair_skinned.json`，
`backend=gpu`、`rootFixedPoints=2`、`skinning.source=FemaleCharacter`、
`skinning.rootBone=Head_M`，胶囊/重力/风与 `SM_female_hair_dynamic.json` 相同）。
选普通 mesh 而不是 blueprint 关卡，是因为 `SkeletalPlayback` 构造时默认 `playing=true`
并选中 clip 0，`World::TickAnimations` 每帧推进它，加载后即自动播放 Idle；blueprint
版本只在 `BeginPlay` 里 `Animation.Play(Walk)`，必须点 Play 才动。两者都使用同一套
正式 FBX/SK/权重，不新增导入资产。相机沿用静态发丝的肖像机位
`[50,165,210]`、目标 `[0,151,0]`，便于检查头皮接缝。

`tool/hair/capture_female_skin.py --output <freshDirectory>` 使用正式启动/Console
截图绑定姿态、Idle 与两个暂停 Walk 时刻；会话临时启用 C# autoPlay，
在 finally 中恢复 config 原始字节。它是外部截图烟测，不注册 CTest 或新增 runtime test，
也不作为贴图、发丝模拟、脚滑或 UE 画面等价验收。

`review_female_feet.py --build` 可另建 `SC_tressfx_female_feet.json` 审阅关卡，
`run_female.ps1 -Feet` 打开该关卡；使用正式 Walk BP、10 cm 的 Y=0 细线网格及
6 cd 点补光，无地板碰撞或足底 IK。生成器拒绝已有输出。
`--capture <freshDirectory>` 是外部暂停采样工具，只取 4 秒 Walk 的前 0.875 秒与
两张正面图；GIF 回放真实采样帧，不是完整片段验收。会话备份/恢复 config 原始字节，
截图进程由工具终止，不作为正常 teardown 证据。交互动画检查使用上述 RT 标签。
