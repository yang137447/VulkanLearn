# NeoX 角色资源与 Shader 对齐合同 V1

## 状态

- **版本**：V1
- **状态**：已确认，作为 NeoX 角色迁移的当前合同；实现仍按本文分阶段补齐
- **适用范围**：`.mtg`、`.fx/.nfx2`、NeoX 纹理、glTF 角色网格、VulkanLearn `M_*.json` / `MI_*.json`、Material Function、RenderPass
- **参考角色**：`b_f_3725`
- **角色 Shader 双源根目录**：`K:\future\res\shader`
- **双源职责**：`.fx` 保存美术宏、参数声明及其编辑器默认值；`.nfx2` 保存总宏展开后的最终宏环境、Uniform 节点及最终默认值。两者必须成对核验，不能只读取其中一份。
- **不承诺**：NeoX 私有光照逐行复刻、完整动画运行时、动态妆容/Texture2DArray glitter、SceneColor 旋转折射、生产级 OIT

本文是当前 NeoX 角色迁移的正式渲染合同。角色迁移必须先满足资源和 RenderState 合同，再进入 Shader 效果对齐；不能用 Blender 画面、文件名相似度或 VulkanLearn 当前已有 MI 反推 NeoX 语义。

NeoX 材质 Shader 必须同时遵守 `documents/rendering/shader-structure-and-material-function.md` 和
`documents/rendering/material-mesh-pass-composition.md`。本文只补充 NeoX 的源语义、离线标准化和源到目标的映射，不得覆盖通用 Shader Structure、Material Function、MeshPass 或 Material Instance 合同。

## 1. 对齐目标与原则

### 1.1 对齐目标

对齐目标是：在 VulkanLearn 的既有 Shading Model 和 RenderPass 能力范围内，保留 NeoX 角色资产的可验证语义，并对暂不支持的能力显式记录差异。

目标不是把所有材质都转换成看起来相似的 Principled BSDF，也不是用单一 `Opaque` / `TransparentAlphaBlend` 覆盖所有材质族。

### 1.2 四条硬规则

1. `.mtg` 的 `<RenderStates>` 与 `<ShaderMacro>` 是材质 RenderState 和编译特征的权威源。
2. `.fx/.nfx2` 及其 HLSL 节点是参数、贴图通道、UV 和 Alpha 语义的权威源。
3. RenderDoc 只用于验证实际 draw 绑定和运行状态，不能替代源资源合同。
4. 目标引擎不支持的状态必须标记为缺口，不得静默降级成 `Opaque`、`OpaqueClip` 或普通透明。

### 1.3 事实源优先级

```text
.mtg RenderStates / ShaderMacro
    -> .fx / .nfx2 / HLSL 参数和贴图语义
    -> MTG 引用的原始纹理与源网格
    -> glTF / VulkanLearn 资产
    -> RenderDoc 验证
    -> Blender 预览
```

Blender/glTF 负责承载几何、法线、材质槽和真实 UV；它不负责决定 NeoX 的透明模式、AlphaRef、Shader Macro 或 ParamMap 通道合同。

## 2. 迁移流水线

每个角色必须经过以下阶段。阶段之间以可审计的中间产物连接，禁止直接从 MTG 生成最终 MI 并丢弃源字段。

```text
NeoX 资源根
  -> MTG/XML 解析
  -> NXRawMaterialContract
  -> NXResolvedMaterialContract
  -> 纹理/网格离线转换
  -> glTF + Texture JSON + MI/M 映射
  -> Shader Reflection / Pipeline 合同校验
  -> 运行时 draw / RenderDoc 验证
```

### 2.1 阶段 A：源资源盘点

盘点输出必须包含：

- 使用的 `.mtg` 文件、相对源路径和源文件哈希；
- 每个 `Material_*` wrapper 的顺序、类型和槽位名；
- `Technique`、`ShaderMacro`、`ParamTable` 原始内容；
- 所有 `RenderStates` 原始属性；
- MTG 引用的纹理、`.fx/.nfx2` 和网格路径；
- 源网格的材质槽顺序、UV0/UV1 实际存在情况和几何变换。

缺失路径、无法解析的 wrapper 或重复槽位必须在盘点阶段报告；不能用 fallback 继续生成可发布 MI。

### 2.2 阶段 B：保真解析

解析器先生成 `NXRawMaterialContract`，至少包含：

```text
sourceMtg
sourceMaterialSlot
sourceSlotIndex
technique
shaderMacros
numericParameters
textureParameters

renderState:
    transparentModeRaw
    newTransparentModeRaw
    effectiveTransparentMode
    alphaRefRaw
    alphaValRaw
    cullBack

sourceTextureResolvedPaths
sourceHash
contractHash
```

