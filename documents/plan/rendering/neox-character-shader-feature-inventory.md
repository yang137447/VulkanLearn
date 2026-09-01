# NeoX 角色 Shader Feature Inventory：b_f_3725

## 状态

截至 2026-08-24，`b_f_3725` 当前 Pose 静态角色已经完成 VulkanLearn 侧的全槽位还原：glTF 保留 35 个真实材质槽，全部绑定已迁移材质实例，`fallbackSlotCount=0`。

本文前半部分保留迁移前的源合同盘点，后半部分记录已落地结果和有意差异。Shading Model 本身由专项对齐，本次只使用现有 `PreintegratedSkin`、`Subsurface`、`Cloth`、`Hair`、`Eye`、`ThinTranslucent` 和 `DefaultLit`，没有新增或扩展 Shading Model。

## 已落地结果

- 静态模型：`VukanLearnResources/models/datas/neox/b_f_3725/b_f_3725.gltf`；
- 模型资产：`VukanLearnResources/models/neox/b_f_3725/SM_b_f_3725_p0.json`；
- 验证场景：`VukanLearnResources/scenes/SC_b_f_3725_p0.json`；
- 迁移清单：`VukanLearnResources/generated/neox/b_f_3725/b_f_3725-material-migration.json`；
- 统一转换器：`tool/neox/convert_bf3725_character.py`；
- 槽位结果：35/35 已迁移、19 个实际 MI 资产、0 fallback；Hair mode 8 额外使用一份 Clip MI；
- UV 合同：9 个网格保留真实 UV0+UV1，`nf2022_f_01` 只导出 UV0，禁止用 UV0 合成 UV1。

已落地的上层母材质/MF：

- `M_neoxDefault` + `mf_neoxPackedSurface`：Default PBR、静态发光遮罩和 Sparkle；
- `M_neoxSilk` + `mf_neoxPackedSurface`：现有 Cloth Shading Model 上的 Silk/Flow/Sparkle/Emission 作者层；
- `M_neoxPearl` + Pearl/Emission/Subsurface MF：真实 UV1 的 Pearl MatCap 和静态 Billboard Bake；
- `M_neoxHair` + `mf_neoxHairInputs`：Tex0 + Normal + RDI 的透明 Hair Card 作者层；
- `M_eye`：现有 Eye Shading Model 的 Iris/Sclera 作者参数；
- `M_neoxCrystal`：现有 ThinTranslucent 上的 Crystal 近似层；
- `M_preintegratedSkin`：身体与脸部皮肤。

## 输入资产

- Blender 装配：D:/YYBWorkSpace/GitHub/NeoxIO/artifacts/player_assembly_current_pose_fixed.blend
- NeoX 资源根：K:/future/res
- 角色资源：K:/future/res/character/players2021/b_f_3725
- Shader 源码：C:/Software/WorkTemp/G66ShaderDevelop/shader-source/pbr

Blender 装配包含 9 个网格对象：身体主网格 12 个材质槽、头发主网格 4 个材质槽、头发/配件网格 4 个材质槽，以及 1/3/3/2/3/4 个材质槽的其他部件。脸部/眼部辅助网格使用 07 - Default、09 - Default、08 - Default 三个槽位。

当前 blend 是同步后的静态 Pose，没有可复用的 Animation Action；本轮只盘点静态效果链路。

## NeoX 材质族

### Default PBR

资源技术：pbr_default.fx。

典型输入：Tex0、ParamMap、NormalMap。常见宏：PLAYERS_SELF、SEPARATED_CHARACTER_LIGHTING，部分材质启用 SPARKLE_ENABLE。

建议映射：

~~~text
UE-aligned DefaultLit
    + MF_BaseColor
    + MF_Normal
    + MF_PbrInputs
    + MF_Coverage（根据 AlphaRef/RenderState 确认）
~~~

ParamMap 的通道语义尚未冻结，必须从 NeoX PBR 参数节点和纹理样本确认后，离线转换为 VulkanLearn 标准 PBR 参数纹理。

### Silk / Cloth-like

资源技术：pbr_silk.fx。

