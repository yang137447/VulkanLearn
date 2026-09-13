# Hair Shading Model 实现合同

## 状态

- **状态**：H0–H7 已落地；Hair ID 7 具备 Forward、Deferred、Card alpha clip、ShadowDepth、UE Legacy R/TT/TRT direct response、direct Kajiya-Kay scatter，以及明确的 IBL/MS fallback。
- **实现身份**：角色生产路径对齐 UE 实时 Hair 近似；CPU 求根与 Hair LUT 属于 Reference/Debug，不再直接驱动角色光照能量。
- **生产边界**：Hair Card 的 masked 路径可进入 Geometry/Deferred；透明探针仍受排序、远景 coverage 和普通 Shadow Map 限制。

## 1. 路径身份与单位

R、TT、TRT 在 CPU reference、Compute LUT、GLSL evaluator 和 Debug View 中使用同一身份：

| Path | 含义 | 颜色来源 |
| --- | --- | --- |
| `R` | 外表面直接反射，不进入纤维内部 | 光源颜色与界面 Fresnel；不乘 Hair absorption |
| `TT` | 折射进入、穿过纤维、折射离开 | `exp(-sigma_a * pathLength)` 一次 |
| `TRT` | 折射进入、一次内部反射、折射离开 | 内部 Fresnel 与 `exp(-sigma_a * pathLength)` 一次 |

所有方向均从交点指向外部。以单位 tangent `t` 为纵轴：

```text
sin(theta) = dot(omega, t)
phi        = atan2(dot(omega, b), dot(omega, n))
thetaH     = (thetaI + thetaO) / 2
thetaD     = (thetaO - thetaI) / 2
deltaPhi   = wrap(phiO - phiI, [-pi, pi])
```

`theta = 0` 表示方向位于纤维法截面；`sigma_a` 的单位是 `1/m`；fiber radius、path length
和世界位置使用米制。普通 Hair 输入的 `Tangent` 采用 rootward 方向，`tangent.w` 只保存
mirrored UV 后的 bitangent handedness（正值为正向，负值为翻转）。NeoX Hair 的 MF 会把
mirrored 符号提前烘入 fiber axis，同时保留原始 `tangent.w` 供横截面基底重建；evaluator
不再用该符号二次翻转 fiber axis。

Demo 的普通 glTF Hair Card 通过 HAIR_STANDARD_CARD=1 显式选择 UE-facing 适配：导入的卡片法线作为 geometric normal，glTF tangent 作为卡片宽度轴，cross(N,T) 先恢复 root-to-tip 轴，再反向为 UE HairShading 所需的 rootward fiber tangent。该宏只用于标准 Hair Card demo，旧的 NeoX 角色资产继续走其专用 frame 语义。Core 使用 OpaqueClip，Fringe 使用 TransparentAlphaBlend 加 alpha 区间裁剪 [fringeMin, fringeMax)；两者共享同一母材质与光照 evaluator，但不重复覆盖同一段卡片 coverage。

NeoX Hair 的源字段名与此目标语义不同：源 shader 先计算
`p = cross(normalTS, float3(1,0,0))`，再在顶点切线/副切线/几何法线基底中生成 fiber axis；
该轴写入目标 `MaterialInputs.tangent`，几何法线写入 `MaterialInputs.normal`，不能直接沿用
普通 PBR 的 normal/tangent 接线。

### 6125 UE Hair Card 资产合同

`SC_hair_showcase_6125` 使用用户提供的 `6125 日韩风二次元女生插片头发3D模型`，其中
`Hairstyle_5` 的 UE 静态网格绑定 `MI_Hair11`，父材质为 `Resourses/M_Hair`。从 UE4.19
资产反射得到的生产链路是 `MSM_Hair + BLEND_Masked + TwoSided`，而不是普通透明 PBR：

- `fiber_albedo` 只提供 RGB 主色；`fiber_alpha` 单独提供 Opacity Mask，UE 的 Masked clip
  由材质实例/母材质控制。
- `fiber_direction` 提供 Hair Card 的根到梢方向；`fiber_root_V2` 提供 root mask；
  `fiber_depth` 只用于 Pixel Depth Offset；`fiber_ID` 只用于可选染色区域。
- `MI_Hair11` 的资产中虽然存在 `RootColor`、`TipColor`、`TipPower=1` 参数，但 UE5.8
  Legacy Hair Card 的生产 `HairShading` 不消费这组 Root/Tip gradient；Hair Card 的
  BaseColor 直接来自材质 BaseColor/卡片颜色通道。VulkanLearn 不把这组扩展接口暴露给
  `M_hair`，避免在光照前把 BaseColor 额外乘成近黑。
