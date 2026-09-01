# Shader Structure 与 Material Function 规范

## 状态

本文是 VulkanLearn 当前 Shader Structure 的正式渲染规范。

已确定的结构决策：

- MaterialInputs 使用 UE 风格的 Material Attributes 语义；
- Material Function（MF）使用显式输入/输出接口，并以自定义返回 struct 表达多个输出；
- 当前 M_*.json -> shaderEvaluation -> include 用法保持不变；
- MF 可以按需 include，可以嵌套并形成大型、复杂的 MF 组合；
- Shading Model 按 UE 对齐路线实现；
- 静态开关、Material Instance 参数和离线资源转换遵循 UE 风格的职责边界；
- module_* 名称保留，但暂不定义为材质作者可见的组合层；
- MeshPass、Shader Variant、缓存、热重载和差分验证纳入同一套结构合同。

MaterialFunctionContext 的最终字段集合和数据所有权仍待单独讨论，不在本文中冻结。

## 目标与非目标

### 目标

Shader 结构需要同时支持：

- UE 风格的 Material Attributes 和 Shading Model；
- 可复用、可嵌套的 Material Function；
- 角色材质中的视差、珠光、闪点、头发、皮肤和眼睛等功能；
- Base、Forward、GBuffer、ShadowDepth、Depth 等 MeshPass；
- 静态 Shader Variant 和动态 Material Instance 参数；
- Shader 编译缓存、反射校验、热重载和 GPU 资源事务发布；
- NeoX 等外部来源的离线纹理规范化，而不是运行时来源格式解码。

### 非目标

本文不定义：

- NeoX 原始纹理通道布局；
- NeoX .mtg、.nfx2 或 .nsf 的运行时解析方式；
- 某个角色的具体材质配方；
- 完整 Material Graph 编辑器；
- 新的二进制 Shader Module 或 SPIR-V Linker；
- CMake 运行时依赖。

## 分层结构

~~~text
离线资产转换
    -> VulkanLearn 标准纹理和材质数据

VertexFactory
    -> 规范化顶点、蒙皮、实例化和 Morph 输入

Material Function 组合
    -> MaterialInputs

Shading Model
    -> 光照、BRDF、透射和专用高光响应

MeshPass
    -> GBuffer、Forward、ShadowDepth、Depth 和状态输出

Post Process
    -> Bloom、Tone Mapping、Fog、SSR 等全局效果
~~~

### VertexFactory

VertexFactory 负责基础几何输入和几何变形：

- Static Mesh 输入；
- Skinned Mesh 蒙皮；
- Instancing；
- Morph Target；
- Previous Frame Position；
- 顶点坐标、法线、切线和 UV 的规范化。

蒙皮、实例变换和 Morph 不属于 Material Function。材质相关的 World Position Offset 可以在 VertexFactory 输出之后作为 Material Function 能力接入，但不能替代基础几何变形。

### Material Function

Material Function 是可复用、可嵌套的材质功能。它负责生成或修改材质输入，不直接完成最终光照。

MF 不得直接负责：

- 访问灯光列表；
- 读取 Shadow Map 并输出最终光照；
- 写入最终颜色附件；
- 直接执行 discard；
- 修改 Blend、Cull、Depth 等 Pipeline 状态。

### MaterialInputs

MaterialInputs 使用 UE 风格的 Material Attributes 语义。字段按用途分为核心输入、覆盖率/几何输入和 Shading Model 扩展输入。

核心输入包括：

~~~glsl
struct MaterialInputs
{
    vec3 baseColor;
    float metallic;
    float specular;
    float roughness;

    vec3 emissiveColor;
    float opacity;
    float opacityMask;

    vec3 normal;
    vec4 tangent;
    float ambientOcclusion;

    vec3 worldPositionOffset;
    float pixelDepthOffset;

    MaterialModelInputs modelInputs;
};
~~~

MaterialModelInputs 承载 Shading Model 专用数据，例如 Clear Coat、Anisotropy、Hair、Skin/Subsurface、Eye、Iridescence、Thin Translucent 和 Refraction。

扩展输入必须具有明确的 Shading Model 所有权。MF 不应直接争用无语义的 customData.xyzw 槽位。

### Shading Model

Shading Model 消费 MaterialInputs，负责 UE 对齐的光照响应。Shading Model 不知道材质来自 NeoX、Blender、glTF 还是其他来源，也不负责解释来源纹理的通道布局。

当前实现状态必须与目标结构分开记录：

