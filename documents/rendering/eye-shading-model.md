# Eye Shading Model

## 文档状态

- 状态：已实现当前计划范围（Forward、Deferred fallback、双壳、局部 SSS、Compute reload）
- 更新日期：2026-08-23
- 实现边界：当前已实现 ForwardOpaque 单壳、Deferred GBuffer fallback、双壳 inner/cornea pass、局部 SSS approximation、LOD/profile/gaze/pupil 合同和 Eye Compute/LUT 热重载；完整 screen-space tissue、path-traced eye 和 UE 私有 shader parity 仍不在承诺范围内。
- 相关计划：`documents/plan/rendering/eye-shading-model-development-plan.md`
- 参考知识库：`D:\YYBWorkSpace\GitHub\yyb-knowledge-book\src\content\docs\rendering\materials\shading-models\eye.mdx`

本文是当前代码应遵守的稳定合同。它描述 VulkanLearn 已经实现的事实，不把 UE 私有 shader 源码、完整 Digital Human 眼球路径或真实 screen-space tissue model 当作已完成能力。

## 1. 当前实现范围

当前实现闭合以下路径：

```text
M_eye / M_eyeInner / M_eyeCornea / M_eyeDeferred + MI
    -> EyeMaterialInputs
    -> ForwardOpaque 单壳或双壳
    -> Deferred GBuffer V1 fallback
    -> Cornea dielectric Fresnel + narrow GGX lobe
    -> View-side Snell refraction
    -> Virtual iris plane intersection
    -> Iris / pupil / limbus / sclera regions
    -> T_i * T_o inner direct and low-frequency inner IBL
    -> Compute-only caustic LUT gain/transmission
    -> Cornea/inner CSM shadow
    -> sssSource rejection/filter/compose（仅 tissue diffuse）
    -> Emissive add
```

- `MATERIAL_IS_EYE=1` 只出现在 Eye shader variant；普通 Forward shader 不声明 Eye 的 LUT binding，也不会引用 Eye evaluator。
- `M_eye` 使用 `ForwardOpaque`；`M_eyeInner` 和 `M_eyeCornea` 分别路由到双壳 pass；`M_eyeDeferred` 走 Geometry/Deferred fallback。
- Forward Eye 不写普通 Geometry GBuffer；Deferred Eye 使用独立版本化的 GBuffer V1 packing，不进入透明排序。
- `ForwardOpaque` 的普通不透明 Shadow 路由复用公共 Shadow pipeline；Eye 当前没有单独 Shadow shader。
- Legacy/Substrate 语义在 VulkanLearn 内部收敛到同一个 `EyeMaterialInputs` 和 `ShadeEyeSurface()` evaluator；`tool/eye-authoring-adapter/` 提供严格的 authoring JSON 迁移入口，但不是完整 UE 节点解析器。

## 2. EyeMaterialInputs 与 frame

`EyeMaterialInputs` 位于 `shader/glsl/engine/materialInputs.glsl`，当前字段为：

- `corneaNormal`：角膜外层反射法线。
- `corneaIor`：角膜介质 IOR，来自 Eye profile。
- `irisNormal`：虹膜内层细节法线；只影响 inner lighting。
- `irisPlaneNormal`：虚拟虹膜平面法线；参与折射交点和内层基准 frame。
- `irisMask`：虹膜区域权重，不等于 opacity 或 pupil mask。
- `irisDistance`、`irisRadius`：运行时米制几何参数。
- `irisColor`、`scleraColor`：内层颜色入口，可叠加材质纹理。
- `pupilRadius`、`limbusWidth`：瞳孔和角膜缘区域参数。
- `causticProfileId`、`causticStrength`：Compute LUT 选择和强度合同。
- `scleraProfileId`：巩膜局部 diffuse approximation 的 profile 开关。
- `eyeLayer`：单壳/inner/cornea 路由层标记。
- `contactVisibility`、`ciliaVisibility`：当前可见性近似的独立权重。
- `uvHandedness`、`pupilDilation`、`gazeDirection`/`gazeWeight`：左右眼、瞳孔和视线合同。

`MaterialSurface` 冻结 `texCoord`，并保留 world position、world normal、world tangent 和 tangent handedness。Eye 不复用同一个 normal 表示角膜、虹膜和虹膜平面三种事实。