典型输入：Tex0、ParamMap、NormalMap、t_surfacemap。已确认 FLOW_MAP_ENABLE、SPARKLE_ENABLE=2、EMISSIVE_MODE=1、TWO_SIDE_ENABLE，以及 detail tiling、Sparkle density/twinkle/brightness/color range、Emission Fresnel 参数。Silk shader 节点还声明了 anisotropy 和 anisotropy_cross。

建议映射：

~~~text
UE-aligned DefaultLit 或 ClearCoat
    + MF_BaseColor
    + MF_Normal
    + MF_PbrInputs
    + MF_SilkAppearance
    + MF_Sparkle（保留时）
    + MF_Emission（启用时）
    + TwoSided RenderState
~~~

如果各向异性形成独立高光 Lobe，应进入 Shading Model 扩展；Flow、Detail、Sparkle 和 Emission 仍属于 MF 能力。

### Skin

资源技术：pbr_skin.fx。

典型输入：Tex0、ParamMap、NormalMap。脸部资源额外出现 IsFace、MODEL_FURNITURE_ENABLE、BATCH_SKINNED_MESH 和 t_glitter_noise_array。

建议映射：

~~~text
UE-aligned Skin/Subsurface
    + MF_BaseColor
    + MF_Normal
    + MF_PbrInputs
    + MF_SkinInputs
    + MF_Sparkle 或 MF_Glitter（待确认）
~~~

次表面颜色、厚度和脸部特殊处理需要继续核对 skin_functions.hlsl、pbr_skin_nodes.hlsl 和参数文件。

### P0 身体基线：b_f_3725_high_0

当前选定的 P0 代表材质：

- Technique：`shader\pbr_skin.fx::TShader`；
- `Tex0`：`textures/b_f_3725001a.tga`；
- `ParamMap`：`textures/b_f_3725001m.tga`；
- `NormalMap`：`textures/b_f_3725_h_001n.tga`；
- 当前材质状态记录：`AlphaRef=0`、`CullBack=True`、`TransparentMode=1`；NeoX 的 mode 1 是不透明、开启深度写入。

#### 已确认的源通道语义

以实际调用的 `SampleParamTexture` 和 `SampleNormalTexture` 为准，而不是只看参数文件中的历史注释：

| 源资源 | 通道 | NeoX 运行时语义 | 颜色空间/采样 |
|---|---|---|---|
| `Tex0` | RGB | Base Color | sRGB；`s_diffuse.bSRGB=TRUE` |
| `Tex0` | A | 皮肤自发光控制量；`pbr_skin_nodes.hlsl` 使用 `1 - A` 作为 emissive mask | 线性控制量；不是默认 coverage |
| `NormalMap` | RG | Tangent-space normal XY | 线性；`RG * 2 - 1` |
| `NormalMap` | B | Curvature，写入 `color_mask.y` | 线性控制量 |
| `NormalMap` | A | Detail normal mask，写入 `color_mask.z` | 线性控制量 |
| `ParamMap` | R | Roughness | 线性；没有 sRGB 标记 |
| `ParamMap` | G | Metallic | 线性；没有 sRGB 标记 |
| `ParamMap` | B | Skin color/tattoo mask，写入 `color_mask.w` | 线性控制量 |
| `ParamMap` | A | Ambient Occlusion | 线性；没有 sRGB 标记 |

`pbr_skin_parameters.hlsl` 顶部的 `R-roughness, G-specular, B-thickness, A-AO` 注释与实际公共采样函数不一致；同一文件的资源 UI 标签和 `pbr_skin_nodes.hlsl` 的后续使用都支持 `G=metallic, B=skin mask`。因此迁移时以采样函数写入 `MaterialInputs` 的结果为准。

对当前 P0 身体源样本做了通道和尺寸核对：

- `nb_f_2023002a.tga` 为 2048×2048 RGB 不透明 BaseColor；
- `nb_f_2023002m.tga` 为 1024×1024 RGBA，R/G/B/A 分别作为粗糙度、金属度、皮肤遮罩和 AO；
- `nb_f_2023002n.tga` 为 1024×1024 RGB，B 为 curvature，缺失的 A 在离线转换中按 1.0 处理；
- `skin_detial_n.tga` 为 256×256 RGB DetailNormal，使用 repeat 采样。

