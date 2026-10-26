# TressFX 资产导入

> 状态：**当前契约**（CPU 导入、静态 ribbon、CPU/GPU guide 模拟、逐帧 SDF 碰撞已落地）。本文件只描述当前源码与验证覆盖的行为。
> 根点骨骼蒙皮（`simulation.skinning`）与逐帧碰撞距离场（`collision.mesh`）都已落地；
> 碰撞胶囊可按关节绑定，碰撞代理使用从绑定姿态身体转移的逐顶点权重；胶囊碰撞仍然保留，
> 两者可以同时作者。

## 实现边界

`source/hair/` 的 `vl_hair_assets` 是纯 CPU 静态库，供根工程 `main`、
`vl_tfx_inspect` 和 `hair_asset_tests` 链接；不依赖 UE、Vulkan、World 或材质系统。
该库提供 `.tfx` 解析、`.tfxbone` 纯 CPU 解析与骨名→目标骨架映射
（`tfxBoneLoader`）、静态几何准备和参考 solver `VL::Hair::GuideSimulation`；
没有 `.tfxmesh` 解析或 follow 生成。`.tfxbone` 由网格 importer 读入、加载期
映射后，GPU 发丝在声明 `simulation.skinning.boneWeights` 时消费逐 strand 权重。GPU 模拟 pass
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
- 导入期还额外保留每 guide 一个 model 空间根点
  （`MeshSection::tressfxRootPositions`，正式资产 40,334 个 ≈ 0.48 MB），只服务
  加载期 `simulation.skinning.weights = "mesh"` 的权重派生，不进入 GPU buffer，
  也不改变 ribbon 拓扑。

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
- `female_head.sdf` 仍**没有**运行时消费者：运行时碰撞场改为每帧从
  `female_body_proxy.obj` 重建（见“运行时 SDF 碰撞”），不读取这个离线 .sdf
  文件。两者都带符号，但离线文件用角度加权伪法线、0.4 cm 固定分辨率，运行时
  场用代理三角形绕序法线、每帧自适应域，因此结果不可互换。

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
- 运行时消费者是 `collision.mesh` 指向的同一个 OBJ：`RenderableObject` 在创建
  device-local 代理顶点/索引、逐顶点影响流与固定分辨率的 uint 距离网格，
  `HairSimulationPass` 每帧按 SdfClear → SdfSplat → SdfCollide 重建并投影发丝点。
  蒙皮优先用从绑定姿态身体转移的逐顶点权重；没有权重/pose 时回退根点同一个
  单骨矩阵；网格域每帧从蒙皮后的代理包围盒重算（见下文“运行时 SDF 碰撞”）。

## 静态运行时路径

场景仍用 `type: "mesh"`、`modelPath: ".../Meshes/TF_*.json"`。描述符为：

