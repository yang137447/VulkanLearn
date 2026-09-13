# Cloth Shading Model v2 各向异性升级执行交接

## 文档状态

- 状态：已执行
- 日期：2026-09-01
- 目标：在不新增 `NeoXCloth` Shading Model 的前提下，升级现有 `Cloth` / ID `8`，支持 NeoX Silk/Cloth 所需的稳定纤维方向、各向异性 sheen 和对应的能量/Deferred 数据闭环。
- 现行合同：`documents/rendering/cloth-shading-model.md`
- 现行实现：`Cloth v2`，保留 `anisotropy = 0` 的 v1 兼容路径，并支持非零 anisotropy 的椭圆 Charlie + v2 visibility

本文是执行交接，不是视觉调参清单。下个线程必须先冻结输入、版本、GBuffer 和 LUT 语义，再修改 shader 或 C++；不能通过直接改最终颜色、复用错误 LUT 或给 tangent 加随机扰动来掩盖数据缺失。

## 0. 最终架构决策

### 必须保持

- 保留 `SHADING_MODEL_CLOTH = 8`。
- 不新增 `NeoXCloth`、`NeoXSilk` 等来源专用 Shading Model。
- `Cloth v1` 的旧资产在 `anisotropy = 0` 时必须保持原有结果和 GBuffer 语义。
- NeoX 来源差异由 Material Function 和离线资产转换负责；Shading Model 只消费标准化 `MaterialInputs`。

### 必须升级

```text
NeoX Silk/Cloth MF
    -> 稳定 fiber/flow 方向、anisotropy 和 sheen 参数
    -> ClothMaterialInputs
    -> Cloth v2 anisotropic closure
    -> Forward / Deferred 共用 evaluator
    -> 版本化 GBuffer 与 directional-albedo 能量补偿
```

MF 负责“输入从哪里来、如何标准化”；Cloth Shading Model 负责“各向异性 BRDF、visibility、能量分配和光照结果”。只扩展 MF 而不升级 Cloth evaluator 不算完成。

## 1. 必读基线

按以下顺序阅读，不要直接从某个 shader 文件开始改：

1. `documents/rendering/cloth-shading-model.md`
2. `documents/plan/rendering/cloth-shading-model-development-plan.md`
3. `documents/rendering/neox-character-alignment-contract-v1.md`
4. `documents/rendering/shader-structure-and-material-function.md`
5. `shader/glsl/engine/materialInputs.glsl`
6. `shader/glsl/engine/materialSurface.glsl`
7. `shader/glsl/engine/gbufferCodec.glsl`
8. `shader/glsl/common/clothBrdf.glsl`
9. `shader/glsl/engine/clothLighting.glsl`
10. `shader/glsl/engine/forwardLighting.glsl` 和 `shader/glsl/engine/deferredLighting.glsl`
11. `source/render/cloth/clothAssets.*`
12. `source/render/cloth/clothResourceLoader.*`
13. `source/render/cloth/clothResourceSet.*`
14. `source/pipeline/clothLookupTableGenerator.*`
15. 知识库基线：`D:\YYBWorkSpace\GitHub\yyb-knowledge-book\src\content\docs\rendering\materials\shading-models\cloth-sheen.mdx`

如果 NeoX 具体 MF、M_ 或原始材质文件在当前 checkout 不存在，先记录缺口并从已有 manifest、离线导出或 fixture 中确认来源；禁止凭文件名猜参数语义。

## 2. 当前 Cloth v1 基线

### 2.1 输入

当前 `ClothMaterialInputs` 只有：

```glsl
struct ClothMaterialInputs
{
    vec3 sheenColor;
    float sheenRoughness;
};
```

当前 `Cloth` 只使用各向同性 Charlie NDF 和 Neubelt visibility。`Cloth v1` 不读取 mesh tangent，不消费 tangent/anisotropy 的 Cloth GBuffer 语义。

### 2.2 GBuffer

当前 Cloth 语义为：

```text
GBufferA.rgb = baseColor
GBufferB.a   = shadingModelId / flags，Cloth ID = 8
GBufferC.r   = metallic，必须为 0
GBufferC.b   = base roughness
GBufferC.a   = ambient occlusion
GBufferD.rgb = sheenColor
GBufferD.a   = sheenRoughness
GBufferF     = Cloth v1 不消费 tangent / anisotropy
```

来源：`documents/rendering/cloth-shading-model.md:106`。

