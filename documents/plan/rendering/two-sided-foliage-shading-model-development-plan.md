# UE5.8 Legacy TwoSidedFoliage Shading Model 接入计划

## 文档状态

| 项目 | 内容 |
| --- | --- |
| 状态 | 待实施；当前仅完成 ID 注册和通用双面材质能力 |
| 目标 | 将 UE5.8 Legacy `TwoSidedFoliage` 以 VulkanLearn 的 Material / GBuffer / Pass 合同接入 |
| Shading Model | `TwoSidedFoliage` / ID `6` |
| 计划建立日期 | 2026-09-03 |
| 当前合同基线 | `documents/rendering/shader-structure-and-material-function.md` |
| 相关现状 | `documents/plan/rendering/foliage-speedtree-sss-wind-roadmap.html`、`documents/plan/rendering/speedtree-sdk-data-probe.md` |
| 首阶段执行域 | Opaque + OpaqueClip；Forward + Deferred；自动 ShadowDepth |
| 实现目标 | UE5.8 Legacy 的语义对齐和可解释的实时近似，不声称复制 UE 私有 shader 的逐行实现 |

本文是未来实现计划，不是当前实现合同。实现完成后，应将冻结后的字段、编码、公式和已知限制迁移到
`documents/rendering/two-sided-foliage-shading-model.md`，并在本文保留阶段记录和验收证据。

## 1. 目标与非目标

### 1.1 总目标

建立独立于普通 `DefaultLit` 和普通 `Subsurface` 的叶片材质路径：

```text
TwoSidedFoliage MI
    -> 双面 + Alpha Clip effective render state
    -> TwoSidedFoliage MaterialInputs
    -> MaterialSurface / GBuffer 显式编码
    -> 前后表面光照与叶片背光透射响应
    -> Forward / Deferred 共用 foliage evaluator
    -> Base + ShadowDepth 覆盖率一致
    -> Debug View、模块测试和 foliage smoke
```

首阶段必须能在同一场景中清楚区分：

```text
叶片        -> TwoSidedFoliage
树干/花盆   -> DefaultLit
```

### 1.2 UE5.8 Legacy 语义边界

首阶段以以下语义为目标：

- `TwoSidedFoliage` 仍使用 Legacy ID `6`，不新建 VulkanLearn 私有 ID；
- 模型必须是 two-sided，背面不能被 Back-face culling 丢弃；
- `Subsurface Color` 是叶片透光/背光颜色的主要材质输入，采用线性 RGB 语义；
- 透光响应是独立于普通 PBR `Base Color` 的 foliage lobe，不把 `Base Color` 改写成透光色；
- 背面法线翻转只解决几何可见性和局部表面方向，不能代替 foliage lighting；
- `Opacity Mask` 仍由 MeshPass Template 统一执行，主 Pass 和 ShadowDepth 使用同一覆盖率来源；
- `AO`、阴影、灯光颜色/强度和 emissive 的职责沿用当前引擎合同；
- SpeedTree 风场、WPO、pivot 和顶点数据解码属于 Vertex / Material Function 层，不属于该 Shading Model；
- 首阶段不把普通 `Subsurface` ID `2` 当作 `TwoSidedFoliage` 的 fallback，也不把模型静默降级为 `DefaultLit`。

### 1.3 非目标

本计划首阶段不包含：

- UE 私有 shader 常数、内部函数或平台专用优化的逐行复制；
- Substrate / Strata foliage；
- 真实厚度场、体积路径追踪、BSSRDF、光线追踪 any-hit 和完整多次散射；
- 叶片内部脉络、法线烘焙、风场、LOD、impostor 或 SpeedTree SDK 新格式；
- Transparent Alpha Blend 的排序、OIT 和普通 Shadow Map 之外的透明阴影；
- 依赖 LUT、Compute 资源或额外 descriptor set 的扩展；
- 通过 per-frame `clamp`、全局补光、`device.waitIdle()` 或 shader fallback 掩盖资产和同步错误；
- 直接编辑 `shader/spv/` 作为实现手段。