#### 离线转换规则

NeoX 的来源解码不进入 VulkanLearn 运行时。P0 离线转换输出以下标准资源：

1. **标准 PBR 参数图**：普通材质使用 `pbrParamMap = (src.R, src.G, src.A, 1)`。这样对齐 VulkanLearn 当前 `mf_pbrSurface.glsl` 的 `R=roughness, G=metallic, B=AO, A=reserved` 合同。不要对 roughness 做 smoothness 反转；Skin P0 不把该图绑定进 `M_preintegratedSkin`。
2. **皮肤参数图**：Skin 使用 `skinParamMap = src.ParamMap`，保留 `R=roughness、G=metallic、B=skinMask、A=AO`，由 `mf_preintegratedSkinInputs.glsl` 直接消费。
3. **标准切线法线图**：由源 `NormalMap.RG` 解码 XY，并重建 Z 后输出标准 RGB normal。源 `NormalMap.B/A` 不可直接当作标准 normal 的 B/A，应分别保存在 `skinAuxMap.R/G` 中，作为 curvature/detail-normal mask。当前 P0 样本两者都是 1，可先用常量，但转换器仍应保留通用路径。
4. **Base Color 与自发光遮罩**：普通材质按材质族拆出 `Tex0.RGB` 与 `1 - Tex0.A`；Skin P0 的 `nb_f_2023002a.tga` 是 RGB 不透明源图，不额外伪造发光或 coverage。P0 的 `TransparentMode=1` 已经确定为 opaque，因此 `AlphaRef=0` 不触发 Alpha Clip；只有源材质明确启用 Alpha Clip/Blend 时，才允许单独生成 coverage 资源。

目标侧建议保持以下职责边界：

- `MF_PbrInputs` 只读取已经转换好的 `pbrParamMap`；
- `MF_SkinInputs` 读取 `skinParamMap`、`skinAuxMap` 和 `skinDetailMap`，生成皮肤专用输入；
- `MF_Coverage` 只消费明确的 coverage 资源或材质常量，不解释 NeoX 的原始 Alpha；
- 运行时不再出现 `ParamMap` 通道解码分支，避免把 NeoX 资源格式耦合到 VulkanLearn 的 Shading Model。
## Shading Model 专项边界

角色所需的 `Subsurface`、`PreintegratedSkin`、`SubsurfaceProfile`、`Hair`、`Eye` 和 `Cloth` 已在独立专项中完成对齐。本迁移任务只消费这些既有合同，继续完成资源转换、Material Function 输入、Render State、MeshPass 和材质槽映射，不在这里扩建 Shading Model。

当前与角色相关的模型状态如下：

| 角色能力 | 目标 Shading Model | 当前处理 |
|---|---|---|
| 身体普通 PBR | `DefaultLit` | 保留为普通表面和未迁移槽位的基线。 |
| 身体/脸部皮肤 | `Subsurface` / `PreintegratedSkin` / `SubsurfaceProfile` | 已实现；P0 身体槽使用 `PreintegratedSkin`，后续补 NeoX 皮肤 MF 输入。 |
| 头发高光与透光 | `Hair` | 已实现；下一步仍需按源材质补 Alpha Clip/Blend、Root/Tip、RDI 和透明 Shadow 等上层合同。 |
| 眼睛虹膜/角膜 | `Eye` | 已实现；后续按眼球/角膜槽接入虹膜、MatCap、Custom IBL 与对应 Pass。 |
| 丝绸表面 | `Cloth` | 已实现；Flow、Sparkle、Emission 等属于材质功能输入，继续按槽位验证。 |
| 晶体折射/焦散 | 尚未定义 | 不在当前已对齐 Shading Model 集合内，待 Refraction/Crystal 专项与场景颜色资源一起设计。 |

迁移文档中的“建议映射”只描述角色资产接入目标。已完成的 Shading Model 直接复用；尚缺的效果按 MF、VertexFactory、MeshPass 或独立后续专项归类，不回到本任务扩建模型层。

## P0 转换落地状态

