# NeoX `b_f_3725` Skin 效果对齐计划

## 文档状态

| 项目 | 内容 |
|---|---|
| 状态 | P4 代码与固定机位视觉验收完成；P5 辅助效果仍待执行 |
| 建立日期 | 2026-08-25 |
| 目标角色 | NeoX `b_f_3725` 当前 Pose 静态角色 |
| 目标槽位 | `b_f_3725_high_0` 身体、`nf2022_f_01` 脸部 |
| 视觉目标 | 保留源肤色、毛孔细节、柔和散射和不过油的皮肤高光 |
| Shading Model 基线 | VulkanLearn `PreintegratedSkin` ID 3 |
| 稳定规则 | `documents/rendering/neox-skin-effect-alignment-contract-v1.md` |
| 关联交接 | `documents/plan/rendering/neox-b-f-3725-character-restoration-handoff.md` |

## 1. 目标与边界

本计划只解决 NeoX `pbr_skin` 到 VulkanLearn 现有 `PreintegratedSkin` 的输入和效果对齐，不新增 Shading Model，不处理动画/Skinning 运行时，也不把脸部妆容和 glitter 作为第一阶段的隐式 fallback。

```text
源材质/源 Shader
    -> 逐槽位 Skin 合同
    -> NeoX Skin MF + MI
    -> 标准 PreintegratedSkin 输入
    -> 公共 Skin 光照
    -> Forward / Deferred / Shadow / IBL 验证
```

## 2. 阶段计划

### P0：冻结源与目标基线

记录身体、脸部各自的 MTG 有效 RenderState、宏、参数、贴图、UV、质量分支和固定机位。输出逐槽位 Golden manifest，并把当前画面、通道 Debug View 和运行日志作为 baseline。

阶段门：任何后续改动都能说明是源语义修复、MF 映射修复、公共 Skin 合同修复或 MI 校准，不能只写“画面更像”。

### P1：拆分 NeoX Skin Material Functions

建立 `M_neoxSkin`、`mf_neoxSkinTextures` 和 `mf_neoxSkinInputs`。将当前 `mf_preintegratedSkinInputs.glsl` 中的 NeoX 专用逻辑迁出，保留 `M_preintegratedSkin` 的通用测试路径。

核对：

- Tex0 RGB/A 的颜色空间与 emissive/coverage 分离；
- ParamMap RGBA 的 roughness/metallic/skin mask/AO；
- NormalMap B/A 的 curvature/detail mask；
- Skin LUT ID、厚度、响应权重和 transmission 的 MI 来源；
- Opaque 身体/脸部 coverage 与 ShadowDepth 一致。

阶段门：NeoX 与普通 PreintegratedSkin 同时编译，NeoX 差异不进入公共 evaluator。

### P2：补齐双层法线与 Detail/Curvature 语义

先在不改公共 Shading Model 光照公式的前提下，恢复源的顶层法线、底层模糊法线、Detail normal mask 和 curvature 关系。

执行顺序：

1. 确认 GBuffer 是否有稳定的 bottom normal 承载；
2. 若没有，扩展现有合同或明确记录 V1 不支持，禁止借用无关字段；
3. 按源 mip/LOD 规则处理 DetailMap；
4. 验证 metallic 区域不使用皮肤细节法线；
5. 验证 curvature 的曲线和强度只应用一次。

阶段门：Top Normal、Bottom Normal、Detail Mask、Curvature Debug View 与离线资产可对照。

### P3：校准 PreintegratedSkin 响应与能量

只在标准 Skin 输入正确后校准公共模型和 MI：

- LUT `finalDiffuseResponse` / `scatteringMultiplier` 分支；
- skin diffuse、transmission 与 baseColor 的能量关系；
- NeoX dual-lobe specular 的 roughness/specular/curvature 输入；
- IBL average、AO 和 specular 分离；
- 禁止用固定 `specular *= 常数` 作为最终修复。

若公共 evaluator 确实偏离 Skin 合同，先补公共合同测试，再修改 evaluator；否则只调整 NeoX MI 或 MF 输入。

阶段门：普通 Skin 测试场景和 NeoX 角色场景都无明显能量重复、过油或暗部断层。

### P4：Shadow、Transmission 与角色光照