## 2. 当前基线与缺口

| 领域 | 当前情况 | 接入影响 |
| --- | --- | --- |
| Shading Model 注册 | `source/material/materialAssetUtils.h` 已把 `TwoSidedFoliage` 映射到 ID `6` | 保持名称、ID 和 shader define 不变 |
| GLSL 常量 | `shader/glsl/common/shadingModel.glsl` 已有 `SHADING_MODEL_TWOSIDED_FOLIAGE` | 仅追加消费路径，不改槽位 |
| 编辑器/稀疏资产 | 编辑器和 MI sparse candidate 已允许该名称 | 增加 two-sided 约束和输入提示 |
| MaterialInputs | 有公共 PBR 输入，并已有 `SubsurfaceMaterialInputs` | 增加独立 `TwoSidedFoliageMaterialInputs`，避免与 ID 2 争用字段 |
| MaterialSurface | 已保存 `modelInputs`、`customData` 和 `shadingModel` | 增加 foliage 的 surface snapshot 与 GBuffer 编码分支 |
| Forward 分发 | `Unlit/ClearCoat/ThinTranslucent/Hair/Cloth/Eye` 有显式分支 | 增加 ID 6 的 foliage evaluator |
| Deferred 分发 | 多个模型有显式分支，ID 6 当前落入 `DefaultLit` | 增加与 Forward 相同合同的 foliage evaluator |
| GBuffer | `GBufferD` 供模型专用 custom data，packed flags 已有 custom-data 位 | 冻结 foliage 的 D 通道语义，保持其它模型显式 decode |
| 双面法线 | `MATERIAL_TWO_SIDED` 和 `gl_FrontFacing` 已被 PBR/SpeedTree/ThinTranslucent 使用 | 复用方向约定，但不把它当作 foliage 光照实现 |
| ShadowDepth | `twoSided` 会触发自动 ShadowDepth 路由 | 验证 Alpha Clip 与 foliage 主 Pass 覆盖率一致 |
| SpeedTree | 已有 `M_speedtree`、风场和 `mf_speedtreeInputs.glsl` | 可作为 foliage 输入来源，但不强制绑定 SpeedTree |
| 资源系统 | 首阶段模型不需要 LUT/外部资源 | 不新增 World-local foliage resource package |
| runtime test | 只支持现有固定 runtime test 命令 | 不新增命令；优先使用 GoogleTest、启动期 shader 编译和场景 smoke |

## 3. 首阶段合同冻结

### 3.1 MaterialInputs

新增独立结构，建议初始字段只包含 UE Legacy 必需语义：

```glsl
struct TwoSidedFoliageMaterialInputs
{
    vec3 subsurfaceColor;
};
```

字段规则：

- `subsurfaceColor` 为线性空间 RGB；颜色来源可以是常量或贴图，但通道转换必须在资产/Material Function 层完成；
- `subsurfaceColor` 不是 `baseColor` 的替代品，也不是普通 `SubsurfaceMaterialInputs.color` 的别名；
- 首阶段不新增 `thickness`、`transmissionWeight`、`wrapWidth`、`profileId` 等非 Legacy 输入；
- 若实现验证表明确实需要额外参数，必须先更新正式合同并说明它是 UE 语义映射、VulkanLearn 运行时控制，还是调试专用参数；
- 默认值必须由 `M_*.json` 声明，不能在 MF、Shading Model 或 Pass 中隐含第二份业务默认值；
- Material Function 只负责填充 `MaterialInputs`，不读取灯光、Shadow Map 或写最终颜色。

### 3.2 Render State

`TwoSidedFoliage` 的 effective state 必须满足：

```text
renderMode = Opaque 或 OpaqueClip
cullMode   = None
```

校验策略：