所有长度在 profile loader 中转换到米。authoring 可使用 `meter`、`centimeter`、`millimeter` 或 `micrometer`，`worldUnitScale` 必须与单位一致；运行时 `unit` 固定为 `meter`。

## 3. View-side evaluator

`shader/glsl/engine/eyeLighting.glsl` 实现当前 evaluator：

1. 使用 `F0=((1-ior)/(1+ior))^2` 和 Schlick Fresnel 计算角膜反射/出射透射。
2. 用 `refract(-viewDirection, corneaNormal, 1/ior)` 得到从角膜进入眼内的传播方向。
3. 将射线与 `irisPlanePoint = surfacePosition - irisPlaneNormal * irisDistance` 求交。
4. 分母接近零、交点在错误方向或超出 `irisRadius` 时返回 invalid hit。
5. invalid hit 的 UV 保持 neutral，并且不会采样 iris color、iris normal、iris mask 或 caustic LUT。
6. 有效命中后计算 iris UV、pupil mask 和 limbus mask；`IrisMask=0` 时不产生 iris/pupil/caustic response。
7. 角膜 direct 使用共享 PBR lobe 的窄 GGX 形状，再按真实角膜 IOR 的 F0 缩放。
8. inner direct 使用独立的 `T_i`（light-side transmission）和 `T_o`（view-side transmission）：

   ```text
   L_iris = L_iris_direct * T_i * C * T_o * shadow_inner
   L_sclera = L_sclera_direct * T_i * T_o * shadow_inner
   L_cornea = L_cornea_specular * shadow_cornea
   ```

9. inner IBL 是明确标记的低频 diffuse approximation；角膜 IBL 使用现有环境/BRDF 路径，并乘以 `T_o`。
10. emissive 最后独立相加，不进入 transmission、shadow 或 caustic 账本。

当前巩膜不是完整 SSS LUT。若 MI 绑定有效 `SubsurfaceProfile`，Eye evaluator 只对巩膜 tissue diffuse 应用局部 wrap/backscatter approximation；角膜 specular 不进入该近似，也没有跨对象 blur。

## 4. Profile asset 合同

Eye profile 位于运行时资源根的 `eyeProfiles/*.json`，`type` 必须为 `eyeProfile`。首版字段包括：

- `schemaVersion=1`、`profileVersion=1`
- `profileId`：`1..15`；`0` 保留给 neutral profile，不允许作为 authored profile。
- `ior`
- `eyeRadius`、`corneaRadius`、`irisDistance`、`irisRadius`
- `pupilRadiusRange`、`pupilRadius`
- `limbusWidth`、`causticStrength`
- `distanceUnit`、`worldUnitScale`
- `causticLutVersion=1`、`kernelVersion=1`
- `sourceIdentity`

loader 会拒绝未知字段、版本不匹配、重复 `profileId`、不合法半径关系、超出 pupil range 的参数和不一致的单位缩放。profile 路径在 `EyeResourceSet` 中使用 lexical-normalized generic path 作为稳定身份。

Eye MI 通过 `eyeProfile` 引用 profile；`u_eyeProfileId`、`u_eyeCorneaIor` 和 `u_eyeCausticStrength` 由引擎从当前 World-local resource set 派生，不能信任旧 generation 的残留参数。

## 5. Compute-only Caustic LUT

生产 LUT 只由 `shader/glsl/generator/eyeCausticLut.comp` 生成。CPU 只提交 profile 参数、创建临时 Vulkan 资源、dispatch 和同步命令，不计算或上传生产 texel。

固定合同：

| 字段 | 值 |
| --- | --- |
| 纹理格式 | `RGBA16F` |
| 二维尺寸 | `64 x 64` |
| profile layers | `16` 个 profile ID 层（ID `0..15`） |
| elevation slices | 每个 profile `16` 层 |
| array layers | `256` |
| 纹理类型 | 2D array |
| radial domain | unit disk `[-1,1]`，有效域为半径不超过 1 |
| elevation domain | normalized front-light `[0,1]` |
| 通道 | `R=gain, G=transmission, B=coverage, A=jacobian` |

