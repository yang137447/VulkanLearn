# Lightweight Material ShadowCaster 执行计划

## 文档状态

- 状态：已实施（2026-07-16）
- 日期：2026-07-16
- 范围：公共 Opaque ShadowCaster、可选材质专用 ShadowCaster、CSM draw 路由
- 替代方向：不再实现 Common Masked ShadowCaster
- 非目标：自动生成 Shadow shader、完整 Material Multi-Pass、Shader Graph、Shadow MI
- 正式运行时合同：`documents/rendering/csm-shadow-map-m1.md`
- 材质/descriptor 合同：`documents/rendering/material-param-authoring-and-reflection.md`

本文保留已审核的实现边界与验收矩阵；后续行为变更应优先更新上述正式合同。

本文定义 VulkanLearn 第一版材质 ShadowCaster。方案保留一个公共 Opaque 快速路径，
需要 Alpha Clip、WPO、Wind 或其他特殊行为的材质由作者提供同名 `.shadow.vert/.shadow.frag`。
缺少专用 Shadow shader 的 `OpaqueClip` 故意使用公共 Opaque 投出实心阴影，让错误在预览中直接可见。

## 最终结论

```text
Material 有 xxx.shadow.vert + xxx.shadow.frag
    -> 使用材质专用 Shadow Pipeline
    -> 复用原 MaterialInstance 参数和纹理

Material 没有专用 Shadow shader
    -> Opaque / OpaqueClip 使用公共 Opaque Shadow Pipeline
    -> Transparent 跳过普通 Shadow Map
```

本方案不维护独立 `shadowMode`。ShadowCaster 选择来自：

1. 材质是否存在完整 `.shadow` shader pair。
2. 没有专用 pair 时，surface `RenderMode` 是否允许进入公共 Opaque 路径。

## 当前代码基线

当前运行时关系是：

```text
Renderpass
    -> MI_shadow
        -> M_shadow runtime Material
            -> Material::renderPipeline

RendererDrawExecutor::DrawShadowScene
    -> 所有 ResolvedMaterialGroup
    -> 统一绑定 M_shadow pipeline
    -> 绑定 Shadow Set 0
    -> 绑定独立 Shadow Object Set 2
```

当前约束：

- `Material` 只持有一条 `renderPipeline`。
- `PipelineFactory` 使用弱引用缓存 pipeline，`Material` 是 pipeline 的强引用 owner。
- Surface descriptor set 物理上由 `RendererObjectGpuResources` 持有，MI 是参数和纹理的逻辑来源。
- Shadow object descriptor 从公共 `M_shadow` 的 Set 2 layout 分配。
- `GraphicsPipeline` 完全根据当前 shader reflection 创建 descriptor set layout。
- `PipelineFactory` 仍用裸 `vk::RenderPass*` 作为 pipeline cache identity。
- 当前源码中没有已接入运行时的 `ShadowPassMaterial` 或 Common Masked pipeline。

## Shader 命名合同

Surface shader 保持现状：

```text
shader/glsl/speedtree.vert
shader/glsl/speedtree.frag
```

可选专用 ShadowCaster 使用 pass 后缀：

```text
shader/glsl/speedtree.shadow.vert
shader/glsl/speedtree.shadow.frag
```

由 Surface shader name 推导：

```cpp
surfaceShaderName = "speedtree";
shadowShaderName = surfaceShaderName + ".shadow";
```

当前 `ShaderCompiler` 已按 `shaderName + ".vert/.frag"` 读取 shader，因此
`ShaderVariantKey::shaderName = "speedtree.shadow"` 可以直接复用现有编译、SPIR-V 路径、
manifest 和 reflection 链路。

第一版规则：

| `.shadow.vert` | `.shadow.frag` | 结果 |
| --- | --- | --- |
| 不存在 | 不存在 | 没有专用 ShadowCaster，进入默认路由 |
| 存在 | 存在 | 创建材质专用 Shadow Pipeline |
| 存在 | 不存在 | 资产错误，加载失败 |
| 不存在 | 存在 | 资产错误，加载失败 |

