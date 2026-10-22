# TressFX 资产导入

> 状态：**当前契约**（CPU 导入与静态 ribbon 路径已落地）。本文件只描述当前源码与验证覆盖的行为。

## 实现边界

`source/hair/` 的 `vl_hair_assets` 是纯 CPU 静态库，供根工程 `main`、
`vl_tfx_inspect` 和 `hair_asset_tests` 链接；不依赖 UE、Vulkan、World 或材质系统。
该库只支持 `.tfx` guide 文件及静态几何准备，没有 `.tfxbone` / `.tfxmesh`
解析、follow 生成或模拟。静态运行时接入位于独立的网格 importer 和引擎顶点工厂，
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

- 必须为 `.tfx`、一个材质槽、有限且 float 可表示的正半径；拒绝 importOptions 与 skinning。
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
  SSBO（Object Set 2 binding 3）。启用 simulation 时，游戏线程在帧边界运行 CPU
  Verlet guide solver，更新根点约束、重力/风、阻尼和长度投影；graphics
  `TressFXStrands` vertex factory 从该 SSBO 读取中心线并完成相机朝向 ribbon 展开。
  未启用 simulation 时，SSBO 仍由静态中心线初始化，保证 descriptor 合同稳定。
  当前没有把 `tressfxGuideSim.comp` 接入实际 dispatch；也没有独立 RenderGraph pass。
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
  OIT、发丝投影、自阴影或模拟；不恢复 Hair ID，不写 GBuffer。
- 内容浏览器把 tressfx 归为 Mesh；标准 mesh API 支持既有字段读改保存，
  Scene/BP 的模型引用接受该类型。`strandRadius` 不在标准修改能力表中，
  描述符半径修改属于能力表外的源码编辑。

`tool/hair/generate_static_probe.py --resource-root <isolatedRoot>` 创建
`Generated/Validation/SC_tressfx_static_probe/`（存在即拒绝），包含合成 TFX、
SM/MI 和两实例场景；正式 Common pass 资产由调用方提供。
它不改 config、不启动引擎，也不是 runtime test。生成场景说明其夸大半径、
Unlit 与非女性资产边界。

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

三个新关卡的相机为 `[250,120,420]`、目标 `[0,90,0]`、水平 FOV 38°、EV100=0；
环境和方向光复用静态版的审阅设置。身体与眼球共用一个蒙皮 mesh component，
各用中性 PBR 材质；蓝图源码属于资源根的 `Blueprints/`，
`cmake --build build --target main vl_gameplay -j8` 构建原生和 Gameplay。
蓝图入口要求构建时启用 C#；宿主默认 Editing，只有 Play 后才推进其动画。
旧静态版保持独立。新动画关卡不挂静态发丝：当前 TFX 无骨骼权重，不能随头部变形。

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
