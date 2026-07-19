# Common Masked ShadowCaster 实现方案

## 文档状态

- 状态：已回退，保留为历史设计上下文
- 原范围：由 `RenderMode` 选择的公共 Opaque / Masked ShadowCaster
- 当前方向：`documents/plan/rendering/lightweight-material-shadow-caster-plan.md`

2026-07-16 的评审结论不再建设 Common Masked。当前待执行方向只保留公共 Opaque；
材质可通过 `xxx.shadow.vert/.frag` 提供专用 ShadowCaster，`OpaqueClip` 缺少专用实现时
故意使用公共 Opaque 显示实心诊断阴影。以下正文仅用于保留此前方案的设计背景。

本文记录 2026-07-13 关于 Alpha Clip ShadowCaster 的最新讨论结论。对于
`documents/plan/rendering/shadow-mode-material-pass-plan.html` 中“Masked 必须生成材质自己的
shadow variant”以及 shadow 专用 descriptor 的早期设想，以本文的近期方案为准。完整
Material Multi-Pass 和自定义 ShadowCaster 仍以长期规划为主。

## 核心结论

VulkanLearn 不维护独立 `ShadowMode`。Common ShadowCaster 行为由 surface `RenderMode`
直接确定：

```text
Opaque                  -> Common Opaque ShadowCaster
OpaqueClip              -> Common Masked ShadowCaster
TransparentAlphaBlend   -> skip common shadow draw
TransparentAdditive     -> skip common shadow draw
```

未来自定义 ShadowCaster 由 Material Multi-Pass 的 `ShadowCaster` pass 表达，不增加另一套
与 `RenderMode` 重叠的静态枚举。

对象是否参与 shadow pass 是独立于材质投影算法的职责。后续可增加对象级
`castShadow`，但不在本方案中把 `None` 混入 ShadowCaster 算法分类。

## Common Masked 能力边界

Common Masked 只支持标准 Alpha Clip 公式：

```glsl
float opacity = texture(albedoMap, fragTexCoord).a * u_tintColor.a;
if (opacity < u_alphaClipThreshold)
{
    discard;
}
```

它依赖以下现有材质接口：

- `albedoMap`: `sampler2D`
- `u_tintColor`: `vec4`
- `u_alphaClipThreshold`: `float`
- mesh UV0

以下情况不属于 Common Masked：

- opacity 不是由 `albedoMap.a * u_tintColor.a` 得到
- 使用自定义 UV、程序化 coverage 或特殊 threshold 规则
- ShadowCaster 必须执行 Wind、WPO、Skinning 或其他顶点变形
- 使用 Dither、LOD Fade 或其他自定义 coverage 策略

这些材质后续使用 Material Multi-Pass 自定义 `ShadowCaster` pass。第一阶段禁止静默退回
Opaque，避免产生实心卡片阴影。

## Descriptor Set 契约

现有 set 分层保持不变：

```text
Set 0: Shadow pass global data
       light view / projection and other shadow globals

Set 1: Existing MaterialInstance data
       material UBO
       albedoMap and other material textures

Set 2: Existing object data
       UBOModel
```

`u_alphaClipThreshold` 同时用于正常 OpaqueClip 显示和 Masked ShadowCaster，因此它始终是
Set 1 中的材质参数。`albedoMap` 和 `u_tintColor` 也继续使用原 MaterialInstance 的 Set 1。

禁止以下实现：

- 把 cutoff 或 tint alpha 复制到 `UBOModel`
- 新建 `UBOShadowMasked`
- 为 shadow 单独创建 material parameter buffer
- 为 shadow 单独创建 descriptor pool 或 descriptor set
- 在 shadow draw 前从 Set 1 复制参数到 Set 2

Common Masked pipeline 在创建时遵守现有材质 Set 1 layout 契约。Draw 阶段直接绑定
`RendererObjectGpuResources::descriptorSets` 中已有的 Set 1 和 Set 2，不进行运行时 layout
适配、descriptor 重写或资源复制。

第一阶段 Common Masked 面向当前标准 PBR / SpeedTree 材质接口。未来材质若不满足该接口，
必须提供自定义 `ShadowCaster` pass，而不是让 Common Masked 动态适配任意 Set 1。

## 材质数据

材质定义只声明已有的 surface render state：

```json
{
  "renderStates": {
    "renderMode": "Opaque",
    "cullMode": "Back"
  }
}
```

叶片 MaterialInstance 覆写为：

```json
{
  "renderStateOverrides": {
    "renderMode": "OpaqueClip",
    "cullMode": "None"
  },
  "parameters": {
    "u_alphaClipThreshold": 0.1
  }
}
```

典型叶片使用 `OpaqueClip`，主显示和 ShadowCaster 因而共同采用 Alpha Clip。这里不是
运行时猜测，而是 `RenderMode::OpaqueClip` 的固定渲染契约。

