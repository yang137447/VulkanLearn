mat3 GetNormalMatrix_WS(mat4 modelMatrix)
{
    return transpose(inverse(mat3(modelMatrix)));
}

vec3 GetNormal_WS(mat4 modelMatrix, vec3 normal)
{
    return normalize(GetNormalMatrix_WS(modelMatrix) * normal);
}

vec3 GetNormal_WS_Unnormalized(mat4 modelMatrix, vec3 normal)
{
    return GetNormalMatrix_WS(modelMatrix) * normal;
}

vec3 GetDirection_WS(mat4 modelMatrix, vec3 direction)
{
    return normalize(mat3(modelMatrix) * direction);
}

vec3 GetDirection_WS_Unnormalized(mat4 modelMatrix, vec3 direction)
{
    return mat3(modelMatrix) * direction;
}

float cheapContrast(float value, float contrast)
{
    return mix(-contrast, 1 + contrast, value);
}

vec3 LinearTosRGB(vec3 linearRGB)
{
    return mix(
        12.92 * linearRGB,
        1.055 * pow(linearRGB, vec3(1.0 / 2.4)) - 0.055,
        step(vec3(0.0031308), linearRGB));
}

vec3 SRGBtoLinear(vec3 sRGB)
{
    return mix(
        sRGB / 12.92,
        pow((sRGB + 0.055) / 1.055, vec3(2.4)),
        step(vec3(0.04045), sRGB));
}
