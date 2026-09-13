# TwoSidedFoliage Shading Model

## 状态与范围

VulkanLearn 当前以 UE5.8 Legacy `TwoSidedBxDF` 的直接光 Transmission closure 为强制
基线，使用 Shading Model ID `6`。核心公式、固定常量、材质输入归属和 Transmission 分量
严格对齐 UE Legacy；当前版本覆盖 `Opaque` / `OpaqueClip`、Forward、Deferred 和自动
`ShadowDepth`。灯光存储、GBuffer 打包、CSM 与 IBL 仍遵守 VulkanLearn 自身的渲染合同，
不把平台专用优化或字节级 shader 复制当作对齐目标。

当前闭包使用固定 Legacy 公式，不额外维护 foliage 专用 GBuffer 版本号。

## Material Inputs

母材质入口为 `shader/glsl/M_twoSidedFoliage.json`，Material Function 为
`shader/glsl/materialFunction/mf_twoSidedFoliageInputs.glsl`。

| 输入 | 语义 |
| --- | --- |
| `BaseColor.rgb` | 叶片表面基础颜色，参与 Default Lit 基础响应 |
| `BaseColor.a` | 未绑定独立 Mask 时的覆盖率回退 |
| `opacityMaskMap.r` | 可选的独立叶片覆盖率，绑定后供 `Opacity Mask` 与 ShadowDepth 共用 |
| `u_subsurfaceColor` | 线性 RGB 的 Subsurface tint（`color` 参数的 RGB），只染 foliage Transmission lobe |
| `subsurfaceColorMap.rgb` | 可选的逐像素 Subsurface Color，通常使用叶片颜色贴图 |
| `u_pbrFactors.x` | Roughness；可由 `roughnessMap` 覆盖 |
| `u_pbrFactors.y` | Metallic |
| `u_pbrFactors.z` | Ambient Occlusion |
| `u_specular` | 介电高光强度，参与 `0.08 * Specular` 的 F0 约定 |

`albedoMap`、`normalMap`、`roughnessMap`、`subsurfaceColorMap` 和 `opacityMaskMap` 由宏显式启用。
未绑定 `opacityMaskMap` 时才回退到
`BaseColor.a`；资产参数负责提供有效范围，shader 不增加额外的逐像素防御性 clamp。

## Render State

`TwoSidedFoliage` 必须使用：

```text
renderMode = Opaque 或 OpaqueClip
cullMode   = None
```

`OpaqueClip` 的主 Pass 和 `ShadowDepth` 使用同一份 `opacityMask` 与 Alpha Clip threshold。
双面状态贯穿 Base 与 ShadowDepth；不把非法单面配置静默改写为双面，也不回退到
`DefaultLit`。

## 法线职责

`worldNormal` 是 MaterialInputs 解析后的最终着色法线，允许 Normal Map，并按双面规则翻到
当前着色半球。BRDF、IBL、Fresnel、foliage base lighting、foliage backlight 和 CSM receiver
bias 都使用这份法线。CSM 不在 foliage evaluator 中引入专用几何法线或额外 bias 语义。

`gl_FrontFacing` 只在 Material Function 阶段即时决定是否翻转 `inputs.normal`，不写入
`MaterialInputs`、`MaterialSurface` 或 GBuffer。Eye 的 `corneaNormal`、`innerNormal` 等专用
交点法线继续由 Eye evaluator 自己管理。Forward 与 Deferred 遵守同一份法线职责。

## 光照闭包

Foliage 使用 Default Lit 基础响应叠加独立背光 lobe：

```text
frontBase = DefaultLit(BaseColor, Roughness, Metallic, Specular, worldNormal, V, L)
backLight = Radiance * visibility * WrapNoL * Scatter
             * subsurfaceColor
Foliage   = frontBase + backLight
```

其中 `Scatter` 使用 UE Legacy 固定 `roughness=0.6` 的 GGX `D` 项，`D` 内部已包含
`1/PI`；背光输出属于 UE 的 `Transmission`，不是 Lambert 漫反射，也不额外乘 `opacityMask`。