Masked 加载校验至少检查：

- 存在 `albedoMap`
- 存在 `u_tintColor`，类型为 `vec4`
- 存在 `u_alphaClipThreshold`，类型为 `float`
- 当前材质 Set 1 满足 Common Masked layout 契约

数据正确性在加载阶段保证。Shadow shader 不增加每帧 fallback、`clamp()` 或缺省纹理分支。

## Shader 结构

### Opaque

继续使用公共 `shadow.vert` 和 depth-only fragment 路径。它只需要 Set 0 和 Set 2。

### Masked Vertex

`shadowMasked.vert` 使用 Set 0 的 light view/projection 和 Set 2 的 model matrix，并向 fragment
传递 UV0：

```glsl
gl_Position = uboVP.projection * uboVP.view * uboM.model * vec4(inPosition, 1.0);
fragTexCoord = inTexCoord;
```

第一阶段不执行 Wind、WPO 或 Skinning。需要这些能力的材质必须使用未来的 `Custom`。

### Masked Fragment

`shadowMasked.frag` 直接读取现有 Set 1 的 `albedoMap`、`u_tintColor` 和
`u_alphaClipThreshold`，通过 `discard` 决定是否写入 shadow depth。

Surface 和 ShadowCaster 应共享一个小型 Alpha Clip helper，统一比较方向和边界语义：

```glsl
void ApplyAlphaClip(float opacity, float threshold)
{
    if (opacity < threshold)
    {
        discard;
    }
}
```

threshold 相等时保留片元。主显示和 shadow 不得各自维护不同的 cutoff 解释。

## Pipeline 选择

Common ShadowCaster 固定为两个 pass-material-owned pipeline：

```text
Opaque -> shadow       + cull Back
Masked -> shadowMasked + cull None
```

Shadow draw 只按 `RenderMode` 选择，不继承 surface Material 的 `cullMode`。Common Masked
语义就是双面薄片 Alpha Clip；未来若需要单面 Alpha Clip ShadowCaster，应使用 `Custom`，
而不是增加单面公共 variant。

Pipeline 必须在场景 / world renderer resource 初始化期间准备完成，禁止在逐帧 draw 录制中创建。

四个 CSM pass 引用同一个 `MI_shadow`。MI 的 cache identity 是规范化的 MI asset path，
不包含 `passName` 或 pipeline state，因此第二个及后续 cascade 会直接复用第一份 MI。
`MI_shadow -> M_shadow -> Material::renderPipeline` 提供固定的 Opaque pipeline；四个兼容
shadow pass 共享该 Material/pipeline。

`RenderGraph` 同时创建一个 shared `ShadowPassMaterial` 作为这组 pass 的 typed variant owner：

```text
ShadowPassMaterial
    opaquePipeline -> adopt MI_shadow base Material pipeline + cull Back
    maskedPipeline -> shadowMasked + cull None
```

这里的“公共”表示 renderer 级少量 variant，而不是强行让 Opaque 和 Masked 共用同一个
`VkPipeline`。两者 shader 和 descriptor 使用不同，因此仍是两个 pipeline variant。
`RenderSystem` 在 pass descriptor 初始化前让它扫描当前 `ResolvedRenderScene`；Opaque 总是准备，
Masked 在场景存在 `OpaqueClip` Material 时准备。resize / shutdown 统一清理。MI 不保存
`passName -> pipeline` 引用或 ShadowCaster 选择状态。

Set 0 descriptor plan 和 descriptor layout 来自 `ShadowPassMaterial` 采用的 Opaque pipeline contract。

Common Masked 的第一份材质 Set 1 layout 建立公共契约。后续 Masked 材质必须使用相同的
Set 1 descriptor layout 和 UBO 成员契约，否则在 renderer resource 初始化阶段报错。

## Shadow Draw 流程

`PassRuntime::RecordShadowPass` 不再为整个 pass 固定绑定对象绘制 pipeline。
`RendererDrawExecutor::DrawShadowScene` 在 MaterialInstance group 层从公共库选择：

```text
for each material instance group
    update existing MaterialInstance UBO
    resolve surface Material RenderMode
    bind common Opaque or common Masked pipeline
    bind shadow pass Set 0

    for each draw
        if Masked: bind existing Set 1
        bind existing Set 2
        update object UBO
        draw
```

Masked 直接绑定：

```cpp
const auto& descriptorSets =
    objectResources.descriptorSets[swapChainImageIndex];

// Set 1: existing MaterialInstance descriptors
descriptorSets[MaterialSetIndex];

// Set 2: existing object descriptors
descriptorSets[ObjectSetIndex];
```

Opaque 不读取材质参数，因此不需要绑定 Set 1。

## 资源清理