已生成 `b_f_3725_high_0` 的 VulkanLearn 运行资源，转换工具位于 `tool/neox/convert_bf3725_p0.py`。工具读取 MTG 指定的 `nb_f_2023002a/m/n`、共享 `skin_detial_n.tga` 以及旧通用贴图审计输入，输出到 `resourcePath/generated/neox/b_f_3725/`，并生成对应的 `T_*.json` 与 `MI_*.json` 描述。

当前材质实例使用 `shader/glsl/M_neoxSkin.json` 和 `skinLuts/PSL_skin.json`，已绑定：

- `nb_f_2023002a.tga` 的 sRGB BaseColor；
- `nb_f_2023002n.tga` 重建后的标准 Normal；
- `nb_f_2023002m.tga` 的 SkinParam；
- `nb_f_2023002n.tga` 拆出的 SkinAux；
- `skin_detial_n.tga` 转换的 DetailNormal。

旧 `pbrParamMap` 与 `emissionMaskMap` 仍生成在 `conversion-manifest.json` 的 `compatibilityTextures` 中，
只用于审计，不再绑定到 P0 Skin MI。

角色网格 P0 已通过 `NeoxIO` 自构建 Blender 4.5.10 导出为静态 glTF。导出工具 `tool/neox/export_bf3725_gltf.py` 会烘焙当前 Pose、Modifier 和对象世界变换，不导出 Skin/Animation，并生成 `b_f_3725.audit.json` 校验材质槽、顶点色和 UV 语义。

第二套 UV 严格按源数据导出：9 个源网格包含 `UVMap_0 + UVMap_1`，对应 glTF `TEXCOORD_0 + TEXCOORD_1`；`nf2022_f_01` 只有 `UVMap_0`，对应 glTF 仅导出 `TEXCOORD_0`，不复制或合成 2U。

当前 P0 模型描述为 `models/neox/b_f_3725/SM_b_f_3725_p0.json`，保留 glTF 的 35 个真实材质槽。`b_f_3725_high_0` 绑定已迁移的 PreintegratedSkin 材质，`b_f_3725_high_1` 绑定 Pearl/Subsurface Billboard 的静态几何版本，其余 33 个未迁移槽显式绑定紫色 fallback；验证场景为 `scenes/SC_b_f_3725_p0.json`。下一阶段按槽位清单继续迁移材质，不新增 Shading Model。

`b_f_3725_high_1` 复用已对齐的 `Subsurface` Shading Model，新增内容仅为 NeoX Pearl 的材质功能输入与 Render State：UV0 圆形覆盖/假球法线、真实 UV1 的 2U MatCap 采样、Fresnel、Pearl Noise、Base/Emission 拆分、Alpha Clip 和双面渲染。`M_neoxPearl.surface.glsl` 只作为组合入口，Pearl、Emission 和 Subsurface 输入分别由 `mf_pearlescentInputs.glsl`、`mf_emission.glsl` 和 `mf_subsurfaceInputs.glsl` 提供；Alpha Clip 仍由 MeshPass 执行。源槽所在 glTF primitive 明确包含 `TEXCOORD_0 + TEXCOORD_1`，未合成第二套 UV；NeoX Billboard 顶点展开不进入运行时，当前 Pose 几何已在静态 glTF 中烘焙。转换工具为 `tool/neox/convert_bf3725_high1.py`，运行材质为 `shader/glsl/M_neoxPearl.json`。
### Hair Transparent

资源技术：pbr_hair_transparent.fx。

典型输入：Tex0、NormalMap、t_rdi、Root/Tip 颜色和强度、Vertex opacity/depth/shadow 控制、Specular shift。

该技术的 NeoX mode 8 是 Alpha Test Core + Alpha Blend Fringe 双 Pass，不是单一
Forward Transparent 材质。当前 VulkanLearn 暂用两份静态场景资源：Hair-only Core
走 `OpaqueClip`，原角色中的 Hair Card 走 `TransparentAlphaBlend`。

建议映射：

~~~text
UE-aligned Hair
    + MF_HairInputs
    + MF_BaseColor
    + MF_Normal
    + MF_Coverage
    + Hair tangent/specular inputs
    + ForwardTransparent MeshPass
~~~

