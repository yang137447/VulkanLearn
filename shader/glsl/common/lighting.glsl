// struct DirectionalLight {
//     vec4 colorIntensity;
//     vec4 directionPad;
// };

// struct PointLight {
//     vec4 colorIntensity;
//     vec4 positionRadius;
// };

// struct SpotLight {
//     vec4 colorIntensity;
//     vec4 positionRadius;
//     vec4 directionPad;
//     vec4 coneAngleOuterInnerPadPad;
// };
struct Light{
    vec4 colorIntensity;
    vec4 positionRadius;
    vec4 directionPad;
    vec4 coneAngleOuterInnerPadPad;
};
// 这里用统一参数 + 分段储存的方式
layout(std430, binding = 1) readonly buffer UBOLight{
    int directionalLightOffset;
    int directionalLightCount;
    int pointLightOffset;
    int pointLightCount;
    int spotLightOffset;
    int spotLightCount;

    Light lights[];
} uboLight;

vec3 CalculatePointLight(
    in mat4 modelMatrix,
    in vec3 normal,
    in vec3 vertexPosition,
    in vec3 cameraPosition,
    in vec3 ambientColor, 
    in Light pointLight)
{
    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = pointLight.colorIntensity.xyz;
    float lightIntensity = pointLight.colorIntensity.w;
    vec3 lightPosition = pointLight.positionRadius.xyz;
    float lightRadius = pointLight.positionRadius.w;

    // 待计算的三部分：散射光、镜面光、环境光
    vec3 diffuse = vec3(0.0, 0.0, 0.0);
    vec3 specular = vec3(0.0, 0.0, 0.0);
    vec3 ambient = vec3(0.0, 0.0, 0.0);

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
    specular = lightColor * pow(max(dot(newNormal, halfDir), 0.0), 32.0);
    // 计算 环境光 = 环境光颜色
    ambient = ambientColor;
    // 返回最终光照颜色
    return diffuse + specular + ambient;
}