### 2.3 LUT 和能量

当前 directional-albedo LUT 是二维 `E_s(NoV, alpha_s)`，只适用于各向同性 Charlie。各向异性版本不能直接把当前二维 LUT 当作新 BRDF 的能量补偿。

当前 IBL 是 `sheenIblVersion = 0` 的明确 diffuse irradiance fallback；除非这次同时完成各向异性环境预积分，否则不能把 Cloth v2 宣称为完整各向异性 IBL。

## 3. 升级范围与非目标

### 3.1 本次必须完成

- 冻结 NeoX Silk/Cloth 的 anisotropy、flow、tangent、handedness 和 roughness 来源语义。
- 扩展 Cloth 的标准化输入结构和 Material Function 组合路径。
- 为 Cloth v2 实现真正的各向异性 sheen closure，而不是只替换 `NdotH`。
- 保持 Forward 和 Deferred 调用同一个 Cloth evaluator。
- 为 Deferred 设计版本化的 Cloth tangent/fiber/anisotropy GBuffer 语义。
- 为各向异性 directional albedo 和 base energy compensation 建立匹配的计算/资源版本。
- 接入 shader build cache、World-local resource set、Compute reload 和 GPU epoch retirement。
- 增加参数、GBuffer round-trip、Forward/Deferred、旋转切线和 `anisotropy = 0` 回归验证。
- 更新正式 Cloth 合同、NeoX 对齐文档和文档索引。

### 3.2 本次明确不做

- 不新增 Shading Model ID。
- 不在 MF 中访问灯光、Shadow Map、SceneColor 或创建 Pipeline。
- 不实现纤维级几何、真实纱线几何、BSSRDF、薄布透射或独立背光闭包。
- 不把 anisotropy 偷塞进未声明的 `customData` 通道。
- 不复用 GGX BRDF LUT、GGX prefilter、GGX visibility 或错误的采样 PDF。
- 不用逐像素 `clamp`、`max` 或随机扰动掩盖非法 tangent、roughness 或 anisotropy 数据。
- 不把 NeoX 特有的源路径解析逻辑搬进运行时 shader。

## 4. Phase 0：源数据与合同冻结

在写 shader 前完成一张源语义表，至少包含以下字段：

| 项目 | 必须确认 |
|---|---|
| anisotropy 数值 | 数值范围、默认值、正负号含义、是否为强度或轴向符号 |
| tangent/flow 来源 | 顶点 tangent、Flow 纹理、UV 派生方向或离线生成方向 |
| tangent 空间 | UV0/UV1、世界空间/切线空间、坐标 handedness |
| normal 关系 | Normal Map 修改后如何重新正交化 tangent 和恢复 bitangent |
| roughness | 单一 sheen roughness 还是双轴 `alpha_T / alpha_B` |
| flow | 是方向旋转、强度 mask，还是独立 lobe/动态时钟输入 |
| 颜色和权重 | sheen tint、sheen weight 是否已经折叠为线性 `sheenColor` |
| LOD/skinning | tangent 在 skinning、LOD、镜像 UV、seam 后是否稳定 |
| 离线转换 | 源纹理是否已完成通道、颜色空间和方向约定转换 |

### 4.1 标准化输入原则

`ClothMaterialInputs` 的最终字段必须只表达 Cloth 光照所需的标准语义。推荐至少覆盖：

```glsl
struct ClothMaterialInputs
{
    vec3 sheenColor;
    float sheenRoughness;
    float anisotropy;
    // 如源语义确实需要，增加双轴 roughness 或显式 fiber rotation。
};
```

不要在未完成源审计前强行决定 `anisotropy` 的正负约定、`alpha_T/alpha_B` 映射或 rotation 的单位。最终选择必须写入合同并有 authoring/schema 校验。

已有的 `MaterialInputs.tangent` / `MaterialSurface.worldTangent` 可以承载方向，但必须确认它表示的是稳定的纱线/flow 主方向，而不是普通 mesh 三角形切线。各向异性闭包所需的方向所有权不能只依赖“当前看起来有 tangent”。

### 4.2 版本字段

至少新增或升级以下版本身份：

