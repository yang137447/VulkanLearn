# NeoX 角色 Shader Feature Inventory：b_f_3725

## 状态

本文记录 b_f_3725 当前 Pose 角色从 NeoxIO/NeoX 资源迁移到 VulkanLearn 前的第一轮材质和 Shader 盘点。

这是角色迁移计划文档，不修改 VulkanLearn 当前 Shader Structure 合同。未确认的纹理通道和公式必须在后续验证中补齐，不能直接当作运行时规范。

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

对当前 P0 样本做了通道统计和预览：

- `ParamMap` 尺寸为 2048×2048；R/G/B/A 分别作为粗糙度、金属度、皮肤遮罩和 AO；
- A 通道范围约为 0.38–1.0，中位数约 0.988，符合 AO 通道而不是二值 coverage；
- `NormalMap` 的 B/A 在该样本中恒为 1.0，因此当前身体基线的 curvature/detail mask 等价于全强度；
- 预览与统计文件：`artifacts/neox-parammap-b_f_3725/contact_sheet.png`、`artifacts/neox-parammap-b_f_3725/stats.json`。

#### 离线转换规则

NeoX 的来源解码不进入 VulkanLearn 运行时。P0 离线转换输出以下标准资源：

1. **标准 PBR 参数图**：`pbrParamMap = (src.R, src.G, src.A, 1)`。这样对齐 VulkanLearn 当前 `mf_pbrSurface.glsl` 的 `R=roughness, G=metallic, B=AO, A=reserved` 合同。不要对 roughness 做 smoothness 反转。
2. **皮肤功能遮罩图**：`skinMaskMap.R = src.ParamMap.B`，供 `MF_SkinInputs` 处理肤色、纹身和相关皮肤功能；其余通道暂保留。
3. **标准切线法线图**：由源 `NormalMap.RG` 解码 XY，并重建 Z 后输出标准 RGB normal。源 `NormalMap.B/A` 不可直接当作标准 normal 的 B/A，应分别保存在 `skinAuxMap.R/G` 中，作为 curvature/detail-normal mask。当前 P0 样本两者都是 1，可先用常量，但转换器仍应保留通用路径。
4. **Base Color 与自发光遮罩**：`Tex0.RGB` 作为 sRGB Base Color；另输出 `emissionMaskMap.R = 1 - Tex0.A`。P0 不应把 `Tex0.A` 直接接到 `surface.opacity`，否则会把 NeoX 的发光控制量误当作覆盖率。当前 P0 的 `TransparentMode=1` 已经确定为 opaque，因此 `AlphaRef=0` 不触发 Alpha Clip；只有源材质明确启用 Alpha Clip/Blend 时，才允许单独生成 coverage 资源。

目标侧建议保持以下职责边界：

- `MF_PbrInputs` 只读取已经转换好的 `pbrParamMap`；
- `MF_SkinInputs` 读取 `skinMaskMap`、`skinAuxMap` 和 `emissionMaskMap`，生成皮肤专用输入；
- `MF_Coverage` 只消费明确的 coverage 资源或材质常量，不解释 NeoX 的原始 Alpha；
- 运行时不再出现 `ParamMap` 通道解码分支，避免把 NeoX 资源格式耦合到 VulkanLearn 的 Shading Model。
## Shading Model 专项边界

角色迁移先使用当前已经落地的 `DefaultLit` 作为 P0 光照基线。P0 可以准备并验证皮肤遮罩、厚度/辅助通道和发光控制纹理，但这些数据在 Skin/Subsurface 专项完成前不进入独立的皮肤 BRDF 或次表面积分。

当前与角色相关的模型状态如下：