- `MI_Hair11` 的有效贴图集合为 `fiber_albedo/alpha/depth/direction/ID/normal/root_V2`；
  直接解析 `MA_HairStyle.uasset` 确认 `fiber_normal` 由
  `MaterialExpressionTextureSampleParameter2D` 以 `SamplerType=SAMPLERTYPE_Normal` 采样，
  是 Normal 法线输入（着色模型 `MDR_ColorNormalRoughness`）。`fiber_translucency`、
  `fiber_ambient` 虽存在于外部贴图包，但未被该 UE 母材质 import，不属于核心输入。
- VulkanLearn 的 `MI_6125_hair_core` 保留 UE-facing `OpaqueClip`；`MI_6125_hair_fringe`
  仅作为同一张卡片的 TransparentAlphaBlend 对比，不替代 UE 的 Masked 主路径。

该资产的 FBX 有两个源材质槽 `lambert1/lambert2`，两个槽都映射到同一套 Hair MI；不能把
网格描述简化成单槽，否则 MeshAssetValidator 会在加载阶段拒绝资源。

NeoX 的 `u_backlit_intensity` 也不是可直接写入 Hair `Backlit` 的最终常量。源材质先以
几何法线的 `NoV`、RDI.R 的 root mask、`u_root_intensity` 和 AO 生成逐像素遮蔽，再把
结果交给 Hair 光照。当前 `b_f_3725` Hair 槽未覆写 `u_root_intensity`，因此 NeoX 的
Backlit 辅助输出保持为零；这不等于关闭主 TT/TRT 路径，主路径仍按源 Hair 公式消费
BaseColor 的透射颜色。
源实现还叠加了专用 `u_dir_direction` 项，VulkanLearn 当前没有对应作者方向输入；在该项
正式进入材质合同前不得用场景灯光方向写回 Shading Model。

## 2. Hair Material Inputs

公共 `baseColor/metallic/specular/roughness/tangent/AO/emissive/PDO` 仍由
`MaterialInputs` 提供。Hair 专用数据只保留 `backlit` 与 debug 用的
`absorption`：

| 字段 | 单位/范围 | 语义 |
| --- | --- | --- |
| `backlit` | `[0,1]` | 对应 UE `CustomData.z`；Legacy Card 按 UE `bUseBacklit=false` 忽略，显式启用 Backlit 的 strands/扩展变体才消费 |
| `absorption` | `1/m`，RGB | 已完成唯一 BaseColor→absorption 转换的 `sigma_a` |

`M_hair.json` 的生产接口只有 `u_pbrFactors = (Roughness, hairScatter,
AmbientOcclusion, Specular)` 与 `u_hairBacklit`；其中 `hairScatter` 复用
`MaterialInputs.metallic` 的 ABI 槽位，但在 Hair 语义中不是金属材质切换，而是
UE Legacy 的 Kajiya-Kay 多重散射控制。固定 `IOR=1.55`、`Shift=0.035`、
`fiberRadius=0.00005m` 不允许被 MI 覆写。
`hairScatter` 保留 UE Legacy 参数的有效范围 `[0,2]`，因此不应按普通 Metallic
参数强制限制到 `[0,1]`；例如 6125 Hair Card 的 UE 参数为 `Scatter=1.20742095`。

### BaseColor 单一转换点

`M_hair` 在 Material Function 阶段执行一次：

```text
referencePathLength = 4 * fiberRadius
sigma_a = -log(clamp(BaseColor.rgb, 1/255, 1))
          / referencePathLength
```

Forward 的 `surface.hairAbsorption` 保存转换后的 `sigma_a`；Hair GBuffer 的
`gbufferC.rgb` 保存源 BaseColor，Deferred decode 按固定四倍半径参考光程重建 `sigma_a`。
UE direct evaluator 统一在四倍半径参考光程恢复：

```text
pathColor = exp(-sigma_a * 4 * fiberRadius)
```

随后按 UE 的 TT/TRT 指数使用该颜色；R 不读取 `hairAbsorption`。该转换必须保持颜色
单调性：BaseColor 越暗，`sigma_a` 越大；不能把颜色直接乘常数后冒充米制吸收系数。
未接入专用 Hair IBL/MS 时只输出 fallback，禁止用普通 SH/GGX 或无色白光冒充 Hair 环境能量。

### NeoX MF 组合

`M_neoxHair.surface.glsl` 只负责把 MI 参数接入 MF。可复用功能拆为：