```json
{
    "name": "TF_guides",
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
- 内容浏览器把 tressfx 归为独立 Tressfx kind；双击 TF 描述符打开既有毛发预览与
  模拟控件宿主。标准 `tressfx.*` API 由独立文档 owner 支持既有字段读改保存，
  `mesh.*` 拒绝此类型。迁移期旧 SM_ 前缀也通过 Tressfx owner 打开，不建立第二份草稿。
  Scene/BP 的模型引用接受该类型。`strandRadius` 不在标准修改能力表中，
  描述符半径修改属于能力表外的源码编辑。

`tool/hair/generate_static_probe.py --resource-root <isolatedRoot>` 创建
`Generated/Validation/SC_tressfx_static_probe/`（存在即拒绝），包含合成 TFX、
TF/MI 和两实例场景；正式 Common pass 资产由调用方提供。
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
| `wind` | `[0,0,0]` | 3 个有限数 | 单个作者风向量：方向归一化后作为 TressFX 风锥轴，长度作为幅值（厘米/秒）；它是 TressFX 风力公式的标量，不是物理风速。Host 按 `SetWind` 生成 `Wind0..3`（绕 Y/Z 各 ±40°，每路乘 `wM = |wind|*(sin(simFrame*0.001)² + 0.5)`）。 |
| `damping` | 0.035 | `[0,1]`，典型 `0..0.07` | TressFX 的 `DampingCoeff`，直接用于 `exp(-damping*dt*60)`；不是“每步保留的速度比例”，不做 `-ln` 映射。 |
| `iterations` | 3 | `[1,16]` | LengthConstraintsWind pass 的偶数/奇数边约束迭代次数；本资产校准为 8。 |
| `rootFixedPoints` | 2 | `[0,32]` | 每根 guide 从根部起固定的点数，即 TressFX 的 `NumVerticesFromRootNoSimulation`。 |
| `maxDt` | 0.016 | `(0,0.016]` | 兼容字段。GPU 原生调度与 `GuideSimulation::StepFrame` 都不读它（每 substep 固定 0.0125 s）；只有旧的 `GuideSimulation::Step(dt)` 变量步长入口仍 clamp 到 `[0,maxDt]`。资产校验保留 `(0,0.016]`。 |
| `bendStiffness` | 0.3 | `[0,1]` | 映射到 LocalShape pass 的逐顶点 `Stiffness` 标量；pass 内再乘 0.5（TressFX 公式）。0 关闭。未提供 `localShapeStiffness` 时整条 strand 使用该标量。 |
| `localShapeStiffness` | 空（未作者） | 数组，1..64 项，每项 `[0,1]` | 可选作者逐顶点 LocalShape Stiffness。第 i 项 = 源曲线在 `i/N` 处的采样值（`TressFXComponent.cpp:706-710`），N = guide 资产的 `pointsPerStrand`；长度必须等于该 TFX 的 `pointsPerStrand`，否则导入期硬错误。加载期对 >0.95 的值 clamp 到 0.95 并向 stderr 记录字段/索引/资产路径（源 `FMath::Min(value, 0.95f)`）；省略时回退到 `bendStiffness` 标量。 |
| `globalShapeStiffness` | 0 | `[0,1]` | TressFX Integration 的 global shape 强度；0 关闭，既有资产外观不变。 |
| `globalShapeEffectiveRange` | 0 | `[0,1]` | 作用范围占每根 strand 顶点数的比例；0 关闭。 |
| `vspCoefficient` | 0 | `[0,1]` | TressFX VSP 基础系数；0 表示关闭，Host 不录制 VSP dispatch。 |
| `vspAccelThresholdMax` | 140 | `(0,1e7]` | 二阶位置差幅值阈值，单位厘米，未除 dt²；超过时强制 VspCoeff = 1。默认 140 对应源默认 1.4 world m；名字沿用源 `Accel`，不是厘米/秒²。 |
| `vspStiffness` | 空（未作者） | 数组，1..64 项，每项 `[0,1]` | 可选作者逐顶点 VSPStiffness。第 i 项 = 源曲线在 `i/N` 处的采样值（`TressFXComponent.cpp:687-691`），长度必须等于该 TFX 的 `pointsPerStrand`，否则导入期硬错误。只在 `VspCoeff < 0.9` 时乘入（源 patch 10153-10160）；省略时回退到根前缀(1)→梢部(0)的线性近似。 |
| `backend` | `"cpu"` | `"cpu"` / `"gpu"` | 参考 solver 或 compute dispatch。 |
| `skinning` | 无 | `{source, rootBone, boneWeights?, weights?}` | 可选；把根点改为跟随场景内源角色的骨架。缺省即历史单骨行为。 |
| `skinning.weights` | `"rootBone"` | `"rootBone"` / `"mesh"` | 根点权重来源。`rootBone` 用单根骨矩阵（声明 `boneWeights` 时改用 `.tfxbone` 文件权重）；`mesh` 在加载期从绑定姿态源网格派生逐 strand 权重。与 `boneWeights` 互斥，且需要 `backend = "gpu"`。只有 `mesh` 追加到模型缓存身份（`|rootweights=mesh`）。 |

固定步字段已删除：`fixedStep` 与 `maxSubsteps` 不再被解析，出现即在资产边界报错
（错误消息指向 `maxDt`），避免旧资产静默失去效果。VL 原有的 `windGust` 与
`windFrequency` 也已删除：它们会与 TressFX `SetWind` 的帧调制叠加成双重调制。
这两个字段出现时在资产边界报错，错误消息给出 `SetWind` 公式。`simulation` 现在按
封闭对象校验，未知键直接失败，避免拼错的新曲线字段静默回退。`gravity`、`wind`、
`damping`、`iterations`、`maxDt`、`bendStiffness`、`localShapeStiffness`、
`vspCoefficient`、`vspAccelThresholdMax`、`vspStiffness` 仍留在模型缓存身份中；
两条曲线逐项以 hexfloat 参与身份，只改一个作者值也会重建缓存条目。
`simulation.skinning` 的 `source`/`rootBone`/`boneWeights` 同样进入身份；
`weights` 只在取值 `"mesh"` 时追加 `|rootweights=mesh`，缺省不追加，
既有资产身份保持不变。

### 碰撞：胶囊与逐帧 SDF 网格

`collision` 是 SM 描述符的顶层对象，与 `simulation` 同级；所有坐标和半径都是
**资产局部空间厘米**，与 positions、gravity、wind 使用同一坐标系：

```json
"collision": {
    "capsules": [
        {"a": [0.0, 168.0, 0.5], "b": [0.0, 168.0, 0.5], "radius": 8.5,
         "bone": "Head_M"},
        {"a": [0.0, 149.0, 0.5], "b": [0.0, 159.0, 0.5], "radius": 6.5,
         "bone": "Neck1_M"}
    ],
    "mesh": {
        "path": "Maps/SC_tressfx_female/Collision/female_body_proxy.obj",
        "cellSize": 1.0,
        "margin": 1.0
    }
}
```

- `capsules` 是必需的数组，可以为空。非空时 Host 在长度 pass 之后录制胶囊
  Collision pass；空数组不录制该 pass。
- 数量上限为 8（`VL::Hair::TressfxMaxColliders`，GPU 侧同名常量），因为当前
  作者规模只需头、颈、肩背少量代理；更多细节应转向 `mesh`，而不是继续堆胶囊。
- `a == b` 表示球。`a`、`b` 必须是 3 个有限数，`radius` 必须有限且 > 0；
  未知键、缺失键、非 3 元向量、超过 8 个胶囊都按现有错误风格拒绝，消息带 SM
  资产路径。
- `bone` 是可选的非空关节名。缺省时胶囊保持**作者绑定空间**的静态行为；
  给出时加载期通过 `simulation.skinning.source` 的同一套目标骨架解析关节，
  运行期每帧用该关节的 bind→pose 矩阵变换 `a`/`b`。未知骨是加载期硬错误，
  消息带 `collision.capsules[i].bone`、骨名、源对象名与 SM 路径；相机胶囊
  绑定关节名（不是 section 名）。`radius` 不缩放：当前导出链路没有独立的
  骨骼缩放提取，碰撞 kernel 仍按球/胶囊处理，把非均匀缩放吸收进半径会失真。
- 声明任何 `bone` 时必须有 `simulation.skinning` 且 `simulation.backend = "gpu"`；
  CPU 参考 solver 没有渲染线程的 `SkinPose`。这与 `simulation.skinning` 的
  加载期约束一致。
- 每个 collider 的 `a`/`b`/`radius`/`bone` 都进入 `modelCacheKey`；修改胶囊会使
  模型缓存失效。
- `mesh` 是可选对象；缺省时**只有胶囊碰撞**。
  - `path` 必需且非空，是资源仓库相对路径；绝对路径或含 `..` 的路径在资产边界
    拒绝。加载期由 `VL::Hair::LoadCollisionMeshObj` 解析成 `SdfTriangleMesh`，
    失败消息带 OBJ 路径与行号。
  - `cellSize` 缺省 1.0，单位厘米，范围 **(0.2, 4]**，是**绑定姿态目标格距**：
    创建时解析成固定分辨率 `clamp(round(bindMaxExtent/cellSize), 48, 64)`，
    之后实际格距 = 每帧域边长 / 分辨率，会随代理包围盒变化。`margin` 缺省 1.0，
    单位厘米，范围 **[0.05, 4]**，是每帧都保持的最小表面距离。两者都必须有限。
  - 只接受 `capsules` 与 `mesh` 两个键；`mesh` 只接受 `path`/`cellSize`/`margin`。
  - 只允许 `simulation.backend = "gpu"`：CPU 参考 solver 没有距离场资源，
    静默忽略会让资产看起来生效但实际没有碰撞。
  - `path`、`cellSize`、`margin` 都进入 `modelCacheKey`（浮点用 hexfloat），
    只差碰撞网格的两个 SM 不共享 renderable 与 GPU 资源。
- 运行时没有 `.tfxmesh` 解析器：碰撞代理就是 `mesh.path` 指向的普通 OBJ
  三角形汤（当前是离线 `vl_build_collision_proxy` 产出的 2,572 顶点 /
  7,391 三角形）。离线 `female_head.sdf` 不参与运行时。
- 代理 OBJ 没有作者权重。声明 `mesh` 且同时有 `simulation.skinning` 时，加载期
  从源对象绑定姿态的身体 section 转移权重：对每个代理顶点，用空间哈希找最近的
  身体顶点（身体位置先乘 section 的 `meshToModel`，与代理同处渲染绑定空间），
  复制其**完整**正权重列表并归一化。GPU 的 `SkinInfluences` 是 CSR、没有 4 骨
  上限；因此不截断影响列表（本资产身体单点最多 4 个正权重，实际结果仍 <= 4）。
  等距时取全局下标更小的身体顶点，输出确定。转移只在加载期做一次并缓存；
  2,572 个代理顶点到最近身体顶点的实测最大距离 0.9256 cm（均值约 0.22 cm），
  超过 2 cm 视为代理与身体不在同一绑定空间/不是同一角色，加载期硬错误。
  没有 `simulation.skinning` 时没有源骨架，代理保持 `params.skinning` 的单骨
  刚性矩阵回退（与本字段引入前一致）。

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
  对象**目标骨架**（`skinning.skeleton` 指向的 SK）里的关节名，大小写敏感。
  `rootBone` 必须存在且非空；`source` 在场景 JSON 路径下同样必填，但由蓝图
  `tressfx` 组件挂载时可以省略——组件树里的父 `mesh` 组件提供源角色与骨架。
  未知键、空字符串在 `MeshAssetValidator::BuildLoadPlan` 拒绝，错误消息带 SM 资产路径。
  两者都进入 `modelCacheKey`，改来源或骨名会得到不同 renderable。
- 蓝图 `tressfx` 组件提供**放置级**绑定：组件的最近 `mesh` 祖先就是源角色，身份为
  `__blueprint/<actor>/<component>`，优先于 SM 的 `source`。两者都不存在时在加载期
  硬错误（消息带对象名与 SM 路径），不静默回退。组件局部 TRS 必须是单位阵：毛发与
  源角色必须共享同一对象空间（`ModelMatricesMatch`），当前没有“毛发相对角色偏移”的语义。
- 权重选择顺序：组件显式 `weights` > SM 显式 `weights` > 组件绑定默认 `"mesh"`；
  显式 `.tfxbone`（`boneWeights`）优先于自动升级，只有显式 `mesh` 与文件并存才按
  作者冲突拒绝。加载期打印一次
  `[TressFX] hair binding: object=… source=… binding=component|scene weights=mesh|bone|file rootBone=…`。
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
- 绑定空间：构建期仍要求两个场景对象的 `model` 一致（逐元素容差 `1e-4`）；
  不一致直接报错，确保资产 rest 与身体绑定空间匹配。运行期根点、绑定胶囊和代理
  palette 统一使用 `inverse(hair.model) * source.model * bindToPose`，把源模型空间
  显式转到发丝局部空间。两个对象同为 identity 时退化为原 bind→pose 行为。
  运行中单独改变 source model 能正确移动根点/碰撞代理，但该变换没有专门的
  teleport epoch，不等于跳变已复位。hair placement 自身变化时，GPU 路径把
  current/previous/prevPrev 三份历史乘 `inverse(currentModel) * lastModel` 换基
  （`motion.z`/`historyToCurrentLocal`），刚体平移/绕轴旋转下世界位置和世界速度
  保持不变，等价于源的世界空间历史；非均匀缩放/剪切只保证点换基，不保证
  rest length、胶囊半径、SDF cell/margin 等局部度量与世界 solver 等价。
- 运行时 `HairSimulationPass` 在冻结快照的 `drawPackets` 里按 `sceneObjectIdentity`
  找源角色 draw，读取全骨架 `skinPose->bindToPoseMatrices`（旧 pose 没有时退回
  `matrices`），以同一份 source→hair 相对 palette 驱动根骨、绑定胶囊与 SDF。
  palette 容量由逐对象 `TressfxFrameContext` 复用。pose 或 draw 本帧不可用时
  保留上一帧矩阵并只报告一次（stderr），不销毁 pass、descriptor 或整帧；
  再次取得有效 pose 时复位，避免把失去姿态期间的差值积成速度。
- 矩阵每帧写入 std140 params UBO 的 `skinning`（mat4，offset 160）；三份历史换基
  矩阵 `historyToCurrentLocal`（mat4，offset 224）；作者曲线数组 `vspStiffness`
  （offset 288）与 `localShapeStiffness`（offset 544）各 16 个 vec4、共 256 bytes；
  整个 UBO 800 bytes，C++ `static_assert` 固定大小、对齐与偏移。没有声明
  `skinning` 时上传单位矩阵。
- 不可移动点（`local < rootFixedPoints` 或逆质量 <= 0）每步写
  `InitialPos = M * rest`；这是 TressFX Integration 的规则。正常积分中可移动点
  只在 global shape 开启时把 `InitialPos` 当作回拉目标；ResetPositions 则把
  所有点和三份位置历史同时写成当前蒙皮 rest，清空旧速度。
- 最终 source→hair 根矩阵的上 3×3 旋转部分在 Host 分解为四元数 `skinningQuat`（offset 144），
  作为 TressFX `SkinningQuat` 供 LocalShape pass 使用。VSP/局部形状需要的是
  bind→pose 的**增量**旋转并转入发丝空间；`SkinPose::bindToPoseMatrices[jointIndex]`
  给出源对象内的增量，随后组合 source→hair 相对变换，不是骨骼的绝对变换。`SkinPose::matrices`
  是逐 section 的版本，未参与该 section 的关节会退化为绝对 model 矩阵，因此
  hair 的根骨/胶囊/代理统一使用全骨架 `bindToPoseMatrices`。
- TressFX 5.0 映射：源实现从 `GuidesBoneIndexBuffer`（逐 strand 的
  `FBoneIndexData`）在积分 pass 里蒙皮 guide，并用 `NumVerticesFromRootNoSimulation`
  标出“蒙皮但不模拟”的根前缀。VL 的逐 strand 多骨权重有两个来源：
  `simulation.skinning.boneWeights`（`.tfxbone`，骨名映射后归一化）或
  `simulation.skinning.weights = "mesh"`（加载期从源网格派生）。两者都上传为每
  strand 最多 4 个 `(joint, weight)`；`motion.w` bit2 置位后 Integration 与
  StrandLevel 读取同一份权重（逐 strand `InitialPos` 与 `SkinningQuat`）。
  没有权重时退回单根骨矩阵与整根 strand 共用的 `SkinningQuat`。
  碰撞代理不走这条路径，它有自己的逐顶点权重。
- `weights = "mesh"` 的派生规则：每个 guide 的根点（导入期保留，见“静态几何”）
  在绑定姿态下匹配最近的身体顶点，复制其**完整**影响列表并归一化；2 cm 上限只是
  错空间检测器（本资产实测发根到最近顶点最大 0.516 cm）。个别根超距按根回退
  `rootBone`；全部根超距说明毛发与 source 不在同一绑定空间，加载期硬错误。
  不插值、不做 k 近邻。加载期打印一次
  `[TressFX] rootWeights: strands=… joints=… maxNearest=… fallback=…`。
- 碰撞胶囊可以按 `collision.capsules[].bone` 绑定到目标骨架：每帧用该关节的
  全骨架 bind→pose 矩阵变换 `a`/`b`，radius 不缩放；没有 `bone` 的槽保持作者
  绑定空间的静态值。声明 `collision.mesh` 时，逐帧 SDF 碰撞使用代理自身的
  逐顶点权重（从最近绑定姿态身体顶点转移），不再用单根骨矩阵；网格域每帧从
  这套 LBS 蒙皮后的代理包围盒重建。没有可用权重/`SkinPose` 时仍回退到单骨
  根矩阵（见下文“运行时 SDF 碰撞”）。

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
- 每个 frame-in-flight 槽一份 std140 params UBO（800 bytes），布局：
  gravity(0)、Wind0..3(16/32/48/64)、controls(80)、shape(96)、motion(112)、
  counts(128)、skinningQuat(144)、skinning(160)、historyToCurrentLocal(224)、
  vspStiffness 16×vec4(288)、localShapeStiffness 16×vec4(544)。
  `motion.y` 是 ResetPositions；`motion.z` 保留但宿主不再使用（placement 换基
  已并入首轮 Integration 的独立 reset/换基参数块）；`motion.w` 的 bit0/bit1
  分别表示 VSP / LocalShape 作者数组存在（其余位保留 0）。数组按
  `[local/4][local%4]` 取第 local 个作者值，未提供时 shader 走各自回退。
  复位帧 `controls.x = 0`。
  另有一份同布局的 resetParams UBO（每对象每槽），只给首个 substep 的
  Integration 使用，避免同一 UBO 被多次 dispatch 读到最后一次写入。不新增
  descriptor set layout 或 buffer 绑定。
- collider SSBO（Collision pass binding 7）**每个 frame-in-flight 槽一份**：
  host-visible/coherent，每个 collider 32 字节（`GuideColliderGpu`：a.xyz +
  a.w=radius、b.xyz），固定 8 槽；`counts.w` 决定 pass 实际读取数量。声明了
  `bone` 的胶囊每帧由 Host 重写（a/b 随关节移动），未绑定槽保持创建时的作者值；
  空列表也保留零填充 buffer。
- 声明 `collision.mesh` 时额外有下列资源：
  - SDF params UBO（64 bytes，host-visible）**每个 frame-in-flight 槽一份**：
    `originCell`（xyz 最小角 + w 格距）、`dims`（xyz 格数 + w 总格数）、
    `counts`（x 代理三角形数，y = 1 表示代理走逐顶点 LBS）、
    `range`（x margin + y truncation）。域每帧变化，Host 在每帧 dispatch 前写
    当前槽；C++ 用 `static_assert` 固定 size/offset，shader 声明在同名 std140
    block。
  - 代理顶点 SSBO（vec4/顶点，w 保留）与索引 SSBO（uint，每 3 个一组）：
    device-local，创建时经 staging 上传一次绑定姿态数据。
  - 代理影响流 SSBO（SdfSplat binding 12）：device-local，CSR word 流
    （`vertexCount+1` 个绝对偏移 + `(joint, floatBits(weight))` 对），创建时
    经 staging 上传一次；本资产为 2,572 顶点、53,788 bytes、最多 4 影响。
  - 代理 palette SSBO（SdfSplat binding 13）**每个 frame-in-flight 槽一份**：
    host-visible/coherent，每关节 128 bytes（current 后跟 previous，本实现两者
    填同一矩阵），按骨架关节数分配（本资产 105 关节 = 13,440 bytes/槽）。
    Host 每帧从源 `SkinPose` 的全骨架 bind→pose 写当前槽。每帧 CPU 只做这份
    13.4 KB 的矩阵拷贝；逐顶点蒙皮在 GPU splat 内完成。
  - 距离网格 SSBO（uint/格）：device-local，固定 `resolution³ * 4` bytes
    （`resolution` 在创建时解析为 48..64）；内容每帧由 SdfClear 重写，因此
    创建时不需要上传初值，buffer 永不逐帧重分配。

buffer 由 `RenderableObject` 拥有并随 World 资源包退休；`HairSimulationPass` 只拥有
9 个 compute pipeline（6 个 solver/胶囊 pass + 3 个 SDF pass）、每个 pass 的
descriptor pool、逐对象 per-pass descriptor set 缓存和逐对象 `simulationFrame`。
未声明 `collision.mesh` 的对象不分配 SDF 三个 pass 的 descriptor set，也不录制
它们；其余 6 个 pass 的 descriptor 合同保持不变。

### Pass 顺序

正常步进时 `HairSimulationPass` 按 TressFX 5.0 的 order 录制，每帧按源调度
执行 0/1/2 个 substep（每个 substep 固定 0.0125 s）。复位帧的首个 substep
Integration 走 ResetPositions 分支并写满三份历史，但同轮后续
StrandLevel/VSP/LocalShape/LengthWind/胶囊/SDF collide 仍执行；第二个 substep
正常步进。SdfClear/SdfSplat 每帧一次（在 substep 循环之前），SdfCollide 每个
substep 一次。前五个 pass 与 `artifacts/tressfx-shaders/` 的源 .usf 一一对应；
第六个 Collision 是 VL 追加的胶囊投影；第 7–9 个是 `collision.mesh` 存在时
逐帧重建距离场并投影的 SDF 阶段：

| # | TressFX 源 | VL shader | dispatch 粒度 | 读取 → 写入 |
| --- | --- | --- | --- | --- |
| 1 | `TressFXSimulationIntegration.usf` → Integration + GlobalShapeConstraints | `tressfxSimIntegration.comp` | 每顶点，`ceil(pointCount/64)` | rest/current/previous/prevPrev → current/previous/prevPrev |
| 2 | `TressFXSimulationStrandLevel.usf` → CalculateStrandLevelData | `tressfxSimStrandLevel.comp` | 每 strand，`ceil(guideCount/64)` | current/previous/prevPrev → strandLevel |
| 3 | `TressFXSimulationVSP.usf` → VelocityShockPropagation | `tressfxSimVsp.comp` | 每顶点，`ceil(pointCount/64)`；仅当 `vspCoefficient > 0` | strandLevel/current/previous → current/previous |
| 4 | `TressFXSimulationLocalShapeConstraints.usf` | `tressfxSimLocalShape.comp` | 每 strand；strand 内串行 | rest/strandLevel/current → current |
| 5 | `TressFXSimulationLengthConstraintsWind.usf` | `tressfxSimLengthWind.comp` | 每 strand；strand 内串行 | restLengths/current/previous → current/previous |
| 6 | 无（VL 追加） | `tressfxSimCollision.comp` | 每 strand；仅当 `colliderCount > 0` | colliders/current → current |
| 7 | `TressFXSimulationSDFGenInit.usf` | `tressfxSimSdfClear.comp` | 每格，`ceil(cellCount/64)`；仅当 `collision.mesh` | SDF params → grid |
| 8 | `TressFXSimulationSDFGenConstruct.usf`（含 MeshSkinning/MeshBox 的职责） | `tressfxSimSdfSplat.comp` | 每三角形，`ceil(triangleCount/64)`；仅当 `collision.mesh` | 代理顶点/索引/影响流/palette/SDF params → grid（atomicMin） |
| 9 | `TressFXSimulationCollideStrandsVerticesWithSDF.usf` | `tressfxSimSdfCollide.comp` | 每 strand；仅当 `collision.mesh` | current/grid/SDF params → current |

TressFX 的 `SDFGenFinalize` 在 VL 里不需要单独 pass：源实现用 `FloatFlip3` +
`InterlockedMin` 写有符号距离、再用 `IFloatFlip3` 还原；VL 的 splat 直接写一个
magnitude-first 的 uint key（最低位是符号，见下），clear 用 `0xFFFFFFFF` 哨兵，
采样时一次解码，因此没有额外的 finalize dispatch。`SDFMeshSkinning`/`SDFMeshBox`
的职责也合并进 splat 与 Host：顶点蒙皮在 splat 的每个三角形上做，网格域每帧由
`RenderableObject` 从蒙皮后的代理包围盒重算并写进 SDF params UBO。

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
   `DampingCoeff = damping`（作者值直接使用），`gravity` 取资产向量。然后若
   `GlobalShapeStiffness > 0 && GlobalShapeEffectiveRange > 0 && local < range*N`，
   `temp += stiffness*(InitialPos - temp)`。最后按 `prevPrev ← prev`、`prev ← current`、
   `current ← temp` 更新三个历史 buffer。ResetPositions 分支在积分之前返回：
   current/previous/prevPrev 全部设为 `(InitialPos, movable ? 1 : 0)`，
   movable 仍由根前缀与导入逆质量共同决定，不照抄 rest.w。
2. **StrandLevel**：取前两个点的 `prevPrev/prev/current`；
   `u = normalize(old1-old0)`、`v = normalize(new1-new0)`、
   `q = QuatFromTwoUnitVectors(u,v)`、`trans = new0 - q*old0`；
   `Accel = |new1 - 2*old1 + oldOld1|`，超过 `vspAccelThresholdMax` 时
   VspCoeff = 1，否则 VspCoeff = `vspCoefficient`。同时写
   `SkinningQuat = skinningQuat`。即使 VSP 关闭此 pass 也执行（LocalShape 需要它）。
3. **VSP**：跳过根前缀；`coeff = VspCoeff`，当 `coeff < 0.9` 时乘逐顶点
   VSP stiffness，然后对 current 和 previous 同时做
   `(1-coeff)*p + coeff*(q*p + trans)`。同时混合 previous 是为了不引入虚假速度。
   作者数组存在时逐顶点值来自 `vspStiffness[local/4][local%4]`；未作者时回退到
   根前缀→梢部线性近似。
4. **LocalShape**：从 `LocalVertexIndex = rootFixedPoints - 1` 到 N-2 串行。用
   `SkinningQuat` 旋转 rest 三点得到 bind-pose；`rotGlobal` 把 bind 段
   `(i-1→i)` 转到当前段方向，再把 bind 段 `(i→i+1)` 旋转得到 `i+1` 目标；
   `del = 0.5*Stiffness[i]*(target - pos_{i+1})`，i 和 i+1 可移动时分别 `-=` / `+=`。
   VL 用 `Stiffness[i] = bendStiffness` 作为回退标量（作者曲线数组存在时改用
   `localShapeStiffness[local/4][local%4]`，加载期已 clamp 到 0.95）；两种
   情况都乘以 TressFX 公式里的 0.5。
5. **LengthConstraintsWind**：
   - 风：Host 每帧把单个作者风向量展开为 TressFX `SetWind` 的四路锥形风：
     `wM = |wind|*(sin(simFrame*0.001)² + 0.5)`；四角是风方向分别绕 Y 轴 ±40°、
     绕 Z 轴 ±40° 后乘 `wM`。shader 仍用
     `w = a*Wind0 + (1-a)*Wind1 + a*Wind2 + (1-a)*Wind3`，`a = (strand % 20)/20`。
     对 `rootFixedPoints <= local < N-1`，`v = pos_i - pos_{i+1}` 保持**未归一化**，
     `Force = -cross(cross(v,w),v) * 1e-2`，`pos_i += Force*dt²`。1e-2 是厘米积分下
     复现 TressFX 米制位移的换算，推导见“单位、忠实度与显式差异”。
   - 长度：`ApplyDistanceConstraint` 用可移动组合的 multiplier（0.5/0.5、1/0、0/1、0/0），
     目标长度来自 rest length buffer；偶数边后奇数边，迭代 `iterations` 次。
   - 速度 clamp：`delta = pos - prev`，`|delta| > 30 cm` 时
     `delta *= 30²/|delta|²`，`prev = pos - delta`；只重写历史，不改当前位置。
     源实现的分母是速度平方，因此结果速度是 `30²/|delta|` 而不是严格 30 cm；
     VL 逐字保留这个 quirk。
6. **Collision**（VL 追加，TressFX 5.0 guide shader 没有胶囊路径）：按作者顺序对
   每个胶囊做位置投影，只改 current；不可移动点跳过；`a == b` 退化为球。
7. **SdfClear**：把全部格子写成 `0xFFFFFFFF`（TressFX `INITIAL_DISTANCE` 的
   等价哨兵，比任何有限距离的 key 都大）。
8. **SdfSplat**：一个 invocation 一个代理三角形。三个顶点先蒙皮：有逐顶点权重
   时（`counts.y == 1`）从影响流 CSR 混合源 `SkinPose` 的全骨架 bind→pose
   palette，每个关节按权重累加 `mat4 current`；没有可用权重/pose 时
   （`counts.y == 0`）回退到 `params.skinning` 的单骨刚性矩阵。之后对
   “三角形 AABB 每侧外扩 truncation”覆盖的每个格子中心算
   `SignedDistancePointToTriangle`（最近点 + 绕序法线符号），只保留
   `|d| <= truncation` 的值，写成 magnitude-first 的 uint key 并 `atomicMin`
   （先按 |d| 选最近三角形，最低位保留符号）。距离数学与 `sdfBuilder` 的
   Ericson 最近点分支一致；格距/域由每帧 SDF params 给出。
9. **SdfCollide**：每根 strand 串行；跳过 `local < rootFixedPoints` 与不可移动点；
   三线性采样**有符号**距离，`distance > margin` 早退（内部点为负，因此总会处理）；
   用同一场做半步长中心差分梯度，梯度退化（平方长度 <= 1e-12）时跳过；否则
   `position += normalize(gradient) * (margin - distance)`。梯度指向距离增大的
   外侧，因此穿进代理内部的点会穿过表面被推到外侧。只写 current，不写
   previous（参考 shader 的 previous 重写是注释掉的）。

### 运行时 SDF 碰撞（`collision.mesh`）

资源仓库的 `Maps/SC_tressfx_female/Collision/female_body_proxy.obj` 是
`vl_build_collision_proxy` 生成的 2,572 顶点 / 7,391 三角形低模代理。它在导入期
被 `VL::Hair::LoadCollisionMeshObj` 读成三角形汤，随 `MeshSection` 共享，运行时
不再读文件。

**网格布局**

- 固定分辨率：创建时按**绑定姿态**代理 `maxExtent` 和作者 `cellSize` 解析
  `resolution = clamp(round(bindMaxExtent / cellSize), 48, 64)`，之后 buffer
  永远是 `resolution³` 格，不逐帧重分配。作者 `cellSize` 是目标格距，不是最终
  格距；实际每帧格距 = 域边长 / resolution。它仍进入模型缓存身份。
- 每帧域：Host 用与 GPU splat 相同的蒙皮路径把 2,572 个代理顶点变换后求包围盒
  （有转移时是逐顶点多骨 LBS，否则是 `params.skinning` 单骨矩阵），取包围盒中心
  为立方体中心，边长同时覆盖包围盒与
  `truncation = margin + 2*cellSize`。解
  `side = (maxExtent + 2*margin) / (1 - 4/resolution)` 使
  `cellSize = side/resolution` 和 `truncation` 自洽；`dims` 恒为
  `{resolution,resolution,resolution}`。包围盒退化为零/负 extent 时按
  `1 cm` 最小边长处理；非有限包围盒（坏蒙皮矩阵）跳过该帧三个 SDF dispatch
  并只报告一次。Host 只做这一遍 2,572 顶点的包围盒归约（Release 实测约
  0.046 ms/帧，含 palette 上传）；逐顶点蒙皮本身在 GPU splat 内完成。
- 格子中心：`origin + cellSize*(index + 0.5)`，与 TressFX 的 `GetSdfCellPosition`
  一致。轴顺序 x 最快、z 最慢，`cellIndex = x + dims.x*(y + dims.y*z)`。
- 存储：每格一个 uint，key = `(floatBitsToUint(|d|) << 1) | (d < 0 ? 1 : 0)`。
  无符号 `atomicMin` 先按 |d| 选**最近**三角形，最低位保留该三角形的符号；
  采样时右移并还原符号。代价是 float 尾数少 1 bit（碰撞尺度下远小于格距）。
- 为什么不是 TressFX 的 `FloatFlip3`（`asuint(f) ^ 0x80000000`）：XOR 编码在
  同号段内按 |d| 排序，但一个格子通常被多个三角形覆盖；min 会选出 band 内
  **最负**的三角形，而不是最近三角形。对正式代理的表面三角形质心实测，
  XOR + min 得到 `-1.24 cm`（应为 0）；magnitude-first key 得到 `+0.0002 cm`，
  且 ±1.5/±2.5 cm 的内外点分别得到 `-1.50/-2.50/+2.00 cm`。这是有证据的
  显式修正，编码本身仍是一个 atomicMin 兼容的 uint key，只是不再与源
  XOR 逐位相同。
- clear 哨兵是 `0xFFFFFFFF`（TressFX `INITIAL_DISTANCE` 的等价物），比任何
  有限距离的 key 都大，不会被 `atomicMin` 覆盖。
- splat 只写 `|d| <= truncation` 的格子：AABB 的角可能让真实距离超过 band 的
  三角形覆盖到该格；把它的值写进来会污染带内最近表面。
- TressFX 的 `SDFDirectionBuffer` 稀疏 distance/direction 链表只存在于
  `PERMUTATION_SDF_VISUALIZATION == 1` 的可视化分支；生产 collision 分支
  （`== 0`）本来就是 dense uint 距离网格，方向由相邻采样求梯度。VL 直接采用
  生产分支的 dense 表示，不实现可视化链表，也不存逐格方向。

**逐帧重建与采样**

- SdfClear/SdfSplat 每帧各一次，SdfCollide 每个 substep 一次；同一对象内相邻
  dispatch 之间由既有 `RecordInterPassBarrier` 建立 compute→compute 内存依赖。
  代理顶点/索引是创建时上传的绑定姿态数据；每帧变化的是 `params.skinning`、
  域（origin/cellSize/truncation）和网格内容。网格 buffer 大小固定，因此域
  跟随骨骼移动不需要重分配。
- SDF params UBO 按 frame-in-flight 分槽；Host 在每帧 dispatch 前写当前槽，
  避免覆盖仍在飞的上帧域。参数块仍是 64 bytes，C++/shader 两边有 `static_assert`
  与镜像注释。
- 三线性采样：点先换算到格子坐标，取相邻两个格子中心做凸组合（与
  `sdfBuilder::SampleSdf` 相同的“减半格”锚点）。**不是** TressFX 的
  `floor((p-origin)/cell)` 锚点；后者在格子后半段会外插，对符号距离会越过零
  产生假符号，因此 VL 有意改成不外插的相邻中心插值。域外或 8 角点任一仍是
  clear 哨兵时返回 `+1e10`（碰撞 pass 早退）。
- 采样值是**有符号**的：表面外侧为正、内侧为负；内部点不会早退。
- 梯度：`h = 0.5*cellSize` 的中心差分，除以 `2h = cellSize`。参考 shader 用
  `h = 0.1*cellSize` 的前向差分；中心差分减少单侧偏置。梯度平方长度
  `<= 1e-12` 视为退化并跳过，避免 `normalize(0)` 产生 NaN。
- 投影：`distance < margin` 时 `p += normalize(g) * (margin - distance)`。
  梯度指向距离增大的外侧，所以内部点会穿过表面到外侧 `margin`；表面外点
  只在 `distance < margin` 时向外移动。`margin` 是发丝中心线与代理表面的
  最小距离（厘米），不是胶囊半径。
- 蒙皮：代理三角形使用自己的逐顶点权重（加载期从最近绑定姿态身体顶点转移，
  见“碰撞：胶囊与逐帧 SDF 网格”）和源 `SkinPose` 的全骨架 bind→pose palette；
  没有可用权重/pose 时回退到根点同一个 `params.skinning` 单骨矩阵。头部转动时
  场随身体正确变形，不再绕 Head_M 刚性旋转整份代理。没有 `.tfxmesh`。

**符号约定与代价**

- 符号来自**最近三角形**的绕序法线：`dot(p-a, cross(b-a,c-a)) < 0` 为负
  （TressFX `SignedDistancePointToTriangle` 的同一约定）。碰撞代理保留身体
  网格绕序，正常情况外侧为正。
- 代理有 522 条开放边、2,092 条非流形边；开放边/非流形边附近没有唯一内外，
  最近三角形的法线可能给出错误符号。距离幅值不受影响，但这些区域应视为弱
  约束。
- 场只在 ±truncation 带内有效：比 `margin + 2*cellSize` 更深的内点或更远的
  外点采样到 clear 哨兵，返回 `+1e10`，不做碰撞。发丝常规情况（距表面
  ~1 cm 以内）在带内；TressFX 的固定格偏移也有同类带限制。
- `female_head.sdf` 是离线有符号场（伪法线、0.4 cm）；它不参与运行时，也不
  能把它的符号/分辨率直接搬进每帧场。

**已知近似与未验证项**

- 代理权重来自绑定姿态下最近的身体顶点（不是表面点、不做跨顶点插值），
  实测最近距离最大 0.9256 cm；权重只在加载期转移一次，运行期不重新求解。
  每帧域用 CPU 蒙皮全部代理顶点求包围盒（Release 实测约 0.046 ms/帧）；
  代理若增长到数万顶点需要改 GPU 归约（TressFX 的 `ICMBox` 路径）。
- 碰撞是位置投影，不重写 previous positions，因此没有摩擦、没有碰撞速度冲量，
  接触略黏；参考 shader 同样把 previous 写入注释掉了。
- 胶囊碰撞仍然存在；声明 `mesh` 不会禁用胶囊，两者在长度 pass 之后先后执行
  （胶囊先、SDF 后），重叠区域的最终位置取决于这个固定顺序。
- 分辨率固定 48..64、作者 `cellSize` 只是目标；实际格距随域边长变化，细长
  代理会让短轴格距偏大。没有按轴非均匀格距。
- 编码用 magnitude-first key（尾数少 1 bit），不是源 `FloatFlip3` 的逐位
  复刻；行为差异见上。
- 没有 GPU 读回或 CPU/GPU 逐点对照；CPU 镜像只保证同一套公式，`sqrt` 舍入、
  FMA 融合和指令调度可能带来末位差异。烟测里的符号数值检查是 CPU 镜像的
  临时诊断，已移除。

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
- SDF 的三个 pass 是 compute-only：距离网格从不经 transfer 写入，clear→splat→
  collide 由相邻 pass 的 compute→compute barrier 定序；SDF 参数 UBO 与 simulation
  params 一样按 frame-in-flight 分槽、每帧由主机写当前槽。代理顶点/索引的
  staging 上传由 dispatch 前 barrier 的 `TRANSFER_WRITE` src scope 覆盖。
- 每个 pass 的 descriptor set layout 不同（各 shader 只声明自己用到的 binding），
  因此 per-object 缓存按 `[pass][frameSlot]` 保存 9×2 个 set（声明
  `collision.mesh`）或 6×2 个 set（只有胶囊/无碰撞），每个 pass 有独立的
  descriptor pool 链。set 里的 buffer 句柄每帧校验，buffer 被替换时重建。

`simulation.enabled = true` 且 backend = gpu 时，CPU 不构造 `GuideSimulation`，
`RendererResourceCache::AdvanceTressfxSimulations` 跳过该对象。tressfx 顶点 buffer
回到普通 device-local 上传路径，不再逐帧 memcpy：vertex factory 只消费 SSBO 中心线，
顶点位置不参与展开。

### 步进与风时间

VL 的 GPU 与 CPU 运行路径都采用源上层调度（`GuideSimulation::StepFrame` 与
`HairSimulationPass` 共用同一计划函数）：

```text
n = 0 (delta <= 0), 1 (0 < delta <= 0.0167), 2 (delta > 0.0167)   // 上限 2
for substep in 0..n-1: solver step 0.0125 s
simulationFrame += 1                       // 每渲染帧一次，同一帧所有 substep 共用风相位
```

- **0.0125 s 是每个 substep 真正传给 shader 的 dt**；`0.0167` 只决定循环次数。
  `maxDt` 不再参与 GPU 原生路径或 `StepFrame`，只有旧 `Step(dt)` 入口仍 clamp。
- `n == 0` 时本帧不录 pass，也不消费 ResetPositions、pose 身份、时钟或历史；
  reset 原因保留到下一个正 delta 帧（源 while 不执行时同样不消费）。
- ResetPositions 只作用于首个 substep 的 Integration（`motion.y=1`、dt=0），
  同轮 StrandLevel/VSP/LocalShape/LengthWind/胶囊/SDF collide 继续执行；
  `n == 2` 时第二轮是正常步进。
- placement 换基只在非 reset 帧的首个 substep Integration 应用一次
  （独立 resetParams UBO 携带 `historyToCurrentLocal`）；后续 substep 用单位矩阵。
- SdfClear/SdfSplat 每帧一次（在 substep 循环之前），SdfCollide 每个 substep 一次。
- 有意差异：源每 substep 的 `SetWind` 还乘 `(0.3 + FRand())`，VL 保持确定性；
  `n == 0` 帧 VL 不递增 `simulationFrame`（源在循环前无条件 `++Guides.Frame`），
  首帧相位因此固定差 1。
- 旧 `Step(dt)` 仍存在，只用于既有变量步长测试/调用，不是运行时路径。

`RecordHairSimulation` 仍在图形 pass 之前调用，但不再使用曝光墙钟。GT 把本帧
模拟 delta 冻结到 WorldSnapshot/RenderScene：

- 绑定角色的 GPU hair 继承**source draw** 的 `simulationDeltaSeconds`；
  普通 mesh 用原生动画 `simulationDelta`，不受 C# Play/Pause/time-scale 控制；
  `__blueprint/` 源对象用 ScriptRuntime 实际 scaled delta，Editing/Paused 为 0。
  source draw 存在但 pose 缺失时仍取该 source delta，并保留上次 palette；
  source draw 完全缺失时回退到场景的 hair delta。
- 未绑定角色的 CPU/GPU hair 用全局 `hairSimulationDeltaSeconds`：
  Editing 正常推进，Playing 使用游戏 scaled delta，Paused 为 0。
- RT 以 `(worldGeneration, frameIndex)` 去重，重绘同一冻结快照不重复积分。
  mailbox 仍为 latest-wins；被覆盖的旧 GT 帧不补算。当前 GT 每帧等待 RT。
- 仅刷新已有场景状态时发布零 delta，不能重复使用上一帧的游戏步长；
  正常快照只有发布成功后才消费待发布的 delta。
- 零 delta 帧跳过 solver，也不消费 ResetPositions；reset 原因保留到下一个
  正 delta 帧。复位帧不额外推进 `simulationFrame`；普通动画 `pause` 只冻结
  骨骼，风仍可驱动发丝。

### 跳变与 ResetPositions

`GuideSimulationHistory` 只保存最后一次已录制的 pose 身份与不连续代数，
不持有播放器。首次有效 pose、播放器身份变化、`SkinPose::discontinuity` 变化、
缺失 pose 后恢复都触发一次 ResetPositions；无骨架对象创建时也复位一次。
seek、clip/硬切换沿用现有动画不连续代数，不使用根位移阈值猜测传送。
连续 pose 保持历史，单纯 animation pause 不制造额外复位。

Reset 帧首个 substep 的 Integration 上传 `motion.y=1`、dt=0，把
current/previous/prevPrev 设为相同的当前 `M*rest`；同轮后续
StrandLevel/VSP/LocalShape/LengthWind/胶囊/SDF collide 继续执行，`n == 2` 时
第二轮正常步进（与源 host `TressFXCoreSimulation.cpp` 1137..1214 的顺序一致）。
只有整帧录制完成才提交已消费身份、时钟与历史，暂停不会提前吞掉 seek。
事件日志是
`[TressFX] ResetPositions: ... reason=... playbackIdentity=... discontinuity=... frame=... substeps=N`；
reason 为 `initial-state` / `first-pose` / `playback-changed` /
`pose-discontinuity` / `pose-recovered` / `requested`。该日志证明 CPU 录制走到复位分支，
不等于 GPU 三份位置的逐点读回。

作者/调试可以显式请求一次复位：Console `hair reset [objectName]`，空参数表示当前
World 里全部 GPU guide 对象。目标按渲染对象名匹配（场景对象名），不是 `mesh|...`
身份。请求经 `RuntimeCommandType::ResetHairSimulation` 交给渲染线程，只在该对象
下一个**有 substep** 的帧消费；0 步/暂停帧保留请求，既不丢弃也不提前吞掉。消费时
复用同一套 `motion.y`/resetParams 分支，强制 `reason=requested` 并关闭同帧 rebase
（三份历史直接写回 InitialPos，换基没有意义）。它不重载 World、不重建 buffer 或
descriptor，也不修改资产文件。编辑器毛发视口的 Reset 按钮发空目标，见
[game-ui-stack.md](game-ui-stack.md)。

### 实时参数覆盖（会话级）

编辑器/调试可以在不重载 World 的前提下覆盖部分模拟标量：

- 命令：Console `hair set <objectName> <field> <value...>`；目标按渲染对象名匹配，
  **必须给名字**（没有“全部对象”语义，与 `hair reset` 不同）。
- 可覆盖字段（单位与作者字段一致）：`gravity`、`wind`（各 3 个分量）、`damping`、
  `iterations`、`bendStiffness`、`globalShapeStiffness`、`globalShapeEffectiveRange`、
  `vspCoefficient`、`vspAccelThresholdMax`。
- 值域按作者字段校验，**越界或非有限直接拒绝，不夹取**；`iterations` 必须是
  `[1,16]` 内的整数，`vspAccelThresholdMax` 属于 `(0,1e7]`。
- 需要重载的字段一律拒绝并提示：`enabled`、`backend`、`rootFixedPoints`、`maxDt`、
  两条逐顶点曲线、`collision.*`、`strandRadius`、`skinning.*`、`boneWeights`、`weights`。
- 只允许 GPU backend：CPU 参考 solver 在构造时另有一份 config 副本，覆盖不会作用于它，
  因此在该对象上明确报错而不是静默无效。
- 覆盖写的是该 `RenderableObject` 自己的 config 副本（不写回导入期的共享 section，
  不影响同一资产的其它 placement），**不写资产、不进入 `modelCacheKey`**，重载 World
  后回到作者值。
- 生效时机：命令经 GT 校验后进入有界队列（上限 64，超限丢最旧），渲染线程在下一个
  实际录制步进的帧按名匹配并应用，同一帧后段组 params UBO 时生效。队列不严格绑定
  World 世代：命令早于 World 加载时可能命中后续同名对象。
- 日志：GT 成功入队打印 `Hair parameter queued: …`；渲染线程应用打印
  `[TressFX] live param: object=… field=… value=…`。未命中对象时报告“不存在”或
  “CPU backend”，并列出本帧可用的 GPU 对象名（至多 8 个）。

### CPU 参考 solver

`VL::Hair::GuideSimulation` 按同样的 pass 顺序逐 strand 串行执行：
Integration+GlobalShape、StrandLevel、VSP、LocalShape、Length/Wind、Collision。
运行时走 `StepFrame(delta)`：按源调度执行 0/1/2 个固定 0.0125 s substep；
`Step(dt)` 仅保留旧变量步长语义。纯数学放在 `source/hair/tressfxSolverMath.h`，
GPU shader 与 CPU solver 共用同一组公式约定。CPU solver 没有渲染线程的
`SkinPose`，因此 `InitialPos` 直接取 rest（等价于单位蒙皮矩阵），且只能运行
未声明 `skinning` 的资产。它共享的是正常 solver 分支的数学公式，不消费 pose
discontinuity，没有 GPU history/Reset 事件时序，也没有 RT 冻结帧去重。

CPU/GPU 的一致性是“相同操作顺序”，不是逐位相等：Eigen 与 GLSL 的
`sqrt`/`sin` 舍入、编译器 FMA 融合和指令调度都可能产生末位差异；frame delta
按上文角色来源/全局时钟规则取值。没有 GPU 位置读回或结果交叉校验，
CPU 参考不消费正式 Female 的根骨/SDF，不能把两者称为已做逐点数值对照。

模拟状态按 placement 隔离：普通网格按 mesh key 共享同一个 `RenderableObject`；
GPU 发丝 section 的 renderable key 追加场景实例身份
（`renderableObjectKey.h`），因此同一 `SM_*` 的多个独立 placement 各自拥有
current/previous/prevPrev、params UBO、SDF 代理与历史，GPU 几何 buffer 也各一份
（CPU 导入数据仍共享）。同一 placement 的重复 section draw 仍共享同一对象；
`TressfxSimulationOwner` 保留为运行期安全网，检测同一对象被不同 identity 复用。
CPU backend 没有这层世界历史，也没有独立的 placement 状态。

作者 `gravity`/`wind` 是世界向量，Host 按当前 hair model 的线性部分换到 hair 局部
后上传；刚体旋转 placement 不旋转世界重力方向，换基保持世界语义。
固定源的 `ApplyVertexBoneSkinning` 在骨骼变换之后组合
`PrimitiveData.LocalToWorld`（patch 8303..8308 行），Integration 再把 world cm
转为 world m（8424..8425 行），三份位置历史留在该世界空间。VL 的 SSBO 是 hair
局部厘米，但 placement 变化时把 current/previous/prevPrev 乘
`inverse(currentModel) * lastModel` 换基；刚体平移、绕任意 pivot 旋转及组合下，
世界位置、世界速度和重力/风/global shape 的点级结果与源世界 solver 等价
（测试见 `tool/hair-tests/tressfxWorldHistoryTests.cpp`）。非均匀缩放/剪切只保证
点换基，rest length、胶囊半径、SDF cell/margin、速度 clamp 等局部度量不与源世界
solver 等价；source model 与 hair model 不一致、hair 单独 teleport 没有专门 epoch，
仍按未对齐处理。

### 单位、忠实度与显式差异

TressFX 源实现在**米**制下工作：rest 位置乘 0.01、速度 clamp 0.3 m。VL 全链路使用
**厘米**，因此所有 0.01 与 0.3 m 都省略/换成 30 cm，位置、半径、重力和风速的数值
保持厘米语义。这个换算不是对所有公式都无损：

- **风的三叉积与 cm→m 位移适配**：源实现在米制下把**未归一化**的段向量代入
  `-cross(cross(v,w),v)`，位移是 `|v_m|²·|w|·dt²`（米）。`w` 是 TressFX 的
  `WindMagnitude`，属于**力标量**而非风速，两种单位下取同一个数。VL 的位置和
  段向量是厘米：`v_cm = 100·v_m` 使三叉积为米结果的 `10^4` 倍，而厘米位移是米
  位移的 100 倍，因此
  `Force_cm = -cross(cross(v_cm,w),v_cm) * 1e-2`。乘 1e-2 后作者幅值与 UE 的
  `WindMagnitude` 数值可直接对照；段长平方量纲必须保留，不能再归一化 v。
- **速度 clamp** 的 `delta *= 30²/|delta|²` 是源实现的既有 quirk，VL 保留。
- **重力方向**：源实现是 `GravityMagnitude*(0,0,-1)`（Z-up）；VL 直接使用资产的
  `gravity` 向量（Y-up）。这不是通用换轴，只是不再注入源引擎的固定方向。
- **风的四角与时间调制**：四路锥形风和 `sin(frame*0.001)²+0.5` 幅值调制是
  TressFX `SetWind` 的语义。源实现每帧还乘 `(0.3 + FRand())` 的随机幅值抖动；
  VL 为保证确定性省略这个随机项，其余 `SetWind` 语义一致。VL 原来的
  `windGust`/`windFrequency` 已删除，不再有第二层正弦调制。
- **长度约束迭代**：TressFX 的资产 UI 把 `LengthConstraintsIterations` 限制在
  1..4；本资产的 32 点 guide 段长只有 0.4–1.5 cm（均值约 1.04 cm，TressFX 参考
  约 2 cm）。3 次 Gauss-Seidel 在根点姿态变化后留下明显拉伸，因此资产校准为
  8 次；solver 公式与偶数/奇数边顺序不变。
- **根点跳变与 ResetPositions**：首次姿态、seek/clip 的 pose 不连续代数与姿态
  恢复已有复位路径，三份历史同时清空。source placement 单独 teleport 没有独立
  事件；普通连续根运动也不按距离阈值触发复位。
- **VSP 逐顶点 stiffness**：TressFX 用最多 64 项的作者曲线
  `VSPStiffness`。VL 现在有可选 `simulation.vspStiffness` 数据路径：按
  `[local/4][local%4]` 打包进 params UBO，只在 `VspCoeff < 0.9` 时参与
  （与源实现同一条件）。未作者时仍用从根前缀（1）到梢部（0）的线性衰减近似。
  只移植非 MT 的 `CalculateStrandLevelData`：MT 变体里
  `Accel < VSPAccelThresholdMin` 时把系数设为 0.6 的分支没有进入当前资产面。
- **VSP 阈值与默认开关**：源 StrandLevel 的
  `length(new - 2*old + oldOld)` 没有除 dt²（patch 9925 行），源 world m 的默认
  1.4 对应 VL 同量纲 140 cm；代码默认已改为 140，旧的 5000 cm 不是该源默认值的
  单位换算（5000 cm = 50 m，是 140 的 35.7 倍）。
  源 `TressFXAssetSimulation.h` 默认启用 VSP（系数 0.9）与 global shape
  （强度/范围均 0.5），VL 两者默认关闭；这些默认参数不等价。
- **作者 stiffness 曲线**：源 `TressFXComponent.cpp` 以 `i/N` 采样 VSP 与 LocalShape
  作者曲线，LocalShape 再限制到 0.95（687..710 行）。VL 已提供
  `simulation.vspStiffness` / `simulation.localShapeStiffness` 的逐顶点数据路径、
  UBO 打包与 CPU/shaders 消费；两条数组都省略时分别回退到 VSP 线性近似与
  LocalShape 统一 `bendStiffness` 标量。当前没有从 Maya/UE 导出的真实曲线值，
  正式 Female 资产仍为空数组，因此实际运行仍走这两条回退。
- **LocalShape 起点**：源实现从 `NumVerticesFromRootNoSimulation - 1` 开始；该值
  为 0 时无符号下溢使循环不执行，为 1 时会读取 `GlobalVertexIndex - 1`（首根
  strand 越界）。VL 保留 0 时跳过，1 时从 local 1 开始；作者资产使用 >= 2。
- **多骨发丝根**：声明 `simulation.skinning.boneWeights` 时，导入期读取 `.tfxbone`，
  把骨名映射到源角色目标骨架，运行期上传逐 strand 最多 4 个 `(joint, weight)`
  （权重归一化）与全骨架 bind→pose palette；Integration 的 `InitialPos` 和
  StrandLevel 的 `SkinningQuat` 使用加权矩阵（`motion.w` bit2）。没有声明、
  空权重或 pose 不可用的 strand 回退单根骨矩阵。正式 Female 资产未带 `.tfxbone`，
  所以正式运行仍是单根骨；碰撞代理不同，它有自己的逐顶点权重（从绑定姿态身体
  转移），见上文。运行期 SDF 域由代理 LBS 后的包围盒重算。
- **串行约束**：见 pass 顺序一节的替换说明。
- **ResetPositions 边界**：复位使用当前根矩阵；声明 `boneWeights` 且 pose 可用时
  使用逐 strand 加权矩阵。不提供源实现的多骨/MT 根三角数据；
  该帧后续约束/风/胶囊/SDF collide 仍执行，所以复位后的姿态不是纯 rest；
  `M*rest` 与碰撞体的初始交叠会在同轮碰撞中被投影。
- **MT/blend-shape 变体**：只移植单骨骼
  `IntegrationAndGlobalShapeConstraints`/`CalculateStrandLevelData` 路径；
  TressFX 的 blend-shape 蒙皮变体依赖逐顶点根三角数据，不在当前资产面内。

显式限制：碰撞是作者解析胶囊 + 可选的有符号 SDF 代理，没有 `.tfxmesh` 解析器、
follow hair、深度阴影、OIT、LOD；两种碰撞都只做位置投影，不修改 previous
positions，因此没有摩擦、没有碰撞速度冲量，接触略黏且可能抖动；胶囊按作者顺序、
SDF 在胶囊之后执行，重叠区域的最终位置取决于这个顺序。
VSP 与 global shape 默认关闭，只在资产显式作者时启用；`vspCoefficient > 0` 时
Host 才录制 VSP dispatch。VSP 阈值默认 140 cm（源默认 1.4 world m 的同量纲换算，
不是旧的 5000 cm）。逐顶点 VSP/LocalShape 曲线数据路径已对齐（可选数组、UBO、
CPU/shaders、长度与 0.95 校验），但真实 Maya 曲线数据尚未导出：正式 Female 资产
仍用空数组，因此运行时仍走 VSP 线性与 LocalShape 单标量回退。GPU 路径有上述
局部历史 + placement 换基，且每个 placement 有独立模拟状态；CPU backend 仍没有
世界历史或独立 placement 状态。

### 运行时实例化（per-placement renderable）

需要逐放置 GPU 状态的网格（当前只有毛发）使用 placement 作用域的 renderable key；
机制本身与类型无关：

- **placement binding**：场景对象携带一个结构化记录（`kind` + `sourceSceneObject` /
  `rootBone` / `weights`），沿 scene object → `WorldMeshObject` → `MeshDrawSnapshot` →
  `RenderDrawPacket` 传递。旧描述符/场景里的 `__tressfx*` 字段仍被兼容读取。
- **resolve 前对账**：`RenderSystem` 在 `ResolvedRenderSceneBuilder::Build` 之前，对 draw
  packet 里未知的 placement 键从模板 renderable 克隆一份（共享导入几何/section，
  自己持有模拟 buffer 与 params/reset UBO），再按 `kind` 调 handler 写绑定。
  spawn 克隆同时重写 `__blueprint/<template>/` 前缀并重建该 actor 的组件网格模板条目。
- **绑定解析**：模板已解析时直接复制（快路径）；模板未解析（异步加载的蓝图）时用本帧
  来源 draw 的骨架解析 root/capsule 关节，并用 `BuildTressfxRootWeights` 从 guide 根点
  派生 mesh 权重，结果按 `(template model key, kind + 参数, source asset identity)` 缓存。
  未解析模板叠加 `.tfxbone` 时明确报错，不静默退化。
- **退休**：`PrunePlacementRenderables` 在 placement 键连续 2 帧不在快照后退休；包级释放
  （`PruneDynamicResources`）也可能先摘走模板键，两条路径都会打 `[Placement] …` 日志。
  跟踪项在键存活时必须保留（清零缺失计数，不能 erase），否则第一次存活就会失去跟踪。
- **日志**：`[Placement] instance renderable: object=… template=… kind=…`、
  `[Placement] instance renderable retired: …`、`… released by package: …`，以及
  `live / placement-scoped in cache / runtime placement renderables` 三个计数（变化时才打）。
- **限制**：spawn 复制模板已解析的绑定（同蓝图同骨架）；`rootBone` 覆盖与模板不一致时
  报错；`weights` 覆盖不重新派生（沿用模板）；每次 spawn 复制约 2 MB `strandBones`。

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
Idle 来自 GASP 重定向）+ `Female_hair_skinned`（`TF_female_hair_skinned.json`，
`backend=gpu`、`rootFixedPoints=2`、`skinning.source=FemaleCharacter`、
`skinning.rootBone=Head_M`）。四个胶囊分别绑定 `Head_M`、`Neck1_M`、`Chest_M`
（肩线）与 `Chest_M`（上背），`collision.mesh` 代理使用从该身体转移的逐顶点
权重；当前蒙皮发丝资产作者 `gravity=[0,-980,0]`、`wind=[2000,0,0]`、
`damping=0.035`、`maxDt=0.016`、`iterations=8`。
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
