# Hair Shading Model 实现合同

## 状态

- **状态**：H0–H7 已落地；Hair ID 7 具备 Forward、Deferred、Card coverage、ShadowDepth、UE-compatible R/TT/TRT direct response、direct scatter 和显式 IBL/MS fallback。
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

NeoX Hair 的源字段名与此目标语义不同：源 shader 先计算
`p = cross(normalTS, float3(1,0,0))`，再在顶点切线/副切线/几何法线基底中生成 fiber axis；
该轴写入目标 `MaterialInputs.tangent`，几何法线写入 `MaterialInputs.normal`，不能直接沿用
普通 PBR 的 normal/tangent 接线。

NeoX 的 `u_backlit_intensity` 也不是可直接写入 Hair `Backlit` 的最终常量。源材质先以
几何法线的 `NoV`、RDI.R 的 root mask、`u_root_intensity` 和 AO 生成逐像素遮蔽，再把
结果交给 Hair 光照。当前 `b_f_3725` Hair 槽未覆写 `u_root_intensity`，因此 NeoX 的
Backlit 辅助输出保持为零；这不等于关闭主 TT/TRT 路径，主路径仍按源 Hair 公式消费
BaseColor 的透射颜色。
源实现还叠加了专用 `u_dir_direction` 项，VulkanLearn 当前没有对应作者方向输入；在该项
正式进入材质合同前不得用场景灯光方向写回 Shading Model。

## 2. Hair Material Inputs

公共 `baseColor/specular/roughness/tangent/AO/emissive/PDO` 仍由 `MaterialInputs` 提供。
Hair 专用数据由 `HairMaterialInputs` 持有：

| 字段 | 单位/范围 | 语义 |
| --- | --- | --- |
| `scatter` | `[0,1]` | through-scatter / inter-fiber scatter 预算；不改变 coverage |
| `backlit` | `[0,1]` | TT/back-light 方向项；不替代 Beer–Lambert |
| `cuticleTilt` | 弧度 | R/TT/TRT 的 longitudinal shift 基础量 |
| `longitudinalRoughness` | 弧度 | `M_p` 峰宽 |
| `azimuthalRoughness` | 无量纲 | LUT 方位平滑参数 |
| `ior` | 无量纲，`>1` | 纤维折射率；Deferred V1 固定 LUT IOR |
| `absorption` | `1/m`，RGB | 已完成唯一 BaseColor→absorption 转换的 `sigma_a` |
| `fiberRadius` | 米 | 内部路径长度换算 |
| `multipleScatteringWeight` | `[0,1]` | 后续 MS 的能量上限 |
| `coverage` | `[0,1]` | Card 可见率；与 absorption 完全分离 |
| `density` | `[0,1]` | 可见纤维密度；只进入 visibility/MS 预算 |
| `characterLighting` | `vec4` | `x/y/z/w` 分别为环境、方向、局部光倍率和无色相机虚拟光强度 |

### BaseColor 单一转换点

`M_hair` 在 Material Function 阶段执行一次：

```text
referencePathLength = 4 * fiberRadius
sigma_a = -log(clamp(BaseColor.rgb, 1/255, 1))
          * u_hairOptical.x / referencePathLength
```

Forward 的 `surface.hairAbsorption` 与 Hair GBuffer 的 `gbufferA.rgb` 都保存这份已经转换
的结果；Deferred decode 直接恢复它。UE direct evaluator 统一在四倍半径参考光程恢复：

```text
pathColor = exp(-sigma_a * 4 * fiberRadius)
```

随后按 UE 的 TT/TRT 指数使用该颜色；R 不读取 `hairAbsorption`。该转换必须保持颜色
单调性：BaseColor 越暗，`sigma_a` 越大；不能把颜色直接乘常数后冒充米制吸收系数。
MS fallback 使用相同 `pathColor`，禁止注入无色白光。

### NeoX MF 组合

`M_neoxHair.surface.glsl` 只负责把 MI 参数接入 MF。可复用功能拆为：

- `mf_neoxHairTextures.glsl`：采样 BaseColor/RDI/Normal，并恢复切线空间法线；
- `mf_neoxHairFiberFrame.glsl`：按 NeoX 的 `cross(normalTS, X)` 规则生成 fiber axis；
- `mf_neoxHairInputs.glsl`：组合颜色、coverage、AO、吸收和 `HairMaterialInputs`。

这些 MF 不读取灯光、不执行 `discard`，Alpha Clip 仍由 MeshPass Template 统一执行。