第一版不支持 `xxx.vert + xxx.shadow.frag` 这种混合 stage。即使 Shadow 只需要特殊 fragment，
也必须提供完整 shader pair；不需要工作的 stage 使用最小实现。

## 资产格式

本阶段不向 `M_*.json` 增加 `passes`、`passTag` 或 `shadowShader` 字段。

原因：

- 当前只需要一个可选 ShadowCaster。
- 文件名可以从现有 Surface shader name 唯一推导。
- 当前 `ShaderVariantKey` 只支持同 basename 的 vertex/fragment pair。
- 避免为了一个 pass 提前落地完整 Material Multi-Pass 数据模型。

必须把 `.shadow` 规则写入正式材质文档，不能让它只存在于 loader 实现中。

## ShadowCaster 路由

新增内部解析结果：

```cpp
enum class ResolvedShadowCasterKind
{
    None,
    CommonOpaque,
    MaterialPass
};
```

它是 resolved render data，不是用户可配置的 `shadowMode`。

解析规则固定为：

```cpp
if (material.HasShadowPipeline())
{
    return ResolvedShadowCasterKind::MaterialPass;
}

switch (material.GetShaderVariantKey().renderMode)
{
case RenderMode::Opaque:
case RenderMode::OpaqueClip:
    return ResolvedShadowCasterKind::CommonOpaque;
case RenderMode::TransparentAlphaBlend:
case RenderMode::TransparentAdditive:
    return ResolvedShadowCasterKind::None;
}
```

行为矩阵：

| Surface | 专用 `.shadow` pair | Shadow 行为 |
| --- | --- | --- |
| Opaque | 无 | 公共 Opaque，正常默认行为 |
| Opaque | 有 | 材质专用，例如 WPO/Wind |
| OpaqueClip | 无 | 公共 Opaque，故意显示实心错误阴影 |
| OpaqueClip | 有 | 材质专用 Masked ShadowCaster |
| Transparent | 无 | 跳过普通 Shadow Map |
| Transparent | 有 | 使用材质专用 ShadowCaster |

缺少 `.shadow` 文件是允许的默认行为；只存在一半文件时输出警告并禁用专用 ShadowCaster，
最终仍按 `renderMode` 路由。完整 pair 的编译失败或接口不兼容必须终止 Material/World 加载。

## Pipeline 所有权

`Material` 改为持有 Surface 和可选 Shadow 两条 pipeline：

```cpp
class Material
{
public:
    const std::shared_ptr<PipelineBase>& GetRenderPipeline() const;
    bool HasShadowPipeline() const;
    const std::shared_ptr<PipelineBase>& GetShadowPipeline() const;

private:
    std::shared_ptr<PipelineBase> renderPipeline;
    std::shared_ptr<PipelineBase> shadowPipeline;
};
```

所有权关系：

```text
RendererResourceCache
    -> shared_ptr<Material variant>
        -> shared_ptr<Surface Pipeline>
        -> optional shared_ptr<Shadow Pipeline>

MaterialInstance
    -> weak_ptr<Material variant>
    -> canonical parameter and texture values

PipelineFactory
    -> weak pipeline cache only
```

动态参数或贴图不同、静态 variant 相同的 MI 共享 Surface/Shadow pipeline。
`renderMode`、宏、ShadingModel 或 cull state 不同仍产生不同 runtime Material variant。

公共 Opaque 继续由唯一 `MI_shadow -> M_shadow Material -> renderPipeline` 持有。
专用 Shadow pipeline 不放进 `MI_shadow`、Renderpass、RenderSystem 或 MaterialInstance。

## Shadow Variant 生成

专用 Shadow variant 从 Surface variant 派生：

```cpp
ShaderVariantKey shadowVariant = surfaceVariant;
shadowVariant.shaderName += ".shadow";
```