对齐源皮肤的阴影颜色、背光透射、角色环境/方向/局部光倍率。所有倍率必须作为显式输入或场景参数，不能写入公共 Skin evaluator 或按槽位硬编码。

阶段门：Direct Diffuse、Transmission、Shadow Color、IBL Diffuse/Specular 均能独立观察，且阴影不会被 LUT 或 AO 重复着色。

### P5：脸部辅助效果与质量分支

在 P0–P4 稳定后，再处理 `tex_s`、glitter、makeup、Rebirth、脸部特殊宏和质量路径。每个效果必须先确认源资源和通道，再新增纹理资产或 MF；没有源资源时记录 unsupported difference，不用假噪声或默认图代替。

阶段门：关闭辅助效果时回到 P4 基线，开启辅助效果只改变对应区域和对应 lobe。

### P6：固定机位视觉验收与文档收口

固定相机、灯光、环境、曝光和资源版本，按“基础色 -> 法线 -> 曲率 -> diffuse -> specular -> transmission/shadow -> IBL -> tone mapping”的顺序验收。

每轮只改一类参数，保存 Beauty、关键 Debug View、MI 快照和日志。完成后更新角色交接文档中的 Skin 当前完成度和有意差异。

## 3. 验证矩阵

### 3.1 静态/编译

- Skin 源槽位解析与 Golden manifest；
- SkinParam/SkinAux/DetailNormal 资产通道审计；
- `M_preintegratedSkin` 与 `M_neoxSkin` reflection；
- Base/Forward/ShadowDepth coverage 一致性；
- 普通 Skin、NeoX 身体、NeoX 脸部三组 MI 加载。

### 3.2 运行时

- `SC_subsurface_models.json`：公共 PreintegratedSkin 基线；
- `SC_b_f_3725_p0.json`：身体与脸部 Skin 联合场景；
- 固定机位 debug view：mask、curvature、top/bottom normal、LUT response、specular、transmission、shadow、IBL；
- 资源缺失、错误 LUT、错误宏、热更失败时保持旧 World，不产生半提交。

### 3.3 视觉验收标准

1. 身体与脸部 BaseColor 不发生非源定义的灰化或染色；
2. 毛孔细节只影响细节法线/指定 lobe，不制造重复高光；
3. 正面高光柔和，不出现公共 DefaultLit 高光尖峰；
4. 逆光有受厚度和权重控制的暖色透射，不出现整片橙色漏光；
5. 阴影保持形状，皮肤色调变化可解释且不重复乘 AO；
6. 身体与脸部使用不同源参数时，不能因为共享 MI 而互相污染；
7. 关闭 NeoX 辅助效果后，画面回到稳定的 P3/P4 基线。

## 4. 非目标

- 新增 Skin Shading Model 或三维 `(N·L, N·V, thickness)` LUT；
- 透明皮肤、半分辨率 SSS、ray-traced diffusion；
- 动画、骨骼、动态妆容系统；
- 没有源资源证据的 glitter、tattoo、Rebirth 或高质量专用分支；
- 通过改曝光、环境强度或 Tone Mapping 掩盖输入/光照合同错误。

## 5. 第一轮执行顺序

```text
P0 源/目标基线
 -> P1 NeoX Skin MF/M_ 拆分
 -> P2 双层法线/曲率
 -> P3 Skin LUT/双 lobe 能量
 -> P4 阴影/透射/角色光照
 -> P5 脸部辅助效果
 -> P6 固定机位验收与文档收口
```
## 6. P0 执行记录（2026-08-25）

静态源/目标审计已完成，记录见 `documents/plan/rendering/neox-b-f-3725-skin-p0-baseline-v1.md`。
身体 `b_f_3725_high_0` 与脸部 Skin primitive 均可反查到目标 MI，SkinParam、SkinAux、DetailNormal
通道和核心源指纹已锁定；角色场景最小 smoke 已退出码 `0`。

P0 尚未完全关闭：Beauty、Skin Debug View、实际 descriptor/RenderState/pass 的运行时证据仍待归档。
在这些证据补齐前，进入 P1 只能做结构准备，不能宣称效果对齐完成。
## 7. P1 执行记录（2026-08-25）

已完成 NeoX Skin 的母材质/MF 结构隔离：

