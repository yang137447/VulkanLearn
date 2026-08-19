# 车漆 Shading Model

## 对齐目标

VulkanLearn 的车漆使用 `SHADING_MODEL_CLEAR_COAT`，对齐目标是 Unreal Engine 5.8
Legacy Clear Coat，而不是只做视觉近似。参考基线为 UE 5.8 commit
`265a0946fedc02a002f77682b402b336b8208ac1`：

- `Engine/Shaders/Private/ShadingModels.ush`
- `Engine/Shaders/Private/ShadingModelsMaterial.ush`
- `Engine/Shaders/Private/ClearCoatCommon.ush`
- `Engine/Shaders/Private/ReflectionEnvironmentPixelShader.usf`
- `Engine/Shaders/Private/BRDF.ush`
- `Engine/Shaders/Private/OctahedralCommon.ush`

UE 5.8 已把 Legacy Clear Coat 标记为由 Substrate 取代，但该模型与当前固定
GBuffer 和 ShadingModelID 架构匹配，因此本项目先对齐 Legacy 合同。未来若引入
Substrate 式材质分层，应作为新的材质架构演进，不在此 ShadingModelID 上继续堆叠。

## 已对齐行为

车漆由底层材质和顶层无色清漆组成：

- 底层复用 `baseColor`、`roughness`、`metallic` 和底层法线
- 顶层固定 IOR 1.5、F0 0.04，并使用独立 Clear Coat Roughness 和顶层法线
- 直接光使用 UE 的 `D_GGX`、`Vis_SmithJointApprox` 和固定 Eta 折射点积近似
- 底层透射使用双程 Fresnel `(1 - F)^2` 和 `SimpleClearCoatTransmittance`
- 底层 Schlick 使用 UE 的掠射反射率规则 `saturate(50 * SpecularColor.g)`
- 间接光使用 UE Legacy Clear Coat 的 diffuse/specular color remap 和双层 IBL 合成
- forward 与 deferred lighting 使用同一套 Clear Coat 入口

当前路径保持 UE Legacy 默认的单次散射行为，没有额外启用 UE 可选的 GGX energy
conservation/preservation 路径。

## 材质输入

`shader/glsl/M_carPaint.json` 暴露与 UE Material 输入同义的参数：

| 输入 | 语义 |
| --- | --- |
| `u_clearCoat` | 常量 Clear Coat，未启用贴图时写入 CustomData.x |
| `u_clearCoatRoughness` | 常量 Clear Coat Roughness，未启用贴图时写入 CustomData.y |
| `clearCoatMap.r` | 启用 `USE_CLEAR_COAT_MAP` 后的逐像素 Clear Coat |
| `clearCoatMap.g` | 启用 `USE_CLEAR_COAT_MAP` 后的逐像素 Clear Coat Roughness |
| `normalMap` | 顶层清漆法线 |
| `clearCoatBottomNormalMap` | 启用 `USE_CLEAR_COAT_BOTTOM_NORMAL_MAP` 后的底层法线 |
| `u_clearCoatBottomNormalTiling` | Clear Coat 底层法线的 UV 重复次数 |
| `u_clearCoatBottomNormalStrength` | 底层法线向中性切线法线混合的强度 |

`clearCoatMap.rg` 启用后直接替代两个常量输入，不与常量相乘。参数和贴图数据应由
材质资产保证在有效范围内，shader 不做额外的逐像素范围修正；仅保留 UE 模型本身的
Clear Coat Roughness 最小值 0.02。

## GBuffer 合同

`SHADING_MODEL_CLEAR_COAT` 使用 `MaterialSurface.customData` / GBufferD：

| 通道 | 语义 |
| --- | --- |
| `x` | Clear Coat |
| `y` | Clear Coat Roughness |
| `w` | Bottom Normal 相对顶层法线的 oct 编码 X |
| `z` | Bottom Normal 相对顶层法线的 oct 编码 Y |

