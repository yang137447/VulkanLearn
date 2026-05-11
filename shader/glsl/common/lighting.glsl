#include "defines.glsl"

struct Light{
    vec4 colorIntensity;
    vec4 positionRadius;
    vec4 directionPad;
    vec4 coneAngleOuterInnerPadPad;
};
// 这里用统一参数 + 分段储存的方式
layout(std430, set = 0, binding = 1) readonly buffer UBOLight{
    int directionalLightOffset;
    int directionalLightCount;
    int pointLightOffset;
    int pointLightCount;
    int spotLightOffset;
    int spotLightCount;

    Light lights[];
} uboLight;

// 绑定 0: 输入的ShadowMap
layout (set = 3, binding = 0) uniform sampler2DShadow shadowMap;

float CalculateShadow(mat4 lightViewProj, vec3 worldPos, float bias)
{
    vec4 lightViewProjPos = lightViewProj * vec4(worldPos, 1.0);
    vec3 shadowNdc = lightViewProjPos.xyz / lightViewProjPos.w;
    vec2 shadowUv = shadowNdc.xy * 0.5 + 0.5;
    float shadow = 1.0;
    if (shadowUv.x >= 0.0 && shadowUv.x <= 1.0 && shadowUv.y >= 0.0 && shadowUv.y <= 1.0 && shadowNdc.z >= 0.0 && shadowNdc.z <= 1.0)
    {
        shadow = texture(shadowMap, vec3(shadowUv, shadowNdc.z - bias));
    }
    return shadow;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// 先把法线方向投影到 9 项 SH 基底上，得到当前像素法线看到的低频环境 irradiance。
// 这里的系数来自 CPU 侧的环境贴图 radiance SH，而不是已经工程化卷积过的 diffuse SH。
// 所以 shader 里还要补一次 Lambert 漫反射卷积在各阶上的 band 系数：
//   L0 -> PI
//   L1 -> 2PI/3
//   L2 -> PI/4
// 如果 CPU 侧未来改成直接存工程化后的 C_i，那么这里的 bandWeights 就可以去掉，
// 最终形式会更接近学习资料里常见的：
//   L_diffuse = rho / PI * sum( C_i * T_i(n) )
// 当前之所以保留 bandWeights 在 shader 里，是为了明确区分：
//   1. CPU 持有的是 radiance SH
//   2. shader 这里正在把 radiance SH 转成 diffuse irradiance
vec3 EvaluateIrradianceSH(vec3 normal_WS)
{
    vec3 n = normalize(normal_WS);
    float x = n.x;
    float y = n.y;
    float z = n.z;

    float basis[9] = float[](
        0.282095,
        0.488603 * y,
        0.488603 * z,
        0.488603 * x,
        1.092548 * x * y,
        1.092548 * y * z,
        0.315392 * (3.0 * z * z - 1.0),
        1.092548 * x * z,
        0.546274 * (x * x - y * y)
    );
    float bandWeights[9] = float[](
        PI,
        2.0 * PI / 3.0,
        2.0 * PI / 3.0,
        2.0 * PI / 3.0,
        PI / 4.0,
        PI / 4.0,
        PI / 4.0,
        PI / 4.0,
        PI / 4.0
    );

    vec3 irradiance = vec3(0.0);
    for (int i = 0; i < 9; ++i)
    {
        irradiance += uboVP.environmentSH[i].rgb * basis[i] * bandWeights[i];
    }
    return max(irradiance, vec3(0.0));
}

// Diffuse IBL 单独作为间接光的一部分存在，不再混进直接光入口里。
// 金属表面的漫反射会被压到 0，保持与 PBR 的能量分配一致。
vec3 CalculateDiffuseIbl(
    in vec3 normal_WS,
    in vec3 baseColor,
    in float metallic)
{
    vec3 irradiance = EvaluateIrradianceSH(normal_WS);
    return irradiance * baseColor * (1.0 - metallic) / PI;
}

// 间接光总入口。目前只封装 diffuse IBL，后续可继续并入 specular IBL、
// probe 混合或其他间接光来源，但层级上始终保持在 Indirect Lighting 之下。
vec3 CalculateIndirectLighting(
    in vec3 normal_WS,
    in vec3 baseColor,
    in float metallic)
{
    return CalculateDiffuseIbl(normal_WS, baseColor, metallic);
}

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a      = roughness*roughness;
    float a2     = a*a;
    float NdotH  = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
	
    float num   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
	
    return num / denom;
}