- 新增 `shader/glsl/M_neoxSkin.json`、`M_neoxSkin.vertex.glsl`、`M_neoxSkin.surface.glsl`；
- 新增 `mf_neoxSkinTextures.glsl` 与 `mf_neoxSkinInputs.glsl`；
- `mf_preintegratedSkinInputs.glsl` 恢复为通用 PreintegratedSkin 输入包装；
- 身体和脸部 MI、两个 NeoX 转换器切换到 `M_neoxSkin`；
- 公共 `M_preintegratedSkin` 测试路径保持原始母材质合同。

验证结果：

```text
material_schema_tests.exe   passed
subsurface_tests.exe        passed
shader-force-rebuild        entries=33, artifacts=21, hits=0, misses=21, compiled=21, failed=0
SC_b_f_3725_p0 framesmoke   exit=0, 1/1 frame completed
```

P1 只完成结构隔离和行为保持，不代表双层法线、Tex0.A Emission、dual-lobe specular、Skin shadow 或脸部辅助效果已经对齐。
## 8. P2 执行记录（2026-08-25）

已补齐源 `HAS_BOTTOM_NORMAL=1` 的目标承载：

- `PreintegratedSkinMaterialInputs.bottomNormal` 作为显式模型字段；
- `M_neoxSkin` 使用 `textureLod` 读取源法线/DetailMap 的模糊层；
- `MaterialSurface` 保存 `preintegratedSkinBottomNormal`；
- GBuffer F.zw 使用 Skin 专用八面体坐标编码，未借用 Clear Coat customData；
- Deferred Skin diffuse LUT 与 IBL diffuse 改用 bottom normal，顶层法线仍服务高光/透射；
- 增加 `TestPreintegratedSkinBottomNormalContract` 静态合同测试。

验证结果：

```text
subsurface_tests.exe        passed
shader-force-rebuild        entries=33, artifacts=21, hits=0, misses=21, compiled=21, failed=0
SC_b_f_3725_p0 framesmoke   exit=0, 1/1 frame completed
```

P2 的代码阶段完成；固定机位的 Bottom Normal、Curvature、LUT response 和视觉能量证据仍待归档。
## 9. P3 执行记录（2026-08-25）

### 9.1 Mask 职责拆分

源 `pbr_skin` 的 `ParamMap.B`（`color_mask.w`）只参与肤色换色、粗糙度和高光参数混合；源节点把 `shading_model_mask` 固定为整张 Skin 槽位开启。因此 `mf_neoxSkinInputs.glsl` 不再把 `skinMask` 乘到 `PreintegratedSkinMaterialInputs.weight` 或 `transmissionWeight`，避免纹身/换色边界同时变成 SSS coverage 边界。

### 9.2 Dual-lobe Specular

已移除 Deferred Skin 中临时的 `directSpecular *= surface.specular * 2` 单 lobe 缩放，改为共享 evaluator 的 NeoX dual-lobe GGX：

- `lobe0 = averageRoughness * 0.61601`；
- `lobe1 = averageRoughness * 1.06777`；
- `D` 按源 `mix=0.85` 混合，Smith `G` 仍使用平均粗糙度；
- 介电 `F0 = specular * 0.08`，再按 metallic 与 BaseColor 混合；
- Direct 与 IBL 高光分别输出，Diffuse、Transmission、AO 不重复计入。

源 `shading_models.hlsl` 中 curvature 对 dual-lobe roughness 的 UE 分支仍为注释状态；本轮保留该行为，不用未经证实的 curvature 倍率调图。Curvature 继续作为 Skin GBuffer/PreintegratedSkin 输入保存，待固定机位证据后再决定是否启用源注释分支。

### 9.3 验证

```text
material schema / subsurface contract tests   passed
shader-force-rebuild                           entries=33, artifacts=21, hits=0, misses=21, compiled=21, failed=0
SC_b_f_3725_p0 framesmoke                      exit=0, 1/1 frame completed
```

已归档当前运行证据：

- `generated/screenshots/skin_beauty_p3_duallobe.bmp`
- `generated/screenshots/skin_sss_weight_p3_after.bmp`
- `generated/screenshots/skin_direct_p3_after.bmp`

P3 尚未宣称脸部 Emission、makeup/glitter、wound/rebirth、Skin shadow color 和环境/方向/局部光倍率全部完成；这些仍属于后续 P4/P5 对齐项。

