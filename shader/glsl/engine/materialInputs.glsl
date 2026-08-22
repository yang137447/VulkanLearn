#ifndef VL_ENGINE_MATERIAL_INPUTS_GLSL
#define VL_ENGINE_MATERIAL_INPUTS_GLSL

// MaterialInputs 是母材质公开 Surface 入口返回的稳定语义合同。
// 它只描述材质输入，不负责灯光、Shadow Map、GBuffer 或最终颜色输出。
struct ClearCoatMaterialInputs
{
    float weight;
    float roughness;
    vec3 bottomNormal;
};

struct ThinTranslucentMaterialInputs
{
    vec3 transmittanceColor;
    float surfaceCoverage;
};

struct MaterialModelInputs
{
    // 每个字段只属于一个 ShadingModel，禁止把模型专用数据塞进无语义 customData。
    ClearCoatMaterialInputs clearCoat;
    ThinTranslucentMaterialInputs thinTranslucent;
    float anisotropy;
};

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

MaterialInputs CreateDefaultMaterialInputs()
{
    MaterialInputs inputs;
    inputs.baseColor = vec3(1.0);
    inputs.metallic = 0.0;
    inputs.specular = 0.5;
    inputs.roughness = 1.0;
    inputs.emissiveColor = vec3(0.0);
    inputs.opacity = 1.0;
    inputs.opacityMask = 1.0;
    inputs.normal = vec3(0.0, 0.0, 1.0);
    inputs.tangent = vec4(1.0, 0.0, 0.0, 1.0);
    inputs.ambientOcclusion = 1.0;
    inputs.worldPositionOffset = vec3(0.0);
    inputs.pixelDepthOffset = 0.0;

    inputs.modelInputs.clearCoat.weight = 0.0;
    inputs.modelInputs.clearCoat.roughness = 0.0;
    inputs.modelInputs.clearCoat.bottomNormal = inputs.normal;
    inputs.modelInputs.thinTranslucent.transmittanceColor = vec3(1.0);
    inputs.modelInputs.thinTranslucent.surfaceCoverage = 1.0;
    inputs.modelInputs.anisotropy = 0.0;
    return inputs;
}

#endif