float GeometrySchlickGGX(float cosTheta, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;

    float num   = cosTheta;
    float denom = cosTheta * (1.0 - k) + k;
	
    return num / denom;
}

// 几何遮蔽函数：Smith 联合遮蔽模型（GGX 版本）
// 同时考虑视线方向 V 与光源方向 L 的遮蔽，避免微面元被自身几何遮挡
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    // 计算法线与视线/光源方向的点积，并截断到非负值
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);

    // 分别计算视线方向与光源方向的 Schlick-GGX 遮蔽因子
    float ggx2 = GeometrySchlickGGX(NdotV, roughness); // 视线遮蔽
    float ggx1 = GeometrySchlickGGX(NdotL, roughness); // 光源遮蔽

    // 联合遮蔽 = 视线遮蔽 × 光源遮蔽
    // 值域 [0,1]，越粗糙的表面遮蔽越明显
    return ggx1 * ggx2;
}

vec3 CalculateDirectionalLight(
    in vec3 normal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in Light directionalLight
)
{
    // 根据金属度计算 F0：金属用 col，非金属用 0.04
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);

    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = directionalLight.colorIntensity.xyz;
    float lightIntensity = directionalLight.colorIntensity.w;
    vec3 lightDirection_WS = directionalLight.directionPad.xyz;

    // 中间量计算
    vec3 N = normalize(normal_WS);
    vec3 L = normalize(-lightDirection_WS);
    vec3 V = normalize(cameraPos_WS - pixelPos_WS);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    vec3 radiance = lightIntensity * lightColor;

    // Cook-Torrance BRDF
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float cosTheta = max(dot(H, V), 0.0);
    vec3  F = fresnelSchlick(cosTheta, F0);

    vec3 kS = F;
        // 金属没有漫反射！光线进入金属内部后会被自由电子迅速吸收并转化为热能。所以纯金属的漫反射颜色应该是黑色（0）
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // BRDF 分量
    vec3 diffuseBRDF  = kD * baseColor / PI;
    // 注意：分母中的 4*NdotL*NdotV 已在 GeometrySmith 中体现，这里直接乘上 NdotL
    vec3 specularBRDF = kS * D * G / max(4.0 * NdotL * NdotV, 1e-4);

    // 最终颜色
    vec3 color = (diffuseBRDF + specularBRDF) * radiance * NdotL;
    return color;
}

vec3 CalculatePointLight(
    in vec3 normal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in Light pointLight)
{
    // 根据金属度计算 F0：金属用 baseColor，非金属用 0.04
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);

    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = pointLight.colorIntensity.xyz;
    float lightIntensity = pointLight.colorIntensity.w;
    vec3 lightPos_WS = pointLight.positionRadius.xyz;
    float lightRadius = pointLight.positionRadius.w;

    // 中间量计算
    vec3 N = normalize(normal_WS);
    vec3 L = normalize(lightPos_WS - pixelPos_WS);
    vec3 V = normalize(cameraPos_WS - pixelPos_WS);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // 距离衰减：平方反比，并额外用 lightRadius 做平滑截断
    float distance = length(lightPos_WS - pixelPos_WS);
    float denom = distance * distance + 1e-4;
    float attenuation = 1.0 / denom;
    //attenuation *= 1.0 - smoothstep(lightRadius * 0.8, lightRadius, distance);
    vec3 radiance = attenuation * lightIntensity * lightColor;

    // Cook-Torrance BRDF
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // BRDF 分量
    vec3 diffuseBRDF  = kD * baseColor / PI;
    // 注意：分母中的 4*NdotL*NdotV 已在 GeometrySmith 中体现，这里直接乘上 NdotL
    vec3 specularBRDF = kS * D * G / max(4.0 * NdotL * NdotV, 1e-4);

    // 最终颜色
    vec3 color = (diffuseBRDF + specularBRDF) * radiance * NdotL;
    return color;
}

