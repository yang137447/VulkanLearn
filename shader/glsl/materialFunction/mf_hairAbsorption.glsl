#ifndef VL_MATERIAL_FUNCTION_HAIR_ABSORPTION_GLSL
#define VL_MATERIAL_FUNCTION_HAIR_ABSORPTION_GLSL

vec3 ConvertHairBaseColorToAbsorption(
    vec3 baseColor,
    float absorptionScale,
    float fiberRadius)
{
    // 以四倍纤维半径作为作者颜色的参考光程：TT 的典型穿透接近 sqrt(BaseColor)，
    // TRT 的更长光程继续自然加深。这样深色会得到更大的 sigma_a，且数值单位为 1/m。
    float referencePathLength = 4.0 * fiberRadius;
    vec3 referenceTransmittance = clamp(
        baseColor,
        vec3(1.0 / 255.0),
        vec3(1.0));
    return -log(referenceTransmittance) * absorptionScale /
        referencePathLength;
}

#endif
