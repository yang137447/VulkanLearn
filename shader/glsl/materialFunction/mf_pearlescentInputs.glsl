#ifndef VL_MATERIAL_FUNCTION_PEARLESCENT_INPUTS_GLSL
#define VL_MATERIAL_FUNCTION_PEARLESCENT_INPUTS_GLSL

// 迁移自 NeoX pbr_subsurface_billboard：这里只负责片元 Pearl 输入。
// Billboard 顶点展开属于几何阶段；当前角色 Pose 已在 glTF 导出时离线烘焙，不能混入此 MF。
struct MFPearlescentInput
{
    vec2 coverageUv;
    vec3 matcapColor;
    vec3 noiseSample;
    vec3 noiseColor;
    vec3 tint;
    float contrast;
    float fresnelPower;
    float brightness;
    float roughness;
    float metallic;
    mat3 cameraToWorld;
    vec3 cameraForward;
};

struct MFPearlescentOutput
{
    vec3 surfaceColor;
    vec3 normal;
    float coverage;
    float roughness;
    float metallic;
};

MFPearlescentOutput EvaluateMFPearlescentInputs(
    in MFPearlescentInput pearlescentInput)
{
    MFPearlescentOutput outputValue;
    vec2 sphereOffset = pearlescentInput.coverageUv - vec2(0.5);
    // 源 shader 明确用 UV0 解析计算圆形边界，避免采样遮罩在珠子边缘损失精度。
    outputValue.coverage = step(length(sphereOffset), 0.49);

    // 保留 NeoX 假球法线的轴向约定：UV0 的 X/Y 映射到相机空间 -X/+Y，Z 指向相机内侧。
    vec2 normalXY = vec2(
        1.0 - pearlescentInput.coverageUv.y,
        pearlescentInput.coverageUv.x) * 2.0 - 1.0;
    vec3 sphereNormalVS = vec3(
        -normalXY.x,
        normalXY.y,
        -sqrt(max(0.0, 1.0 - dot(normalXY, normalXY))));
    outputValue.normal = normalize(
        pearlescentInput.cameraToWorld * sphereNormalVS);

    // 源实现使用相机前向而非逐像素视线计算 Fresnel，使整颗 Billboard 珠子的响应保持稳定。
    float fresnel = pow(
        1.0 - dot(outputValue.normal, pearlescentInput.cameraForward),
        pearlescentInput.fresnelPower);
    // 通用 Pearl 算法只消费已采样颜色；具体 UV、图集和纹理开关由调用方负责，避免绑定 NeoX 资源 ABI。
    outputValue.surfaceColor =
        pearlescentInput.tint *
        pearlescentInput.brightness *
        pow(pearlescentInput.matcapColor, vec3(pearlescentInput.contrast)) +
        vec3(fresnel);
#if USE_PEARL_NOISE_MAP
    // 源噪声被保留为可扩展的视角细节/焦散输入；当前槽位按源贴图和颜色权重直接叠加。
    outputValue.surfaceColor +=
        pearlescentInput.noiseSample * pearlescentInput.noiseColor;
#endif
    // 这里对应源 shader 的 saturate，是 Pearl 颜色公式的一部分，不是运行时防御性 clamp。
    outputValue.surfaceColor = clamp(
        outputValue.surfaceColor,
        vec3(0.0),
        vec3(1.0));
    outputValue.roughness = pearlescentInput.roughness;
    outputValue.metallic = pearlescentInput.metallic;
    return outputValue;
}

#endif
