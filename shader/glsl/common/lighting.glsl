#ifndef VL_COMMON_LIGHTING_GLSL
#define VL_COMMON_LIGHTING_GLSL

#include "defines.glsl"
#include "shadowFiltering.glsl"

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

layout(set = 0, binding = 2) uniform samplerCube prefilteredEnvironmentCube;
layout(set = 0, binding = 3) uniform sampler2D brdfLut;

// ShadowMap 属于“当前 pass 输入”，因此具体 binding 必须由 pass shader 自己声明。
// 这样 common/lighting.glsl 不会偷偷占用 Set 3，deferredLighting 才能把 Set 3 用作 GBuffer 输入集合。
int SelectShadowCascade(vec3 worldPos)
{
    vec4 viewPos = uboVP.view * vec4(worldPos, 1.0);
    float viewDepth = -viewPos.z;
    int activeCascadeCount = int(uboVP.csmParameters.z + 0.5);
    for (int cascadeIndex = 0; cascadeIndex < 4; ++cascadeIndex)
    {
        if (cascadeIndex >= activeCascadeCount - 1 ||
            viewDepth <= uboVP.cascadeSplits[cascadeIndex])
        {
            return cascadeIndex;
        }
    }
    return activeCascadeCount - 1;
}

float ShadowCascadeDebugValue(int cascadeIndex)
{
    float denominator =
        max(uboVP.csmParameters.z - 1.0, 1.0);
    return float(cascadeIndex) / denominator;
}

float SampleCsmCascade(
    in sampler2DArrayShadow inputShadowMap,
    vec3 worldPos,
    float receiverSlope,
    int cascadeIndex)
{
    vec4 lightViewProjPos =
        uboVP.lightViewProj[cascadeIndex] *
        vec4(worldPos, 1.0);
    vec3 shadowNdc =
        lightViewProjPos.xyz / lightViewProjPos.w;
    vec2 shadowUv =
        shadowNdc.xy * 0.5 + 0.5;
    if (shadowUv.x < 0.0 || shadowUv.x > 1.0 ||
        shadowUv.y < 0.0 || shadowUv.y > 1.0 ||
        shadowNdc.z < 0.0 || shadowNdc.z > 1.0)
    {
        return 1.0;
    }

    vec4 biasParameters =
        uboVP.shadowBias[cascadeIndex];
    float receiverBias =
        biasParameters.x *
        (1.0 + biasParameters.y * receiverSlope);

    // 阴影采样器使用 LESS_OR_EQUAL 比较。将接收者深度向光源方向偏移，可以减少
    // 自阴影痤疮；Bias 过大则会让接触阴影与投射物分离，产生 Peter-panning。
    float compareDepth =
        shadowNdc.z - receiverBias;
    return FilterShadowMap(
        inputShadowMap,
        shadowUv,
        cascadeIndex,
        compareDepth);
}