| 版本 | 要求 |
|---|---|
| `clothModelVersion` | 从 `1` 升到 `2`，标识 Cloth 输入/闭包主版本 |
| `clothAnisotropyMappingVersion` | 记录 anisotropy 到双轴 roughness/方向的映射 |
| `charlieDistributionVersion` | 各向异性 NDF 与参数化改变时升级 |
| `clothVisibilityVersion` | anisotropic visibility 改变时升级；不要沿用不匹配的 Neubelt 假设 |
| `clothDirectionalAlbedoLutVersion` | LUT 维度、采样域、积分公式或通道改变时升级 |
| `clothGBufferEncodingVersion` | Cloth v2 tangent/fiber/anisotropy 编码改变时升级 |
| `sheenIblVersion` | 只有 sheen 环境响应/预积分语义改变时升级 |

版本必须进入 resource source digest，且随 World-local resource set 一起事务化发布。旧 LUT、旧 GBuffer 语义和新 BRDF 不能隐式兼容。

## 5. Phase 1：MF、材质和参数接线

### 5.1 MF 责任

NeoX Silk/Cloth MF 负责：

- 采样标准化后的 Flow、Mask、Normal 或方向纹理；
- 将源参数转换成线性颜色、合法 roughness 和 Cloth anisotropy 输入；
- 在 normal map 修改后重新正交化 tangent；
- 使用 tangent handedness 恢复 bitangent；
- 输出命名明确的结构，再由 M_ 完成最终 wiring。

MF 不负责：

- 计算 BRDF、visibility、directional albedo 或灯光积分；
- 读取光源、Shadow Map、LUT 或 SceneColor；
- 修改 Blend、Cull、Depth、Pass 路由；
- 直接读取原始 NeoX 文件路径。

### 5.2 参数和资产

- 为通用 `M_cloth` 增加 v2 所需参数时，默认值必须使 `anisotropy = 0`。
- NeoX Silk 参数通过对应 MF 接线，不能把 NeoX 特有的采样逻辑复制进通用 `M_cloth.surface.glsl`。
- 所有参数范围在材质加载/authoring 阶段验证；非法数据拒绝资产或 candidate。
- MI、shader reflection、参数 include 和 Material Instance Inspector 必须看到同一套字段和范围。
- 如果 flow 是纹理方向而不是运行时动态值，优先离线转换为稳定的标准纹理语义。

## 6. Phase 2：各向异性 Cloth 闭包

### 6.1 必须定义的数学对象

各向异性版本至少需要在局部 frame `(T, B, N)` 中定义：

```text
h_T = dot(T, H)
h_B = dot(B, H)
h_N = dot(N, H)
alpha_T / alpha_B 或等价的双轴参数
D_C^aniso(h_T, h_B, h_N)
V_C^aniso(N_i, N_o, 方向参数)
```

不能只把 isotropic Charlie 的 `NdotH` 替换成 `dot(T, H)`。各向异性闭包必须同时重新验证：

- NDF projected-area normalization；
- BRDF 互易性；
- grazing 域和有效半球边界；
- visibility 与 NDF 的匹配；
- sampling PDF（如果当前路径使用采样）；
- 单位白色 sheen 的 directional albedo；
- base/sheen 能量分配。

### 6.2 兼容性要求

```text
anisotropy = 0
    -> 当前 Cloth v1 各向同性结果
    -> 相同 sheenColor / sheenRoughness 语义
    -> 相同 direct shadow / AO / emissive 边界
```

Forward 和 Deferred 必须共享同一份 `clothBrdf.glsl` / `clothLighting.glsl` 公式，不允许一条路径使用 anisotropic closure、另一条路径继续使用 isotropic closure。

## 7. Phase 3：GBuffer 与坐标所有权

当前 Cloth v1 明确不消费 `GBufferF` 的 tangent/anisotropy。Cloth v2 必须新增显式、版本化的编码分支，建议方向如下：

```text
GBufferD.rgb = sheenColor
GBufferD.a   = sheenRoughness 或版本化的 Cloth sheen 参数
GBufferF.xyz = fiber/tangent direction 的稳定编码
GBufferF.w   = anisotropy 或版本化的方向参数
```

最终打包不能直接照抄上表，必须先确认：

- `GBufferF.xyz` 的方向编码精度是否足够；
- mirrored UV 的 handedness 是否需要显式保存；
- anisotropic elliptical lobe 是否对 `T/B` 的符号具有 180° 对称性；
- normal map 后的 tangent 是否与 encode 前的 world normal 正交；
- `GBUFFER_HAS_ANISOTROPY_MASK` 如何表示 Cloth v2；
- Hair、Subsurface、PreintegratedSkin、Eye 和其它模型的 GBufferF 所有权不被破坏。