其中：

```text
WrapNoL = saturate((-dot(worldNormal, L) + 0.5) / (1 + 0.5)^2)
Scatter = D_GGX(0.6^2, saturate(-dot(V, L)))
```

Directional、point 和 spot light 都使用同一 UE 对齐的 transmission evaluator；背光颜色不
改写 BaseColor，也不额外引入厚度、BSSRDF、LUT 或透明透光路径。Alpha 只负责 `OpaqueClip`
的几何裁剪。

AO 只沿用现有基础/间接光照合同；emissive 独立于 foliage backlight。CSM visibility
在 Forward/Deferred 的 foliage 输出中统一乘到显式光照和背光项。

## GBuffer 合同

普通 GBuffer 通道保持现有模型语义；ID `6` 的特殊字段如下：

```text
GBufferA.rgb      = worldNormal
GBufferB.r/g/b    = metallic / specular / roughness
GBufferB.a        = packed ShadingModelID + selective flags
GBufferC.rgb/a    = baseColor / ambientOcclusion
GBufferD.rgb      = subsurfaceColor
GBufferD.a        = 0.0（保留槽，不承载 foliage 作者参数）
GBufferF.rgb      = encoded worldTangent
GBufferF.a        = worldTangent.w
```

`GBufferF` 对 foliage 不承载额外法线；Deferred 只恢复普通的 tangent 快照，foliage
lighting 不消费其 tangent。`GBufferD` 仅在 `GBUFFER_HAS_CUSTOM_DATA_MASK` 有效时消费，否则
`subsurfaceColor` 回退为零，固定 `Wrap=0.5` 不依赖 GBuffer 数据。

## Forward / Deferred / ShadowDepth

- Forward 和 Deferred 都显式分发到 `ShadeTwoSidedFoliageSurface`，不进入 DefaultLit 的
  fallback 分支。
- 两条光照路径共享同一 foliage evaluator、`opacityMask` coverage 和 `worldNormal` 着色语义；
  未绑定独立 Mask 时才使用 BaseColor A 回退。
- Forward/Deferred 的 CSM receiver bias 都传入 `worldNormal`，不引入 foliage 专用 bias。
- ShadowDepth 复用母材质的 `opacityMask` 输入；阴影 caster 的覆盖率、WPO、wind 和 discard
  条件必须与 Base Pass 一致。当前 foliage 母材质未启用 WPO/PDO。
- 当前不承诺半透明阴影、透光 Shadow Map、OIT、厚度贴图或 Ray Tracing any-hit。

## Debug 与验证

foliage 调试数据包含：背光颜色、背光贡献、背光因子、shadow visibility、opacity coverage
和 custom-data 有效性。调试输出不维护 foliage 专用版本号或 front-facing GBuffer 快照。

最小验证场景为 `SC_foliage_potted_plant_02`：左侧普通 DefaultLit PBR 植物，右侧
TwoSidedFoliage 植物，环境光强度 `0.1`，并在世界原点放置坐标轴。验收至少包含：

- 正面与背面观察；
- 正面光、背光和掠射光；
- Alpha Clip 边缘与 ShadowDepth 边缘一致；
- `subsurfaceColor = 0` 的无背光边界；
- Forward/Deferred 结果和 CSM shadow visibility 可解释；
- `debugview 95` 检查 foliage shadow visibility，不用额外补光掩盖链路问题。

## 有意差异与后续路线

当前实现严格对齐 Legacy 直接光 Transmission closure；真实叶片厚度、BSSRDF、多次散射、
环境透光、透明排序、OIT、SpeedTree 专用风场数据和 Substrate/Strata foliage 不属于该
Legacy closure 的当前范围。后续若加入厚度、transmission weight、profile 或环境透光，
必须建立独立版本，不能修改 Legacy 的固定公式与常量，并同步扩展 MaterialInputs、GBuffer
合同、Forward/Deferred 共用 evaluator 和验证矩阵。
