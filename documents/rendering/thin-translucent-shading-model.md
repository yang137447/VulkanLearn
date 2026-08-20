# Thin Translucent Shading Model

## 对齐目标

VulkanLearn 的灯罩材质使用 `SHADING_MODEL_THIN_TRANSLUCENT`，对齐目标是 Unreal
Engine 5.8 Classic/Legacy Material Shading Model 路径，而不是普通 AlphaBlend 的视觉近似。
参考基线固定为 UE 5.8 commit
`265a0946fedc02a002f77682b402b336b8208ac1`，Substrate 关闭：

- `Engine/Shaders/Private/ThinTranslucentCommon.ush`
- `Engine/Shaders/Private/ShadingModels.ush`
- `Engine/Shaders/Private/ShadingModelsMaterial.ush`
- `Engine/Shaders/Private/BRDF.ush`
- Legacy translucent base-pass blend-state setup

该合同只用于单层、无体积厚度几何的薄介质表面，例如汽车灯罩。反光碗、LED、灯泡和
其他发光/反射结构必须作为灯罩后方的独立几何存在，不能烘进灯罩 Surface 输出。

## Legacy 闭包

材质 Surface 的表面反射复用 VulkanLearn 当前 Default Lit 的 BRDF 和 IBL：直接光拆成
diffuse/specular 两个 lobes，以保留 UE Root Opacity 只覆盖顶部 diffuse/emissive、但不
覆盖表面 specular 的语义。这里复用的是 renderer 现有 Default Lit 数学，不宣称与 UE
Legacy 的反射 BRDF 完全相同：

```text
Surface = (Diffuse + Emissive) * RootOpacity + Specular
```

Default Lit 表面反射使用 renderer 现有的 `F0 = lerp(0.04, BaseColor, Metallic)`。
Thin Translucent 的 `Specular` 输入不参与这条表面反射路径，只继续用于下方的 UE Legacy
透射 Fresnel：

```text
TransmissionSpecularColor = lerp(0.08 * Specular, BaseColor, Metallic)
```

透射分支严格使用 UE 5.8 Legacy 的零次内部往返模型：

```text
NoV        = saturate(abs(dot(N, V)) + 1e-5)
Absorption = TransmittanceColor ^ (1 / NoV)
F          = F_Schlick(SpecularColor, NoV)
T_UE       = (1 - F)^2 * Absorption * (1 - RootOpacity)
```

`Roughness` 只影响表面直接/间接高光，不产生透射模糊。`Normal` 同时影响表面反射、
Fresnel 和 `1 / NoV` 吸收路径长度。`RootOpacity` 不是整个表面的 coverage；整个闭包的
覆盖率由独立 `SurfaceCoverage` 控制：

```text
Add = SurfaceCoverage * Surface
Mul = (1 - SurfaceCoverage) + SurfaceCoverage * T_UE
Framebuffer = Add + Mul * Destination
```

数值基准：当 `F0=0.04`、`TransmittanceColor=1`、`NoV=1`、`RootOpacity=0` 时，
Legacy 结果为 `0.9216`。完整非相干平板包含无限内部往返时为 `12/13 = 0.923076923...`；
两者不相等是有意行为，不能在实现里擅自补多次反射。

## Vulkan 输出与混合

`ThinTranslucent` 是专用 `RenderMode`，只允许绑定 `forwardTransparent` pass。该 pass
保持一个 `sceneColor` attachment；第二个 fragment 输出是同一 attachment 的 dual-source
index，不是第二张 MRT：

```glsl
layout(location = 0, index = 0) out vec4 ThinAdd;
layout(location = 0, index = 1) out vec4 ThinMul;
```

设备创建时查询并按支持情况启用 core feature `dualSrcBlend`。支持时管线使用：

| 项 | 设置 |
| --- | --- |
| Color Src Factor | `One` |
| Color Dst Factor | `Src1Color` |
| Color Blend Op | `Add` |
| Alpha Src Factor | `One` |
| Alpha Dst Factor | `Src1Alpha` |

Material shader identity包含 `VL_THIN_TRANSLUCENT_DUAL_SOURCE=1`，因此 native 与 fallback
SPIR-V、ABI 和 pipeline cache identity 不会混用。

## 平台回退

不支持 `dualSrcBlend` 的设备自动编译
`VL_THIN_TRANSLUCENT_DUAL_SOURCE=0`，并把彩色 `Mul` 压缩为标量 premultiplied alpha：

```text
FallbackAlpha = 1 - average(Mul.rgb)
Output.rgb    = Add.rgb
Blend         = One, OneMinusSrcAlpha
```

该回退保留平均透射强度，但必然丢失彩色 destination modulation，因此不能宣称与 UE
native dual-source 路径颜色等价。回退是显式平台降级，不会静默改成普通 straight alpha。

## 材质资产合同