- `MaterialInstanceValidator` 在 M_/MI_ 合并后校验 effective `shadingModel`；
- 模型为 `TwoSidedFoliage` 且 `cullMode != None` 时，资产加载失败并给出可定位错误；
- 不在运行时静默修改 cull，也不把该材质静默回退到 DefaultLit；
- 编辑器可以在作者选择模型时建议或联动设置 `cullMode = None`，但最终合同仍由有效状态校验保证；
- Transparent Alpha Blend 首阶段明确拒绝，避免把透明排序问题伪装成 foliage 模型支持。

### 3.3 光照闭包

首阶段采用“普通基础响应 + 叶片背光 lobe”的显式结构：

```text
L_foliage = L_default_lit(baseColor, roughness, specular, N, V, L)
          + L_backlit(subsurfaceColor, N, V, L, visibility)
```

实现前必须通过 UE 5.8 Legacy 参考行为、现有 VulkanLearn 光照约定和数值 probe 冻结以下内容：

- 背光项使用的法线方向（几何法线、材质法线或双面修正法线）；
- `N·L`、`N·V` 和 `N·H` 的取值域与是否使用 wrap/half-vector 修正；
- 前表面与背表面的贡献如何组合；
- `subsurfaceColor` 是否只染背光项，以及是否需要与 `baseColor` 做能量分配；
- directional light、point light、spot light、环境光和阴影对 foliage lobe 的作用；
- AO 是否只影响基础漫反射，还是同时影响 foliage lobe；
- emissive 是否完全独立于 foliage lobe；
- 非法或超范围输入由资产校验拒绝，不在 shader 中重复做防御性截断。

计划冻结的 MVP 参考 closure 是：

```text
frontBase   = DefaultLit diffuse/specular response
backLight   = lightRadiance * shadowVisibility * foliageVisibility
              * max(dot(-N, L), 0)
              * subsurfaceColor
foliage     = frontBase + backLight
```

上式是实现和白炉验证的起始参考，不得在没有数值对照的情况下宣称与 UE 逐项等价。若 UE 参考行为需要
额外的 view term、wrap 或能量补偿，应提升 `foliageModelVersion`，在合同中记录公式和有意差异，并同步
更新 Forward/Deferred 共用 evaluator。

### 3.4 GBuffer 合同

建议首阶段使用现有 `GBufferD`，不覆盖通用通道：

```text
GBufferA.rgb = worldNormal
GBufferB.rgb = metallic/specular/roughness
GBufferB.a   = packed ShadingModelID + SelectiveOutputMask
GBufferC.rgb = baseColor
GBufferC.a   = ambientOcclusion
GBufferD.rgb = foliage subsurfaceColor
GBufferD.a   = 0，保留为版本/权重扩展槽
GBufferE     = precomputed shadow / existing model contract
GBufferF     = existing tangent / anisotropy contract
```

编码规则：

- `GBufferB.a` 的低 4 bit 写 ID `6`，高位设置 `GBUFFER_HAS_CUSTOM_DATA_MASK`；
- `GBufferD` 只有在 ID `6` 分支中按 foliage 语义解码；
- 不复用 Subsurface、PreintegratedSkin、Hair、Cloth 或 Eye 的 custom data 解释；
- Forward 不应从 Material UBO 直接读取 Deferred 才有的字段；两条路径都消费同一份 `MaterialSurface` 语义；
- 增加 round-trip 测试，确认颜色误差、默认值和未知 flags 的行为；
- 如果后续加入 thickness/weight，必须新增 `foliageGBufferVersion` 或明确字段版本，不得悄悄改写 D.a。

### 3.5 ShadowDepth 与覆盖率

- `Opaque` 走全覆盖 foliage ShadowDepth；
- `OpaqueClip` 使用同一份 `opacityMask` 和同一套 Alpha Clip threshold；
- `cullMode = None` 的 pipeline 状态贯穿 Base 与 ShadowDepth；
- 主 Pass 和 ShadowDepth 的叶片法线方向可以不同，但 coverage、WPO、wind 和 discard 条件必须一致；
- 阴影验证必须包含叶片正面、背面和光线穿过薄叶片平面的情况；
- 不在首阶段承诺半透明阴影、透光阴影贴图或光源穿透 Shadow Map 的额外 pass。

