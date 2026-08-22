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
    // worldNormal 表示清漆顶层法线；底层法线用于底漆的漫反射和高光。
    vec3 worldNormal;
    vec3 clearCoatBottomNormal;
    vec3 baseColor;
    float opacity;
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
    surface.worldNormal = vec3(0.0, 0.0, 1.0);
    surface.clearCoatBottomNormal = vec3(0.0, 0.0, 1.0);
    surface.baseColor = vec3(1.0);
    surface.opacity = 1.0;
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
    surface.worldNormal = normalize(inputs.normal);
    surface.clearCoatBottomNormal =
        normalize(inputs.modelInputs.clearCoat.bottomNormal);
    surface.worldTangent = inputs.tangent;
    surface.baseColor = inputs.baseColor;
    surface.opacity = inputs.opacity;
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

    // 这里把模型专用输入一次性编码到 GBuffer customData；后续 pass 不再读取 MaterialInputs。
    if (surface.shadingModel == SHADING_MODEL_CLEAR_COAT)
    {
        // Clear Coat 的模型专用输入只在这里转换为 GBuffer 内部编码。
        surface.customData.xy = vec2(
            inputs.modelInputs.clearCoat.weight,
            inputs.modelInputs.clearCoat.roughness);
        surface.selectiveOutputMask |= GBUFFER_HAS_CUSTOM_DATA_MASK;
    }
    else if (surface.shadingModel == SHADING_MODEL_SUBSURFACE)
    {
        surface.customData = vec4(
            inputs.modelInputs.subsurface.color,
            inputs.modelInputs.subsurface.weight);
        surface.selectiveOutputMask |= GBUFFER_HAS_CUSTOM_DATA_MASK;
    }
    else if (surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN)
    {
        surface.customData = vec4(
            inputs.modelInputs.preintegratedSkin.skinLutId,
            inputs.modelInputs.preintegratedSkin.thickness,
            inputs.modelInputs.preintegratedSkin.thicknessScale,
            inputs.modelInputs.preintegratedSkin.weight);
        surface.selectiveOutputMask |= GBUFFER_HAS_CUSTOM_DATA_MASK;
    }
    else if (surface.shadingModel == SHADING_MODEL_SUBSURFACE_PROFILE)
    {
        surface.customData = vec4(
            inputs.modelInputs.subsurfaceProfile.profileId,
            inputs.modelInputs.subsurfaceProfile.weight,
            inputs.modelInputs.subsurfaceProfile.thickness,
            inputs.modelInputs.subsurfaceProfile.transmissionWeight);
        surface.selectiveOutputMask |= GBUFFER_HAS_CUSTOM_DATA_MASK;
    }

    return surface;
}

#endif
