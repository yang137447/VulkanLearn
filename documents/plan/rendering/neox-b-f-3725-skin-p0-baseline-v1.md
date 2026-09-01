# `b_f_3725` Skin P0 基线审计 V1

## 状态

- **阶段**：P0 源与目标基线审计
- **审计日期**：2026-08-25
- **结果**：源槽位、目标资产、关键纹理通道和当前 Shader 身份已锁定；运行时 Beauty/Debug 截图尚未作为本轮证据提交。
- **稳定规则**：`documents/rendering/neox-skin-effect-alignment-contract-v1.md`
- **执行计划**：`documents/plan/rendering/neox-b-f-3725-skin-effect-alignment-plan.md`

## 1. 审计范围与指纹

### 1.1 源 Shader

| 文件 | SHA-256 |
|---|---|
| `K:\future\res\shader\pbr_skin.fx` | `E6E265D519134B3A1D27C19F4480CB34869A665BF3A09836F9A036E83922F28A` |
| `K:\future\res\shader\pbr_skin.nfx2` | `C1830ED5A59B6E7FDCB3F231B6CC769C4D71FB5CED3F5DA2B434203157C0FFFB` |
| `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\pbr\pbr_skin.nsf` | `E970B99451DECFE9C7A0C33C617471726E13E0F04C428C8818FF5B315E1CB006` |
| `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\pbr\nodes\pbr_skin_nodes.hlsl` | `37EB97EC7ABDE5A413B1106D0309798CE00EF129580D7D8DE6B42549A3AB48A4` |
| `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\pbr\nodes\pbr_skin_parameters.hlsl` | `E4E4DE40EB1E16CEA8C53CFA2B10F2B34509549D77DE337D7544955502183496` |
| `C:\Software\WorkTemp\G66ShaderDevelop\shader-source\pbr\nodes\skin_functions.hlsl` | `30F13A9CC20D8A801BDC97B067D09FEE5DAD9AF74C57F02CA7EE33FDC5084413` |

源 `.fx/.nfx2` 用于宏、材质默认和最终槽位事实；`shader-source` 用于节点执行顺序和迁移语义。两者发生冲突时必须重新核验，不能按文件名猜测。
### 1.3 NeoX 源码根路径与视觉审计机位

- NeoX shader 源码根目录固定为 `C:\Software\WorkTemp\G66ShaderDevelop\shader-source`。
- 皮肤对齐必须结合该目录下的 `.nsf`、`.hlsl`、节点函数和 sampler 语义核对；不能只根据 RenderDoc 反编译结果或目标 GLSL 猜测。
- `b_f_3725` 的脸部视觉验收以后以侧面/三分之二侧面为主机位。该机位优先暴露脸部覆盖、透明/深度、发际线遮挡、法线方向和脸部材质边界；正面截图仅作辅助对照。

### 1.2 目标运行资产

| 文件 | SHA-256 |
|---|---|
| `D:\YYBWorkSpace\GitHub\VukanLearnResources\scenes\SC_b_f_3725_p0.json` | `5B9B44F524E0BB5C2AF25788B6650D067FF6F99EC118F7E6FCF1FA68BFCDB16F` |
| `D:\YYBWorkSpace\GitHub\VukanLearnResources\models\neox\b_f_3725\SM_b_f_3725_p0.json` | `F2C84371B9D00398C2BBF8453D65852B62B7EE002A90E650EBD1A6F59FE2BF01` |
| `D:\YYBWorkSpace\GitHub\VulkanLearn\shader\glsl\M_preintegratedSkin.json` | `C84E49C78B3374FE191801EA98BCA3AC40B22E8AC61702A580C654B98F28B87F` |
| `D:\YYBWorkSpace\GitHub\VulkanLearn\shader\glsl\M_preintegratedSkin.surface.glsl` | `12E2ABECC653ED8444169143F9F05F8FC24C5BA658D3D527D361F0468B5A2DA6` |
| `D:\YYBWorkSpace\GitHub\VulkanLearn\shader\glsl\materialFunction\mf_preintegratedSkinInputs.glsl` | `45A718C1AE949778F5E229923907E5283B8FB20BC5652A41129C11D8A2492B21` |
| `D:\YYBWorkSpace\GitHub\VulkanLearn\shader\glsl\engine\preintegratedSkinLighting.glsl` | `DDCBC6D288CE28E5A37E2AAE8C905A267266F0539B27E4423D5656E5D2B0C8B5` |
| `D:\YYBWorkSpace\GitHub\VulkanLearn\shader\glsl\engine\deferredLighting.glsl` | `10D167496C34089F856866AEA48D23779637D11CF5D009975FC5F1EA89F4D254` |