| 角色能力 | 目标 Shading Model | 当前处理 |
|---|---|---|
| 身体普通 PBR | `DefaultLit` | P0 直接使用，作为资源转换和几何/光照对齐基线。 |
| 身体/脸部皮肤 | `Subsurface` / `PreintegratedSkin` / `SubsurfaceProfile` | 待 Skin 专项；P0 只保留标准 PBR 输入和显式皮肤遮罩。 |
| 头发高光与透光 | `Hair` | 待 Hair 专项；Alpha Clip/Blend 和透明 Shadow 属于 MeshPass/Render State 合同，不能代替 Hair BRDF。 |
| 眼睛虹膜/角膜 | `Eye` | 待 Eye 专项；虹膜视差、MatCap 和 Custom IBL 先作为待验证输入记录。 |
| 丝绸各向异性 | `Cloth` 或 Anisotropy 扩展 | 待 Cloth/Anisotropy 专项；Flow、Sparkle、Emission 仍按 MF 能力单独盘点。 |
| 晶体折射/焦散 | 尚未定义 | 不在当前 Shading Model 合同内，待 Refraction/Crystal 专项与场景颜色资源一起设计。 |

迁移文档中的“建议映射”表示最终目标，不表示当前代码已经支持该模型。任何材质接入都必须先确认对应 Shading Model 专项已完成，否则先落到 `DefaultLit` 基线或停留在离线资源验证阶段。

### Hair Transparent

资源技术：pbr_hair_transparent.fx。

典型输入：Tex0、NormalMap、t_rdi、Root/Tip 颜色和强度、Vertex opacity/depth/shadow 控制、Specular shift。

该技术使用 Forward Transparent 路径，包含 Alpha Blend 和透明 Velocity，不是普通 Deferred GBuffer 材质。

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

Hair 的 Alpha、双 Pass Clip、Depth 和 Shadow 语义必须和 MeshPass 一起实现。

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
| Pearl/MatCap billboard | MF_Pearlescent + Billboard VertexFactory | 后续验证 |
| Crystal refraction/caustic | Transparent/Refraction 路径 | 暂缓 |
| NeoX 通道解码 | 离线转换 | 不进入运行时 |

## 迁移优先级

### P0：身体基础材质

选择一个 pbr_default 或 pbr_skin 材质，先完成标准 BaseColor、Normal、PBR 参数、UE-aligned DefaultLit、主 Pass、Alpha/Shadow 合同和中间结果 Debug View；Skin/Subsurface 响应留给专项实现。

### P1：头发

完成 Hair Shading Model、Alpha Clip/Blend、Root/Tip 颜色、Hair Normal/RDI/Mask、Shadow 与透明 Velocity。

### P2：脸部和眼睛

完成 Skin Face、Eye Iris、Eye MatCap/Custom IBL 和 Eye Edge 透明辅助层。

### P3：Silk、Sparkle、Pearl

完成 Silk Appearance、Flow、Sparkle、Emissive Fresnel 和 Pearl Noise/MatCap。

### P4：Crystal 和复杂场景效果

最后处理 Refraction、Caustic、Crystal Fresnel、Billboard Subsurface、场景采样和透明排序。

## 待验证问题

1. P0 `Tex0.A` 的发光遮罩在 VulkanLearn 目标画面中的强度标定；
2. t_surfacemap、t_rdi、t_pearl_noise 的标准纹理语义；
3. TransparentMode 与 VulkanLearn RenderMode 的精确映射；
4. AlphaRef 是否对应统一 Alpha Clip 阈值，还是不同 shader 有额外转换；
5. pbr_silk 的各向异性是否必须进入独立 Shading Model Lobe；
6. Sparkle 的最终输出是高光、Emission 还是二者混合；
7. pbr_eye 的视差是否影响当前静态 Pose 的可见效果；
8. pbr_crystal 的折射/焦散是否需要 VulkanLearn 新的场景颜色资源；
9. 身体主网格的各材质槽与实际身体部位的可视化对应关系。

## 当前结论

b_f_3725 不是一个单一 PBR 材质，而是由 Default、Skin、Silk、Hair、Eye、Subsurface Billboard 和 Crystal 组成的材质族集合。

第一阶段不能从 M_pbr 直接覆盖全部效果。P0 的通道合同已经确认：NeoX ParamMap 需要离线重排为 VulkanLearn 的 PBR 图，并把皮肤遮罩、法线辅助通道和发光控制量拆成显式资源。接下来先用 `DefaultLit` 接入标准 PBR 基线，再按 MF、Shading Model、VertexFactory 和 MeshPass 的边界逐步加入角色特征；皮肤、头发和眼睛的专用响应分别等待对应专项完成。