因此自动继承：

- `RenderMode`，包括 `RENDER_MODE_OPAQUE_CLIP`。
- ShadingModel define。
- MI 有效宏，例如 `USE_ALBEDO_MAP=1`。

Shadow pipeline state：

- depth test/write/compare 来自 canonical Shadow RenderGraph Pass。
- sample count、attachment count 来自 canonical Shadow RenderGraph Pass。
- cull mode 使用当前有效 Material variant 的 cull mode。
- blend 固定 Opaque。
- `bIsShadowPass = true`。

这样 SpeedTree 叶片 MI 的 `cullMode=None` 会进入其专用 Shadow pipeline；公共 Opaque pipeline
仍保持固定 back-face culling。

## Descriptor Set 合同

整套 descriptor set 复用只针对存在 `.shadow.vert/.shadow.frag` 的材质专用路径。
公共 Opaque 路径保持当前独立 Shadow descriptor 合同，不继承任意 Surface Material layout。

专用 Shadow pipeline 必须满足：

```text
Set 0: 与当前 Surface Material 完全兼容
Set 1: 与当前 Surface Material 完全兼容
Set 2: 与当前 Surface Material 完全兼容
Set 3: 第一版禁止使用
```

专用路径直接绑定当前对象 Surface descriptor package：

```cpp
Bind(materialShadowPipeline);
BindSurfaceDescriptorSets0To2(
    materialShadowPipeline,
    objectResources.descriptorSets[swapchainImageIndex]);
```

Surface Set 0 引用的 Global UBO 与 Shadow Pass 更新的是同一个 per-frame buffer。
`UpdateShadowGlobalUBOForPass()` 在每个 cascade 前写入 light view/projection，descriptor set
不需要重写。Set 0 中 Light、IBL 等 Shadow shader 未使用的额外资源可以保留。

本批不迁移 descriptor 的物理所有权。MI 仍是 Set 1 数据的逻辑 owner，
`RendererObjectGpuResources` 继续持有 Surface Set 0..2 的实际 descriptor sets。

公共 Opaque 路径保持当前方式：

```text
Set 0: 当前 Shadow Renderpass descriptor set
Set 1: 不使用
Set 2: 当前对象 shadowDescriptorSets[ObjectSetIndex]
```

因此本计划不删除独立 Shadow descriptor pool/set；它们继续服务公共 Opaque fallback。

### 为什么需要完整 Surface Layout Override

专用 Shadow shader 通常只读取 Surface Set 0..2 的一部分资源。若只按 Shadow SPIR-V reflection
创建 layout，各 Set 都可能缩小，不能直接绑定原 Surface descriptor package。

新增：

```cpp
struct GraphicsPipelineLayoutDesc
{
    std::array<std::vector<ShaderBinding>, MAX_DESCRIPTOR_SETS> setBindings;
    std::array<bool, MAX_DESCRIPTOR_SETS> overrideSets = {};
};
```

创建专用 Shadow pipeline 时：

1. 反射 `.shadow` shader 的实际绑定。
2. 从 Surface pipeline 提取完整 Set 0、Set 1、Set 2 binding contract。
3. 校验 Shadow shader 访问的每个 binding 都能在对应 Surface Set 中找到。
4. 校验 binding number、descriptor type、UBO size/member offset 等一致。
5. 校验 Shadow shader 没有在 Surface 未授权的 shader stage 访问 binding。
6. 创建 pipeline layout 时用 Surface 完整 Set 0..2 覆盖 Shadow 反射结果。

`PipelineFactory` cache key 必须包含规范化 `GraphicsPipelineLayoutKey`，避免相同 Shadow shader
在不同 Surface descriptor contract 下错误复用 pipeline。

第一版禁止专用 Shadow shader 使用“Surface shader 没有实际绑定，但只在 `M_*.json` 声明”的资源。
Shadow 对 Set 0..2 的实际访问必须是当前 Surface runtime contract 的子集。