### 9.4 RenderDoc 脸部参数对齐

RenderDoc `event 887` 已确认脸部 `ParamMap.B` 同时控制肤色换色、roughness 偏移和
specular 替换。本轮将以下抓帧有效值接入 `M_neoxSkin` 的 MI 参数路径：

```text
u_skinColor            = [0.906, 0.892, 0.859, 1.0]
u_skinBright           = 1.1
u_skinRoughnessOffset  = 0.05
u_skinSpecular         = 0.5
```

MF 保留源公式中的颜色平方与 mask 混合；亮度只作用于换色目标，不作为整张脸或公共
Skin evaluator 的曝光补偿。当前抓帧 `tex_s` 四通道全黑，face/lip glitter 与 makeup
emissive 强度也为 `0`，因此本轮不新增假妆容、程序噪声或 Texture2DArray 替代绑定。


### 9.5 RenderDoc 脸部 UV 与贴图核对（2026-08-26）

本轮以 RenderDoc 抓帧为准，不用最终 Beauty 截图反推 UV。抓帧与事件固定为：

```text
capture = K:\future\Documents\pc_13th Gen Intel(R) Core(TM) i9-13900K_NVIDIA GeForce RTX 4090_08.24_12.56.04_frame306538.rdc
event   = 887
marker  = nf2022_f_01_high001.gim - Sub2
```

NeoX 源 shader 的唯一参考路径仍为：

```text
C:\Software\WorkTemp\G66ShaderDevelop\shader-source
```

RenderDoc `event 887` 的输入布局和 buffer 读取结果：

| 项目 | RenderDoc 证据 |
|---|---|
| 顶点流 0 | stride `32`，读取 POSITION/BLENDINDICES/BLENDWEIGHT |
| 顶点流 1 | stride `20`，读取 NORMAL/TANGENT/TEXCOORD |
| TEXCOORD | 顶点流 1，byte offset `16`，格式 `R16G16_FLOAT` |
| Index buffer | resource `11303`，stride `2` |
| Face Draw | `81234` indices，`StartIndexLocation=7158`，使用 `14151` 个唯一顶点 |
| 源 UV 范围 | `U=[0.0028477, 0.9970703]`，`V=[0.0078278, 0.9912109]` |

目标 glTF `nf2022_f_01` 的 Skin primitive（mesh primitive 2）只有真实的
`TEXCOORD_0`，没有 `TEXCOORD_1`：

```text
vertex count = 7243
index count  = 41124
UV range     = U=[0.0028000, 0.9972000], V=[0.0078000, 0.9914163]
```

源/目标 UV 范围在数值上重合；按位置和局部拓扑做匹配后，原样 UV 的误差低于上下翻转、
左右镜像和双翻转假设。由于 RenderDoc 源 draw 使用 GIM 合并 vertex/index buffer，不能用
目标 glTF 的局部 vertex index 直接与源 index 一一对应；本轮使用位置匹配而不是伪造 index 对齐。
因此当前结论是：**脸部没有遗漏 UV1，也不存在全局 Y 翻转、X 镜像或 UV 未还原。脸部采样应继续使用 UV0。**

贴图绑定也按源 shader 语义核对：

| RenderDoc 资源 | 源语义 | 目标资产 | 核对结论 |
|---|---|---|---|
| `14544` / `Tex0` | BaseColor RGB，sRGB | `T_nf2022_f_01_skin_BaseColor` | 面部、眼、唇和耳朵 UV 岛布局一致；颜色差异属于离线转换/材质 tint，不是 UV 翻转 |
| `14546` / `NormalMap` | RG=normal XY，B=curvature，A=detail mask | `skin_Normal` + `skin_SkinAux` | 只比较 Normal RG；B/A 已按合同拆入 SkinAux，不能把源 B 当 normal.Z |
| `14547` / `ParamMap` | R=roughness，G=metallic，B=skin mask，A=AO | `skin_SkinParam` | R/A 与源导出图布局近似逐像素一致，未发现翻转或岛偏移 |
| `14545` / `tex_s` | 抓帧中全透明黑 | 无绑定 | 保持 unsupported，不创建假贴图 |
| `DetailMap` | `uv0 * u_detail_tilling` | `skin_DetailNormal` | 仍使用 UV0；采样器保留源的 repeat 语义 |

