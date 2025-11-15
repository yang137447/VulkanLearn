vec4 PointLight(in mat4 modelMatrix, in vec3 lightPosition, in vec4 lightColor, in vec3 normal, in vec3 vertexPosition)
{
    // 点差法计算变换后的法线
    // TODO: 以后可优化为法线矩阵
    vec3 normalOffsetPosition = vertexPosition + normal;
    vec3 transNorOffsetPos = (modelMatrix * vec4(normalOffsetPosition, 1.0)).xyz;
    vec3 transVertexPos = (modelMatrix * vec4(vertexPosition, 1.0)).xyz;
    vec3 newNormal = normalize(transNorOffsetPos - transVertexPos);
    newNormal = normalize(newNormal);
    // 计算光源方向
    vec3 lightDir = normalize(lightPosition - transVertexPos);
    // 计算光照强度
    float intensity = max(dot(newNormal, lightDir), 0.0);
    // 计算光照颜色，并返回
    return lightColor * intensity;
}