### Shader Stage 可见性

继承的 layout 必须与 Surface descriptor set 的创建 layout 完全一致，不能为了 Shadow 单独扩大
stage flags。若 Shadow Vertex 读取 Material Set 1，Surface Vertex 也必须通过同一材质行为读取该
binding，使 Surface contract 本身已经包含 Vertex stage。

这符合 WPO 的实际语义：需要在 Shadow 重放的 Material WPO，也必须在 Surface Vertex 中执行。

加载时校验：

- binding number。
- descriptor type/count。
- shader stage visibility。
- UBO block size和 member offset。

不兼容直接终止加载，不为专用 Shadow pipeline 创建另一套 Global/Material/Object descriptor。

## Resolved Render Scene

`ResolvedMaterialGroup` 增加：

```cpp
ResolvedShadowCasterKind shadowCasterKind = ResolvedShadowCasterKind::None;
std::shared_ptr<PipelineBase> shadowPipeline;
bool diagnosticOpaqueFallback = false;
```

`ResolvedRenderSceneBuilder` 在 scene/material resolve 阶段完成选择：

- 有专用 pipeline：`MaterialPass`，保存 pipeline 强引用。
- Opaque 无专用：`CommonOpaque`。
- OpaqueClip 无专用：`CommonOpaque + diagnosticOpaqueFallback`。
- Transparent 无专用：`None`。

Shadow Pass 录制阶段只读取 resolved enum 和 pipeline，不查询文件、JSON、shader name 或 passTag。

`diagnosticOpaqueFallback` 用于 debug region 和一次性 warning，不改变绘制结果。

## Draw 执行

`PassRuntime::RecordShadowPass`：

1. 更新当前 cascade 的 Global UBO。
2. 开始 RenderPass。
3. 严格取得公共 `MI_shadow` 和 Opaque pipeline；缺失直接抛错。
4. 不再预先绑定唯一 pipeline。
5. 把公共 Opaque pipeline 交给 `RendererDrawExecutor::DrawShadowScene()`。

`DrawShadowScene()` 按 resolved material group 执行：

```cpp
for (const ResolvedMaterialGroup& materialGroup : scene.materialGroups)
{
    if (materialGroup.shadowCasterKind == ResolvedShadowCasterKind::None)
    {
        continue;
    }

    const PipelineBase& pipeline =
        materialGroup.shadowCasterKind == ResolvedShadowCasterKind::MaterialPass
            ? *materialGroup.shadowPipeline
            : commonOpaquePipeline;

    BindPipelineIfChanged(pipeline);

    for (const ResolvedMaterialInstanceGroup& instanceGroup : materialGroup.materialInstances)
    {
        if (materialGroup.shadowCasterKind == ResolvedShadowCasterKind::MaterialPass)
        {
            UpdateMaterialInstanceUbo(instanceGroup.materialInstance);
            BindSurfaceDescriptorSets0To2(pipeline, objectResources);
        }
        else
        {
            BindShadowGlobalSet0(pipeline);
            BindShadowObjectSet2(pipeline, objectResources);
        }

        DrawObject();
    }
}
```

专用路径复用 Surface Set 0..2；公共 Opaque 路径保持独立 Shadow Set 0/2 且不使用 Set 1。

## 公共 Shadow Pipeline 唯一性

当前 `GraphicsPipelineKey` 使用 `vk::RenderPass*`，四个 CSM pass 即使兼容也可能创建四份 pipeline。
本计划要求改为 render-pass compatibility identity：

```text
GraphicsPipelineKey
    -> RenderPassCompatibilityKey
    -> ShaderVariantKey
    -> GraphicsPipelineLayoutKey
    -> sample/state/attachment contract
```

裸 `vk::RenderPass*` 只作为 `vkCreateGraphicsPipelines` 的创建输入，不参与长期 cache identity。