规则：

- `NewTransparentMode` 有效且大于 `0` 时优先于 `TransparentMode`；
- `TransparentMode=0` 必须保留为 raw 值，再依据 AlphaRef 计算 effective mode；
- `AlphaRef`、`AlphaVal` 保留 0~255 原始值，同时生成归一化值；
- 原始宏值保留字符串和解析后的布尔/整数表示；
- 相同 Shader 但不同 RenderState、参数、宏、贴图或 UV 合同的槽位不能合并；
- 合同哈希只用于审计和身份，不得使用时间戳、`std::hash` 或截断摘要。
#### 源参数默认值与有效值

NeoX 的 ParamTable 只表示材质槽显式写入的值，未出现的参数不能当作“没有语义”。
对每个源参数必须按以下固定优先级解析有效值：

```text
MTG ParamTable 显式值
    > Technique 所属 .fx/.nfx2/HLSL 节点声明的默认值
    > 已批准并记录原因的 migrationConstant
    > 解析失败（禁止发布目标 MI）
```

规则：

- 显式 ParamTable 值必须保留原始字符串、解析类型和标准化数值；
- 缺失参数必须记录 `sourceDefault`，包括默认值所在文件、节点/符号和源文件哈希；
- `migrationConstant` 只能用于目标引擎明确不支持源能力的有意差异，必须同时记录
  `reason`、`targetParameter` 和 `intentionalDifference`，不能用来掩盖解析缺失；
- 目标 `M_*.json` 的 `default` 必须来自上述源有效值的标准化映射，或是有审计记录的
  `migrationConstant`；目标 MI 只写相对 M_ 默认值的显式差异；
- 任何未解析、未归因或只依赖运行时 Shader 隐含默认值的参数，都属于迁移缺口，不能生成
  可发布的 MI 或在画面对比中按“经验值”继续调参。

迁移清单的每个槽位至少应能反查：

```text
sourceParameter
resolvedValue
valueOrigin: mtgOverride | sourceDefault | migrationConstant
originFile
originSymbol
originSha256
targetParameter
targetDefault
```

### 2.3 阶段 C：标准化资源

离线转换可以把 NeoX 打包贴图拆成标准语义资产，但必须保存来源和通道合同：

```text
source path / source hash
generated path
color space
wrap mode
filter / mip policy
UV set
channel semantics
conversion version
```

标准化允许拆分 `ParamMap`、`SurfaceMap`、Skin 辅助图和 Detail 图；不允许在运行时每帧重新猜测或解释已经可以离线确认的通道。

`Tex0.A` 不能跨材质族统一解释。它可能是连续 opacity、Alpha Test coverage、Hair coverage，也可能只是 Skin/控制图中不应参与透明的通道。

### 2.4 阶段 D：目标资产生成

目标资产至少包括：

- glTF 网格及材质槽顺序；
- `Texture JSON V1` 纹理资产；
- `M_*.json` 母材质；
- `MI_*.json` 材质实例；
- 源槽位到目标 MI 的逐槽映射；
- `conversion-manifest.json` 或等价审计清单。

目标 MI 必须可以反查到一个或多个源槽位；共享 MI 只允许合并完整合同相同的源槽位。

## 3. NeoX RenderState 合同

NeoX `TransparentMode` 的定义以 `C:\Software\WorkTemp\G66ShaderDevelop\shadercompiler\check\rules.py` 和 NeoX 引擎 RenderState 语义为准。

### 3.1 模式映射

| Raw Mode | NeoX 语义 | Blend | ZWrite | Alpha Test | 目标路由 |
|---:|---|---|---:|---:|---|
| `0` | `UNSET`；AlphaRef>0 时按 mode 3，否则按 mode 1 | 由 effective mode 决定 | 由 effective mode 决定 | 由 effective mode 决定 | 先解析，不直接发布 |
| `1` | `OPAQUE` | None | 是 | 否 | `Opaque` + Geometry |
| `2` | `ALPHA_R_Z` | Alpha | 否 | 否 | `TransparentAlphaBlend` |
| `3` | `ALPHA_TEST` | None | 是 | 是 | `OpaqueClip` + Geometry |
| `4` | `ALPHA_RW_Z` | Alpha | 是 | 否 | 专用透明写深度路径 |
| `5` | `BLEND_ADD` | Additive | 否 | 否 | `TransparentAdditive` |
| `6` | `ALPHA_RW_Z_TEST` | Alpha | 是 | 是 | 专用透明写深度 + Alpha Test 路径 |
| `7` | `ALPHA_R_Z_TEST` | Alpha | 否 | 是 | 透明 + Alpha Test 路径 |
| `8` | `ALPHA_TEST_BLEND_HAIR` | Alpha | 否 | 是 | Hair 专用两遍/coverage 路径 |