本次实现已移除原有残留或实验性的 shadow 专用资源：

- `UBOModel::alphaClip`
- `uboModelFields.glsl` 中的 alpha clip tail
- `RendererObjectGpuResources::shadowDescriptorPool`
- `RendererObjectGpuResources::shadowDescriptorSets`
- `RendererObjectGpuResources::shadowWriteDescriptorSets`
- `CreateShadowDescriptorSets`
- `UpdateShadowDescriptorSets`
- `DestroyShadowDescriptorSets`

对象已有的 `descriptorSets` 是 Opaque 和 Masked shadow draw 的唯一对象 / 材质 descriptor 来源。

## 代码落点

- `source/shaderVariant.h`
  - 保留 `RenderMode::OpaqueClip`
  - 不定义独立 `ShadowMode`
- `source/materialInstanceValidator.*`
  - 当 `renderMode=OpaqueClip` 时校验 Common Masked 所需参数和纹理
  - MI cache key 使用规范化 asset path
  - Material/pipeline cache key 使用 render-pass compatibility，不使用 `passName`
- `source/materialInstance.*`
  - 不保存 ShadowCaster 模式或 shadow pipeline 引用
- `source/render/shadow/shadowPassMaterial.*`
  - 持有固定 Opaque / Masked 公共 pipeline
  - Opaque pipeline 采用共享 `MI_shadow` 的 Base Material pipeline
  - 建立和校验 Common Masked Set 1 契约
- `source/renderGraph.*`
  - 让所有 CSM pass 共享一个 typed `ShadowPassMaterial`
  - 从 typed pass material 建立 Shadow Set 0 descriptor contract
- `source/renderSystem.*`
  - 在 pass descriptor 初始化前准备 `ShadowPassMaterial`
- `source/render/pass/passRuntime.cpp`
  - 不再为整个 shadow pass 固定一条对象绘制 pipeline
- `source/render/backend/rendererDrawExecutor.cpp`
  - 按 MaterialInstance group 选择 Opaque / Masked pipeline
  - 直接绑定已有 Set 1 / Set 2
- `source/render/backend/rendererObjectGpuResources.h`
  - 删除 shadow 专用 descriptor 资源
- `source/render/backend/rendererObjectResourceManager.cpp`
  - 删除 shadow descriptor 创建、更新和销毁逻辑
- `shader/glsl/shadowMasked.vert`
  - light-space transform 和 UV0 输出
- `shader/glsl/shadowMasked.frag`
  - 使用现有 Set 1 执行标准 Alpha Clip
- `shader/glsl/materialFunction/` 或 `shader/glsl/common/`
  - 提供 Surface / ShadowCaster 共用的 Alpha Clip helper

## 已完成的实施顺序

1. 清理 `UBOModel.alphaClip` 和 shadow 专用 descriptor 实验代码。
2. 使用 `RenderMode::OpaqueClip` 触发 Common Masked 加载校验。
3. 完成公共 `shadowMasked.vert/.frag` 和共享 Alpha Clip helper。
4. 创建固定 `Opaque + Back` / `Masked + None` 公共 shadow pipelines。
5. 改造 shadow draw，使其按 MaterialInstance group 选择 pipeline。
6. Masked 直接绑定现有 Set 1 / Set 2；Opaque 直接绑定现有 Set 2。
7. 为 SpeedTree 叶片配置 `OpaqueClip + cull None`。
8. 完成主显示、shadow silhouette 和运行时 cutoff 修改验证。

## 验收标准

- 普通 Opaque mesh 继续产生正确实心阴影。
- SpeedTree 树干继续走 Opaque ShadowCaster。
- 叶片主显示与 shadow map 都有相同 Alpha Clip 轮廓。
- 修改同一个 `u_alphaClipThreshold` 后，主显示和 shadow 同时变化。
- `u_tintColor.a` 对主显示与 shadow opacity 的影响一致。
- Common Masked 固定双面时叶片阴影不缺面。
- ShadowCaster 不读取或写入 `UBOModel` 中的材质参数。
- 初始化过程中不再分配 shadow 专用 descriptor pool / descriptor set。
- 声明 `Custom` 时得到清楚的未实现错误，而不是退回 Opaque。
- Vulkan validation layer 不报告 descriptor set 或 pipeline layout 错误。

## 后续 Custom ShadowCaster

未来 `Custom` 对应完整 Material Multi-Pass 中的 `ShadowCaster` material pass。它应继续复用
同一个 MaterialInstance Set 1，但允许提供自己的 vertex / fragment shader，并负责：

- 与主 Pass 相同的 Wind / WPO / Skinning
- 自定义 opacity / coverage
- LOD fade 和 dither
- 特殊 shadow raster state

Common Masked 是固定标准接口的快速路径，不应逐步堆叠成另一套可编程材质系统。
