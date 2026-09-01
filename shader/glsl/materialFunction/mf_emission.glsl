#ifndef VL_MATERIAL_FUNCTION_EMISSION_GLSL
#define VL_MATERIAL_FUNCTION_EMISSION_GLSL

// 将同一表面颜色按作者给定比例拆成受光 BaseColor 与自发光，保持源材质的能量分配语义。
struct MFEmissionSplitInput
{
    vec3 surfaceColor;
    float emissiveAmount;
};

struct MFEmissionSplitOutput
{
    vec3 baseColor;
    vec3 emissiveColor;
};

MFEmissionSplitOutput EvaluateMFEmissionSplit(
    in MFEmissionSplitInput emissionInput)
{
    MFEmissionSplitOutput outputValue;
    outputValue.baseColor =
        emissionInput.surfaceColor * (1.0 - emissionInput.emissiveAmount);
    outputValue.emissiveColor =
        emissionInput.surfaceColor * emissionInput.emissiveAmount;
    // NeoX 末尾还会除以全局 HDR luminance；VulkanLearn 的曝光/色调映射由后处理统一负责，MF 不重复归一化。
    return outputValue;
}

#endif