## 4. 分阶段执行路线

| 阶段 | 目标 | 主要产出 | 阶段门 |
| --- | --- | --- | --- |
| P0 | UE 语义与参考行为冻结 | 源行为审计、输入表、公式版本、GBuffer/Pass 决策 | 未冻结不进入 shader 实现 |
| P1 | 材质输入与资产合同 | `TwoSidedFoliageMaterialInputs`、MF、M_/MI schema、two-sided 校验 | 非双面资产明确失败 |
| P2 | Forward/Deferred evaluator | 共用 foliage lighting、两条分发路径、光照 probe | 两条路径响应一致 |
| P3 | GBuffer 编解码 | D 通道写入/读取、flags、调试快照 | Deferred 不再落 DefaultLit |
| P4 | Pass、SpeedTree 与 ShadowDepth | Opaque/Masked、WPO/风场复用、coverage/阴影一致性 | 主 Pass 与 ShadowDepth 边缘一致 |
| P5 | 场景、Debug 和测试 | 盆栽/叶片场景、对照材质、GoogleTest、截图/日志证据 | 正背面、背光、回归全部通过 |
| P6 | 正式合同与收口 | rendering contract、README 索引、限制和后续路线 | 形成可追溯完成记录 |

## 5. P0：UE 语义审计与版本冻结

### 5.1 参考输入收集

实现前建立一份可复核的 UE5.8 Legacy 语义记录：

- Legacy `TwoSidedFoliage` 的公开 Material Input 列表；
- `Subsurface Color` 的颜色空间、默认值和 authoring 约定；
- 双面渲染、背面法线、光照方向和 foliage transmission 的关系；
- Opaque、Masked、Forward、Deferred、ShadowDepth 各路径是否共享同一语义；
- UE 文档、许可代码、编译 shader 输出或离线参考图中哪些内容可作为公开对照；
- 哪些行为只能做黑盒数值 probe，不能复制私有实现。

### 5.2 版本身份

至少冻结以下身份字段，并进入 shader/build cache 或测试 reference 的可追溯描述：

```text
foliageModelVersion
foliageLightingClosureVersion
foliageGBufferVersion
foliageSubsurfaceColorSpaceVersion
foliageShadowCoverageVersion
```

公式、颜色空间、GBuffer 通道、法线方向或 ShadowDepth coverage 变化时，必须提升对应版本，不能只改
shader 文件名或依赖时间戳触发重新编译。

### 5.3 P0 验收

- [ ] 明确记录 UE Legacy 目标语义与 VulkanLearn 有意差异；
- [ ] 明确 `subsurfaceColor` 的线性/非线性边界；
- [ ] 冻结 MVP closure、visibility、AO 和 shadow 规则；
- [ ] 冻结 GBufferD 和 flags 所有权；
- [ ] 冻结 `Opaque` / `OpaqueClip` 支持边界；
- [ ] 确认不需要首阶段 LUT、Compute 或新增 descriptor binding；
- [ ] 为最终参考图和数值 probe 记录固定相机、灯光、曝光和材质参数。

## 6. P1：MaterialInputs、MF 与资产合同

### 6.1 Shader 输入实现

修改范围：

- `shader/glsl/engine/materialInputs.glsl`
  - 新增 `TwoSidedFoliageMaterialInputs`；
  - 在默认构造中提供中性默认值；
  - 保持 `SubsurfaceMaterialInputs` 只服务于 ID `2`。
- 新增 `shader/glsl/materialFunction/mf_twoSidedFoliageInputs.glsl`
  - 采样或读取 `subsurfaceColor`；
  - 处理颜色空间已经由资产合同保证的标准输入；
  - 复用 `mf_normal.glsl` 和公共 PBR 输入约定；
  - 不读取灯光、Shadow Map，不执行 discard。