源实现依据为 `pbr/nodes/pbr_skin_parameters.hlsl`、
`shaderlib/texture_samples.hlsl` 和 `pbr/nodes/pbr_skin_nodes.hlsl`：BaseColor、NormalMap、
ParamMap 都采样 `MaterialParameters.uv0`；NormalMap 的 B/A 是辅助通道，源代码把 normal.Z
固定为 `1.0`。因此后续若侧面画面仍有颜色或高光差异，应优先检查 BaseColor 的颜色空间/
离线转换、Normal RG 的重建、SkinAux 曲率和材质参数，不再修改脸部 UV 或强行接入 UV1。

### 9.6 纹理方向合同核对与最终约定（2026-08-26）

本轮曾根据 Blender/glTF 的局部画面暂时移除 `Texture` 的统一翻转，但随后核对 Git 历史
确认：从旧 `TextureLoader::stbi_set_flip_vertically_on_load(true)` 到 2026 年 6 月的
`TextureIO::FlipYMode::ForceOn`，文件型游戏材质纹理长期采用“解码后垂直翻转一次”的
引擎约定。临时取消该行为不符合既有资产基线，因此不作为最终合同。

最终约定恢复为：

- `TextureIO` 保留显式 `ForceOn/ForceOff` 行翻转能力，通用默认仍为 `ForceOff`；
- 所有文件型材质 `Texture` 固定选择 `ForceOn`，在 CPU decoded rows 上翻转一次后上传；
- Environment HDRI 明确选择 `ForceOff`，生成型 Vulkan 纹理不经过材质文件加载路径；
- `Texture::CreateDesc`、`TextureBindingLoadDesc` 和纹理缓存 identity 不携带方向开关；
- `T_*.json` 资产门禁继续拒绝 `flipY` 与其他未知字段，禁止逐资产覆盖引擎约定；
- Blender/glTF 与离线转换工具必须按该固定约定准备源图和 UV，Shader 不再追加临时 V 偏移。

此前 `skin_side_flipY_fixed.bmp` 属于临时取消引擎翻转时的诊断截图，不再作为当前合同的
视觉基线。后续侧面验收应在恢复统一翻转后的角色场景重新生成，并继续从颜色空间、法线
通道、SkinAux 曲率和材质参数定位剩余差异。

### 9.7 鼻梁亮斑资源归因（2026-08-26）

侧面 Debug View 进一步核对后，鼻梁的宽范围亮斑不是贴图岛错位：

- `T_nf2022_f_01_skin_BaseColor.png` 的鼻部颜色布局与源 TGA/Blender 资产一致；
- `T_nf2022_f_01_skin_Normal.png` 的 R/G 与源 `nf2022_f_01n.tga` 一致，负责鼻梁形体法线；
- `T_nf2022_f_01_skin_SkinAux.png` 的 R/G 分别由源 NormalMap.B/A 拆出，数值逐像素一致；
- `T_nf2022_f_01_skin_DetailNormal.png` 只有平铺的微孔法线，不可能产生宽范围鼻梁偏移；
- 亮斑在 Skin Virtual Light Debug View 中仍然出现，说明当前可见差异来自摄像机补光
  与 Skin 法线/BRDF 的响应，不应修改上述资源图像。

当前首先应检查 `MI_face_skin.json` 的 `u_skinCharacterLighting.w = 3.0` 与
`shader/glsl/engine/preintegratedSkinLighting.glsl` 中的 Virtual Light 累积；这属于
光照参数/Shader 响应问题，不是 BaseColor、Normal 或 SkinAux 的导出问题。
## 10. P4 执行记录（2026-08-25）

### 10.1 Shadow、Transmission 与 Virtual Light

- Skin 的真实方向光、点光和聚光继续统一经过 `result.shadow`；Transmission 只由真实光累积，Virtual Light 明确传入 `transmissionMultiplier = 0`。
- Virtual Light 的 diffuse/specular 在 PreintegratedSkinLighting 中独立分账，Deferred composition 不再让 CSM 阴影抹掉摄像机绑定补光。
- 当前目标没有独立 NeoX shadow-color 资产输入，因此没有把未证实的颜色硬编码进 LUT 或公共 evaluator；shadow color 保留为待源证据确认的有意差异。