float CalculateCsmShadow(
    in sampler2DArrayShadow inputShadowMap,
    vec3 worldPos,
    vec3 worldNormal,
    out int cascadeIndex)
{
    cascadeIndex = 0;
    int activeCascadeCount =
        int(uboVP.csmParameters.z + 0.5);
    if (activeCascadeCount <= 0)
    {
        return 1.0;
    }

    cascadeIndex = SelectShadowCascade(worldPos);
    float viewDepth = -(uboVP.view * vec4(worldPos, 1.0)).z;
    float shadowDistance = uboVP.csmParameters.w;
    if (viewDepth > shadowDistance)
    {
        return 1.0;
    }

    // DirectionalLight.directionPad 保存的是光线传播方向（光源指向场景）。
    // 这里取反得到表面指向光源的方向，用于计算接收面随法线夹角变化的 Bias。
    vec3 surfaceToLight = normalize(
        -uboLight.lights[
            uboLight.directionalLightOffset]
            .directionPad.xyz);

    // 正对光源时 slope 为 0；接近掠射角或背光时 slope 趋近 1，需要更大的 Bias。
    float receiverSlope =
        1.0 -
        max(
            dot(
                normalize(worldNormal),
                surfaceToLight),
            0.0);
    float shadow = SampleCsmCascade(
        inputShadowMap,
        worldPos,
        receiverSlope,
        cascadeIndex);

    // UE 风格 Cascade Transition Fraction：在当前级联末端同时采样下一层，
    // 用深度区间平滑混合，避免级联分界处出现突变。
    if (cascadeIndex + 1 < activeCascadeCount)
    {
        float splitNear =
            cascadeIndex == 0
                ? 0.0
                : uboVP.cascadeSplits[
                    cascadeIndex - 1];
        float splitFar =
            uboVP.cascadeSplits[cascadeIndex];
        float transitionLength =
            (splitFar - splitNear) *
            uboVP.csmParameters.x;
        if (transitionLength > 0.0)
        {
            float transitionWeight = smoothstep(
                splitFar - transitionLength,
                splitFar,
                viewDepth);
            float nextCascadeShadow = SampleCsmCascade(
                inputShadowMap,
                worldPos,
                receiverSlope,
                cascadeIndex + 1);
            shadow = mix(
                shadow,
                nextCascadeShadow,
                transitionWeight);
        }
    }

    // UE 风格 Shadow Distance Fadeout Fraction：最后一段距离逐渐淡到全亮，
    // 避免超过 Dynamic Shadow Distance 时阴影突然消失。
    float fadeoutLength =
        shadowDistance *
        uboVP.csmParameters.y;
    if (fadeoutLength > 0.0)
    {
        float fadeoutWeight = smoothstep(
            shadowDistance - fadeoutLength,
            shadowDistance,
            viewDepth);
        shadow = mix(
            shadow,
            1.0,
            fadeoutWeight);
    }

    return shadow;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness)
{
    vec3 oneMinusRoughness = vec3(1.0 - roughness);
    return F0 + (max(oneMinusRoughness, F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
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
    return max(irradiance, vec3(0.0)) * uboVP.environmentIntensity;
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

vec3 SamplePrefilteredEnvironment(
    in vec3 normal_WS,
    in vec3 viewDir_WS,
    in float roughness)
{
    vec3 N = normalize(normal_WS);
    vec3 V = normalize(viewDir_WS);
    vec3 R = reflect(-V, N);
    float maxLod = float(max(textureQueryLevels(prefilteredEnvironmentCube) - 1, 0));
    return textureLod(
        prefilteredEnvironmentCube,
        R,
        roughness * maxLod).rgb;
}

vec2 SampleEnvironmentBrdf(float NdotV, float roughness)
{
    return texture(brdfLut, vec2(NdotV, roughness)).rg;
}

vec3 CalculateSpecularIblWithF0(
    in vec3 normal_WS,
    in vec3 viewDir_WS,
    in vec3 F0,
    in float roughness)
{
    vec3 N = normalize(normal_WS);
    vec3 V = normalize(viewDir_WS);
    float NdotV = max(dot(N, V), 0.0);
    vec3 prefilteredColor = SamplePrefilteredEnvironment(
        normal_WS,
        viewDir_WS,
        roughness);
    vec2 brdfAB = SampleEnvironmentBrdf(NdotV, roughness);
    return
        prefilteredColor *
        (F0 * brdfAB.x + brdfAB.y) *
        uboVP.environmentIntensity;
}

vec3 CalculateSpecularIbl(
    in vec3 normal_WS,
    in vec3 viewDir_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic)
{
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    return CalculateSpecularIblWithF0(
        normal_WS,
        viewDir_WS,
        F0,
        roughness);
}

// UE Legacy Clear Coat 把顶层视为固定 IOR 1.5 的无色介质：
// 空气进入清漆时 eta=1/1.5，对应法向入射 F0=0.04。
const float UE_CLEAR_COAT_F0 = 0.04;
const float UE_CLEAR_COAT_ETA = 1.0 / 1.5;
// 0.02 是模型合同的一部分，用于避免顶层 GGX 在完全光滑时退化成数值奇点。
const float UE_CLEAR_COAT_MIN_ROUGHNESS = 0.02;

float GetUeClearCoatRoughness(float roughness)
{
    return max(roughness, UE_CLEAR_COAT_MIN_ROUGHNESS);
}

float UeClearCoatFresnel(float cosine)
{
    // 顶层是固定无色介质，因此这里只需要计算标量 Schlick Fresnel。
    float fresnelCurve = pow(1.0 - cosine, 5.0);
    return fresnelCurve + (1.0 - fresnelCurve) * UE_CLEAR_COAT_F0;
}

vec3 FresnelSchlickUe(vec3 specularColor, float cosine)
{
    float fresnelCurve = pow(1.0 - cosine, 5.0);
    float grazingReflectance = clamp(50.0 * specularColor.g, 0.0, 1.0);
    return
        grazingReflectance * fresnelCurve +
        (1.0 - fresnelCurve) * specularColor;
}

void CalculateUeClearCoatIndirectColors(
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight,
    in float topNdotV,
    out vec3 diffuseColor,
    out vec3 specularColor)
{
    // UE Legacy Clear Coat 的间接光不是简单在 Default Lit 上叠一层高光。
    // 这里先根据顶层视角、底层粗糙度和金属度重映射底层 diffuse/specular 颜色，
    // 使底层在清漆折射和吸收之后仍与 UE 的能量分配保持一致。
    float refractionScale =
        ((topNdotV * 0.5 + 0.5) * topNdotV - 1.0) *
        clamp(1.25 - 1.25 * roughness, 0.0, 1.0) +
        1.0;

    const float metalSpecular = 0.9;
    vec3 absorptionColor = baseColor / metalSpecular;
    vec3 absorption =
        absorptionColor *
        ((topNdotV - 1.0) * 0.85 *
            (vec3(1.0) - mix(
                absorptionColor,
                absorptionColor * absorptionColor,
                -0.78)) +
        vec3(1.0));

    float layerAttenuation = mix(
        1.0,
        1.0 - UeClearCoatFresnel(topNdotV),
        clearCoatWeight);
    vec3 remappedBaseColor = mix(
        baseColor * layerAttenuation,
        metalSpecular * absorption * refractionScale,
        metallic * clearCoatWeight);
    diffuseColor = remappedBaseColor * (1.0 - metallic);

    float specular = mix(0.5, refractionScale, clearCoatWeight);
    specularColor = mix(
        vec3(0.08 * specular),
        remappedBaseColor,
        metallic);
}

vec3 CalculateClearCoatDiffuseIbl(
    in vec3 topNormal_WS,
    in vec3 bottomNormal_WS,
    in vec3 viewDir_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight)
{
    vec3 topNormal = normalize(topNormal_WS);
    vec3 viewDir = normalize(viewDir_WS);
    float topNdotV = clamp(abs(dot(topNormal, viewDir)) + 1e-5, 0.0, 1.0);
    vec3 diffuseColor;
    vec3 unusedSpecularColor;
    CalculateUeClearCoatIndirectColors(
        baseColor,
        roughness,
        metallic,
        clearCoatWeight,
        topNdotV,
        diffuseColor,
        unusedSpecularColor);
    // 漫反射来自清漆下方的底层，因此球谐辐照度使用 bottom normal 采样。
    return
        EvaluateIrradianceSH(bottomNormal_WS) *
        diffuseColor /
        PI;
}

vec3 CalculateClearCoatSpecularIbl(
    in vec3 topNormal_WS,
    in vec3 bottomNormal_WS,
    in vec3 viewDir_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight,
    in float clearCoatRoughness)
{
    vec3 topNormal = normalize(topNormal_WS);
    vec3 bottomNormal = normalize(bottomNormal_WS);
    vec3 viewDir = normalize(viewDir_WS);

    // 对齐 UE 稳定 NoV 约定：双面情况下使用绝对值，并把 BRDF LUT 坐标限制在有效范围。
    float topNdotV = clamp(abs(dot(topNormal, viewDir)) + 1e-5, 0.0, 1.0);
    float bottomNdotV = clamp(abs(dot(bottomNormal, viewDir)) + 1e-5, 0.0, 1.0);

    vec3 unusedDiffuseColor;
    vec3 specularColor;
    CalculateUeClearCoatIndirectColors(
        baseColor,
        roughness,
        metallic,
        clearCoatWeight,
        topNdotV,
        unusedDiffuseColor,
        specularColor);

    // 底层环境高光使用底层法线、底层粗糙度和重映射后的 specularColor。
    vec3 bottomEnvironment = SamplePrefilteredEnvironment(
        bottomNormal_WS,
        viewDir_WS,
        roughness);
    vec2 bottomBrdf = SampleEnvironmentBrdf(bottomNdotV, roughness);
    vec3 bottomFactor =
        specularColor * bottomBrdf.x +
        bottomBrdf.y *
            clamp(50.0 * specularColor.g, 0.0, 1.0) *
            (1.0 - clearCoatWeight);
    vec3 bottomLayer = bottomEnvironment * bottomFactor;

    float coatRoughness = GetUeClearCoatRoughness(clearCoatRoughness);

    // 顶层环境高光固定使用 F0=0.04，并由独立的 Clear Coat Roughness 选择 mip。
    vec3 topEnvironment = SamplePrefilteredEnvironment(
        topNormal_WS,
        viewDir_WS,
        coatRoughness);
    vec2 topBrdf = SampleEnvironmentBrdf(
        topNdotV,
        coatRoughness);
    float topFresnel =
        (UE_CLEAR_COAT_F0 * topBrdf.x + topBrdf.y) *
        clearCoatWeight;

    // topFresnel 同时决定顶层反射能量以及能进入底层的剩余能量。
    return
        (bottomLayer * (1.0 - topFresnel) +
            topEnvironment * topFresnel) *
        uboVP.environmentIntensity;
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

float DistributionGgxUe(float alphaSquared, float NdotH)
{
    float denominator =
        (NdotH * alphaSquared - NdotH) * NdotH + 1.0;
    return alphaSquared / (PI * denominator * denominator);
}

float VisibilitySmithJointApproxUe(
    float alphaSquared,
    float NdotV,
    float NdotL)
{
    float alpha = sqrt(alphaSquared);
    float visibilityV = NdotL * (NdotV * (1.0 - alpha) + alpha);
    float visibilityL = NdotV * (NdotL * (1.0 - alpha) + alpha);
    return 0.5 / (visibilityV + visibilityL);
}

float RefractBlendClearCoatApprox(float VdotH)
{
    // UE 针对 eta=1/1.5 的多项式近似，用于避免逐光源执行完整折射向量运算。
    return (0.63 - 0.22 * VdotH) * VdotH - 0.745;
}

struct UeClearCoatRefractionContext
{
    float NdotV;
    float NdotL;
    float VdotH;
};

UeClearCoatRefractionContext RefractClearCoatContext(
    float NdotV,
    float NdotL,
    float NdotH,
    float VdotH)
{
    // 把底层 BRDF 使用的 NoV/NoL/VoH 投影到穿过清漆后的近似折射空间。
    // 后续只需替换这些点积，不需要显式构造折射后的 L/V/H 向量。
    float refractionBlend = RefractBlendClearCoatApprox(VdotH);
    float refractionProjection = refractionBlend * NdotH;

    UeClearCoatRefractionContext context;
    context.NdotV = clamp(
        UE_CLEAR_COAT_ETA * NdotV - refractionProjection,
        0.001,
        1.0);
    context.NdotL = clamp(
        UE_CLEAR_COAT_ETA * NdotL - refractionProjection,
        0.001,
        1.0);
    context.VdotH = clamp(
        UE_CLEAR_COAT_ETA * VdotH - refractionBlend,
        0.0,
        1.0);
    return context;
}

vec3 SimpleClearCoatTransmittance(
    float NdotL,
    float NdotV,
    float metallic,
    vec3 baseColor)
{
    // UE 的简化薄层透射：非金属底层保持无色透射；金属底层根据 baseColor
    // 构造消光系数，并用 1/NoV + 1/NoL 近似光线往返薄层的路径长度。
    vec3 transmittance = vec3(1.0);
    if (metallic > 0.0)
    {
        vec3 transmittanceColor = baseColor / PI;
        vec3 extinctionCoefficient =
            -log(max(transmittanceColor, vec3(0.0001))) * 0.5;
        float thinDistance = 1.0 / NdotV + 1.0 / NdotL;
        vec3 opticalDepth =
            extinctionCoefficient * max(thinDistance - 2.0, 0.0);
        transmittance = mix(
            vec3(1.0),
            exp(-opticalDepth),
            metallic);
    }
    return transmittance;
}

vec3 EvaluateDefaultPbrLight(
    in vec3 normal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in vec3 lightDirection_WS,
    in vec3 radiance)
{
    vec3 N = normalize(normal_WS);
    vec3 L = normalize(lightDirection_WS);
    vec3 V = normalize(cameraPos_WS - pixelPos_WS);
    vec3 H = normalize(L + V);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 0.0);
    vec3 F0 = mix(vec3(0.04), baseColor, metallic);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 diffuseWeight = (vec3(1.0) - F) * (1.0 - metallic);
    float distribution = DistributionGGX(N, H, roughness);
    float geometry = GeometrySmith(N, V, L, roughness);
    vec3 diffuseBrdf = diffuseWeight * baseColor / PI;
    vec3 specularBrdf =
        F * distribution * geometry /
        max(4.0 * NdotL * NdotV, 1e-4);
    return (diffuseBrdf + specularBrdf) * radiance * NdotL;
}

vec3 EvaluateUeClearCoatLight(
    in vec3 topNormal_WS,
    in vec3 bottomNormal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight,
    in float clearCoatRoughness,
    in vec3 lightDirection_WS,
    in vec3 radiance)
{
    vec3 topNormal = normalize(topNormal_WS);
    vec3 bottomNormal = normalize(bottomNormal_WS);
    vec3 lightDirection = normalize(lightDirection_WS);
    vec3 viewDirection = normalize(cameraPos_WS - pixelPos_WS);
    vec3 halfDirection = normalize(lightDirection + viewDirection);

    float bottomNdotL = max(dot(bottomNormal, lightDirection), 0.0);
    float bottomNdotV = clamp(
        abs(dot(bottomNormal, viewDirection)) + 1e-5,
        0.0,
        1.0);
    float bottomNdotH = max(dot(bottomNormal, halfDirection), 0.0);
    float VdotH = max(dot(viewDirection, halfDirection), 0.0);
    if (bottomNdotL <= 0.0)
    {
        return vec3(0.0);
    }

    // UE Legacy Clear Coat 使用顶层法线计算清漆 BRDF，但是否受光仍沿用
    // UE 延迟光照预处理语义中的底层 NoL 门限与余弦项。
    float topNdotV = clamp(
        abs(dot(topNormal, viewDirection)) + 1e-5,
        0.0,
        1.0);
    float topNdotH = max(dot(topNormal, halfDirection), 0.0);
    float coatRoughness = GetUeClearCoatRoughness(clearCoatRoughness);
    float coatAlpha = coatRoughness * coatRoughness;
    float coatAlphaSquared = coatAlpha * coatAlpha;
    float coatFresnel = UeClearCoatFresnel(VdotH);
    float coatDistribution = DistributionGgxUe(
        coatAlphaSquared,
        topNdotH);
    float coatVisibility = VisibilitySmithJointApproxUe(
        coatAlphaSquared,
        topNdotV,
        bottomNdotL);
    // 顶层只包含固定介质 GGX 高光，强度由 Clear Coat Weight 控制。
    vec3 topLayer =
        radiance * bottomNdotL *
        coatDistribution * coatVisibility * coatFresnel *
        clearCoatWeight;

    // 同时计算未穿过清漆的默认底层响应，以及穿过清漆后的折射底层响应，
    // 最后按 Clear Coat Weight 混合；Weight=0 时选择未折射的底层分支。
    UeClearCoatRefractionContext refracted =
        RefractClearCoatContext(
            bottomNdotV,
            bottomNdotL,
            bottomNdotH,
            VdotH);
    float bottomAlphaSquared =
        roughness * roughness * roughness * roughness;
    float bottomDistribution = DistributionGgxUe(
        bottomAlphaSquared,
        bottomNdotH);
    float defaultVisibility = VisibilitySmithJointApproxUe(
        bottomAlphaSquared,
        bottomNdotV,
        bottomNdotL);

    float refractedVisibility = VisibilitySmithJointApproxUe(
        bottomAlphaSquared,
        refracted.NdotV,
        refracted.NdotL);

    vec3 defaultCommonSpecular =
        radiance * bottomNdotL *
        bottomDistribution * defaultVisibility;

    vec3 refractedCommonSpecular =
        radiance * bottomNdotL *
        bottomDistribution * refractedVisibility;

    vec3 bottomF0 = mix(vec3(0.04), baseColor, metallic);

    vec3 defaultSpecular =
        defaultCommonSpecular *
        FresnelSchlickUe(bottomF0, VdotH);

    // 光线进入和离开清漆各经历一次 Fresnel 透射，因此使用 (1-F)^2。
    float fresnelTransmission = 1.0 - coatFresnel;
    fresnelTransmission *= fresnelTransmission;

    // 介质吸收只作用于穿过清漆的底层分量，不影响顶层直接反射。
    vec3 mediumTransmission = SimpleClearCoatTransmittance(
        refracted.NdotL,
        refracted.NdotV,
        metallic,
        baseColor);

    vec3 refractedSpecular =
        refractedCommonSpecular *
        fresnelTransmission *
        mediumTransmission *
        FresnelSchlickUe(bottomF0, refracted.VdotH);

    vec3 defaultDiffuse =
        radiance * bottomNdotL *
        baseColor * (1.0 - metallic) / PI;
    vec3 refractedDiffuse =
        defaultDiffuse *
        fresnelTransmission * mediumTransmission;

    vec3 defaultBottomLayer =
        defaultDiffuse + defaultSpecular;

    vec3 refractedBottomLayer =
        refractedDiffuse + refractedSpecular;

    vec3 bottomLayer = mix(
        defaultBottomLayer,
        refractedBottomLayer,
        clearCoatWeight);
    return bottomLayer + topLayer;
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
    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = directionalLight.colorIntensity.xyz;
    float lightIntensity = directionalLight.colorIntensity.w;
    vec3 lightDirection_WS = directionalLight.directionPad.xyz;
    vec3 radiance = lightIntensity * lightColor;
    return EvaluateDefaultPbrLight(
        normal_WS,
        pixelPos_WS,
        cameraPos_WS,
        baseColor,
        roughness,
        metallic,
        normalize(-lightDirection_WS),
        radiance);
}

vec3 CalculateClearCoatDirectionalLight(
    in vec3 topNormal_WS,
    in vec3 bottomNormal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight,
    in float clearCoatRoughness,
    in Light directionalLight)
{
    vec3 radiance =
        directionalLight.colorIntensity.xyz *
        directionalLight.colorIntensity.w;
    return EvaluateUeClearCoatLight(
        topNormal_WS,
        bottomNormal_WS,
        pixelPos_WS,
        cameraPos_WS,
        baseColor,
        roughness,
        metallic,
        clearCoatWeight,
        clearCoatRoughness,
        normalize(-directionalLight.directionPad.xyz),
        radiance);
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
    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = pointLight.colorIntensity.xyz;
    float lightIntensity = pointLight.colorIntensity.w;
    vec3 lightPos_WS = pointLight.positionRadius.xyz;
    float lightRadius = pointLight.positionRadius.w;

    // 距离衰减：平方反比，并额外用 lightRadius 做平滑截断
    float distance = length(lightPos_WS - pixelPos_WS);
    float denom = distance * distance + 1e-4;
    float attenuation = 1.0 / denom;
    //attenuation *= 1.0 - smoothstep(lightRadius * 0.8, lightRadius, distance);
    vec3 radiance = attenuation * lightIntensity * lightColor;
    return EvaluateDefaultPbrLight(
        normal_WS,
        pixelPos_WS,
        cameraPos_WS,
        baseColor,
        roughness,
        metallic,
        normalize(lightPos_WS - pixelPos_WS),
        radiance);
}

vec3 CalculateClearCoatPointLight(
    in vec3 topNormal_WS,
    in vec3 bottomNormal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight,
    in float clearCoatRoughness,
    in Light pointLight)
{
    vec3 lightOffset = pointLight.positionRadius.xyz - pixelPos_WS;
    float distance = length(lightOffset);
    float attenuation = 1.0 / (distance * distance + 1e-4);
    vec3 radiance =
        attenuation *
        pointLight.colorIntensity.w *
        pointLight.colorIntensity.xyz;
    return EvaluateUeClearCoatLight(
        topNormal_WS,
        bottomNormal_WS,
        pixelPos_WS,
        cameraPos_WS,
        baseColor,
        roughness,
        metallic,
        clearCoatWeight,
        clearCoatRoughness,
        normalize(lightOffset),
        radiance);
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
    // 提取点光源的位置、半径、颜色、强度
    vec3 lightColor = spotLight.colorIntensity.xyz;
    float lightIntensity = spotLight.colorIntensity.w;
    vec3 lightPos_WS = spotLight.positionRadius.xyz;
    float lightRadius = spotLight.positionRadius.w;
    vec3 lightDirection_WS = spotLight.directionPad.xyz;
    float outerConeAngle = spotLight.coneAngleOuterInnerPadPad.x;
    float innerConeAngle = spotLight.coneAngleOuterInnerPadPad.y;

    // 聚光灯角度
    vec3 lightDirectionToSurface_WS = normalize(lightPos_WS - pixelPos_WS);
    float spotLightAngle = acos(dot(lightDirectionToSurface_WS, -lightDirection_WS));
    float epsilon = innerConeAngle - outerConeAngle;
    float angleIntensity = clamp((spotLightAngle - outerConeAngle) / epsilon, 0.0, 1.0);
    lightIntensity *= angleIntensity;

    // 距离衰减：平方反比，并额外用 lightRadius 做平滑截断
    float distance = length(lightPos_WS - pixelPos_WS);
    float denom = distance * distance + 1e-4;
    float attenuation = 1.0 / denom;
    //attenuation *= 1.0 - smoothstep(lightRadius * 0.8, lightRadius, distance);
    vec3 radiance = attenuation * lightIntensity * lightColor;
    return EvaluateDefaultPbrLight(
        normal_WS,
        pixelPos_WS,
        cameraPos_WS,
        baseColor,
        roughness,
        metallic,
        lightDirectionToSurface_WS,
        radiance);
}

vec3 CalculateClearCoatSpotLight(
    in vec3 topNormal_WS,
    in vec3 bottomNormal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight,
    in float clearCoatRoughness,
    in Light spotLight)
{
    vec3 lightOffset = spotLight.positionRadius.xyz - pixelPos_WS;
    vec3 lightDirectionToSurface_WS = normalize(lightOffset);
    float spotLightAngle = acos(dot(
        lightDirectionToSurface_WS,
        -spotLight.directionPad.xyz));
    float angleRange =
        spotLight.coneAngleOuterInnerPadPad.y -
        spotLight.coneAngleOuterInnerPadPad.x;
    float angleIntensity = clamp(
        (spotLightAngle - spotLight.coneAngleOuterInnerPadPad.x) /
            angleRange,
        0.0,
        1.0);
    float distance = length(lightOffset);
    float attenuation = 1.0 / (distance * distance + 1e-4);
    vec3 radiance =
        attenuation * angleIntensity *
        spotLight.colorIntensity.w *
        spotLight.colorIntensity.xyz;
    return EvaluateUeClearCoatLight(
        topNormal_WS,
        bottomNormal_WS,
        pixelPos_WS,
        cameraPos_WS,
        baseColor,
        roughness,
        metallic,
        clearCoatWeight,
        clearCoatRoughness,
        lightDirectionToSurface_WS,
        radiance);
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

vec3 CalculateClearCoatDirectLighting(
    in vec3 topNormal_WS,
    in vec3 bottomNormal_WS,
    in vec3 pixelPos_WS,
    in vec3 cameraPos_WS,
    in vec3 baseColor,
    in float roughness,
    in float metallic,
    in float clearCoatWeight,
    in float clearCoatRoughness)
{
    vec3 lighting = vec3(0.0);
    int offset = uboLight.directionalLightOffset;
    int end = offset + uboLight.directionalLightCount;
    for (int i = offset; i < end; ++i)
    {
        lighting += CalculateClearCoatDirectionalLight(
            topNormal_WS,
            bottomNormal_WS,
            pixelPos_WS,
            cameraPos_WS,
            baseColor,
            roughness,
            metallic,
            clearCoatWeight,
            clearCoatRoughness,
            uboLight.lights[i]);
    }

    offset = uboLight.pointLightOffset;
    end = offset + uboLight.pointLightCount;
    for (int i = offset; i < end; ++i)
    {
        lighting += CalculateClearCoatPointLight(
            topNormal_WS,
            bottomNormal_WS,
            pixelPos_WS,
            cameraPos_WS,
            baseColor,
            roughness,
            metallic,
            clearCoatWeight,
            clearCoatRoughness,
            uboLight.lights[i]);
    }

    offset = uboLight.spotLightOffset;
    end = offset + uboLight.spotLightCount;
    for (int i = offset; i < end; ++i)
    {
        lighting += CalculateClearCoatSpotLight(
            topNormal_WS,
            bottomNormal_WS,
            pixelPos_WS,
            cameraPos_WS,
            baseColor,
            roughness,
            metallic,
            clearCoatWeight,
            clearCoatRoughness,
            uboLight.lights[i]);
    }
    return lighting;
}

#endif
