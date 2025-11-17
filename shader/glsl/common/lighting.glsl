vec4 PointLight(
    in mat4 modelMatrix,
    in vec3 normal,
    in vec3 vertexPosition,
    in vec3 cameraPosition,
    in vec4 ambientColor, 
    in vec3 lightPosition, 
    in vec4 lightColor, 
    in vec4 lightSpecular
    )
{
    // 待计算的三部分：散射光、镜面光、环境光
    vec4 diffuse = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 specular = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 ambient = vec4(0.0, 0.0, 0.0, 0.0);

    // 中间量计算
        // 点差法计算世界空间下的法线
        // TODO: 以后可优化为法线矩阵
    vec3 normalOffsetPosition = vertexPosition + normal;
    vec3 transNorOffsetPos = (modelMatrix * vec4(normalOffsetPosition, 1.0)).xyz;
    vec3 transVertexPos = (modelMatrix * vec4(vertexPosition, 1.0)).xyz;
    vec3 newNormal = normalize(transNorOffsetPos - transVertexPos);
    newNormal = normalize(newNormal);
        // 计算光源方向
    vec3 lightDir = normalize(lightPosition - transVertexPos);
        // 计算摄像机方向
    vec3 transCameraPos = (modelMatrix * vec4(cameraPosition, 1.0)).xyz;
    vec3 cameraDir = normalize(transCameraPos - transVertexPos);
        // 计算半程向量
    vec3 halfDir = normalize(lightDir + cameraDir);

    // 计算 散射光 = 光源颜色 * 光照强度
    diffuse = lightColor * max(dot(newNormal, lightDir), 0.0);
    // 计算 镜面光 = 镜面光颜色 * 反射光强度
    specular = lightSpecular * pow(max(dot(newNormal, halfDir), 0.0), 32.0);
    // 计算 环境光 = 环境光颜色
    ambient = ambientColor;
    // 返回最终光照颜色
    return diffuse + specular + ambient;
}