### 10.2 Skin Debug View

新增 Debug View 74–79：

- 74：Skin direct diffuse；
- 75：Skin transmission；
- 76：Skin shadow visibility；
- 77：Skin IBL diffuse；
- 78：Skin IBL specular；
- 79：Skin Virtual Light diffuse + specular。

这些视图只在 Deferred Skin lighting snapshot 中填充，其他 shading model 保持中性默认值。

### 10.3 有意差异

- characterLighting.z 仍只跨 GBuffer 保存，不映射到当前引擎没有独立分类的 GI 光源。
- 源 Virtual Light 只在 Mesh Pass；VulkanLearn 的 Deferred 使用是显式兼容扩展，不代表源存在 Deferred 实现。
- `tex_s`、glitter、makeup、Rebirth 仍因抓帧资源证据不足留到 P5，不能用假噪声替代。
### 10.4 固定机位验收与 Debug 覆盖修复

固定机位冻结为：

```text
position = (0, 1.45, 1.45)
forward  = (0, 0, -1)
target   = (0, 1.45, 0)
scene    = SC_b_f_3725_p0.json
```

该机位下已确认槽位绑定：`07 - Default -> MI_eye`、`09 - Default -> MI_eye_edge`、
`08 - Default -> MI_face_skin`；脸部最终解析为 `shaderName=neoxSkin` 与
`SHADING_MODEL_PREINTEGRATED_SKIN`。

本轮发现并修复一个会污染验收结论的调试路径问题：`MI_eye_edge` 是透明前向材质，
会在 Skin Deferred Debug View 之后覆盖脸部区域，使 Debug 74–79 和部分通用视图出现
大片黑块。修复位于 `shader/glsl/engine/materialForwardOutput.glsl`：当 Debug View 为
74–79 时，非 Skin 前向材质返回零输出，只保留 Deferred Skin 快照。该过滤仅作用于
Skin 专项 Debug View，不改变 Beauty 或正常前向合成。

已重新采集以下固定机位证据，均位于资源目录的
`generated/screenshots/`：

- `skin_p4_debug12_shading_model_filtered.bmp`
- `skin_p4_debug74_direct_diffuse_filtered.bmp`
- `skin_p4_debug75_transmission_filtered.bmp`
- `skin_p4_debug76_shadow_filtered.bmp`
- `skin_p4_debug77_ibl_diffuse_filtered.bmp`
- `skin_p4_debug78_ibl_specular_filtered.bmp`
- `skin_p4_debug79_virtual_light_filtered.bmp`

视觉结论：Debug 12 正确显示脸部 Skin ID；Debug 74 显示脸部真实 Direct Diffuse；
Debug 75 在正面机位下基本为黑，符合正面 Transmission 较弱的预期；Debug 76 阴影
可见性总体接近全亮并保留局部遮挡变化；Debug 77 主要在脸部、肩膀、手臂和手部显示
IBL Diffuse；Debug 78 的 IBL Specular 较弱；Debug 79 对脸部和身体局部有可见贡献。

另外，身体与脸部 MI 已移除与 `M_neoxSkin.json` 默认值完全相同的
`u_skinCharacterLighting` 重复声明，遵守加载期材质默认值合同；脸部其他显式抓帧参数
保持不变。

后续复核发现脸部“像半透/没有显示”并非 Alpha 或 Transmission：脸部 BaseColor Alpha
为 `1.0`，`M_neoxSkin` 为 `Opaque`，关闭脸部 Transmission 及隐藏 `MI_eye_edge` 的
A/B 结果均未改变该现象；关闭 CSM 后脸部立即恢复。因此根因是发片 CSM 投影造成的
脸部欠曝。当前只在 `MI_face_skin` 增加显式 `u_skinCharacterLighting.w = 3.0` 作为
脸部摄像机 Virtual Light 校准，不修改公共 Skin evaluator、不改变 `MI_eye_edge` 的
透明 RenderState。
### 10.5 验证结果

```text
cmake --build build --target main subsurface_tests -j       passed
ctest -R "subsurface_contract|material_schema_contract"    2/2 passed
SC_b_f_3725_p0 --framesmoke 1 --exit-after-tests            exit=0
```
