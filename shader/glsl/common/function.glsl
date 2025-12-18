vec3 GetNormal_WS(mat4 modelMatrix, vec3 normal, vec3 vertexPosition)
{
    // 点差法计算世界空间下的法线
        // TODO: 以后可优化为法线矩阵
    vec3 normalOffsetPosition = vertexPosition + normal;
    vec3 transNorOffsetPos = (modelMatrix * vec4(normalOffsetPosition, 1.0)).xyz;
    vec3 transVertexPos = (modelMatrix * vec4(vertexPosition, 1.0)).xyz;
    vec3 newNormal = normalize(transNorOffsetPos - transVertexPos);
    newNormal = normalize(newNormal);
    return newNormal;
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