Alpha Test 阈值为：

```text
alphaTestThreshold = AlphaRef / 255.0
```

`AlphaRef>0` 不能单独把材质变成 Alpha Test；只有 effective mode `3/6/7/8` 才启用 Alpha Test。`AlphaVal` 必须保留并进入审计，除非源 Shader 合同明确规定其如何参与最终 Alpha，否则不得擅自当作 AlphaRef 或透明度乘数。

### 3.2 Cull 与宏的关系

- `CullBack=True` 映射为 Back Face Culling；
- `CullBack=False` 映射为 Two-sided；
- `TWO_SIDE_ENABLE` 等 Shader Macro 仍必须保留，因为它可能控制 Shader 内法线、背面或光照分支；
- RenderState 与宏不能互相覆盖。两者不一致时必须报告合同冲突，而不是静默选择一个。

### 3.3 目标引擎能力状态

VulkanLearn 已有 `Opaque`、`OpaqueClip`、`TransparentAlphaBlend`、`TransparentAlphaBlendWriteDepth`、`TransparentAdditive` 和 `ThinTranslucent`。普通 `forwardTransparent` pass 默认不写深度；`TransparentAlphaBlendWriteDepth` 复用同一个 RenderPass，但在材质管线合同中显式开启 DepthWrite，因此不会修改环境配置或复制 Pass。

mode 4 的目标能力必须显式建模为以下之一：

```text
TransparentAlphaBlendWriteDepth
```

或：

```text
forwardTransparentDepthWrite
```

其最低合同为：

- Alpha Blend：`SrcAlpha, OneMinusSrcAlpha`；
- Depth Test：开启；
- Depth Write：开启；
- Alpha Test：关闭；
- 绘制顺序：在普通不透明/Alpha Test 之后、普通不写深度透明之前；
- 不进入普通 GBuffer Geometry MRT 路径；
- 阴影策略单独声明，不能默认复制普通 Opaque 阴影。

该能力现已用于普通 Default/Silk mode 4 槽位。mode 6/7 仍需要同时表达 Alpha Test 与透明混合，未实现前只能报告为 `unsupportedTargetRenderState`，不能静默降级。

## 4. Shader 家族与输入合同

### 4.1 Technique 映射

| NeoX Technique | VulkanLearn 目标 | 对齐边界 |
|---|---|---|
| `pbr_skin.fx` | `PreintegratedSkin` | 身体/脸部 Skin 输入拆分；不把 Skin 控制通道当透明 |
| `pbr_default.fx` | `DefaultLit` | Base/PBR/Surface/Emission/Sparkle |
| `pbr_silk.fx` | `Cloth` | Silk sheen、Flow、Emission、Sparkle、两面性 |
| `pbr_subsurface_billboard.fx` | `Subsurface` / Pearl | coverage、MatCap、真实 UV1、Noise |
| `pbr_hair_transparent.fx` | `Hair` | Hair coverage、RDI、Alpha Test + Blend |
| `pbr_crystal.fx` | `ThinTranslucent` | 透射/coverage/caustic 近似；必须标记源差异 |

本文不新增 Shading Model。目标 Shader 只能使用已有模型；若源语义无法在现有模型中表达，记录 `unsupportedShaderFeature` 并进入后续专项。

### 4.2 M_、MF、Shading Model 与 MeshPass 分层

NeoX 材质必须采用“`M_` 薄组合、`mf_*` 功能封装、Shading Model 光照、MeshPass 状态与输出”的结构。源 Shader 的功能不能因为迁移方便而全部堆进 `M_*.surface.glsl`。

```text
标准化 NeoX Texture JSON / MI 参数 / 静态宏
    -> mf_*：采样、通道语义、覆盖率、法线、表面功能和模型输入
    -> M_*.surface.glsl：组合 MF，组装 MaterialInputs
    -> Shading Model：消费 MaterialInputs，完成光照闭包
    -> MeshPass Template：Alpha Clip、Pass 输出、Blend/Cull/Depth 合同
```

#### `M_` 母材质职责

`M_*.json` 和对应的 `M_*.vertex.glsl` / `M_*.surface.glsl` 只负责：