Hair 的 Alpha、双 Pass Clip、Depth 和 Shadow 最终仍应由 Material Multi-Pass/MeshPass
正式表达。当前两份资源共享同一 MF：Core 负责 GBuffer、Depth、Shadow，Fringe 只补
透明发梢；源双 Pass 的有效 ClipValue 为 `u_two_pass_clip_value=0.5`，不是 raw
RenderState 中的 `AlphaRef=51`。

### Eye

脸部资源的 07 - Default 使用 pbr_eye.fx，典型输入为 t_iris、t_mapcap_specular、t_custom_ibl、u_iris_range 和 u_pipil_scale。

建议映射：

~~~text
UE-aligned Eye
    + MF_EyeIris
    + MF_EyeSpecular/Matcap
    + MF_EyeOcclusion
    + Eye-specific IBL input
~~~

pbr_eye 的虹膜视差、MatCap 高光和 Custom IBL 需要单独验证，不能直接退化为普通 PBR。

### Simple Eye Edge

脸部资源的 08 - Default 使用 pbr_simple.fx，主要使用眼缘贴图并走透明状态。

第一版可以映射为 DefaultLit 或 Unlit，加 MF_BaseColor、MF_Coverage 和 Transparent/AlphaClip MeshPass。需要通过截图确认它是睫毛、眼缘还是其他面部辅助层。

### Subsurface Billboard / Pearl

资源技术：pbr_subsurface_billboard.fx。

已确认输入包括 t_pearl_noise、MatCap/UV、u_fresnel、u_brightness、u_roughness、u_metallic、u_emissive_amount、u_subsurface_color、u_thickness、Billboard 顶点和 USE_2U_MIX。

该技术同时包含 Billboard 几何、珠光/MatCap、Fresnel、次表面和 Emission，不能直接当作一个普通 Pearl MF。

建议拆为：

~~~text
VertexFactory/Billboard
    + MF_PearlescentInputs
    + MF_SubsurfaceInputs
    + MF_Emission
    + Skin/Subsurface 或 ThinTranslucent ShadingModel
~~~

Billboard 顶点逻辑不属于普通片元 MF。

### Crystal / Refraction

资源技术：pbr_crystal.fx。

已确认输入包括 BaseColor、DetailMap、NormalMap、Detail Normal、Crystal Color、Refraction Color/Brightness/Contrast/Rotation、Caustic、Subsurface Color 和 Crystal Fresnel。

建议暂时列为独立后续目标：

~~~text
MF_CrystalInputs
    + Refraction/ThinTranslucent 或专用 ShadingModel
    + Detail Normal
    + Caustic/Scene effect
~~~

折射、焦散和场景采样不能全部塞进普通 MF，需要与 VulkanLearn 的透明和后处理资源合同一起设计。

## 初版 Feature 映射

| NeoX Feature | VulkanLearn 归属 | 第一版处理 |
|---|---|---|
| Tex0/BaseColor | 离线标准纹理 + MF_BaseColor | 保留 |
| ParamMap | 离线 PBR 纹理转换 + MF_PbrInputs | 保留，先确认通道 |
| NormalMap | 标准 Normal Texture + MF_Normal | 保留 |
| Detail Normal | MF_Normal 组合能力 | 身体/晶体再确认 |
| AlphaRef/Opacity | MF_Coverage + MeshPass Clip | 保留 |
| Two-sided | Render State | 保留 |
| Silk anisotropy | Shading Model 扩展 | 先验证是否主视觉 |
| Flow Map | MF_UV/Surface Flow | 有明显效果才保留 |
| Sparkle | MF_Sparkle | 优先保留 |
| Emissive Fresnel | MF_Emission + Shading Model 输入 | 保留 |
| Skin subsurface | Skin/Subsurface ShadingModel + MF_SkinInputs | 保留 |
| Hair transparency | Hair ShadingModel + Transparent MeshPass | 保留 |
| Eye iris/MatCap/IBL | Eye ShadingModel + MF_Eye | 保留 |
| Pearl/MatCap billboard | `Subsurface` + Pearl MF；当前 Pose 静态几何烘焙 | `b_f_3725_high_1` 已接入 |
| Crystal refraction/caustic | `ThinTranslucent` + Crystal 上层 MF | 已接入近似；无 SceneColor 折射 |
| NeoX 通道解码 | 离线转换 | 不进入运行时 |