- `shader/glsl/engine/materialSurface.glsl`
  - 增加 foliage surface snapshot；
  - 仅在 shading model 为 ID `6` 时写入 foliage custom data。

### 6.2 母材质与实例

建议新增最小母材质 fixture：

```text
shader/glsl/M_twoSidedFoliage.json
shader/glsl/M_twoSidedFoliage.vertex.glsl
shader/glsl/M_twoSidedFoliage.surface.glsl
```

母材质至少声明：

- `baseColor` / albedo；
- `subsurfaceColor`；
- `roughness`、`metallic`、`specular`、`ambientOcclusion`；
- `opacityMask` 或透明度来源；
- `normal`、`emissive`、可选 WPO；
- `features` 中的 `modifiesMeshPosition`；
- `renderMode` 和默认 `cullMode = None`。

对应 MI fixture 应覆盖：

- 默认叶片；
- `subsurfaceColor = 0` 的双面 DefaultLit 对照；
- 强背光色；
- `OpaqueClip` alpha mask；
- 非法单面配置，确认加载被拒绝；
- SpeedTree 输入来源和普通静态叶片输入来源各一份。

### 6.3 C++ 校验

建议新增或扩展：

- `source/render/foliage/twoSidedFoliageMaterialContract.h/.cpp`；
- `source/materialInstanceValidator.cpp` 的 effective render-state 校验；
- `source/material/materialAssetUtils.h` 的 authoring 描述（名称和 ID 保持不变）；
- 编辑器面板的模型选择提示、`cullMode` 联动和输入字段展示；
- 材质 schema / 反射校验，保证 `subsurfaceColor` 只来自声明过的 M_ 参数或贴图。

合同错误至少包括：

- `TwoSidedFoliage` 使用非 `None` cull；
- 缺少 `subsurfaceColor` 声明或声明类型错误；
- 透明 RenderMode 与首阶段模型边界冲突；
- foliage 专用参数被写进普通 Subsurface 或未声明 customData；
- Base 与 ShadowDepth 使用不一致的 Material Schema。

## 7. P2：Forward/Deferred foliage evaluator

### 7.1 共享 shader 文件

新增建议文件：

```text
shader/glsl/engine/twoSidedFoliageLighting.glsl
```

该文件只提供纯 evaluator/helper：

- 正面基础 diffuse/specular 调用现有 DefaultLit 约定；
- 背光 foliage lobe 使用冻结后的 `subsurfaceColor`；
- 消费统一的 light radiance、distance/spot attenuation、shadow visibility 和 AO 输入；
- 保持 emissive 独立；
- 返回 Forward/Deferred 所需的命名结果结构；
- 不直接写 GBuffer，不访问材质 UBO，不决定 Blend/Cull/Depth 状态。

### 7.2 Forward 接入

在 `shader/glsl/engine/forwardLighting.glsl`：

- 增加 `ShadeTwoSidedFoliageForwardSurface()`；
- 在 `ShadeForwardSurfaceDetailed()` 中为 ID `6` 增加显式分支；
- 确认模型分支不会因 `MATERIAL_IS_EYE`、Hair、Cloth 或 ThinTranslucent 宏而误编译；
- 对同一 `MaterialSurface` 与同一光照输入，Forward 结果只依赖 evaluator，不读取 Deferred 专用字段。

### 7.3 Deferred 接入

在 `shader/glsl/engine/deferredLighting.glsl`：

- 增加 `ShadeTwoSidedFoliageDeferredSurfaceDetailed()`；
- 在 `ShadeDeferredSurfaceDetailed()` 中为 ID `6` 增加显式分支；
- 从 `GBufferD` 恢复 `subsurfaceColor`；
- 使用同一 `twoSidedFoliageLighting.glsl` evaluator；
- 不允许未知或缺失 foliage custom-data flag 时读取 stale D 通道；应回退到中性 foliage 参数并记录 debug 状态，或按合同拒绝该像素路径。