RenderGraph 为每个 pass 建立 compatibility key，至少覆盖：

- attachment format和 sample count。
- color/depth attachment角色。
- subpass attachment引用结构。

四个 CSM Pass 必须在加载时验证 compatibility key 一致，并使用第一个 Shadow Pass 作为
canonical pipeline 创建输入。

四个 CSM Pass 的 `materialInstancePath` 都是 `MI_shadow`。Loader 按规范化 MI asset path
只创建一个实例，并把同一 weak/shared identity 分配给所有兼容 CSM Pass。

## SpeedTree 第一份专用实现

新增：

```text
shader/glsl/speedtree.shadow.vert
shader/glsl/speedtree.shadow.frag
shader/glsl/materialFunction/mf_speedtreeDeformation.glsl
shader/glsl/materialFunction/mf_speedtreeOpacity.glsl
```

`mf_speedtreeDeformation.glsl` 提供 Surface/Shadow 共用的局部位置变形入口。第一版即使只返回
原位置，也先建立唯一函数入口，后续 Wind/WPO 只修改这里。

`mf_speedtreeOpacity.glsl` 提供：

```glsl
float EvaluateSpeedTreeOpacity(vec2 texCoord)
{
#if USE_ALBEDO_MAP
    return texture(albedoMap, texCoord).a * u_tintColor.a;
#else
    return u_tintColor.a;
#endif
}
```

Surface 和 Shadow 都调用 `EvaluateSpeedTreeOpacity()` 与现有 `ApplyAlphaClip()`。

`speedtree.shadow.vert`：

- 使用 Shadow Set 0 的 light view/projection。
- 使用 Object Set 2 的 model matrix。
- 调用共享 deformation。
- 只向 fragment 输出 opacity 所需 varying。

`speedtree.shadow.frag`：

- `OpaqueClip` 调用共享 opacity 和 alpha clip。
- `Opaque` 保持最小 depth-only fragment。
- 不计算 PBR、normal、roughness、IBL 或 lighting。

由于 `.shadow` pair 按基础 shader name 自动发现，不需要修改 `M_speedtree.json` 或任何
SpeedTree MI JSON 来声明 ShadowCaster。

## 诊断与失败语义

允许的诊断 fallback：

```text
OpaqueClip + 没有任何 .shadow stage
    -> 公共 Opaque
    -> 实心阴影
    -> 每个 Material variant 输出一次 warning

只存在一个 .shadow stage
    -> 输出不完整 shader pair warning
    -> 禁用专用 ShadowCaster
    -> Opaque/OpaqueClip 使用公共 Opaque，Transparent 跳过
```

必须失败：

- `.shadow` shader 编译失败。
- Set 0/2 不兼容公共 Shadow contract。
- Set 1 访问不兼容 Surface Material contract。
- 声明了不存在的 binding、错误 UBO offset 或错误 descriptor type。
- 公共 `MI_shadow` 缺失。
- CSM RenderPass compatibility 不一致却尝试共享 pipeline。

完整 `.shadow` pair 的编译或反射合同错误不能解释为“没有专用 shader”。

## 生命周期

- Surface/Shadow pipeline 跟随 runtime `Material` 生命周期。
- Material/MI 由 `RendererResourceCache` 的 world-local snapshot/rollback 管理。
- PipelineFactory 只保存弱引用，不延长资源生命周期。
- swapchain resize 不因尺寸变化重建 shadow pipeline。
- RenderGraph reload 使用完整 pass pipeline contract 判断已有材质是否仍可复用。
- shader reload 失效对应 Surface/Shadow variant 和 resolved draw references。
- shutdown 先停止提交并等待安全点，再释放 Material 强引用和 pipeline。

## 实施批次

### P1：Shader Pair 发现与状态处理

修改：

- `source/render/shadow/materialShadowPipelineBuilder.*`
- `source/render/resource/rendererMaterialLoader.cpp`

完成：