- 声明静态 `shaderEvaluation`、`renderStates`、参数、纹理和宏；
- 选择并组合一个或多个 `mf_*`；
- 把 MF 的显式输出接入 `MaterialInputs` 和对应 `modelInputs.*`；
- 提供极少量与该母材质 ABI 绑定的默认值和输入 wiring；
- 保持同一母材质可以被 Base、Forward、ShadowDepth 等模板复用。

`M_*.surface.glsl` 不得：

- 直接解释原始 NeoX `.tga` / packed ParamMap / SurfaceMap / RDI 格式；
- 复制另一材质族的贴图采样和通道解码逻辑；
- 直接完成灯光、BRDF、Shadow Map 或最终颜色输出；
- 直接执行 `discard` 或根据源文件路径改变行为；
- 通过 Shader Alpha 绕过 MTG 解析出的 Blend、Cull、Depth 和 Pass 状态。

M_ 中出现的每一次直接 `texture()` 调用都必须能说明其不是可复用功能；涉及 NeoX 通道、coverage、detail、emission、sparkle、flow、hair、skin、pearl 或 crystal 的采样必须下沉到 MF。新代码不得在 M_ 中新增这类直接采样。

#### `mf_*` 功能模块职责

MF 使用显式输入/输出结构，单个 MF 只拥有一个清晰的功能边界，并且只生成或修改 `MaterialInputs` / `MaterialModelInputs`，不完成最终光照。

推荐的 NeoX MF 分层如下：

| MF 层 | 责任 | 允许消费 |
|---|---|---|
| NeoX packed surface MF | 标准化 BaseColor/Normal/Param/Surface 输入与 NeoX 表面辅助量 | 规范化 Texture JSON、材质参数、静态宏 |
| Coverage / opacity MF | 把源 coverage/opacity 变成 `opacity` 和 `opacityMask` | BaseColor.A、coverage 图、Alpha 参数 |
| Normal/detail MF | 切线空间法线、Detail Normal、curvature、detail mask 合成 | Normal/SkinAux/DetailNormal |
| Emission / sparkle / flow MF | 发光、Fresnel、闪点和流光遮罩 | Surface 语义图、材质参数、UV |
| Skin MF | SkinParam、SkinAux、DetailNormal 到 Skin MaterialInputs | Skin 标准化贴图和 Skin 参数 |
| Silk/Cloth MF | Silk Surface、sheen、anisotropy、flow 相关输入 | Surface/PBR/Normal 和 Cloth 参数 |
| Pearl/Subsurface MF | Pearl coverage、MatCap、Noise、Subsurface 输入 | Pearl 贴图、UV0/UV1 和参数 |
| Hair MF | Hair coverage、RDI、root/depth/strand/AO 和 Hair 输入 | Hair BaseColor/Normal/RDI 和 Hair 参数 |
| Crystal/ThinTranslucent MF | Crystal coverage、厚度、caustic/emission 和薄透输入 | Crystal mask、BaseColor/Normal 和参数 |

MF 的硬边界：

- 不访问灯光列表、Shadow Map 或最终 SceneColor 光照结果；
- 不写颜色附件，不创建或切换 Pipeline；
- 不执行 `discard`；Alpha Test 只输出 `opacityMask`，由 MeshPass Template 统一消费；
- 不修改 Blend、Cull、Depth、排序或 Pass 路由；
- 不读取原始 NeoX 文件路径，运行时只消费离线生成的标准语义资源；
- 不使用无语义的 customData 通道，扩展数据必须归属对应 Shading Model。

#### Shading Model 与 MeshPass 职责

- Shading Model 只消费 `MaterialInputs`，不能知道输入来自 NeoX、glTF 或其他来源；
- Shading Model 负责 BRDF、透射、Subsurface、Hair、Cloth 或 Eye 等光照闭包；
- MeshPass Template 统一执行 Alpha Clip、Pass 输出和模板级坐标/深度处理；
- RenderState 合同决定 Blend、ZWrite、Cull、排序和目标 Pass，不能由 MF 或 Shading Model 临时覆盖；
- Base 与 ShadowDepth 必须使用同一覆盖率语义，避免主 Pass 与阴影边缘不一致。

#### MF 调用约定

NeoX `M_*.surface.glsl` 应保持接近以下形态，具体输入结构按功能模块定义：

```glsl
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();

    MFNeoXPackedSurfaceOutput packed =
        EvaluateMFNeoXPackedSurface(context);
    inputs = ApplyMFNeoXPackedSurface(inputs, packed);

    MFNeoXSilkOutput silk = EvaluateMFNeoXSilk(context, packed);
    inputs = ApplyMFNeoXSilk(inputs, silk);
    return inputs;
}
```