### 7.4 光照探针

建立最小、固定参数的验证矩阵：

| Probe | 预期 |
| --- | --- |
| 正面主光、无背光 | 与 DefaultLit 的基础响应接近 |
| 光源位于叶片背面 | 出现 `subsurfaceColor` 染色的透光/背光响应 |
| `subsurfaceColor = 0` | 无 foliage 增量，但仍保留双面可见 |
| 翻转叶片 winding | 正反面都可见，响应不出现未解释的黑屏 |
| 关闭阴影 | foliage lobe 随 visibility 变化，不绕过阴影 |
| 仅改变 emissive | foliage lobe 不被 emissive 二次染色 |
| 改变 roughness/specular | 基础高光变化，不能改变 foliage 输入语义 |

## 8. P3：GBuffer 与 Deferred 合同

### 8.1 编码实现

修改 `shader/glsl/engine/gbufferCodec.glsl`：

- Base encode 时为 ID `6` 写 `GBufferD.rgb = subsurfaceColor`；
- 设置 `GBUFFER_HAS_CUSTOM_DATA_MASK`；
- 保持 D.a 的预留语义，不借用为未经版本化的强度；
- Decode 时按 ID `6` 显式恢复 `surface.modelInputs.twoSidedFoliage.subsurfaceColor`；
- 保持 ClearCoat、Subsurface、PreintegratedSkin、SubsurfaceProfile、Hair、Cloth、Eye 的分支和通道解释不变；
- 加入 debug/validation 辅助，能区分“ID 6 + 缺失 custom data”和正常 foliage 像素。

### 8.2 通道和精度验收

- `subsurfaceColor` 在目标 attachment 格式下的量化误差可接受；
- 低亮度叶片颜色不因 UNORM/线性转换出现明显色带；
- HDR/超过 1 的输入若不在当前 attachment 语义内，必须由资产校验限制或明确编码方案；
- GBuffer round-trip 不得改变 shading model ID、flags 或其它模型专用数据；
- Deferred 直接从 GBuffer 得到的结果与 Forward 在相同输入下误差落入冻结阈值。

## 9. P4：MeshPass、SpeedTree 与 ShadowDepth

### 9.1 MeshPass 路由

检查并补齐：

- `MaterialFeatureKey` / shader variant identity 已包含 shading model、two-sided、render mode 和 static macros；
- `MaterialShaderComposer` 生成的 `MATERIAL_SHADING_MODEL` 和 `MATERIAL_TWO_SIDED` 一致；
- pipeline cull、blend、depth 状态从 effective render state 生成，不由 shader 分支猜测；
- Base 与 ShadowDepth 的 descriptor schema 仍来自同一 M_；
- 不新增公共 Shadow pipeline 的隐式 foliage fallback。

### 9.2 SpeedTree 组合

允许以下组合：

```text
SpeedTree Vertex/Wind MF
    + SpeedTree coverage/normal/base input MF
    + TwoSidedFoliage Shading Model
```

但必须保持职责边界：

- `mf_speedtreeVertex.glsl` 只处理顶点变形；
- `mf_speedtreeInputs.glsl` 可以输出 foliage 所需的 `subsurfaceColor`；
- shading model 不解析 SpeedTree 原始 SDK 数据；
- 叶片静态网格和 SpeedTree 网格都能进入同一 foliage evaluator；
- 风场位移在 Base 与 ShadowDepth 中使用同一 frozen snapshot/输入规则。

### 9.3 ShadowDepth

- `OpaqueClip` 使用与 Base 相同的 alpha texture、UV、vertex color 和 threshold；
- `MATERIAL_TWO_SIDED = 1` 与 `cullMode = None` 同步；
- WPO/SpeedTree wind 的 ShadowDepth 路径与 Base 保持一致；
- 阴影边缘不因 foliage lighting 分支变化；
- 只验证现有普通 Shadow Map 合同，不新增透光阴影 pass。