- 从 Surface shader name 推导 `.shadow` basename。
- 区分 both/missing/partial 三种状态。
- partial pair 输出警告并禁用专用 ShadowCaster，最终按 `renderMode` 路由。
- complete pair 才进入编译、反射合同校验和 pipeline 创建。

### P2：Material 双 Pipeline 所有权

修改：

- `source/material.h/.cpp`
- `source/render/resource/rendererMaterialLoader.cpp`

完成：

- 保留 `renderPipeline` 作为 Material 的正常渲染管线。
- 增加 optional `shadowPipeline`。
- 从 Surface `ShaderVariantKey` 派生 Shadow variant。
- 正常渲染统一使用 `GetRenderPipeline()`；专用 Shadow 只通过 `GetShadowPipeline()` 访问。

### P3：Surface Descriptor Layout Inheritance

修改/新增：

- `source/pipeline/graphicsPipelineLayoutDesc.h`
- `source/pipeline/graphicsPipeline.*`
- `source/pipeline/pipelineFactory.*`
- `source/pipeline/pipelineLayoutBuilder.*`

完成：

- Surface Set 0..2 完整 contract 注入专用 Shadow pipeline layout。
- Shadow reflection 对每个 Set 做子集、stage 和 UBO 校验。
- Pipeline key 包含完整 GraphicsPipelineLayoutKey。

### P4：专用 Shadow 继承合同校验

修改：

- `source/render/resource/rendererMaterialLoader.cpp`
- 可新增 `source/render/shadow/shadowShaderContract.*`

完成：

- 专用 Shadow Set 0..2 与当前 Surface contract 比较。
- Set 3 禁止。
- 错误在 world/material load 阶段报告。
- 仅 `CommonOpaque` 对象创建独立 Shadow Set 0/2；专用和跳过对象不分配。

### P5：Resolved ShadowCaster 路由

修改：

- `source/render/backend/resolvedRenderScene.h/.cpp`

完成：

- 增加 `ResolvedShadowCasterKind`。
- material group 保存专用 pipeline 或公共路由结果。
- Transparent skip。
- OpaqueClip fallback 标记。

### P6：Shadow Draw Executor

修改：

- `source/render/pass/passRuntime.cpp`
- `source/render/backend/rendererDrawExecutor.*`

完成：

- pipeline bind 移到 material group 粒度。
- 专用路径绑定原 Surface Set 0..2。
- 公共路径继续绑定独立 Shadow Set 0/2。
- 删除 Shadow pass 中静默 return。

### P7：RenderPass Compatibility 与共享 MI_shadow

修改：

- `source/render/rendergraph/renderGraphCompiler.*`
- `source/renderGraph.*`
- `source/pipeline/pipelineFactory.*`
- `source/render/resource/rendererMaterialLoader.cpp`
- `source/materialInstanceValidator.cpp`

完成：

- pipeline key 改用 compatibility key。
- 四个 CSM Pass 共享一个 `MI_shadow` 和公共 Opaque pipeline。
- graph reload/resize 只按 compatibility 失效。

### P8：SpeedTree 专用 Shader 与共享函数

修改/新增：

- `shader/glsl/speedtree.vert/.frag`
- `shader/glsl/speedtree.shadow.vert/.frag`
- `shader/glsl/materialFunction/mf_speedtreeDeformation.glsl`
- `shader/glsl/materialFunction/mf_speedtreeOpacity.glsl`

完成：

- SpeedTree Alpha Clip shadow 正确。
- Surface/Shadow 共用 opacity/deformation 源码。
- PBR `OpaqueClip` 若没有 `.shadow` pair，继续显示实心诊断阴影。

### P9：清理旧 Common Masked 实验

删除：

- `shader/glsl/shadowMasked.vert`
- `shader/glsl/shadowMasked.frag`
- 未接入运行时的 Common Masked pipeline/layout 验证代码。

保留：

- `shader/glsl/materialFunction/mf_alphaClip.glsl`
- `RenderMode::OpaqueClip`。