示例只表达结构，不规定所有 MF 必须返回完整 `MaterialInputs`。功能 MF 优先返回命名明确的输出结构，M_ 负责最终 wiring；不要通过隐式全局变量在 MF 之间传递源语义。

#### 当前实现差异

- `M_neoxDefault.surface.glsl` 与 `M_neoxSilk.surface.glsl` 已采用单入口 MF 组合路径；
  共享采样位于 `mf_neoxPackedSurfaceTextures.glsl`，共享表面语义位于
  `mf_neoxPackedSurface.glsl`，DefaultLit/Cloth 专用约束分别位于
  `mf_neoxDefaultInputs.glsl` 与 `mf_neoxSilkInputs.glsl`；
- `M_neoxPearl.surface.glsl` 已采用单入口 Pearl 组合 MF；UV/图集与噪声采样位于
  `mf_neoxPearlTextures.glsl`，Pearl/Emission/Subsurface 拼装位于
  `mf_neoxPearlInputs.glsl`；
- `M_neoxHair.surface.glsl` 已通过 `mf_neoxHairTextures.glsl`、`mf_neoxHairFiberFrame.glsl` 与
  `mf_neoxHairInputs.glsl` 下沉 Hair BaseColor、Normal、RDI、fiber axis、coverage 与模型输入；
  母材质只保留参数接线；
- `M_neoxCrystal.surface.glsl` 已采用单入口 Crystal MF；Crystal 贴图采样位于
  `mf_neoxCrystalTextures.glsl`，ThinTranslucent/Emission/coverage 组装位于
  `mf_neoxCrystalInputs.glsl`；
- `mf_preintegratedSkinInputs.glsl` 保持通用 Skin 包装；NeoX 皮肤使用 `mf_neoxSkinTextures.glsl` / `mf_neoxSkinInputs.glsl`，其它可复用模块包括 `mf_pearlescentInputs.glsl`、`mf_emission.glsl` 和 `mf_neoxPackedSurface.glsl`，不得在各个 M_ 中复制其逻辑。

本轮只收口其它 NeoX 材质的 MF 抽离与拼装；现有 Skin MF 仍直接消费对应生成参数 include 中的 `u_skin*` 全局字段，
这是当前 Skin 方案的既有实现边界，不在本轮视觉调参范围内。Skin 的 Deferred Opaque 路径已接入，Forward `PreintegratedSkin`
分支仍属于后续公共 Shading Model 专项，不能因本轮其它材质结构收口而宣称完整。

上述结构差异已清零；当前角色仍只标记为“资源合同已对齐、Shader 结构已对齐、视觉细节待调”，
因为 Crystal 的 SceneColor 折射、Pearl 的动态 Billboard 与 Silk/Default 的源时钟仍属于已记录的有意差异。

### 4.3 通用 MaterialInputs

所有目标 Material Function 必须明确填写以下字段的来源：

```text
baseColor
opacity
opacityMask
roughness
metallic
specular
ambientOcclusion
normal
tangent
emissiveColor
modelInputs.*
```

透明语义分离如下：

- `opacity`：连续 Alpha Blend 的源颜色权重；
- `opacityMask`：Alpha Test / coverage 的裁切依据；
- Opaque Skin：通常输出 `opacity=1`、`opacityMask=1`，不能默认读取 `Tex0.A`；
- Alpha Test：由 effective mode 控制 discard，阈值来自 `AlphaRef/255`；
- Alpha Blend：不能用 Alpha Test 替代连续 opacity；
- RenderState 决定 Blend、ZWrite、Pass 和排序，Shader 不能通过改 Alpha 绕过 RenderState 合同。

### 4.4 Default 与 Silk

NeoX 默认通道合同：

```text
Tex0.RGB      BaseColor
Tex0.A        材质族定义的 opacity / coverage
ParamMap.R    Roughness
ParamMap.G    Metallic
ParamMap.B    Thickness / 材质辅助量
ParamMap.A    Ambient Occlusion
```

Silk `SurfaceMap`：

```text
R    Mask1 / 换色区域
G    Emission / Pearl / Flow Mask
B    Sparkle Mask
A    Detail / 2U 相关 Mask
```

只有源宏启用对应功能时，目标 Shader 才绑定并消费对应 Surface 语义；不允许因为纹理存在就自动改变材质 RenderState。

### 4.5 Skin

身体和脸部 Skin 使用独立的 Skin 辅助输入：