- `mf_neoxHairTextures.glsl`：采样 BaseColor/RDI/Normal，并恢复切线空间法线；
- `mf_neoxHairFiberFrame.glsl`：按 NeoX 的 `cross(normalTS, X)` 规则生成 fiber axis；
- `mf_neoxHairInputs.glsl`：组合颜色、coverage、AO、吸收和 `HairMaterialInputs`。

这些 MF 不读取灯光、不执行 `discard`，Alpha Clip 仍由 MeshPass Template 统一执行。

Hair Card 不注入角色专用光照倍率或 Virtual Light；directional 只使用 CSM，
point/spot 只使用自身衰减，避免把工程补光误当成 UE HairBxDF 输入。

## 3. UE Direct 与 Reference LUT

角色 Direct Hair 使用 UE 实时近似，而不是把 Reference 焦散 LUT 的幅值直接当成生产能量：

```text
Alpha = [-0.07, 0.035, 0.14]
B     = roughness^2 * [1, 0.5, 2]  // Legacy HairBxDF 的 Area=0
R     = Hair_G(...) * (0.25 * CosHalfPhi) * Hair_F(...) * (Specular * 2)
TT    = Hair_G(...) * exp(-3.65 * CosPhi - 3.98) * F_TT^2
        * pow(pathColor, ttPathExponent)
TRT   = Hair_G(...) * exp(17 * CosPhi - 16.78) * F_TRT^2 * F_internal
        * pow(pathColor, 0.8 / CosThetaD)
```

`F_TT`/`F_TRT` 使用源 shader 的 `0.046521/0.953479` 常量；`Backlit` 只有在
显式启用 `HAIR_USE_BACKLIT` 的 strands 变体中参与方向项，Legacy Card 的
`activeBacklit` 固定为 1。

`Metallic` 使用 UE 风格 soft Kajiya-Kay direct body response；它不改写 R/TT/TRT 的路径身份。
Hair closure 不再额外乘 card 几何法线的 `NdotL/crossSection`，避免
发片表面法线把圆柱纤维响应重新压成普通表面高光。

CPU reference 仍保留 `Phi_p(h)` 的全部有效 roots 和 `1/abs(dPhi/dh)` Jacobian。
`hairAzimuthalLut` 继续由运行时事务管理并服务 Debug/Reference 对照：

- `R/TT/TRT` 分别是 array layer `0/1/2`；
- `x` 是 `deltaPhi`，范围 `[-pi, pi]`，repeat；
- `y` 是 `thetaD` 与 roughness 的线性 atlas，thetaD clamp；
- `thetaH` 的 longitudinal lobe 在 shader 中直接求值；
- kernel v2 直接数值求解与 CPU oracle 相同的 `Phi_p(h)` roots，并写入
  `sum(interfaceWeight / abs(dPhi/dh))`；禁止用经验 phase 高斯或倒置 Jacobian 替代；
- LUT metadata 的 `schemaVersion/lutVersion/kernelVersion/IOR/coordinate/wrap/pathConvention`
  必须与 shader 合同一致，不接受未知版本。
- 运行时只接受 `resourcePath/Common/Profiles/Hair/hairAzimuthalLut.json` 作者 metadata；
  `resourcePath/Generated/Runtime/hairAzimuthalLut.json` 仅是成功 World transaction 后的生成记录，
  不能作为下一次加载的输入，也不能替代作者资产。

当前资源合同固定为 `128x512x3 RGBA16F`，roughness 8 slice、thetaD 64 sample。生产 texel
只能由 `shader/glsl/generator/hairAzimuthalLut.comp` 生成；CPU 只冻结输入、dispatch、barrier、
可选 readback 和 transaction-scoped metadata commit。

## 4. Forward / Deferred / GBuffer

Hair evaluator 只有一份，Forward/Deferred 只提供输入与输出壳。Forward 使用 `forwardTransparent`
作为透明探针；生产 Card 使用 `OpaqueClip`，主 Pass 与 ShadowDepth 共享 `EvaluateMaterialInputs`
和 `opacityMask`。

Hair GBuffer V1：

```text
gbufferA.rgb = encoded world normal，gbufferA.a = PerObjectGBufferData（当前为 0）
gbufferB.r   = Metallic / Kajiya-Kay scatter control
gbufferB.g   = Hair Specular / R interface scale
gbufferB.b   = common roughness
gbufferB.a   = ShadingModelID 7 + SelectiveOutputMask
gbufferC.rgb = source BaseColor
gbufferC.a   = material AO
gbufferD.rg  = UE Legacy Hair WorldNormal 八面体编码
gbufferD.b   = Backlit
gbufferD.a   = reserved
gbufferE     = precomputed shadow factors
gbufferF.rgb = encoded world tangent
gbufferF.a   = tangent handedness
sceneColorBase.a = surface opacity
```