| Shading Model | 当前状态 | 说明 |
|---|---|---|
| DefaultLit | 已实现 | Deferred 和 Forward 基础 PBR 路径。 |
| Unlit | 已实现 | Deferred 和 Forward 非光照路径。 |
| ClearCoat | 已实现 | Deferred 和 Forward，使用 GBufferD/customData 的双层法线与清漆输入。 |
| ThinTranslucent | 已实现 | Forward 专用；依赖 ThinTranslucent RenderMode，不等同于普通 AlphaBlend。 |
| PreintegratedSkin | 部分实现（Deferred MVP） | 已接入 Skin MaterialInputs、GBuffer/customData 和 Deferred evaluator；当前 M_neoxSkin 为 Opaque 路径，Forward 仍回退到 DefaultLit。 |
| Subsurface / SubsurfaceProfile | 待专项 | ID 已注册，但仍需独立完成光照闭包、GBuffer 和 Pass 合同。 |
| Hair | 已实现（VulkanLearn MVP） | 已补齐 HairMaterialInputs、Forward/Deferred evaluator、Hair GBuffer、Card coverage、ShadowDepth 和 Debug View 21–41；runtime validation 入口已接入但仍要求正式 authoring LUT，不声称 UE 私有 shader 逐行 parity。 |
| Eye | 待专项 | ID 已注册，虹膜、角膜和眼部 IBL 尚无独立实现。 |
| Cloth | 待专项 | ID 已注册，布料专用高光尚无独立实现。 |
| TwoSidedFoliage | 待专项 | ID 已注册，双面叶片/透光响应尚无独立实现。 |
| Iridescence | 待定义 | 当前没有独立的 Shading Model ID 和 GBuffer 合同。 |

对仍标记为“待专项”的模型，“已注册 ID”只表示资产校验和 GBuffer 编码可以识别该名称；Hair 是已完成专项实现的例外，
已补齐 Forward/Deferred 分发、MaterialInputs、GBuffer/customData 合同和 Debug View；runtime validation 入口已接入，但正式 authoring LUT 缺失时必须失败。其他专项仍需在各自合同中明确 BRDF、光照 Lobe、Pass 和验收边界。

### MeshPass

MeshPass 负责具体渲染路径和输出合同：Base GBuffer、Base Forward、ShadowDepth、DepthOnly、Velocity 以及其他明确注册的材质 Pass。

MeshPass 负责世界空间和裁剪空间变换、GBuffer/Forward 输出、Alpha Clip、Blend/Cull/Depth 状态以及 Pass 级资源和 Pipeline Contract。

MF 可以生成 opacityMask，但最终 Alpha Clip 由 MeshPass Template 统一执行，确保主 Pass 和 ShadowDepth 的覆盖率行为一致。

## Material Function 接口

### 输入和输出

MF 使用显式输入和输出结构。每个 MF 可以定义自己的输入和返回结构，不要求所有 MF 直接返回完整的 MaterialInputs。

~~~glsl
struct MFParallaxInput
{
    vec2 uv;
    vec3 viewDirectionTS;
    float scale;
};

struct MFParallaxOutput
{
    vec2 uv;
    float height;
};

MFParallaxOutput EvaluateMFParallax(
    in MaterialFunctionContext context,
    in MFParallaxInput input);
~~~

多输出 MF 必须通过返回结构表达命名输出，而不是依赖隐式全局变量或固定的 inout 全局材质状态。

### 嵌套和组合

MF 可以通过 include 组合其他 MF：

~~~text
MF_CharacterSurface
    -> MF_BaseColor
    -> MF_Normal
    -> MF_Pbr
    -> MF_Emission
    -> MF_Coverage
    -> MF_Parallax
~~~

大型 MF 组合是受支持的结构，不要求每个 MF 都直接连接到最终 MaterialInputs。复杂组合应通过清晰的输入/输出结构分层，避免多个 MF 隐式写入同一字段。

### MaterialFunctionContext

MaterialFunctionContext 用于承载多个 MF 共享的阶段数据，例如顶点阶段输入、片元阶段输入、UV、世界位置、法线、视线方向和时间信息。

当前已冻结以下原则：

- 共享的引擎阶段数据可以放入 Context；
- MF 私有的材质参数必须放入 MF 的 Input Struct；
- Context 不得成为任意全局资源访问入口；
- 灯光、Shadow Map 和最终 Pass 输出不通过 Context 暴露给普通 MF。

`MaterialFunctionContext` 的片元阶段字段以 `materialContext.glsl` 为准，只承载位置、法线、切线、UV、顶点色和裁剪空间信息；不得通过 Context 暴露灯光列表、Shadow Map 或最终 Pass 输出。