```text
SkinParam.R    Roughness
SkinParam.G    Metallic
SkinParam.B    Skin Color Mask
SkinParam.A    Ambient Occlusion

SkinAux.R      Curvature
SkinAux.G      Detail Normal Mask

DetailNormal.RGB  Detail Normal XYZ
DetailNormal.A    Pore / Detail Modulation
```

Skin 的 BaseColor、Normal、SkinParam、SkinAux、DetailNormal 必须在离线阶段记录来源。Skin 的 `Tex0.A` 不得被通用透明逻辑消费，除非对应 MTG/源 Shader 明确把它定义为 opacity。

### 4.6 Hair

Hair mode 8 的输入合同为：

```text
Tex0.A    Base coverage / opacity
RDI.R     Root
RDI.G     Depth
RDI.B     Strand ID
RDI.A     Ambient Occlusion
```

mode 8 必须：

- 识别 `NewTransparentMode=8`；
- 启用 Alpha Test；
- 保留 ClipValue 到连续 opacity 的归一化；
- 使用 Hair 专用排序和 coverage 路径；
- 不把它降级成普通 Opaque 或普通 Alpha Blend。

当前 `b_f_3725` 在正式 Material Multi-Pass 落地前使用两份场景资源表达同一 Hair
primitive：Hair-only `OpaqueClip` Core 先写 GBuffer、Depth 和 Shadow，原角色中的
`TransparentAlphaBlend` Fringe 后绘制。两份 MI 共享 `M_neoxHair`、纹理、参数和
`mf_neoxHairInputs.glsl`；Core 使用原始 `Tex0.A` 做 Clip，Fringe 使用
`saturate(Tex0.A / ClipValue)`。相同几何和变换配合 Base Pass 的 `LESS` 深度比较，
使 Core 已覆盖的像素拒绝第二遍 Blend，只留下低于 ClipValue 的透明发梢。

源 MTG 的 `AlphaRef=51` 必须继续保留在 raw RenderState 审计中；但该
`pbr_hair_transparent` 双 Pass 分支实际使用未被 MTG 覆写的
`u_two_pass_clip_value=0.5`。当前两份 MI 因此统一使用 `0.5`，不能用 `51/255`
替代源 Shader 的双 Pass ClipValue。

Hair `Backlit` 必须先经过 NeoX 输入层再进入 UE Hair Shading Model；其参数默认值也必须遵守上面的源值优先级：

- `u_backlit_intensity` 只是最大强度，不是最终逐像素 `Backlit`；
- 源输入层使用几何 `NoV`、RDI.R root mask、`u_root_intensity`、AO 与专用方向项构造遮蔽；当前 MF 先保留不依赖灯光的 root gate，非零 root 的完整方向合同仍待接入；
- `h_f_3725_high_0` 没有在 MTG 中覆写 `u_root_intensity`，因此必须从
  `pbr_hair_transparent_nodes.hlsl` 的 `u_root_intensity=0.0` 解析源默认；该默认只关闭
  NeoX 的辅助 Backlit 输出，不关闭主 TT/TRT 路径；
- 不允许把默认 `u_backlit_intensity=0.5` 直接写入 Hair GBuffer。它必须先经过 RDI root、
  AO 和作者强度组合，不能伪装成 TT 主路径的颜色或能量；
- NeoX 源 `u_dir_direction` 加项暂未成为目标作者参数，不得塞入 UE R/TT/TRT 公式。

### 4.7 Pearl 与 Crystal

- Pearl `Tex0.A` 是 coverage，mode 3 时按 Alpha Test 处理；
- Crystal `Tex0.A` 是 coverage，但 mode 3 与 mode 4 的 RenderState 仍必须分别保留；
- Crystal 的 `ThinTranslucent` 只表示目标模型的透射近似，不代表已经完成 NeoX SceneColor 折射；
- 缺少 SceneColor 旋转采样、深度偏移或动态资源时，必须在 manifest 中写明 intentional difference。

## 5. 纹理、UV 与几何合同

### 5.1 纹理

所有 Texture JSON 必须遵守 `documents/rendering/texture-asset-json-v1.md`，并补充 NeoX 来源审计：

```text
sourcePath
sourceHash
sourceSlot
semantic
colorSpace
wrapMode
uvSet
channelDescription
conversionVersion
```

规则：

- BaseColor 通常为 sRGB，但必须由源 Shader 语义确认；
- Normal、Param、Surface、RDI、Skin 辅助图默认按线性数据处理；
- Repeat/Clamp 必须按源采样行为确定；
- 文件型材质纹理由 `Texture` 在上传前统一执行一次 decoded-row 垂直翻转；Blender/glTF 导出链路必须按该引擎约定准备源图与 UV；`T_*.json` 禁止声明 `flipY`，Shader 也禁止再次做垂直补偿；
- 禁止缺图静默 fallback；
- 禁止把不同 ParamMap、SurfaceMap 或 RDI 仅按尺寸/文件名相似度合并。