`u_hairCharacterLighting` 是 NeoX 分离角色光照在 Hair 材质侧的冻结合同。默认
`[1,1,1,0]` 不改变通用 Hair；`b_f_3725` 根据 event 1708 的实值使用
目标角色 `b_f_3725` 的 P5 固定机位校准值为 `[0.25,1,0.55,0.25]`；普通 `M_hair`
仍使用默认值。虚拟光以 `camera_vector` 同时作为 `L`，先按卡片几何法线
抑制背面漏光，再复用完整 Direct HairBxDF；它不是发色增益，也不能通过修改 BaseColor
替代。

## 3. UE Direct 与 Reference LUT

角色 Direct Hair 使用 UE 实时近似，而不是把 Reference 焦散 LUT 的幅值直接当成生产能量：

```text
Alpha = [-2 * cuticleTilt, cuticleTilt, 4 * cuticleTilt]
B     = 0.2 + roughness^2 * [1, 0.5, 2]
R     = Hair_G(...) * (0.25 * CosHalfPhi) * Hair_F(...) * (Specular * 2)
TT    = Hair_G(...) * exp(-3.65 * CosPhi - 3.98) * F_TT^2
        * pow(pathColor, ttPathExponent)
TRT   = Hair_G(...) * exp(17 * CosPhi - 16.78) * F_TRT^2 * F_internal
        * pow(pathColor, 0.8 / CosThetaD)
```

`F_TT`/`F_TRT` 使用源 shader 的 `0.046521/0.953479` 常量；`Backlit` 是 NeoX 的
逐像素辅助输出，不会被误当成 TT 主路径的开关。

`Scatter` 使用 UE 风格 soft Kajiya-Kay direct body response，并继续约束 MS fallback；它不改写
R/TT/TRT 的路径身份。Hair closure 不再额外乘 card 几何法线的 `NdotL/crossSection`，避免
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
- 运行时只接受 `resourcePath/hair/hairAzimuthalLut.json` 作者 metadata；
  `resourcePath/generated/hairAzimuthalLut.json` 仅是成功 World transaction 后的生成记录，
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
gbufferA.rgb = hairAbsorption (BaseColor→absorption 结果)
gbufferA.a   = surface opacity
gbufferB.a   = ShadingModelID 7 + SelectiveOutputMask
gbufferC.r   = character ambient multiplier
gbufferC.g   = Hair specular / R-TT-TRT interface energy scale
gbufferC.b   = common roughness
gbufferC.a   = material AO
gbufferD.r   = scatter
gbufferD.g   = backlit
gbufferD.b   = cuticleTilt
gbufferD.a   = multipleScatteringWeight
gbufferE.r   = precomputed shadow factor
gbufferE.g   = character directional multiplier
gbufferE.b   = character local-light multiplier
gbufferE.a   = achromatic Virtual Light intensity
gbufferF.rgb = encoded world tangent
gbufferF.a   = tangent handedness
```

Hair 必须显式恢复 `gbufferC.g` 的作者 specular 和 `gbufferC.r/gbufferE.gba` 的角色光照；
如果像普通旧模型一样固定 specular 为 `0.5`，
深色发丝的无色 R 路径会被放大成白色高光带。非 Hair 模型继续使用
`gbufferF.a = anisotropy`。Deferred V1 不偷偷复用通道保存独立 IOR、
fiber radius、melanin、path weights 或 LUT ID；decode 使用稳定 LUT 默认 IOR/radius 和统一
roughness mapping。

## 5. Coverage、Shadow 与 fallback

- `opacityMask` 决定 Alpha Clip；`coverage` 只调制 Hair 可见率，两者不当作 Beer–Lambert。
- `OpaqueClip` 的主 Pass 与 ShadowDepth 读取同一 Material Function，因此边缘覆盖一致。
- `TransparentAlphaBlend` 只作为 Hair lighting 探针，默认不投普通 Shadow Map；排序和远景
  coverage 限制在资产/Debug 中保持可见。
- directional 使用现有 CSM visibility；point/spot 使用各自 attenuation，当前没有额外阴影
  输入时只影响对应 direct light path。
- Hair specular IBL 在专用 UE Hair 环境卷积接入前保持为零并输出 fallback 状态；禁止复用普通 GGX reflection vector/mip 制造白色发片宽带。
- MS 预算不超过剩余单次散射能量、coverage 和 `multipleScatteringWeight`，关闭 MS 可回到
  direct single scattering。
- Strands/curves 只通过 `HairVisibilityInputs` 接口提供 fiber visibility、self-shadow、
  transmittance、LOD/CLOD，不新建 Shading Model。
- UE soft Kajiya-Kay 只承担 `Scatter` 的低频 body response；若未来增加 Kajiya-only 低端 fallback，必须与当前 R/TT/TRT evaluator 分开标识。

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