## M_、MI_ 与 MF 的职责

当前 M_*.json 的使用方式保持不变。每个母材质必须把两个公开求值入口与自身 JSON 放在同一目录：

~~~json
{
    "shaderEvaluation": {
        "vertex": "M_character.vertex.glsl",
        "surface": "M_character.surface.glsl"
    }
}
~~~

`M_character.json`、`M_character.vertex.glsl` 和 `M_character.surface.glsl` 必须同目录存在。
公开入口内部再按需 include `materialFunction/mf_*.glsl`；MF 文件不是母材质公开入口。
当前阶段不引入新的 Material Graph JSON schema，也不迁移现有 `shaderEvaluation` 字段。

职责划分如下：

~~~text
M_
    Material Function 组合、Shading Model、Render Mode、静态特征

MF
    可复用材质功能和嵌套组合

MI_
    纹理引用、动态参数和实例覆盖

MaterialShaderComposer
    按 M_ 入口和 include 关系生成最终 shader translation unit
~~~

MF 的参数和纹理仍必须进入当前 Material Schema 合同。MF 不应绕过 M_/MI_ 的参数生成、descriptor 分配和反射校验机制。

当前 NeoX Default/Silk/Pearl/Crystal 静态材质是该边界的落地示例：

- `M_neoxDefault.surface.glsl` 与 `M_neoxSilk.surface.glsl` 只选择各自的输入 MF；
- `mf_neoxPackedSurfaceTextures.glsl` 统一采样 Default/Silk 的标准化纹理，
  `mf_neoxPackedSurface.glsl` 解释共享表面语义，DefaultLit/Cloth MF 再补专用字段；
- `M_neoxPearl.surface.glsl` 只选择 Pearl 组合 MF；`mf_neoxPearlTextures.glsl` 负责图集/噪声采样，
  `mf_neoxPearlInputs.glsl` 负责 Pearl、Emission 和 Subsurface 的最终拼装；
- `mf_pearlescentInputs.glsl` 生成 Pearl 颜色、假球法线、覆盖率和 PBR 输入；
- `mf_emission.glsl` 负责 Base/Emission 能量拆分；
- `mf_subsurfaceInputs.glsl` 只返回 `SubsurfaceMaterialInputs`，不修改整份 `MaterialInputs`；
- `M_neoxCrystal.surface.glsl` 只选择 Crystal 输入 MF；`mf_neoxCrystalTextures.glsl` 负责贴图采样，
  `mf_neoxCrystalInputs.glsl` 负责 ThinTranslucent、Emission、coverage 和双面法线组装；
- Alpha Clip 和 Cull 仍分别由 MeshPass 与 M_ Render State 负责；
- Billboard 几何展开不属于片元 MF，当前 Pose 版本在离线 glTF 中烘焙。

M_ 的参数 include 必须先于 `materialSurface.glsl` 和 `M_*.surface.glsl` 进入 Fragment translation unit，
以便 `MATERIAL_SHADING_MODEL` 在 Engine 默认值生效前完成定义；这一顺序由
`source/material/compiler/materialShaderComposer.cpp` 统一保证。

组合 MF 应优先返回其拥有的专属输出结构。除非该 MF 明确拥有完整表面生成职责，否则不得通过 `inout MaterialInputs` 或复制整份 `MaterialInputs` 隐式修改其他功能字段。

## 静态开关、动态参数和离线转换

一个功能可能同时包含算法、静态选项、动态参数和来源资源转换。这四类内容必须分开。

### 静态 Shader 选项

会改变 shader 代码、资源布局、顶点输入或渲染状态的内容属于静态选项，并进入 Shader Variant Identity，例如：

~~~text
USE_PARALLAX
PARALLAX_MODE
USE_CLEAR_COAT
SHADING_MODEL
RENDER_MODE
TWO_SIDED
~~~

改变静态选项需要重新生成、编译和验证 Shader Variant。

### Material Instance 参数

只改变运行时数值、不改变 shader 结构的内容属于 MI 动态参数，例如：

~~~text
parallaxScale
pearlIntensity
sparkleColor
roughness
emissionStrength
~~~

这类参数通过现有 Material UBO/descriptor 机制更新，不因为数值改变而重新编译 shader。

### 离线资产转换

来源格式相关的工作全部在离线转换阶段完成，包括：