vec3 CalculateSpotLight(
    in vec3 normal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in Light spotLight)
{
    // 根据金属度计算 F0：金属用 baseColor，非金属用 0.04
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);

    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = spotLight.colorIntensity.xyz;
    float lightIntensity = spotLight.colorIntensity.w;
    vec3 lightPos_WS = spotLight.positionRadius.xyz;
    float lightRadius = spotLight.positionRadius.w;
    vec3 lightDirection_WS = spotLight.directionPad.xyz;
    float outerConeAngle = spotLight.coneAngleOuterInnerPadPad.x;
    float innerConeAngle = spotLight.coneAngleOuterInnerPadPad.y;
    
    // 中间量计算
    vec3 N = normalize(normal_WS);
    vec3 L = normalize(lightPos_WS - pixelPos_WS);
    vec3 V = normalize(cameraPos_WS - pixelPos_WS);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);

    // 聚光灯角度
    float spotLightAngle = acos(dot(L, -lightDirection_WS));
    float epsilon = innerConeAngle - outerConeAngle;
    float angleIntensity = clamp((spotLightAngle - outerConeAngle) / epsilon, 0.0, 1.0);
    lightIntensity *= angleIntensity;

    // 距离衰减：平方反比，并额外用 lightRadius 做平滑截断
    float distance = length(lightPos_WS - pixelPos_WS);
    float denom = distance * distance + 1e-4;
    float attenuation = 1.0 / denom;
    //attenuation *= 1.0 - smoothstep(lightRadius * 0.8, lightRadius, distance);
    vec3 radiance = attenuation * lightIntensity * lightColor;

    // Cook-Torrance BRDF
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);

    vec3 kS = F;
    vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);

    // BRDF 分量
    vec3 diffuseBRDF  = kD * baseColor / PI;
    // 注意：分母中的 4*NdotL*NdotV 已在 GeometrySmith 中体现，这里直接乘上 NdotL
    vec3 specularBRDF = kS * D * G / max(4.0 * NdotL * NdotV, 1e-4);

    // 最终颜色
    vec3 color = (diffuseBRDF + specularBRDF) * radiance * NdotL;
    return color;
}

vec3 CalculateDirectLighting(
    in vec3 normal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic
)
{
    // 直接光总入口：只聚合显式光源，不承担任何 IBL 或其他间接光职责。
    vec3 lighting = vec3(0.0);
    // 计算方向光
    int offset = uboLight.directionalLightOffset;
    int dirCount = uboLight.directionalLightCount;
    int end = offset + dirCount;
    for(int i = offset; i < end; i++)
    {
        lighting += CalculateDirectionalLight(
                                normal_WS, 
                                pixelPos_WS, 
                                cameraPos_WS, 
                                baseColor, 
                                roughness, 
                                metallic, 
                                uboLight.lights[i]);
    }
    // 计算点光源
    offset = uboLight.pointLightOffset;
    int pointCount = uboLight.pointLightCount;
    end = offset + pointCount;
    for(int i = offset; i < end; i++)
    {
        lighting += CalculatePointLight(
                                normal_WS, 
                                pixelPos_WS, 
                                cameraPos_WS, 
                                baseColor, 
                                roughness, 
                                metallic, 
                                uboLight.lights[i]);
    }
    // 计算聚光灯
    offset = uboLight.spotLightOffset;
    int spotCount = uboLight.spotLightCount;
    end = offset + spotCount;
    for(int i = offset; i < end; i++)
    {
        lighting += CalculateSpotLight(
                                normal_WS, 
                                pixelPos_WS, 
                                cameraPos_WS, 
                                baseColor, 
                                roughness, 
                                metallic, 
                                uboLight.lights[i]);
    }

    return lighting;
}