如果现有四通道不足，必须设计明确的 packed encoding 或增加版本化 attachment/资源；不能让 Deferred 读取未定义的旧字段。

`gbufferCodec.glsl` 的 encode/decode、`MaterialSurface` 的字段、debug snapshot 和 Forward 输入必须保持同一坐标约定。

## 8. Phase 4：Directional Albedo、LUT 与 IBL

当前各向同性 LUT 为：

```text
E_s(NoV, alpha_s)
```

各向异性版本的响应至少会依赖观察方向相对于纤维 frame 的方位角，以及双轴 roughness：

```text
E_s(NoV, phi_o, alpha_T, alpha_B)
```

因此必须选择并记录一种明确方案：

1. Compute 生成新的多维/参数化 anisotropic directional-albedo 资源；或
2. 使用经过 CPU reference、白炉积分和误差矩阵验证的解析/低维近似。

无论选择哪种方案：

- 生产 LUT/派生数据只能由 Compute Shader 生成；
- CPU 只做参数解析、资源编排、digest、事务和验证；
- 不得继续复用旧二维 LUT 伪装成各向异性结果；
- `T_b` 必须与新 `E_s` 使用同一组 anisotropic 参数；
- direct 和 IBL 的能量账本必须明确记录。

如果本轮只完成各向异性 direct、而 IBL 仍采用 `sheenIblVersion = 0` fallback，必须在合同、debug 和验收结果中标记为“anisotropic direct / isotropic IBL fallback”，不能写成完整 Silk 各向异性支持。

## 9. Phase 5：C++ 资源、热重载和生命周期

检查并按现有 ownership contract 修改：

- `source/render/cloth/clothAssets.*`
- `source/render/cloth/clothResourceLoader.*`
- `source/render/cloth/clothResourceSet.*`
- `source/render/cloth/clothComputeReloadParticipant.*`
- `source/pipeline/clothLookupTableGenerator.*`
- shader build cache 与生成 include 的 digest 输入

要求：

- v2 版本字段、generator source digest、参数化和 LUT 资源身份保持一致；
- World candidate 中先构建完整 v2 resource package，再通过现有事务提交；
- Compute 生成失败拒绝整个 candidate，不发布半成品；
- reload 期间旧 LUT、descriptor、pipeline 和资源包按 GPU epoch 退休；
- worker 不触碰 live Vulkan、Material、PipelineFactory 或 active World；
- 旧 Cloth v1 资源不能被错误解释为 v2 资源。

## 10. Phase 6：Debug、测试和场景

### 10.1 Debug

至少新增或复用以下 debug 数据：

- Cloth Shading Model / Cloth model version；
- `WorldTangent` 或 fiber direction；
- anisotropy 原值和映射后的双轴 roughness；
- anisotropic Charlie `D`；
- anisotropic visibility；
- directional albedo；
- base energy scale / base transmission；
- direct sheen、indirect sheen；
- GBuffer encoding version 和 fallback 状态。

各向同性模式旋转 tangent 后结果应不变；各向异性模式旋转 tangent 后高光方向应连续旋转，不能发生离散跳变。

### 10.2 验证矩阵

必须覆盖：

1. `anisotropy = 0` 与 Cloth v1 的数值/图像兼容性。
2. isotropic 模式旋转 tangent frame 不改变结果。
3. anisotropic 模式旋转 tangent frame 会连续旋转高光方向。
4. 正负 anisotropy 的方向和能量语义符合源合同。
5. mirrored UV、handedness、normal map、skinning、LOD 和 seam 不产生跳变。
6. GBuffer encode/decode 后 tangent、anisotropy、roughness 误差在预算内。
7. Forward/Deferred 同视角逐像素对照使用同一 evaluator。
8. 单位白炉下 `c_s * E_s + T_b` 不超过合法能量预算。
9. directional、point、spot light 的 shadow 只作用于对应 direct contribution。
10. Compute LUT 失败、版本冲突、digest 冲突和 reload 失败均保持旧资源，不发布半成品。
11. Default Lit、Hair、Eye、Subsurface、Skin 和现有 Cloth v1 路径无回归。
12. NeoX Silk/Cloth fixture 的参数、纹理、材质实例、场景和启动期 shader 编译通过。

### 10.3 测试位置

