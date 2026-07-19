#pragma once

// 文件职责：声明材质专用 Shadow 管线的构建入口，由材质加载阶段调用；
// 普通 Opaque 的公共 Shadow 管线和 ShadowCaster 路由不由本文件负责。
// File responsibility: Declares material-specific shadow pipeline construction for the material loading stage;
// common opaque shadow pipelines and ShadowCaster routing remain outside this file.

#include <memory>

class Material;
class PipelineBase;
class PipelineFactory;
struct MaterialInstanceBuildPlan;
struct Renderpass;

namespace VL
{

// 按显式 `.shadow` override、自动 Material ShadowDepth 的优先级构建材质专用管线。
// 输入来自已验证的 MI 构建计划；普通 Opaque 返回空指针，由渲染场景选择公共管线。
// Builds a material-specific pipeline, preferring an explicit `.shadow` override over generated ShadowDepth.
// Input comes from a validated MI plan; common opaque materials return null for scene-level routing.
std::shared_ptr<PipelineBase> BuildMaterialShadowPipeline(
    PipelineFactory& pipelineFactory,
    Renderpass* canonicalShadowPass,
    const MaterialInstanceBuildPlan& loadPlan,
    const Material& material);

} // namespace VL