### 5.2 UV

- 源网格只有真实 UV0 时，只导出 `TEXCOORD_0`；
- 源网格存在真实 UV1 时，原样导出 `TEXCOORD_1`；
- 禁止用 UV0 复制或合成假的 UV1；
- MTG/Shader 使用的 UV1 语义必须进入材质合同；
- 目标 Shader 的采样 UV 必须与源节点合同一一对应。

### 5.3 槽位与几何

- glTF 材质槽顺序必须与源模型可反查；
- 几何导出不改变源材质槽身份；
- 静态 Pose、Skeleton/Animation、动态 Billboard 必须分开标注；
- Blender 生成的几何不能被称为“NeoX 运行时装配已恢复”。

## 6. MI 合并与身份合同

源槽位只有在以下字段全部一致时才允许共享目标 MI：

```text
Technique
ShaderMacro
Numeric parameters
Texture paths / texture semantics
Effective TransparentMode
AlphaRef
AlphaVal
CullBack
UV requirements
ShadingModel-specific features
```

共享 MI 不得丢失源槽位列表。建议在迁移清单中保存：

```text
targetMaterialInstance
sourceMtgFiles[]
sourceSlots[]
contractHashes[]
```

如果两个槽位只是在颜色或纹理上相似，但 mode、AlphaRef、Cull 或宏不同，必须拆成不同 MI。

## 7.1 角色核验的热更优先原则

角色材质调试必须灵活应用热重载，不能把“重启程序”作为默认验证手段：

- 仅修改 `M_*.json` 默认值、`MI_*.json` 显式参数/纹理/宏、Material Function 或可热更 Shader 源时，优先使用 `shaderreload changed` 或 FileMonitor 自动热更，在固定场景、相机和灯光下观察结果。
- Hair 双 Pass 的 Core/Fringe 必须作为同一轮核验观察；任何一份资源热更失败、回滚或仍持有旧 artifact，都不能据此判定最终效果。
- 热更后先看提交摘要、candidate 是否完整提交、旧资源是否进入 retire 队列，再做截图、RenderDoc 或 debug view 对照；不能只凭窗口画面判断新参数已经生效。
- 只有涉及窗口/设备初始化、不可热更的 RenderState/Pass 拓扑、资源路径或启动配置变化时，才安排完整重启验证。
- 热更验证仍服从 shader-hot-reload 的 all-or-nothing、ABI/schema 校验、GPU epoch 退休和旧资源回滚合同；热更不是绕过合同的临时覆盖。
## 7. 验证合同

### 7.1 解析器 Golden 验证

解析 `b_f_3725` 高模 MTG 后，必须得到：

```text
effective mode 1: 5 slots
effective mode 3: 19 slots
effective mode 4: 10 slots
effective mode 8: 2 slots
```

并且 Hair 的 `TransparentMode=0, NewTransparentMode=8` 必须报告 effective mode 8。

### 7.2 每槽位审计

每个槽位必须能输出：

```text
source mtg
source slot
technique
effective render state
alpha ref / alpha val
cull
macros
texture bindings
target M / MI
target render route
unsupported differences
```

### 7.3 运行时验证

固定场景、相机、灯光和资源版本后，至少验证：

1. 身体 Skin mode 1；
2. 脸部 Skin；
3. 一个 mode 3 Alpha Test 槽；
4. 一个 mode 4 Alpha Blend + ZWrite 槽；
5. Hair mode 8；
6. Crystal ThinTranslucent；
7. Eye / Eye Edge。

运行时必须能检查实际：

```text
material slot
M / MI identity
render mode
blend state
depth test / depth write
alpha test
cull mode
shader variant macros
texture descriptors
pass name/type
```

### 7.4 RenderDoc 验证

RenderDoc 对照项目必须包含：

- draw event；
- pipeline blend/depth/cull 状态；
- texture/sampler 绑定；
- material uniform / push constant；
- 目标 pass 和 attachment；
- 与源 MTG 槽位的映射。

最终截图只能作为结果证据，不能代替逐槽位合同检查。

## 8. b_f_3725 当前基线

`b_f_3725` 已确认的源资产根为：

```text
K:\future\res
```

当前高模 `.mtg` 的 effective mode 分布：