优先扩展现有 Cloth/runtime/shader 验证路径；若当前 checkout 没有可复用的 Cloth test target，再按现有项目风格增加最小、可重复的 test target。不要为了证明数学正确而引入 CPU production LUT 路径。

## 11. 推荐执行顺序

```text
Phase 0  源参数/纹理/TBN 审计与 v2 合同冻结
    -> Phase 1  ClothMaterialInputs、MF、M_/MI/schema 接线
    -> Phase 2  anisotropic Cloth BRDF + Forward/Deferred 共享 evaluator
    -> Phase 3  Cloth v2 GBuffer encode/decode 与 flag/version
    -> Phase 4  directional-albedo / 能量补偿 / Compute 资源
    -> Phase 5  C++ resource package、digest、reload、GPU epoch
    -> Phase 6  debug、fixture、场景、回归和启动验证
    -> Phase 7  更新正式 contract、NeoX 对齐文档、README 索引
```

每个 Phase 结束时先运行最小验证，再进入下一阶段。不要把源数据不确定性、GBuffer 不足或 LUT 误差留到最后一起处理。

## 12. 停线条件

遇到以下任一情况必须停线并在交接结果中说明，不得继续视觉调参：

- 找不到 NeoX anisotropy/flow 的可信来源或无法确认范围/符号；
- tangent 受 skinning、LOD、镜像 UV 或 seam 影响而不稳定；
- 无法在 Deferred 中恢复与 Forward 一致的方向语义；
- 不能为 GBuffer、LUT 或 BRDF 版本提供稳定身份；
- anisotropic closure 只有 `D` 没有匹配 visibility、PDF 或 directional albedo；
- `anisotropy = 0` 破坏现有 Cloth v1；
- Compute 失败会发布半成品 resource package；
- 只能通过 shader 内 clamp、max、随机扰动或颜色倍率掩盖非法输入；
- 只能把新参数写入无语义的旧 `customData` 通道。

## 13. 完成交付物

下个线程完成后必须返回：

- 修改过的源文件、shader、材质/MI、fixture 和文档路径；
- `Cloth v2` 的版本字段、参数范围、GBuffer 和 LUT 语义；
- NeoX source -> MF -> ClothMaterialInputs 的映射表；
- Forward/Deferred 一致性和 `anisotropy = 0` 兼容性证据；
- Compute/reload/resource ownership 验证结果；
- 运行过的 build/test/启动命令及结果；
- 若仍保留 anisotropic direct / isotropic IBL fallback，必须明确列出未完成的 IBL 能力和后续计划。

最终结论必须明确写成以下二者之一：

```text
Cloth v2 anisotropic closure 已完成并通过完整验证
```

或：

```text
Cloth v2 仅完成输入/GBuffer/direct 阶段，IBL 或其它能力仍为显式缺口
```

## 14. 本次执行记录（2026-09-01）

- 已完成 NeoX Silk 源审计：`K:\future\res\shader\pbr_silk.fx` 的 `u_anisotropy`、`ParamMap.B` anisotropy mask、`u_anisotropy_cross`、mesh tangent/tangent.w 语义已冻结；当前 checkout 缺少 `M_neoxSilk`，未伪造 NeoX full fixture。
- 已完成 `ClothMaterialInputs`、Cloth MF、M_cloth 参数、参数校验、GBuffer v2、椭圆 Charlie/v2 visibility、Forward/Deferred shared evaluator、双 LUT Compute 生成、resource digest/package、Compute reload 和 GPU epoch retirement 接线。
- 已完成 Forward/Deferred debug snapshot 传播及 UI Debug View 80–89；模式 68 保留为兼容入口但显示统一后的 v2 visibility。
- 已增加 `tool/cloth-tests`，覆盖版本合同、参数量化、5+5 bit GBuffer round-trip、正负 anisotropy 轴交换和 `cos(2phi)` 连续插值；测试不生成生产 LUT。
- 关键实现注释已补充中文，覆盖 TBN 正交化、GBuffer flag/packed 语义、椭圆 D/visibility、LUT 轴向插值和资源 ownership。
- 待验证命令：`cmake --build build -j`、`ctest --test-dir build -R cloth_contract --output-on-failure`、项目 shader 编译/启动期 smoke、`git diff --check`。

最终结论：

```text
Cloth v2 仅完成输入/GBuffer/direct 阶段，IBL 或其它能力仍为显式缺口
```