源材质为 `shader/glsl/M_thinTranslucent.json`，Surface 入口为
`shader/glsl/materialFunction/mf_thinTranslucentSurface.glsl`：

| 参数 | 通道 | 语义 |
| --- | --- | --- |
| `u_baseColorOpacity` | RGB | 顶层 diffuse Base Color |
| `u_baseColorOpacity` | A | Root Opacity |
| `u_transmittanceColorCoverage` | RGB | 归一化厚度的 Transmittance Color |
| `u_transmittanceColorCoverage` | A | 整个薄层闭包的 Surface Coverage |
| `u_surfaceFactors` | X | Roughness，仅影响表面反射 |
| `u_surfaceFactors` | Y | Metallic |
| `u_surfaceFactors` | Z | Specular，默认 `0.5` 对应非金属 `F0=0.04` |
| `u_surfaceFactors` | W | Ambient Occlusion |
| `u_emissiveColorStrength` | RGB | 顶层 Emissive Color |
| `u_emissiveColorStrength` | A | Emissive Strength |

可选贴图宏为 `USE_BASE_COLOR_MAP`、`USE_NORMAL_MAP`、
`USE_TRANSMITTANCE_MAP` 和 `USE_EMISSION_MAP`。参数范围由 M_/MI_ 资产保证，shader 不做
额外逐像素 clamp；仅保留 UE 闭包定义本身的 `NoV` saturate。

汽车展示灯罩实例位于资源仓 `materials/car/MI_car_light_glass.json`，并与
`Car_02.blend` 的 `M_Glass_Light` 统一为 clean dielectric thin lens：

| Blender 输入/状态 | 当前值 | VulkanLearn Thin 映射 |
| --- | --- | --- |
| Base Color | `(0.90, 0.94, 0.98)` | `u_transmittanceColorCoverage.rgb` |
| Metallic | `0` | `u_surfaceFactors.y` |
| Roughness | `0.06` | `u_surfaceFactors.x` |
| IOR | `1.5` | Specular `0.5`，对应非金属 `F0=0.04` |
| Alpha | `1` | Surface Coverage `1` |
| Transmission Weight | `1` | Root Opacity `0`，使用 M_ 默认值 |
| Emission Strength | `0` | 使用 M_ 默认无自发光 |
| Backface Culling | `true` | 使用 M_ 默认 `cullMode=Back` |
| Transparent Overlap | `false` | 单层闭合代理不重复合成背面 |

Blender Principled Transmission 与 UE Legacy Thin Translucent 不是同一个闭包，因此这里
对齐的是资产输入语义和界面参数，而不是承诺逐像素一致。灯罩的冷色只进入
Transmittance Color；Base Color / Root Opacity 保持 M_ 默认零值，避免把 transmission tint
或 coverage 重复应用。历史的 Metallic `1`、Alpha `0.409090906` 双面 AlphaBlend 参数不再是
汽车 demo 的活动材质。

## 绘制顺序与阴影

所有透明 RenderMode 在 `forwardTransparent` pass 前按世界包围盒中心到相机的距离全局
后向前稳定排序。排序跨 Material/MaterialInstance 生效，不能为了减少 pipeline 切换恢复成
材质分组顺序。相同距离以原始 draw packet index 保持确定性。

Thin Translucent 默认不进入普通 Shadow Map。若未来需要彩色透射阴影，应新增独立的
transmission-shadow 合同，不能把当前不透明深度 ShadowCaster 当作等价实现。

## 验证入口

- `thin_translucent` CTest：验证 `0.9216` 基准、与完整平板的差异、Coverage 合成、
  fallback alpha 和 RenderMode 路由
- `cmake --build build -j`：验证 C++ 管线、设备能力和材质系统接口
- `build/bin/main.exe --framesmoke 2 --exit-after-tests`：加载汽车场景、生成参数 include、
  编译实际 Thin variant、创建 Vulkan pipeline 并完成帧提交
- 当前验证设备支持 `dualSrcBlend`，实际 variant identity 包含
  `VL_THIN_TRANSLUCENT_DUAL_SOURCE=1`

## 当前边界

以下能力不属于当前 renderer，不能宣称与 UE 整条透明渲染管线等价：

- 场景 fog / aerial perspective 尚未实现，因此当前没有 UE 对 Add 与 Mul 的 fog 修正
- 没有 refraction、屏幕空间扰动或 rough transmission blur
- 没有 colored translucent shadow、volumetric translucent shadow 或 Fourier opacity map
- 没有 per-pixel linked-list / OIT；相交透明几何仍受传统后向前排序限制
- 没有 UE Separate Translucency / After Motion Blur pass；当前唯一合法路由是
  `forwardTransparent`
- 没有 Substrate slab、多层介质或内部多次反射

这些能力应作为 fog、refraction、shadow 或透明合成系统演进，不应改变本页锁定的
UE 5.8 Legacy Thin Translucent 闭包公式。