## 10. P5：Debug、测试与场景

### 10.1 Debug View

增加独立 debug view 或沿用当前 shading-model debug 机制，至少能查看：

- `ShadingModelID == 6`；
- `subsurfaceColor`；
- `gl_FrontFacing` / 双面法线方向；
- 背光 factor；
- shadow visibility 与 foliage lobe；
- alpha mask coverage；
- GBuffer custom-data flag 和 decode version。

Debug 输出必须使用未压缩或可解释的中间量，不能只显示最终颜色，否则无法区分材质颜色、背光颜色和灯光问题。

### 10.2 GoogleTest / 模块测试

遵守 `documents/architecture/coding-guidelines.md` 的 test governance：

- 不添加 test-only `main()`；
- 通过根目录 `vulkanlearn_add_gtest(...)` 注册；
- 每个 `TEST` 只验证一个合同；
- 优先扩展最近的材质 schema / material contract suite；
- 若 GBuffer codec 需要独立纯逻辑覆盖，再新增 `tool/foliage-tests/`，否则不要为了模型名称增加空测试目标。

建议测试项：

1. `TwoSidedFoliage` 名称到 ID/define 的稳定映射；
2. 非双面 effective state 被拒绝；
3. `subsurfaceColor` 类型、默认值和颜色空间元数据校验；
4. GBufferD encode/decode round-trip；
5. custom-data flag 缺失时不消费 stale D；
6. 背光 factor 在正面光、背面光和法线翻转条件下的单调性；
7. `subsurfaceColor = 0` 不产生额外 foliage lobe；
8. 其它 Shading Model 的 GBuffer 编解码不回归。

### 10.3 Runtime smoke

首阶段不新增 `--foliage-validation` 等 runtime command，原因是仓库只允许固定 runtime test 入口，新增入口
需要额外用户同意和文档化理由。验证使用：

- 正常启动时的 shader 编译/反射日志；
- foliage 场景加载和绘制 smoke；
- 现有 shader reload / world transaction 流程中的材质兼容性检查；
- 固定相机和灯光下的 viewport 截图；
- 必要时在后续独立任务中提出 foliage 专用 runtime command。

### 10.4 最小验证场景

建议场景包含：

```text
LeafCard_FrontBack       -> TwoSidedFoliage / OpaqueClip
LeafCard_DefaultLit      -> DefaultLit 对照
LeafCard_NoTransmission  -> TwoSidedFoliage + subsurfaceColor = 0
SpeedTree_LeafCluster    -> SpeedTree + TwoSidedFoliage
Trunk                    -> DefaultLit
Ground                   -> DefaultLit
```

相机和灯光至少覆盖：

- 正面主光；
- 叶片背后的侧后方轮廓光；
- 叶片法线与光线近似平行/垂直；
- 正反面相机观察；
- 阴影开启和关闭；
- 近景与远景 alpha clip 边缘。

## 11. P6：正式合同与完成条件

实现完成后新增：

```text
documents/rendering/two-sided-foliage-shading-model.md
```

正式合同必须包含：

- ID `6`、MaterialInputs、M_/MI authoring schema；
- `subsurfaceColor` 颜色空间和通道语义；
- Forward/Deferred 共用 closure 与版本字段；
- GBufferD/flags 的最终编码；
- Opaque/OpaqueClip/ShadowDepth 边界；
- SpeedTree MF 组合边界；
- Debug View、模块测试、runtime smoke 证据；
- 与 UE Legacy 的对齐项和有意差异；
- 未实现的透明透光、厚度、BSSRDF、RT 和 Substrate 路线。

同时更新：