文档：

- 更新 `documents/rendering/csm-shadow-map-m1.md` 为实现后的正式合同。
- 将 `common-masked-shadow-caster-plan.md` 标记为已回退历史方案。
- 更新 `material-multipass-pass-tag-plan.md`，说明完整 Multi-Pass 仍是长期方向。

## 验收矩阵

| 用例 | 预期 |
| --- | --- |
| 普通 Opaque，无 `.shadow` pair | 公共 Opaque 正常投影 |
| OpaqueClip，无 `.shadow` pair | 实心阴影并产生一次 warning |
| SpeedTree OpaqueClip，有完整 pair | Alpha Clip 阴影正确 |
| SpeedTree Opaque，有完整 pair | 使用专用 pipeline，不执行 clip |
| 只存在 `.shadow.frag` | world/material load 失败并回滚 |
| 专用 shader 编译错误 | 失败，不退回公共 Opaque |
| 专用 Shadow 任一 Set 0..2 不兼容 | 加载期失败 |
| Transparent 无专用 pair | 不进入普通 Shadow Map |
| 两个不同 Surface Set 0..2 contract 的专用材质 | 各自 pipeline 正确绑定原 Surface descriptor package |
| 四个 CSM cascade | 共用公共/专用 pipeline object |
| resize/minimize | 不因尺寸变化重建 shadow pipeline，无 Vulkan error |
| graph reload | compatibility 相同则复用，不同则安全重建 |
| world reload rollback | 旧 Material/MI/pipeline 恢复正确 |
| shutdown | descriptor/pipeline/renderpass 生命周期无 validation error |

## 验证命令

```powershell
cmake --build build -j
build/bin/main.exe --framesmoke 30 --exit-after-tests
build/bin/main.exe --resizestress 3 --exit-after-tests
build/bin/main.exe --graphreloadstress 3 --exit-after-tests
build/bin/main.exe --reloadstress scenes/SC_speedtree.json 3 --exit-after-tests
```

除自动命令外必须手工确认：

1. 移走 `speedtree.shadow.*` 后叶片阴影变成实心。
2. 恢复 pair 后叶片阴影重新出现 Alpha Clip 孔洞。
3. Surface 与 Shadow 修改同一个 `u_alphaClipThreshold` 后同步变化。
4. RenderDoc 中专用路径绑定原 Surface Set 0..2，公共路径仍绑定独立 Shadow Set 0/2。
5. 全流程没有 VUID、validation error、关闭时报错或最小化崩溃。

## 第一版非目标

- 不自动从 Surface shader 生成 Shadow shader。
- 不检测 GLSL 是否包含 WPO。
- 不实现 UE Material Graph/ShaderMap 系统。
- 不实现通用 `passes` JSON 或任意 PassTag。
- 不允许独立 vertex/fragment basename。
- 不创建 Shadow MaterialInstance 或 Shadow 专用材质参数副本。
- 不实现 Common Masked、bindless Material Table、透明彩色阴影。
- 不增加对象级 `castShadow`；保持当前对象参与规则。

## 开工完成定义

只有同时满足以下条件才算本计划完成：

- `.shadow` pair 约定成为正式材质合同。
- 公共 Opaque 和专用 Material ShadowCaster 都通过 resolved data 显式路由。
- OpaqueClip 缺失专用实现时稳定显示实心诊断阴影。
- 专用路径复用原 MI 参数和 Surface Set 0..2，不复制 Shadow descriptor/data。
- 公共 Opaque 路径继续使用独立 Shadow Set 0/2，不依赖任意 Surface layout。
- 四个兼容 CSM Pass 不按裸 RenderPass 指针重复创建 pipeline。
- SpeedTree 使用第一份真实专用 ShadowCaster 验证 Alpha Clip 和后续 deformation 扩展点。
- resize、reload、rollback、shutdown 与 Vulkan validation 全部通过。