## 2. 逐槽位基线

| 源槽位 | 源技术/模式 | 目标 MI | 目标材质 | RenderState | 结论 |
|---|---|---|---|---|---|
| `b_f_3725_high_0` | `pbr_skin` / mode 1 | `MI_b_f_3725_body_p0.json` | `M_preintegratedSkin` | Opaque + Back | 资源和基础输入已接通 |
| `09 - Default` | `pbr_skin` / face Skin | `MI_face_skin.json` | `M_preintegratedSkin` | Opaque + Back（目标默认） | 资源和基础输入已接通，脸部 RenderState 仍需 MTG 逐槽位复核 |

两个 MI 当前都启用：

```text
USE_ALBEDO_MAP=1
USE_NORMAL_MAP=1
USE_SKIN_PARAM_MAP=1
USE_SKIN_AUX_MAP=1
USE_SKIN_DETAIL_MAP=1
```

身体 MI 未显式覆写 `u_skinSurface` / `u_skinTransmissionWeight`；脸部 MI 当前为：

```text
u_skinSurface = [0.004, 1.0, 1.0, 0.0]
u_skinTransmissionWeight = 0.08
```

两个槽位都绑定 `skinLuts/PSL_skin.json`，`skinLutId=1`、`finalDiffuseResponse`、`thicknessMax=8 mm`。

## 3. 纹理通道基线

### 3.1 身体

| 目标资源 | 源/目标通道 | 色彩空间 | Wrap | 当前 MI |
|---|---|---|---|---|
| `T_b_f_3725_body_BaseColor.json` | RGB BaseColor；A opaque | sRGB | Clamp | 已绑定 |
| `T_b_f_3725_body_Normal.json` | RG normal XY；B normal Z；A reserved | Linear | Clamp | 已绑定 |
| `T_b_f_3725_body_SkinParam.json` | R roughness；G metallic；B skinColorMask；A AO | Linear | Repeat | 已绑定 |
| `T_b_f_3725_body_SkinAux.json` | R curvature；G detailNormalMask；B/A reserved | Linear | Clamp | 已绑定 |
| `T_b_f_3725_body_DetailNormal.json` | RG/B detail normal；A poreModulation | Linear | Repeat | 已绑定 |
| `T_b_f_3725_body_EmissionMask.json` | RGB emissionMask | Linear | Repeat | 未绑定 |

### 3.2 脸部

| 目标资源 | 源/目标通道 | 色彩空间 | Wrap | 当前 MI |
|---|---|---|---|---|
| `T_nf2022_f_01_skin_BaseColor.json` | RGB BaseColor；A opacity descriptor | sRGB | Clamp | 已绑定 |
| `T_nf2022_f_01_skin_Normal.json` | RG normal XY；B normal Z；A reserved | Linear | Clamp | 已绑定 |
| `T_nf2022_f_01_skin_SkinParam.json` | R roughness；G metallic；B skinColorMask；A AO | Linear | Repeat | 已绑定 |
| `T_nf2022_f_01_skin_SkinAux.json` | R curvature；G detailNormalMask；B/A reserved | Linear | Clamp | 已绑定 |
| `T_nf2022_f_01_skin_DetailNormal.json` | RG/B detail normal；A poreModulation | Linear | Repeat | 已绑定 |
| `T_nf2022_f_01_skin_Surface.json` | mask1 / emissionPearlFlow / sparkle / detailMask | Linear | Repeat | 未绑定 |

`Tex0.A` 在源 `pbr_skin` 中是自发光控制语义，不能因为目标 BaseColor 描述为 opaque 就宣称该语义已迁移。当前目标 MI 没有绑定对应 Emission 资源，属于明确未完成项。

## 4. 源宏与输入语义

源 `pbr_skin` 已核实：

