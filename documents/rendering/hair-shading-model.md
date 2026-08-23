# Hair Shading Model 实现合同

## 状态

- **状态**：H0–H7 已落地；Hair ID 7 具备 Forward、Deferred、Card coverage、ShadowDepth、R/TT/TRT direct single scattering 和显式 IBL/MS fallback。
- **实现身份**：VulkanLearn MVP，不声称 UE 私有 shader 逐行 parity，也不把 Kajiya–Kay 当作 Reference。
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
和世界位置使用米制。`Tangent` 采用 rootward 方向，`tangent.w` 只保存 mirrored UV 后的
bitangent handedness（正值为正向，负值为翻转）；evaluator 不重复翻转 tangent。

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

### BaseColor 单一转换点

`M_hair` 在 Material Function 阶段执行一次：

```text
sigma_a = max(BaseColor.rgb * u_hairOptical.x, vec3(0.001))
```

Forward 的 `surface.hairAbsorption` 与 Hair GBuffer 的 `gbufferA.rgb` 都保存这份已经转换
的结果；Deferred decode 直接恢复它。R 路径不读取 `hairAbsorption`，TT/TRT 只在 evaluator
中各自应用一次，禁止再乘 `BaseColor`。

## 3. Longitudinal 与 Azimuthal

CPU reference 保留 `Phi_p(h)` 的全部有效 roots 和 `1/abs(dPhi/dh)` Jacobian。生产路径不在
每像素求根，而是采样 `hairAzimuthalLut`：

- `R/TT/TRT` 分别是 array layer `0/1/2`；
- `x` 是 `deltaPhi`，范围 `[-pi, pi]`，repeat；
- `y` 是 `thetaD` 与 roughness 的线性 atlas，thetaD clamp；
- `thetaH` 的 longitudinal lobe 在 shader 中直接求值；
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
gbufferC.b   = common roughness
gbufferD.r   = scatter
gbufferD.g   = backlit
gbufferD.b   = cuticleTilt
gbufferD.a   = multipleScatteringWeight
gbufferF.rgb = encoded world tangent
gbufferF.a   = tangent handedness
```

非 Hair 模型继续使用 `gbufferF.a = anisotropy`。Deferred V1 不偷偷复用通道保存独立 IOR、
fiber radius、melanin、path weights 或 LUT ID；decode 使用稳定 LUT 默认 IOR/radius 和统一
roughness mapping。

## 5. Coverage、Shadow 与 fallback

- `opacityMask` 决定 Alpha Clip；`coverage` 只调制 Hair 可见率，两者不当作 Beer–Lambert。
- `OpaqueClip` 的主 Pass 与 ShadowDepth 读取同一 Material Function，因此边缘覆盖一致。
- `TransparentAlphaBlend` 只作为 Hair lighting 探针，默认不投普通 Shadow Map；排序和远景
  coverage 限制在资产/Debug 中保持可见。
- directional 使用现有 CSM visibility；point/spot 使用各自 attenuation，当前没有额外阴影
  输入时只影响对应 direct light path。
- Hair IBL V1 只提供 R 的低阶方向 basis；TT/TRT IBL 和 MS 缺失时输出显式 fallback 状态。
- MS 预算不超过剩余单次散射能量、coverage 和 `multipleScatteringWeight`，关闭 MS 可回到
  direct single scattering。
- Strands/curves 只通过 `HairVisibilityInputs` 接口提供 fiber visibility、self-shadow、
  transmittance、LOD/CLOD，不新建 Shading Model。
- Kajiya–Kay fallback 是低端非 Reference 路径，Debug/日志中明确标识。

## 6. 参数隔离验收表

| 输入 | 应改变 | 禁止直接改变 |
| --- | --- | --- |
| `Tangent` | 高光沿 rootward 轴旋转/移动 | BaseColor、coverage |
| `Roughness` | longitudinal/azimuthal 峰宽 | path identity、shadow |
| `Specular` | R/TT/TRT 界面 lobe 总体强度 | Fresnel 本身、coverage |
| `Scatter` | MS/through-scatter 预算 | opacityMask、R 单次散射 |
| `Backlit` | TT/back-light body response | absorption、R |
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
