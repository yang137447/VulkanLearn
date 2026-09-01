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

struct SubsurfaceMaterialInputs
{
    vec3 color;
    float weight;
    float wrapWidth;
    float backscatterPower;
    float backscatterWeight;
    float thickness;
    float transmissionWeight;
};

struct PreintegratedSkinMaterialInputs
{
    float skinLutId;
    float thickness;
    float thicknessScale;
    float weight;
    float curvature;
    float transmissionWeight;
    // 顶层法线负责高光，bottomNormal 负责 Skin LUT 的漫反射/散射方向。
    vec3 bottomNormal;
    // x/y/z/w 对齐源角色光照倍率：环境、方向光、GI、虚拟光。
    vec4 characterLighting;
};

struct SubsurfaceProfileMaterialInputs
{
    float profileId;
    float weight;
    float thickness;
    float transmissionWeight;
};

struct HairMaterialInputs
{
    float scatter;
    float backlit;
    float cuticleTilt;
    float longitudinalRoughness;
    float azimuthalRoughness;
    float ior;
    vec3 absorption;
    float fiberRadius;
    float multipleScatteringWeight;
    float coverage;
    float density;
    // x/y/z/w 分别是角色环境光、方向光、局部光倍率和相机虚拟光强度。
    vec4 characterLighting;
};

struct ClothMaterialInputs
{
    vec3 sheenColor;
    float sheenRoughness;
    // signed anisotropy：正值沿 fiber tangent 拉伸，负值交换 tangent/bitangent 主轴。
    float anisotropy;
    // cross=1 时等权叠加正交纤维瓣；0 时只保留主纤维瓣。
    float anisotropyCross;
};

struct EyeMaterialInputs
{
    vec3 corneaNormal;
    float corneaIor;
    vec3 irisNormal;
    float irisMask;
    vec3 irisPlaneNormal;
    float irisDistance;
    vec3 irisColor;
    float irisRadius;
    vec3 scleraColor;
    float pupilRadius;
    float limbusWidth;
    float causticProfileId;
    float scleraProfileId;
    float causticStrength;
    float validIrisHit;
    vec2 irisUv;
    float irisHitDistance;
    float pupilMask;
    float limbusMask;
    float eyeLayer;
    float contactVisibility;
    float ciliaVisibility;
    float uvHandedness;
    float pupilDilation;
    vec3 gazeDirection;
    float gazeWeight;
};

struct MaterialModelInputs
{
    // 每个字段只属于一个 ShadingModel，禁止把模型专用数据塞进无语义 customData。
    ClearCoatMaterialInputs clearCoat;
    ThinTranslucentMaterialInputs thinTranslucent;
    SubsurfaceMaterialInputs subsurface;
    PreintegratedSkinMaterialInputs preintegratedSkin;
    SubsurfaceProfileMaterialInputs subsurfaceProfile;
    HairMaterialInputs hair;
    ClothMaterialInputs cloth;
    EyeMaterialInputs eye;
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
    inputs.modelInputs.subsurface.color = vec3(1.0);
    inputs.modelInputs.subsurface.weight = 0.0;
    inputs.modelInputs.subsurface.wrapWidth = 0.5;
    inputs.modelInputs.subsurface.backscatterPower = 4.0;
    inputs.modelInputs.subsurface.backscatterWeight = 0.5;
    inputs.modelInputs.subsurface.thickness = 0.01;
    inputs.modelInputs.subsurface.transmissionWeight = 0.0;
    inputs.modelInputs.preintegratedSkin.skinLutId = 0.0;
    inputs.modelInputs.preintegratedSkin.thickness = 0.01;
    inputs.modelInputs.preintegratedSkin.thicknessScale = 1.0;
    inputs.modelInputs.preintegratedSkin.weight = 0.0;
    inputs.modelInputs.preintegratedSkin.curvature = 0.0;
    inputs.modelInputs.preintegratedSkin.transmissionWeight = 0.0;
    inputs.modelInputs.preintegratedSkin.bottomNormal = inputs.normal;
    inputs.modelInputs.preintegratedSkin.characterLighting =
        vec4(1.0, 1.0, 1.0, 0.0);
    inputs.modelInputs.subsurfaceProfile.profileId = 0.0;
    inputs.modelInputs.subsurfaceProfile.weight = 0.0;
    inputs.modelInputs.subsurfaceProfile.thickness = 0.01;
    inputs.modelInputs.subsurfaceProfile.transmissionWeight = 0.0;
    inputs.modelInputs.hair.scatter = 0.0;
    inputs.modelInputs.hair.backlit = 0.0;
    inputs.modelInputs.hair.cuticleTilt = 0.0;
    inputs.modelInputs.hair.longitudinalRoughness = 0.22;
    inputs.modelInputs.hair.azimuthalRoughness = 0.25;
    inputs.modelInputs.hair.ior = 1.55;
    inputs.modelInputs.hair.absorption = vec3(1.0);
    inputs.modelInputs.hair.fiberRadius = 0.00005;
    inputs.modelInputs.hair.multipleScatteringWeight = 0.0;
    inputs.modelInputs.hair.coverage = 1.0;
    inputs.modelInputs.hair.density = 1.0;
    inputs.modelInputs.hair.characterLighting = vec4(1.0, 1.0, 1.0, 0.0);
    inputs.modelInputs.cloth.sheenColor = vec3(0.0);
    inputs.modelInputs.cloth.sheenRoughness = 0.5;
    inputs.modelInputs.cloth.anisotropy = 0.0;
    inputs.modelInputs.cloth.anisotropyCross = 0.0;
    inputs.modelInputs.eye.corneaNormal = inputs.normal;
    inputs.modelInputs.eye.corneaIor = 1.376;
    inputs.modelInputs.eye.irisNormal = inputs.normal;
    inputs.modelInputs.eye.irisMask = 1.0;
    inputs.modelInputs.eye.irisPlaneNormal = inputs.normal;
    inputs.modelInputs.eye.irisDistance = 0.003;
    inputs.modelInputs.eye.irisColor = vec3(0.35, 0.12, 0.04);
    inputs.modelInputs.eye.irisRadius = 0.006;
    inputs.modelInputs.eye.scleraColor = vec3(0.9, 0.92, 0.95);
    inputs.modelInputs.eye.pupilRadius = 0.002;
    inputs.modelInputs.eye.limbusWidth = 0.0005;
    inputs.modelInputs.eye.causticProfileId = 0.0;
    inputs.modelInputs.eye.scleraProfileId = 0.0;
    inputs.modelInputs.eye.causticStrength = 0.0;
    inputs.modelInputs.eye.validIrisHit = 0.0;
    inputs.modelInputs.eye.irisUv = vec2(0.0);
    inputs.modelInputs.eye.irisHitDistance = 0.0;
    inputs.modelInputs.eye.pupilMask = 0.0;
    inputs.modelInputs.eye.limbusMask = 0.0;
    inputs.modelInputs.eye.eyeLayer = 0.0;
    inputs.modelInputs.eye.contactVisibility = 1.0;
    inputs.modelInputs.eye.ciliaVisibility = 1.0;
    inputs.modelInputs.eye.uvHandedness = 1.0;
    inputs.modelInputs.eye.pupilDilation = 0.5;
    inputs.modelInputs.eye.gazeDirection = vec3(0.0, 0.0, 1.0);
    inputs.modelInputs.eye.gazeWeight = 1.0;
    inputs.modelInputs.anisotropy = 0.0;
    return inputs;
}

#endif