Bottom Normal 使用 UE 的 relative octahedral encoding：先分别编码顶层和底层世界法线，
再把差值缩放到剩余两个 CustomData 通道。没有底层法线贴图时，底层法线等于顶层法线，
编码结果是 UE 使用的中心值 `128 / 255`。

## 资产与反射

车展示实例位于资源仓 `materials/car/MI_car_carpaint.json`。实例必须保持：

- Clear Coat wrapper 固定写入 `customData.xy` 与 `GBUFFER_HAS_CUSTOM_DATA_MASK`，不再暴露恒为 1 的实例宏
- 按需启用 `USE_CLEAR_COAT_MAP` 和 `USE_CLEAR_COAT_BOTTOM_NORMAL_MAP`

Clear Coat 的命名参数入口由 `mf_clearCoatSurface.glsl` 固定选择，不作为 MI 宏暴露；
这样调参面板不会出现一个可关闭但关闭后会破坏参数合同的技术开关。

材质调参面板不能只读取 SPIR-V reflection。宏会让未使用参数或贴图从当前 variant 的
SPIR-V 中消失，因此面板应组合三类数据：

- `M_*.json` schema 提供完整参数、类型、默认值和贴图槽
- 运行时 `MaterialInstance` 提供当前值、宏值和纹理绑定
- SPIR-V reflection 只标记当前 pass/variant 的 active 子集

这样宏关闭时参数仍可见但可标记为 inactive，重新启用宏后不需要重新发现参数合同。

## Blender 车辆展示基线

车辆源文件 `Car_02.blend` 的 `M_Carpaint` 使用以下 Principled BSDF 输入：

- Base Color = `(0.0, 0.309913635, 1.0)`
- Metallic = `1.0`
- Roughness = `0.34090909`
- IOR = `1.5`
- Coat Weight = `1.5`
- Coat Roughness = `0.0`
- Coat IOR = `1.5`

VulkanLearn 的 Legacy Clear Coat 输入范围是 `[0, 1]`，因此 Coat Weight 在资产迁移时
饱和为 `1.0`。其余参数直接写入 `MI_car_carpaint.json`。

Blender 的微表面链路是 Object Coordinate -> 3D Smooth F1 Manhattan Voronoi，Scale
为 `1000`，Voronoi Color 经过 Strength `7.4 / 100 = 0.074` 的 Normal Map 节点后接到
Principled BSDF 的底层 `Normal`。它没有接到 `Coat Normal`，所以 VulkanLearn 必须把该
细节绑定到 `clearCoatBottomNormalMap`，顶层清漆法线保持光滑。

运行时使用 `tool/car/generate_carpaint_flake_normal.py` 生成确定性的 64 x 64 周期
Voronoi 法线。贴图包含 64 x 64 个随机微表面单元，材质默认 tiling 为 64，约等于每个
UV 方向 4096 个单元；默认 strength 为 `0.074`。这种方式比整车 2K UV 烘焙更省，mip
过滤也更稳定。它保留 Blender 的频率、随机微法线和强度语义，但由于 Blender 使用
Object-space 3D Voronoi，而运行时使用 tangent-space 2D 周期贴图，因此属于展示级近似，
不是逐像素复现。

源 `.blend` 不包含相机和灯光。车辆展示的灯光、环境和曝光不能声称来自该文件；判断
材质对齐时应区分材质参数差异与展示场景差异。

## 当前边界

以下能力不属于当前 renderer，不能宣称已与 UE 整条渲染管线等价：

- Rect Light / LTC 和 UE 的 area-light roughness compensation
- anisotropic Clear Coat 底层
- SSR、Lumen、reflection capture 混合和 specular occlusion
- UE 可选的 GGX energy conservation/preservation
- Substrate 多层闭包
- 独立金属片 BRDF / 闪烁能量模型（当前只有 normal-map 微表面近似）
- 橘皮、划痕、污渍、彩色清漆和角度相关色偏

这些项应作为灯光系统、反射系统或材质功能模块继续实现，不应新增车漆专用
ShadingModelID。