## 迁移优先级

### P0：身体基础材质

选择一个 pbr_default 或 pbr_skin 材质，先完成标准 BaseColor、Normal、PBR 参数、UE-aligned DefaultLit、主 Pass、Alpha/Shadow 合同和中间结果 Debug View；Skin/Subsurface 响应留给专项实现。

### P1：头发

完成 Hair Shading Model、Alpha Clip/Blend、Root/Tip 颜色、Hair Normal/RDI/Mask、Shadow 与透明 Velocity。

### P2：脸部和眼睛

完成 Skin Face、Eye Iris、Eye MatCap/Custom IBL 和 Eye Edge 透明辅助层。

### P3：Silk、Sparkle、Pearl

完成 Silk Appearance、Flow、Sparkle、Emissive Fresnel 和 Pearl Noise/MatCap。Pearl 的首个槽位 `b_f_3725_high_1` 已按静态几何路径接入，后续继续验证其他 Pearl/Silk 槽位和动态 Billboard 需求。

### P4：Crystal 和复杂场景效果

最后处理 Refraction、Caustic、Crystal Fresnel、Billboard Subsurface、场景采样和透明排序。

## 已确认与剩余视觉校准

已确认：

1. `Tex0.A` 按材质族分别处理：Skin 的发光遮罩离线拆分，Default/Silk/Hair 的覆盖率保留；
2. Silk SurfaceMap 为 `R=Mask1、G=Emission/Pearl/Flow、B=Sparkle、A=Detail Mask`；Hair RDI 为 `R=Root、G=Depth、B=ID、A=AO`；
3. glTF 35 个槽位与 MTG 合同一一对应，AlphaRef 使用 8-bit 阈值归一化；
4. Silk 使用现有 Cloth Shading Model，Sparkle、Flow、Emission 属于上层 MF，不新增 Shading Model；
5. Eye、Hair、Skin、Pearl 和 Crystal 均已进入对应现有模型/Pass 路径；
6. 全量转换器可重复生成 35 槽、19 个实际 MI 和零 fallback 模型资产；Hair mode 8
   同时生成 Hair-only Core glTF/模型描述并在验证场景叠加第二个 mesh 对象。

剩余工作只属于画面校准，不阻塞角色资产恢复：

- 根据 NeoX 与 VulkanLearn 同机截图微调 Emission、Sparkle、Sheen、Eye 几何和 Crystal 透射强度；
- 如果未来补齐统一动画时钟合同，再恢复 Flow/Twinkle 动画相位；
- 如果未来增加 SceneColor 透明折射资源，再替换当前 Crystal 的 `ThinTranslucent` 近似。
## 当前结论

`b_f_3725` 由 Default、Skin、Silk、Hair、Eye、Subsurface Billboard 和 Crystal 材质族组成，不能由单一 `M_pbr` 覆盖。当前静态角色已经完成 35 个槽位的运行时还原，并通过 C++ 构建、Hair/Eye/Subsurface/ThinTranslucent/Shader Cache 测试和 3 帧角色场景烟测。

NeoX 打包通道均在离线阶段转为标准资源，运行时 Shader 不再重复解码旧格式。当前保留的有意差异如下：

1. Billboard 当前 Pose 已烘焙到 glTF，不恢复动态相机朝向、上一帧 Velocity/TAA 修正和 `u_depth_offset`；
2. Silk/Default 保留遮罩、密度、发光菲涅尔和视角 Sparkle，但源动画时钟合同缺失，因此不伪造 Flow/Twinkle 相位；
3. Crystal 复用 `ThinTranslucent`，保留透射色、厚度、覆盖率、焦散遮罩和发光层次；当前没有 NeoX SceneColor 折射、旋转折射和深度偏移；
4. Eye 复用现有 Eye Shading Model，源 MatCap 和自定义 Cube IBL 由 VulkanLearn 眼球光照与场景环境替代；
5. Hair 的 RDI.B strand ID 映射为轻微粗糙度扰动，因为现有 Hair 输入没有独立 strand ID 字段。

后续若继续提升画面对齐，应以截图差异为依据调参或补上层 MF，不应在本角色任务中新增 Shading Model。