profile 0 和无效域输出 neutral/无贡献值；shader 只有在有效 iris hit、有效 profile ID 和 `IrisMask>0` 时采样 LUT。LUT metadata、source digest、Compute artifact generation key、kernel/version 一起构成资源身份。任何改变采样坐标、公式或版本的改动都必须提升版本并产生新的 digest。

当前 LUT 是首版稳定的归一化近似，不宣称已经完成真实 differential refraction 的高精度积分；GPU readback 已作为验证链路实现，当前各通道与 normalization error 合同为 `<= 0.01`。

## 6. RenderGraph、descriptor 与生命周期

`config/renderGraphConfig.json` 的 Forward 与 Deferred pass 为 Eye 提供公共 Set 3 输入。绑定合同为：

```text
forwardOpaque / forwardEyeInner / forwardEyeCornea:
  Set 3 / binding 2: eyeCausticLut (source=worldTexture)
  Set 3 / binding 1: hairAzimuthalLut (shared forward lighting input)
deferredLighting:
  Set 3 / binding 11: eyeCausticLut (source=worldTexture)
```

Deferred GBuffer V1 当前固定 9 个输出：`gbufferA`–`gbufferE`、`gbufferVelocity`、`gbufferF`、`sceneColorBase`、`sceneDepth`。其中 A/B/C 已按 UE Legacy 对齐为法线、材质参数、基础色/AO；Eye 的 IOR/caustic strength 使用 B.rg 扩展语义，iris color 使用 C.rgb，opacity 使用 `sceneColorBase.a`。Eye 的 profile/valid-hit/UV/normal/geometry 字段由独立 codec 编解码；`sssSource` 是独立 attachment，经过 rejection、水平/垂直 filter 和 composition 后才进入最终颜色。

`EyeResourceSet` 是一个 World generation 对应的不可变 package，包含 profile 列表、稳定 ID map、LUT metadata、source digest、Compute artifact generation key 和 LUT texture。资源加载顺序保证 Eye resource set 在 Eye MI 解析前完成。

World prepare/reload 行为：

- 同一 source digest 且旧 World 有有效 LUT 时复用 texture identity。
- digest、profile、shader artifact 或版本变化时，在 candidate World 中重新生成 LUT。
- candidate 构建失败时不发布 active World、RenderGraph、descriptor 或 LUT。
- 成功提交后旧 pipeline、descriptor 和 World-local texture 按现有 GPU epoch retirement 规则退休。
- steady-state frame 不执行 Eye LUT precompute。
- Eye Compute reload participant 在 ABI-compatible commit 时原子替换 pipeline、LUT、World-local package 和 descriptor route；ABI 不兼容会拒绝候选并保留当前 package。
- descriptor refresh 会先重建 pass input image-info snapshot，再写入所有 runtime descriptor set，避免旧 image view 在 epoch retirement 前重新写回。

## 7. M_eye 与 MI_eye

材质定义为 `shader/glsl/M_eye.json`、`M_eyeInner.json`、`M_eyeCornea.json` 和 `M_eyeDeferred.json`，分别使用对应的 vertex/surface evaluation。当前默认宏和参数由 M_ schema 拥有；MI 只应覆写确实需要变化的值，不能重复写与 M_ 默认相同的宏或参数。

典型 MI：

```json
{
  "type": "materialInstance",
  "material": "shader/glsl/M_eye.json",
  "eyeProfile": "eyeProfiles/EP_human_default.json",
  "subsurfaceProfile": "subsurfaceProfiles/SSP_skin.json",
  "textures": {
    "irisColorMap": "textures/T_eye_iris_checker.json",
    "scleraColorMap": "textures/T_eye_sclera_checker.json"
  }
}
```

Eye material 必须同时满足：

- `shadingModel = Eye`
- `renderMode` 为 `ForwardOpaque`、`ForwardEyeInner`、`ForwardEyeCornea` 或 `Opaque`，并与目标 pass 合同匹配
- `eyeProfile` 非空且属于当前 Eye resource set
- geometry 参数处于选定 profile 的有效域
- 不绑定 `skinLut`
- descriptor schema、reflection 和对应 active pass 完全兼容

