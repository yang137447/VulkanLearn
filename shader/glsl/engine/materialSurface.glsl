#ifndef VL_ENGINE_MATERIAL_SURFACE_GLSL
#define VL_ENGINE_MATERIAL_SURFACE_GLSL

#include "../common/shadingModel.glsl"
#include "materialContext.glsl"
#include "materialInputs.glsl"

#ifndef MATERIAL_SHADING_MODEL
#define MATERIAL_SHADING_MODEL SHADING_MODEL_DEFAULT_LIT
#endif

// MaterialSurface 是 Engine 将 MaterialInputs 与当前像素上下文合并后的内部数据。
// 它可以携带 Lighting/GBuffer 所需的世界空间位置和编码辅助字段，
// 但母材质公开入口不得直接构造或返回它。
struct MaterialSurface
{
    vec3 worldPosition;
    vec2 texCoord;
    // worldNormal 是顶层着色法线；Skin bottom normal 只属于 Skin LUT 漫反射输入，
    // Clear Coat bottom normal 仍保持独立，不能互相借用。
    vec3 worldNormal;
    vec3 clearCoatBottomNormal;
    vec3 preintegratedSkinBottomNormal;
    vec3 baseColor;
    // Hair 的 BaseColor 已在 Material Function 一次性转换成 sigma_a；Forward/Deferred 只消费该快照。
    vec3 hairAbsorption;
    float opacity;
    float opacityMask;
    vec3 emissiveColor;
    float roughness;
    float metallic;
    // Thin Translucent 的 Specular 只进入 UE Legacy 透射 Fresnel；表面反射使用 Default Lit F0。
    float specular;
    float ambientOcclusion;
    // 薄介质透射颜色和覆盖率分别控制透射吸收与 Add/Mul 对目标的作用范围。
    vec3 transmittanceColor;
    float surfaceCoverage;
    MaterialModelInputs modelInputs;
    uint shadingModel;
    uint selectiveOutputMask;
    // customData 只属于 GBuffer 编码层，不是 Material Function 的公共输入接口。
    vec4 customData;
    vec4 precomputedShadowFactors;
    vec4 worldTangent;
    float anisotropy;
};

MaterialSurface CreateDefaultMaterialSurface()
{
    MaterialSurface surface;
    surface.worldPosition = vec3(0.0);
    surface.texCoord = vec2(0.0);
    surface.worldNormal = vec3(0.0, 0.0, 1.0);
    surface.clearCoatBottomNormal = vec3(0.0, 0.0, 1.0);
    surface.preintegratedSkinBottomNormal = vec3(0.0, 0.0, 1.0);
    surface.baseColor = vec3(1.0);
    surface.hairAbsorption = vec3(1.0);
    surface.opacity = 1.0;
    surface.opacityMask = 1.0;
    surface.emissiveColor = vec3(0.0);
    surface.roughness = 1.0;
    surface.metallic = 0.0;
    surface.specular = 0.5;
    surface.ambientOcclusion = 1.0;
    surface.transmittanceColor = vec3(1.0);
    surface.surfaceCoverage = 1.0;
    surface.modelInputs = CreateDefaultMaterialInputs().modelInputs;
    surface.shadingModel = SHADING_MODEL_DEFAULT_LIT;
    surface.selectiveOutputMask = 0u;
    surface.customData = vec4(0.0);
    surface.precomputedShadowFactors = vec4(1.0);
    surface.worldTangent = vec4(1.0, 0.0, 0.0, 1.0);
    surface.anisotropy = 0.0;
    return surface;
}

// MeshPass 在调用 ShadingModel 或输出编码前统一执行这次语义解析。
// 这样 MaterialInputs 的所有权保持在母材质，Surface 的临时编码字段保持在 Engine。
MaterialSurface ResolveMaterialSurface(
    in MaterialInputs inputs,
    in MaterialFunctionContext context)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldPosition = context.worldPosition;
    surface.texCoord = context.texCoord;
    surface.worldNormal = normalize(inputs.normal);
    surface.clearCoatBottomNormal =
        normalize(inputs.modelInputs.clearCoat.bottomNormal);
    surface.preintegratedSkinBottomNormal = normalize(
        inputs.modelInputs.preintegratedSkin.bottomNormal);
    surface.worldTangent = inputs.tangent;
    surface.baseColor = inputs.baseColor;
    surface.hairAbsorption = inputs.modelInputs.hair.absorption;
    surface.opacity = inputs.opacity;
    surface.opacityMask = inputs.opacityMask;
    surface.emissiveColor = inputs.emissiveColor;
    surface.roughness = inputs.roughness;
    surface.metallic = inputs.metallic;
    surface.specular = inputs.specular;
    surface.ambientOcclusion = inputs.ambientOcclusion;
    surface.transmittanceColor =
        inputs.modelInputs.thinTranslucent.transmittanceColor;
    surface.surfaceCoverage =
        inputs.modelInputs.thinTranslucent.surfaceCoverage;
    surface.modelInputs = inputs.modelInputs;
    surface.shadingModel = MATERIAL_SHADING_MODEL;
    surface.precomputedShadowFactors = vec4(1.0);
    surface.anisotropy = inputs.modelInputs.anisotropy;

    // 这里原本是"把模型专用输入编码进 GBuffer customData"的分发链（Clear Coat / Subsurface /
    // TwoSidedFoliage / PreintegratedSkin / SubsurfaceProfile / Eye / Hair / Cloth）。
    // 2026-09-12 的旧实现清理把这些模型整体删除，因此分发链一并删除；MaterialSurface 的字段、
    // MaterialInputs 的模型结构体与 GBuffer 槽位**保留**，它们是 UE 对齐的接口面，
    // 按论文重建每个模型时在这里接回自己的编码分支。

    return surface;
}

#endif