```text
SHADINGMODELID        = SHADINGMODELID_PREINTEGRATED_SKIN (3)
SHADER_QUALITY        = 2
NORMAL_MAP_ENABLE     = 1
IS_TRANSPARENT        = 0
ALPHA_TEST_ENABLE     = 0
HAS_TWO_SIDE          = 0
HAS_BOTTOM_NORMAL     = 1
HAS_EMISSIVE          = 1
DYNAMIC_GI_TYPE       = DYNAMIC_GI_SH
```

源节点执行顺序为：

```text
SampleBaseColorTexture
 -> SampleNormalTexture / SampleBlurNormalTexture
 -> SampleParamTexture
 -> curvature curve/intensity
 -> DetailMap top/bottom normal
 -> SkinColor / specular / roughness adjustment
 -> emissive
 -> AO / specular AO
 -> optional makeup / rebirth / wound branches
```

当前 VulkanLearn 目标已覆盖基础贴图采样、SkinParam、SkinAux、DetailNormal 和 LUT 输入，但尚未证明以下源语义已完整落地：

- `HAS_BOTTOM_NORMAL=1` 的底层模糊法线；
- `HAS_EMISSIVE=1` 的 Tex0.A / Emission 路径；
- 源 `skinColorMask` 对 BaseColor、roughness、specular 的独立混合；
- 源 `DetailMap.B` 对 curvature 的质量分支影响；
- 源 PreintegratedSkin dual-lobe specular；
- 源皮肤阴影颜色调制和定制 Skin IBL；
- 脸部 `Surface`、`tex_s`、glitter、makeup、Rebirth 和 wound 分支。

## 5. 当前实现差异证据

1. NeoX 逻辑目前位于 `mf_preintegratedSkinInputs.glsl`，身体/脸部与通用 `M_preintegratedSkin` 共用一条 MF；尚未按 Hair 原则拆出 `M_neoxSkin` / `mf_neoxSkin*`。
2. 当前 MF 只生成一份 `inputs.normal`，没有可供 Deferred 使用的 bottom normal 独立字段。
3. 当前 Deferred Skin 路径先调用 DefaultLit，再对 specular 做临时缩放；这不能作为 dual-lobe 对齐完成证明。
4. 当前 MI 没有绑定 EmissionMask / Surface 辅助图；因此脸部妆容、glitter 和源自发光仍是有意差异。
5. 当前固定机位运行时 Beauty/Debug 证据尚未归档，本审计不对最终视觉相似度作结论。

## 6. P0 阶段门

### 已通过

- [x] 身体和脸部 Skin 源槽位可反查到目标 MI。
- [x] 源 Shader、目标 M_、MF、lighting 和核心资源指纹已锁定。
- [x] SkinParam、SkinAux、DetailNormal 的通道、颜色空间和地址模式已记录。
- [x] 两个 MI 的宏、LUT 和脸部显式参数已记录。
- [x] `Tex0.A`、bottom normal、Surface/glitter 等未完成语义已显式列出。

### 待补证据

- [ ] 运行 `SC_b_f_3725_p0.json`，归档 Beauty baseline。
- [ ] 归档 Skin mask、Curvature、Top Normal、LUT response、Specular、Transmission、Shadow、IBL Debug View。
- [ ] 用 RenderDoc 或现有运行时诊断复核两个 Skin draw 的实际 descriptor、RenderState 和 pass。

P0 只有在“已通过”与“待补证据”全部完成后才关闭；在此之前进入 P1 只能视为代码结构准备，不能宣称视觉对齐。
## 7. P0 运行时 smoke 记录

已执行：

```text
build/bin/main.exe --initial-scene scenes/SC_b_f_3725_p0.json --no-dev-ui --framesmoke 1 --exit-after-tests
```

结果：

- 进程退出码：`0`；
- World/Graph transaction：`generation=1`，角色场景成功提交；
- Shader build：`entries=33`、`artifacts=21`、`hits=21`、`misses=0`、`failed=0`；
- Frame smoke：`1/1` 完成，`avgFrameMs=59.730900`，`avgRenderLoopMs=10.994900`；
- 运行日志：`D:\YYBWorkSpace\GitHub\VulkanLearn\tmp\skin_p0_runtime.log`。

本 smoke 只证明启动、资源事务和一帧渲染可用，不证明 Skin 视觉对齐；Beauty、逐项 Debug View 和 RenderDoc 证据仍待补齐。