当前 M_ schema 的 required Eye 参数共 14 个：`u_eyeSurface`、`u_eyeGeometry`、`u_eyeIrisColor`、`u_eyeScleraColor`、`u_eyeProfileId`、`u_eyeScleraProfileId`、`u_eyeCorneaIor`、`u_eyeCausticStrength`、`u_eyeLayer`、`u_eyeContactVisibility`、`u_eyeCiliaVisibility`、`u_eyeUvHandedness`、`u_eyePupilDilation`、`u_eyeGaze`。

## 8. Debug View

Eye debug mode 固定为 `42..63`，不可重排已有编号：

| 范围 | 内容 |
| --- | --- |
| 42–45 | Eye frame、Cornea Normal、Iris Normal、Iris Plane Normal |
| 46–48 | Cornea Fresnel、Cornea Specular、Refracted View Direction |
| 49–54 | Iris Hit Distance、Iris UV、Valid Iris Hit、Iris/Pupil/Limbus Mask |
| 55–60 | Light Transmission In、View Transmission Out、Iris Direct、Sclera Direct、Inner IBL、Caustic Gain |
| 61–63 | Inner Shadow、Cornea Shadow、Eye Profile |

shader、`UiSubsystem::GetDebugViewName()`、英文/中文 `ui/localization.json` 和 runtime validation 必须使用同一编号表。

## 9. 验证

纯 C++ 合同测试：

```powershell
build/bin/eye_tests.exe
build/bin/eye_authoring_adapter_tests.exe
ctest --test-dir build --output-on-failure
```

模块测试覆盖：

- Eye resource set、LUT metadata 和 World texture identity；
- `forwardOpaque` binding 2 与 Eye descriptor layout；
- Deferred binding 11、9-output GBuffer V1、`sssSource` rejection/filter/compose；
- 双壳 inner/cornea pass、layer/contact/cilia、gaze、pupil dilation 和 LOD/profile contract；
- Eye material 的 ForwardOpaque 路由和公共 Shadow route；
- 同 digest World reload 的 source/artifact identity 与 LUT texture identity 复用；
- 缺失 profile 的失败 candidate 不改变 active World；
- debug mode `42..63` 全部可达。
- ABI-compatible / ABI-incompatible / restore 的 Eye Compute reload、descriptor refresh 和 epoch retirement；
- Forward、Deferred、Dual-shell 各 3 个 steady-state sample frame 的 draw/descriptor/LUT sample budget。

运行时 probe 资产位于当前 `config/config.json -> resourcePath` 下：
`eyeProfiles/EP_human_default.json`、`materials/MI_eye_probe.json`、
`models/SM_eye_probe.json` 和 `scenes/SC_eye_probe.json`。这些资产属于运行时资源根，
不应复制到 `shader/spv/` 或作为 shader 生成物手工维护。

## 10. 性能预算

Eye LUT 固定为 `64 x 64 x 256 x RGBA16F`，内存为 `8,388,608` bytes（约 `8 MiB`）。当前 steady-state budget：

| 指标 | 上限 |
| --- | ---: |
| Eye draw | `4 / frame` |
| Eye descriptor bind | `8 / frame` |
| Eye LUT sample | `2 / draw` |

`eyeLutSampleCount` 是 draw-domain estimate，用于防止路由或 pass 数量意外膨胀；它不是 fragment invocation 数，也不是硬件纹理采样 counter。结构体保留 inner/cornea/deferred/SSS GPU 时间字段，但当前自动验收不把未接入的 timestamp 值伪装成正式 GPU 基线。LUT readback、Debug View 和 shader 编译不计入 steady-state 性能样本。

## 11. 明确非目标与扩展项

以下能力不属于本轮已完成计划，不能从当前实现范围推断出来：

- 完整 Sclera SSS profile LUT、真实 screen-space tissue blur 或 path-traced tissue model。
- 真实眼睑、睫毛、contact shadow 几何可见性求解。
- 高阶角膜内反射、房水体积和完整 differential caustic 积分。
- 完整 UE Legacy/Substrate 私有 shader/节点 parity 和生产资产迁移覆盖率。
- 目标硬件上的正式 GPU counter 性能基线；当前只有 CPU-side draw-domain budget。

新增这些能力时，应先扩展本合同和版本字段，再修改 shader、descriptor 或资源 loader；不得通过把未实现路径塞入 `customData`、普通 GBuffer 或每帧 fallback 来绕过合同。