- `documents/rendering/shader-structure-and-material-function.md` 中的模型状态表；
- `documents/README.md` 的正式合同和计划索引；
- 如新增稳定参数，再更新 `documents/rendering/material-param-authoring-and-reflection.md`；
- 如新增场景，补充资产来源、授权和运行时资源根说明。

## 12. 完成验收矩阵

### 功能

- [ ] `TwoSidedFoliage` 不再进入 Forward/Deferred `default` 分支；
- [ ] `subsurfaceColor` 能独立控制叶片背光颜色；
- [ ] 正反面均可见，且 normal、tangent、winding 不出现未解释翻转；
- [ ] `OpaqueClip` 的 Base 与 ShadowDepth coverage 一致；
- [ ] SpeedTree 风场与静态叶片都能消费同一 foliage evaluator；
- [ ] `subsurfaceColor = 0`、无阴影、无 emissive 等边界行为可解释。

### 合同与资源

- [ ] ID `6`、shader define、GBuffer packed ID 保持一致；
- [ ] 非双面材质在加载期失败，不静默修改或回退；
- [ ] M_/MI schema、generated include、reflection 和 descriptor layout 一致；
- [ ] 不新增未版本化 customData 解释；
- [ ] 不依赖手工生成的 `shader/spv/` 输出作为实现真相源。

### 回归

- [ ] DefaultLit、Subsurface、PreintegratedSkin、Hair、Cloth、Eye、ClearCoat 和 ThinTranslucent 现有路径不变；
- [ ] GBuffer flags、D/F 通道和 Deferred decode 没有跨模型串读；
- [ ] shader build cache 能捕获新增 MF/evaluator/include 的传递依赖；
- [ ] shader hot reload 的 ABI 兼容路径不会暴露半成品材质；
- [ ] 构建、启动期 shader 编译和允许的现有测试均通过。

## 13. 风险、取舍与后续路线

| 风险 | 处理方式 |
| --- | --- |
| UE Legacy 私有实现细节不可公开复刻 | 以公开语义和黑盒 probe 建立等价目标，记录有意差异，不声称逐行 parity |
| 双面 normal 翻转与 foliage backlight 使用不同方向 | 在 `MaterialSurface` 中明确保存正面判断和最终 shading normal，禁止 evaluator 隐式猜测 |
| `subsurfaceColor` 与 BaseColor 能量重复 | P0 冻结能量预算；用白炉和颜色 sweep 验证，必要时提升 closure version |
| GBufferD 未来被其它模型占用 | 通过 shading-model 显式分支、flags 和版本字段保护，不共享模糊 customData |
| Alpha Clip 与风场在 ShadowDepth 不一致 | 主 Pass/ShadowDepth 共用 Material Function 和 coverage 入口 |
| 误把 SpeedTree 风场当作 foliage model | 保持 VertexFactory/MF/Shading Model 三层边界，静态叶片先独立验证 |
| 过早引入 thickness/LUT/Compute | 首阶段只实现 Legacy 核心输入；扩展必须先更新合同和资源所有权设计 |

后续可独立规划：

- 厚度贴图和叶片局部 transmission control；
- 更完整的 wrap / multiple scattering 近似；
- 环境光 foliage transmission；
- 透明/排序/OIT 和透光阴影；
- 叶脉、风场、LOD、impostor 和 SpeedTree 资产 authoring；
- Substrate/Strata foliage 对齐；
- GPU reference image compare 和专用 runtime validation command。

## 14. 执行起点

实现任务开始时按以下顺序落地：

1. 先完成 P0 的 UE 语义和公式审计，不直接写 shader；
2. 新增 foliage MaterialInputs/MF 和最小 M_/MI fixture；
3. 先接入 Forward，建立正面/背光数值 probe；
4. 再接入 GBuffer 与 Deferred，完成 round-trip 和双路径对照；
5. 最后接入 ShadowDepth、SpeedTree 组合、Debug View、场景和测试；
6. 所有阶段门通过后再创建正式 `rendering/` 合同，不把计划文档提前标记为已实现。
