#pragma once

// 文件职责：集中决定材质在 Shadow Pass 中跳过、使用公共 Opaque 管线或使用材质专用管线；
// 不创建管线，也不提交 Shadow draw call。
// File responsibility: Centralizes whether a material skips the shadow pass, uses the common opaque pipeline,
// or uses a material-specific pipeline; it neither creates pipelines nor submits shadow draw calls.

class Material;

namespace VL
{

// ShadowCaster 路由结果的类别。
// Category of a resolved ShadowCaster route.
enum class MaterialShadowCasterKind
{
    None,
    CommonOpaque,
    MaterialPass
};

// 资源解析阶段写入 ResolvedScene、绘制阶段只读消费的 ShadowCaster 决策。
// ShadowCaster decision written into ResolvedScene during resolution and read during drawing.
struct MaterialShadowCasterDecision
{
    MaterialShadowCasterKind kind = MaterialShadowCasterKind::None;
};

// ShadowCaster 路由的唯一决策入口。ResolvedScene 和 DrawExecutor 消费结果，
// 不再各自解释 renderMode 或 shadowPipeline。
// The single ShadowCaster routing entry point, shared by ResolvedScene and DrawExecutor so they do not
// independently interpret renderMode or shadowPipeline.
MaterialShadowCasterDecision ResolveMaterialShadowCaster(const Material& material);

} // namespace VL
