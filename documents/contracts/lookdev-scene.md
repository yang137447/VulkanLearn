# Lookdev 中性审阅场景

> 状态：**当前契约**（实现已落地）。本文件描述系统**现在是什么**，每条可核对到源码、测试或 	ool/architecture-boundary-audit.ps1 的门禁规则；不含对未来行为的承诺 —— 那些在 documents/rationale/ 或 documents/plan/。

## 目的与范围

`Maps/SC_lookdev/SC_lookdev.json` 是项目内可复现的基础材质审阅场景。
布局参考用户提供的双球、色卡、灰阶图；方法参考李文磊 2021-03-08 的
《关于建立资产统一审查预览环境的一些说明》：
固定曝光、中性照明、灰板测光、稳定显示条件。

正式 `SC_lookdev` 校准基线不宣称复刻 UE 4.26.1 的具体 HDRI、显示变换或
Substance 输出；它使用一张为 VL 数值校准生成的合成 HDRI，也不是认证实物
ColorChecker 的光谱/色度替代品。文章复原场景单独使用 UE 导出的
`EpicQuadPanorama_Gray.hdr`，只以网页观感为对齐目标。当前没有 TAA、FSR、
GI 或实时面光。

## 场景与显示合同

- 世界长度为 cm；相机 `[0,-20,630]`，朝向 `[0,-20,0]`，水平 FOV 44 度。
- 手动 `EV100=0`，补偿 0；现有曝光公式给出倍率 `1/1.2`。
- `tonemap 3` 使用项目现有 ACES fitted；不是完整 ACES/OCIO 管线。
- `bloom strength 0`，默认饱和度 1；正常 Beauty 不改变色卡材质值。
- 单个中性合成 HDRI 提供全部照明和反射；没有额外方向光或假面光。
  这与文章的照明组织方式一致：后续若换成文章原始 HDRI，也只替换这一张
  环境图，不再叠加一盏方向光模拟 HDRI 中已经存在的亮源。
- HDRI 的亮矩形、顶灯和地面格纹是同一张 radiance 图的内容。
  它们不是场景局部灯具；不产生场景遮挡，也不是测量得到的绝对亮度。
- 可见背景使用独立 Unlit 平板，显示线性目标为 `0.04`，不参与环境照明。
  背景仍经过正常曝光和 tone mapping，没有绕过显示链路。
- 镜面球：金属度 1、粗糙度 0.025、线性反射颜色 0.95。
- 灰球、参考卡、灰阶与灰片：金属度 0、Specular 0，正常受光。
- 三块小灰片反射率均为 0.18，粗糙度分别为 0.05 / 0.5 / 1，
  用于验证零高光时粗糙度不改变漫反射测光。
- 24 色卡使用生成器中固定的参考 sRGB 三元组，转换一次到线性反射率。
  这些数值只定义本项目参考色板，不能作为认证色卡的测量数据。
- 渐变的线性反射率从 0.9 到 0.003，源 PNG 以 sRGB 编码；
  texture asset 声明 sRGB，GPU 采样恢复线性，不重复 gamma。

## DefaultLit 零高光

`M_pbr.u_specular` 默认 0.5，映射为介电 `F0=0.08*Specular`。
`MaterialInputs -> MaterialSurface -> GBuffer B.g -> deferred` 和 forward
都传递并使用同一作者值；没有改变 GBuffer 布局。

非金属且 Specular=0 是显式关闭镜面 lobe 的语义：

- 直接光输出完整 Lambert 漫反射，镜面为零，不保留 Schlick 掠射项；
- IBL 镜面为零，不保留 split-sum 的 B 项；
- 金属 F0 来自 BaseColor，不因该参数为零而关闭反射。

默认 Specular=0.5 的材质保持原有计算。已经作者为 Specular=0 的
`M_sharedLambert` 现在也实际消费该语义，不再被固定 0.5 覆盖；
它的实时图像因此可能改变，但离线 CPU evaluator 未改动。
这项变化是显式作者语义，不是对默认 BRDF 的整体重写。

## 资产归属与重建

`tool/lookdev/build_scene.py` 是该预设的离线作者工具，只依赖 Python 3 标准库。

- `Maps/SC_lookdev/`：场景、稀疏 MI、SM 和 texture asset JSON。
- `Generated/Lookdev/`：重建的 HDR、球/平板 OBJ、渐变 PNG、清单与验证日志。
- `Generated/Screenshots/`：引擎正常截图入口输出的 BMP。

生成器只写自己的命名空间，但会重建其中的同名 JSON。通过 UI 保存的
lookdev MI 改动应在再次生成前另存为独立资产，或回写生成器。
加载本场景前必须先生成派生文件；它们是 gitignored 的本地产品。
不修改 `initScene`、共享 Pass MI 或全局配置。

```powershell
py -3 tool/lookdev/build_scene.py
.\tool\lookdev\run_lookdev.ps1
```