Hair 必须显式恢复 `gbufferB.r/g/b` 的 Metallic/Specular/Roughness；不能固定
specular 为 `0.5`，否则深色发丝的 R 路径会被放大成白色高光带。非 Hair 模型继续
使用 `gbufferF.a = anisotropy`。Deferred V1 不复用通道保存独立 IOR、fiber radius、
melanin、path weights 或 LUT ID。

## 5. Coverage、Shadow 与 fallback

- `opacityMask` 决定 Alpha Clip；Hair Card 不再额外维护一份 coverage 参数。
- `OpaqueClip` 的主 Pass 与 ShadowDepth 读取同一 Material Function，因此边缘覆盖一致。
- `TransparentAlphaBlend` 只作为 Hair lighting 探针，默认不投普通 Shadow Map；排序和远景
  coverage 限制在资产/Debug 中保持可见。
- directional 使用现有 CSM visibility；point/spot 使用各自 attenuation，当前没有额外阴影
  输入时只影响对应 direct light path。
- Hair specular IBL 在专用 UE Hair 环境卷积接入前保持为零并输出 fallback 状态；禁止复用普通 GGX reflection vector/mip 制造白色发片宽带。
- Legacy Card 的 direct scatter 由标准 `Metallic` 驱动；专用 Hair IBL/MS 尚未接入时
  显式输出 fallback，不用普通 SH/GGX 伪造 Hair 环境能量。
- Strands/curves 未来只通过 `Backlit` 与 visibility backend 接入，不新建 Shading Model。

## 6. 参数隔离验收表

| 输入 | 应改变 | 禁止直接改变 |
| --- | --- | --- |
| `Tangent` | 高光沿 rootward 轴旋转/移动 | BaseColor、coverage |
| `Roughness` | longitudinal/azimuthal 峰宽 | path identity、shadow |
| `Specular` | UE primary/R 界面 lobe 强度 | Fresnel 本身、coverage、TT/TRT path tint |
| `Scatter` | UE soft Kajiya-Kay direct body response 与 MS 预算 | opacityMask、R/TT/TRT 路径身份 |
| `Backlit` | NeoX root/depth/AO 产生的辅助背光量 | 直接改写 TT 主路径、absorption、path tint |
| `BaseColor` | 一次 absorption 转换后的 TT/TRT 颜色 | R 颜色 |

## 7. Debug View 编号

保留现有 `1–17`；SSS 占用 `18–20`，Hair 占用 `21–41`：

| Mode | 名称 |
| ---: | --- |
| 18 | `DiffuseAfterSSS` |
| 19 | `SSSPixelRadius` |
| 20 | `SSSValidWeight` |
| 21 | `HairWorldTangent` |
| 22 | `HairRootwardTangent` |
| 23 | `HairThetaI/O` |
| 24 | `HairDeltaPhi` |
| 25 | `HairR` |
| 26 | `HairTT` |
| 27 | `HairTRT` |
| 28 | `HairPathLength` |
| 29 | `HairAbsorption` |
| 30 | `HairCoverage` |
| 31 | `HairShadowTransmittance` |
| 32 | `HairLUTCoordinates` |
| 33 | `HairPrimaryHighlight` |
| 34 | `HairSecondaryHighlight` |
| 35 | `HairScatter` |
| 36 | `HairBacklit` |
| 37 | `HairRPathColor` |
| 38 | `HairTTPathColor` |
| 39 | `HairTRTPathColor` |
| 40 | `HairIBLFallback` |
| 41 | `HairMultipleScatteringFallback` |

## 8. 生命周期与资产校验

Hair LUT 属于当前 World-local resource package。Compute pipeline、descriptor、image、sampler 和
Texture 由 candidate loader 创建；资源通过已有 World/Graph transaction 发布，旧 generation
按 GPU epoch retirement。Shader worker 不访问 Vulkan/live Material。生成 metadata 在 candidate
阶段只进入 pending 文件列表，和 shader/generated include 一起原子发布；candidate 或后续
prepare 失败不会修改 `generated/`。

缺失、版本不匹配、尺寸/通道错误、角度 convention 不一致和非法 IOR 必须在资产/加载阶段
失败；不得在每像素用 `clamp` 或无条件环境补光掩盖错误。生成文件只写到
`config/config.json -> resourcePath -> generated/`。

## 9. 当前未承诺内容

- 完整 TT/TRT Hair IBL；
- 生产级 multiple-scattering LUT；
- 深度不透明图、完整 OIT、ray-traced any-hit；
- strands/curves 的具体 visibility backend；
- UE 私有 shader 常数或逐行 parity。