- Glossiness 到 Roughness 的转换；
- 通道重排和纹理打包；
- Normal Y 方向统一；
- 颜色空间转换；
- Mask 拆分或合并；
- 固定颜色修正；
- NeoX 特定纹理语义到 VulkanLearn 标准纹理语义的映射。

运行时 MF 只消费 VulkanLearn 标准纹理，不包含 NeoX Decode Function。

## 功能分类规则

NeoX 或其他来源的效果逐项判断，不进行机械一比一迁移。

### Material Function

适合做成 MF 的能力：

- BaseColor 组合；
- Normal 组合；
- PBR 输入生成；
- Emission；
- Coverage/Opacity Mask；
- Parallax；
- Pearl 输入；
- Sparkle 输入；
- Hair、Skin、Eye 的材质输入。

### Shading Model

如果效果改变 BRDF、光照 Lobe、透射或能量分配，应归入 Shading Model 或其扩展：

- Hair 高光 Lobe；
- Skin/Subsurface；
- Clear Coat；
- Iridescence；
- Thin Translucent；
- Eye/Cornea 光照响应。

### MeshPass 或 Engine

以下内容不属于普通 MF：

- Alpha Clip 的最终 discard；
- Blend、Cull、Depth 状态；
- Shadow Map 生成和采样；
- Fog；
- SSR；
- Bloom；
- Tone Mapping；
- 其他依赖完整场景或屏幕空间的效果。

## module_* 保留规则

module_* 名称保留，但本规范暂不把它定义为材质作者可见的组合抽象。

未来的 module_* 可以用于：

- MF 内部共享算法；
- TBN、Fresnel、Noise、Normal Blend 等底层 helper；
- 多个 MF 共用的 GLSL 实现；
- 未来独立的 Shader 构建/链接单元。

在用途未确定前，module_* 不得拥有独立的 Material Function Pin、Material Instance 参数合同或 Shading Model 输出语义。

## MeshPass 与大型 MF 组合

大型 MF 组合应保持一个材质求值入口，由不同 MeshPass 使用相同的材质语义合同。

~~~text
Material Evaluation
    -> MaterialInputs

Base Pass
    -> 读取颜色、法线、PBR、Emission、Coverage

ShadowDepth
    -> 读取位置、Coverage 和必要的顶点变形

DepthOnly
    -> 读取位置和必要的 Coverage
~~~

第一阶段继续采用源码级 include 和现有 Composer。不要为了大型组合立即引入 SPIR-V Module Linker。后续如果编译时间、descriptor 裁剪或热重载影响范围成为实际瓶颈，再增加基于 MF 依赖的 Pass-specific 裁剪。

## Shader 构建、缓存和热重载

MF include 关系属于 Shader Source Dependency Graph 的一部分。

MF 修改必须能够传播到：

~~~text
MF
    -> M_ Material Evaluation
    -> Base/Shadow/其他 Pass Variant
    -> Pipeline Candidate
~~~

Shader Identity 至少继续包含：

- Material Source；
- Material Pass；
- VertexFactory；
- Shading Model；
- Render Mode；
- 静态 MF 组合和静态选项；
- Descriptor Schema；
- 编译策略和目标平台。

实现必须继续遵循现有 BLAKE3-256 Shader Cache、批量发布、反射校验、热重载事务和 GPU Epoch Retirement 合同。

## 验证合同

Shader 对齐不能只比较最终截图。固定角色、Pose、相机、灯光、环境、分辨率、曝光和后处理后，按以下层级验证：

~~~text
Geometry / Silhouette
    -> BaseColor
    -> Normal
    -> Roughness / Metallic / AO
    -> Opacity / Coverage
    -> Direct Lighting
    -> Shadow
    -> Emission
    -> Post Process
~~~

每层都应尽量提供可视化 debug view 或中间输出，以区分资源转换、MF 组合、Shading Model、MeshPass 和 Post Process 的误差来源。

## 当前落地顺序

1. 冻结 UE 风格 MaterialInputs 核心字段和模型扩展字段；
2. 继续讨论并冻结 MaterialFunctionContext 的字段和所有权；
3. 保持现有 M_ 和 shaderEvaluation 用法，按需 include MF；
4. 用现有 PBR 材质验证大型 MF 组合；
5. 按 UE 风格区分静态开关、MI 参数和离线转换；
6. 为角色迁移定义 NeoX Compatibility Profile；
7. 逐个实现并验证角色的 Parallax、Pearlescent、Sparkle、Hair、Skin 和 Eye MF/扩展 Shading Model；
8. 最后再评估是否需要 module_* 的独立构建语义或 Pass-specific MF 裁剪。