| Mode | 数量 | 说明 |
|---:|---:|---|
| `1` | 5 | Opaque |
| `3` | 19 | Alpha Test / Cutout |
| `4` | 10 | Alpha Blend + ZWrite |
| `8` | 2 | Hair Alpha Test + Alpha Blend |

关键源槽位：

| 源槽位 | Technique | Mode | 目标含义 |
|---|---|---:|---|
| `b_f_3725_high_0` | `pbr_skin` | 1 | 身体 Skin，不透明 |
| `b_f_3725_high_4/5/6/7` | `pbr_silk` | 4 | 半透明丝绸，写深度 |
| `b_f_3725_high_1` | `pbr_subsurface_billboard` | 3 | Pearl Alpha Test |
| `b_f_3725_high_9` | `pbr_crystal` | 3 | Crystal Alpha Test + ThinTranslucent |
| `h_f_3725_high_0` | `pbr_hair_transparent` | 8 | Hair coverage |

当前普通 Default/Silk mode 4 已映射到 `TransparentAlphaBlendWriteDepth`，并在 `forwardTransparent` 内使用独立 DepthWrite 管线状态。Crystal 继续使用 `ThinTranslucent` 透射合同；mode 6/7 的混合加裁剪组合仍是后续能力缺口。

## 9. 实现顺序

按以下顺序落地，禁止跳过中间合同：

1. MTG 解析保留 raw/effective RenderState、`NewTransparentMode`、`AlphaVal` 和宏；
2. 生成逐槽位 `NXResolvedMaterialContract` 和 Golden 审计；
3. 扩展 VulkanLearn RenderMode/Pass 以表达 mode 4、6、7、8 的必要状态；mode 4 已完成；
4. 让转换器依据合同生成 MI，不再硬编码 `Opaque` / `OpaqueClip` 替代源 mode；`b_f_3725` 的 mode 1-5 已按 MTG 推导；
5. 按 Shader Structure 规范拆分 `M_` 与 `mf_*`：先建立功能 MF，再让 M_ 只做组合和 MaterialInputs wiring；
6. 对齐各 Shader 家族的通道、UV、Alpha 和模型输入，并确保 Base/ShadowDepth 复用同一 MF 语义；
7. 加入运行时材质状态诊断、Shader reflection 检查和 RenderDoc 对照；
8. 最后再处理妆容、glitter、完整 NeoX 光照和动画运行时。

## 10. 完成定义

一个 NeoX 角色只有同时满足以下条件，才可称为“资源与 Shader 合同对齐”：

- 所有源槽位均可解析、可反查、无静默 fallback；
- raw/effective RenderState 与 MTG 一致；
- Alpha Test、Alpha Blend、ZWrite、Cull 和 Pass 路由一致；
- 纹理来源、色彩空间、采样器、UV 和通道语义均有记录；
- Shader MaterialInputs 的每个关键字段都有源语义；
- `M_*.surface.glsl` 只负责 MF 组合和 MaterialInputs wiring，NeoX 功能采样/解码均封装在可复用 `mf_*` 中；
- MF 使用显式输入/输出结构，不直接访问灯光、执行 `discard`、写附件或修改 Pipeline 状态；
- Base、Forward、ShadowDepth 的 coverage 语义来自同一 MF，不因 Pass 复制而分叉；
- 目标引擎不支持的能力在 manifest 中明确列出；
- 关键槽位通过自动化解析测试和 RenderDoc 运行时验证；
- 最终画面对比在固定相机、光照和曝光下进行，而不是用主观“看起来差不多”判定。

## 11. 当前有意差异

以下内容不属于本合同已经完成的能力：

- NeoX 完整统一角色光照、Fresnel 和动画时钟；
- `tex_s` 动态妆容；
- Texture2DArray glitter；
- Crystal SceneColor 旋转折射和源深度偏移；
- 生产级 Hair OIT、多层透明排序；
- 正式 Material Multi-Pass/PassTag；当前 Hair mode 8 由 Hair-only Core 与原角色
  Fringe 两份场景资源临时表达，静态 Pose 下有效，但会增加一份 Hair primitive 资源和对象；
- Skeleton/Animation 运行时导入；
- Skin 的 MF 仍直接消费生成参数 include 中的 `u_skin*` 全局字段，尚未改成由 `M_neoxSkin.surface.glsl`
  构造显式 `MFNeoXSkinInput`；此外 Forward `PreintegratedSkin` evaluator 仍待公共 Shading Model 专项补齐；
- VulkanLearn 尚未实现的 mode 6/7 透明混合与 Alpha Test 组合路径。

这些差异必须在迁移清单和验证报告中保留，不能用“已迁移”覆盖。