## 文章 HDRI 构图还原

UE `AssetsPreview.zip` 是一个 UE 4.26 项目，`.uasset` 不作为 VulkanLearn
的运行时输入。当前迁移链路使用本机 UE 5.7 打开项目，通过
`tool/lookdev/export_ue_assets.py` 导出文章 HDRI，再由
`tool/lookdev/import_ue_export.py` 将 HDR 复制到资源目录，最后由
`tool/lookdev/build_ue_preview_scene.py` 生成：

```text
Generated/Lookdev/SC_lookdev_article.json
```

可以用一条命令重复执行完整流程：

```powershell
.\tool\lookdev\convert_assets_preview.ps1 `
  -ZipPath 'C:\Users\pb.adcycyys001\Downloads\AssetsPreview.zip' `
  -UnrealEditor 'D:\sofeware\Epic Games\UE_5.7\Engine\Binaries\Win64\UnrealEditor.exe'
```

文章场景直接使用 UE `SM_ColorCalibrator` 导出的可见 section、两个参考球、
24 色卡、灰阶渐变和 `EpicQuadPanorama_Gray.hdr`。导入时排除
`UCX_SM_ColorCalibrator` 碰撞代理。UE 材质关系由
`tool/lookdev/probe_ue_materials.py` 在编辑器侧离线探查后记录：
`Gray_MI` / `Gray_NoSpec` 继承 `Base_M`，`M_ColorGrid` 和 `M_SkinBall`
是独立 Material，`M_ClearCoat` 来自 UE 编辑器内置 ColorCalibrator 资源。
VL 材质只按这些关系重建，并以网页效果为对齐目标，不做材质节点和显示变换的
二进制等价迁移。正式 `SC_lookdev` 校准基线不会被文章场景覆盖。
该链路暂不处理 TAA/FSR。

启动脚本通过已有 console command 固定 tone mapping 与 Bloom，
不引入场景格式扩展；这些状态是进程内状态。从别的场景在 UI 中切入时，
必须确认 tone mapping=3、Bloom strength=0，否则不属于已标定预设。
脚本等待 180 帧再应用；截图脚本等待 240 帧。这只适用于当前静态
场景的有限 SH/mip 初始化，不是通用的 GPU 代际完成查询。

## 截图验证

```powershell
$beauty = .\tool\lookdev\capture_lookdev.ps1 -View beauty
$specular = .\tool\lookdev\capture_lookdev.ps1 -View specular
$direct = .\tool\lookdev\capture_lookdev.ps1 -View direct
py -3 tool/validation/verify_lookdev.py $beauty --specular $specular --direct $direct `
  --output "<resourcePath>/Generated/Lookdev/validation.json"
```

脚本串行启动普通 VL 进程、采图后关闭自己启动的进程，不新增 runtime test。
验证脚本按固定相机投影采样，要求保留当前场景姿态和构图。
`-View linear` 可额外采集 linear-clamp 显示，仅作调试，不是 Beauty 基线。
`-View direct` 使用 `Generated/Lookdev/SC_lookdev_direct.json`，保持材质、
相机和显示条件不变，只用正面方向光替代 IBL，按 Lambert 公式独立校验。

测量空间是 **tone mapping 后、sRGB 编码前的显示线性值**：
验证器对 swapchain BMP 执行 inverse sRGB，精度受 8-bit 量化限制。
这不是浮点 SceneColor 回读，也不用于证明绝对照度或光谱准确性。

2026-10-10，1280x720 实测：

| 项目 | 结果 |
| --- | --- |
| 三块灰片显示线性值 | 均为 0.177888，目标 0.18，容差 0.006 |
| 三块灰片间差值 | 0 |
| 灰球、灰片及 24 色块间接镜面残留 | 0 |
| 镜面球中心间接镜面 | 0.128659，未被零高光开关关闭 |
| 方向光验证场景三块灰片 | 均为 0.181164，目标 0.18 |
| 背景显示线性值 | RGB 均为 0.039546，目标 0.04 |
| 六档灰阶 | 严格递减 |
| 连续渐变 | 单调，采样端点约 0.686685 / 0.010554 |

合成 HDR 首先按正面 Lambert 积分求得理论强度，再固定
`STUDIO_CALIBRATION_GAIN=0.967593067655343` 修正当前 SH9/cubemap 路径的
实测偏差。该修正属于本场景 HDR 资产，不改曝光公式或全局 IBL。
更换显示曲线、SH/prefilter 算法或 HDRI 后必须重新测量，不能沿用此结论。

构建成功，根 CTest 的 282 项模块测试全部通过。三次最终
Beauty/Specular/Direct 采集的日志无 Vulkan validation 报错。
较早的一次长时间交互采集中，窗口 extent 变成 0 后触发既有
swapchain 重建失败；该路径未在本次修改。不要最小化验证窗口，
也不要把本场景验收当作窗口最小化/恢复功能的验收。
