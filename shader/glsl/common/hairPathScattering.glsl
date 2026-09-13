#ifndef VL_COMMON_HAIR_PATH_SCATTERING_GLSL
#define VL_COMMON_HAIR_PATH_SCATTERING_GLSL

// 头发路径散射的纯数学部分：R / TT / TRT 纵向高斯、Fresnel 与散射上下文。
//
// 这些函数不依赖 UBO，也不采样任何 LUT，因此 Material Surface 阶段可以直接调用。
// 2026-09-12 的旧实现清理删掉了原来的宿主文件 engine/hairScattering.glsl（连同
// SampleHairAzimuthalLut() 与 hairAzimuthalLut sampler）。本文件被保留下来，
// 因为论文曲线探针 M_brdfPlot 的 mode 1 正是绘制这里的 R / TT / TRT 曲线：
// 探针必须复用与论文公式同一份实现，而不是另抄一遍。
// 重建 Hair 时，方位角 LUT 的采样函数与它的 sampler 声明要在模型自己的文件里重新引入。

const float HAIR_PI = 3.14159265358979323846;
const float HAIR_UE_SHIFT = 0.035;
const float HAIR_UE_F0 = 0.046521;
const float HAIR_UE_ONE_MINUS_F0 = 0.953479;

struct HairUeScatteringContext
{
    float viewLightDot;
    float sinThetaL;
    float sinThetaV;
    float cosThetaD;
    float cosPhi;
    float cosHalfPhi;
};

HairUeScatteringContext BuildHairUeScatteringContext(
    vec3 tangent,
    vec3 lightDirection,
    vec3 viewDirection)
{
    HairUeScatteringContext context;
    context.viewLightDot = dot(viewDirection, lightDirection);
    context.sinThetaL = clamp(dot(tangent, lightDirection), -1.0, 1.0);
    context.sinThetaV = clamp(dot(tangent, viewDirection), -1.0, 1.0);
    context.cosThetaD = cos(0.5 * abs(
        asin(context.sinThetaV) - asin(context.sinThetaL)));

    vec3 projectedLight = lightDirection - context.sinThetaL * tangent;
    vec3 projectedView = viewDirection - context.sinThetaV * tangent;
    context.cosPhi = dot(projectedLight, projectedView) * inversesqrt(
        dot(projectedLight, projectedLight) *
            dot(projectedView, projectedView) +
        1.0e-4);
    context.cosHalfPhi = sqrt(clamp(
        0.5 + 0.5 * context.cosPhi,
        0.0,
        1.0));
    return context;
}

float EvaluateHairUeGaussian(float width, float theta)
{
    // UE Hair_g 只钳制归一化分母，不钳制指数中的 B；低粗糙度仍保留
    // 原始高光形状，同时避免归一化项产生过大的数值。
    float denominatorWidth = max(width, 0.01);
    return exp(-0.5 * theta * theta / (width * width)) /
        (sqrt(2.0 * HAIR_PI) * denominatorWidth);
}

float EvaluateHairUeFresnel(float cosine)
{
    // UE Legacy Hair_F 使用固定 IOR=1.55 的 Schlick Fresnel；R 传入
    // sqrt((1+VoL)/2)，TT/TRT 传入各自的内部界面 cosine。
    float grazing = 1.0 - clamp(cosine, 0.0, 1.0);
    return HAIR_UE_F0 + HAIR_UE_ONE_MINUS_F0 *
        grazing * grazing * grazing * grazing * grazing;
}

vec3 EvaluateHairUeR(
    HairUeScatteringContext context,
    float roughness,
    float specular,
    float area,
    float backlit)
{
    // UE Legacy 固定 Shift=0.035，R 使用 -2*Shift。
    float alpha = -2.0 * HAIR_UE_SHIFT;
    float sinAlpha = sin(alpha);
    float cosAlpha = cos(alpha);
    float shift = 2.0 * sinAlpha * (
        cosAlpha * context.cosHalfPhi *
            sqrt(max(1.0 - context.sinThetaV * context.sinThetaV, 0.0)) +
        sinAlpha * context.sinThetaV);
    // alpha=-0.07（默认 cuticleTilt=0.035）正好对应源生成代码中的
    // -0.139886、0.997551、-0.069943 三个常量。
    float roughnessSquared = roughness * roughness;
    float width = (area + roughnessSquared) *
        sqrt(2.0) * context.cosHalfPhi;
    float longitudinal = EvaluateHairUeGaussian(
        width,
        context.sinThetaL + context.sinThetaV - shift);
    float azimuthal = 0.25 * context.cosHalfPhi;
    float fresnel = EvaluateHairUeFresnel(sqrt(clamp(
        0.5 + 0.5 * context.viewLightDot,
        0.0,
        1.0)));
    float activeBacklit = 1.0;
#if HAIR_USE_BACKLIT
    activeBacklit = backlit;
#endif
    return vec3(
        longitudinal * azimuthal * fresnel * specular * 2.0 *
        mix(1.0, activeBacklit, clamp(-context.viewLightDot, 0.0, 1.0)));
}

vec3 EvaluateHairUeTT(
    HairUeScatteringContext context,
    float roughness,
    vec3 baseColor,
    float backlit,
    float area)
{
    // UE Legacy 固定 Shift=0.035，TT 使用 +Shift。
    float alpha = HAIR_UE_SHIFT;
    float roughnessSquared = roughness * roughness;
    float width = area + 0.5 * roughnessSquared;
    float longitudinal = EvaluateHairUeGaussian(
        width,
        context.sinThetaL + context.sinThetaV - alpha);
    float nPrime = 1.19 / context.cosThetaD + 0.36 * context.cosThetaD;
    float inverseNPrime = 1.0 / nPrime;
    float h = context.cosHalfPhi *
        (1.0 + inverseNPrime * (0.6 - 0.8 * context.cosPhi));
    // Legacy TT 先在入射界面求 Fresnel，再使用两次透射概率 (1-F)^2。
    float fresnel = EvaluateHairUeFresnel(
        context.cosThetaD * sqrt(clamp(1.0 - h * h, 0.0, 1.0)));
    float interfaceWeight = (1.0 - fresnel) * (1.0 - fresnel);
    float activeBacklit = 1.0;
#if HAIR_USE_BACKLIT
    activeBacklit = backlit;
#endif
    float pathExponent = 0.5 *
        sqrt(max(
            1.0 - (h * inverseNPrime) * (h * inverseNPrime),
            0.0)) /
            context.cosThetaD;
    vec3 transmittance = pow(
        abs(baseColor),
        vec3(pathExponent));
    float azimuthal = exp(-3.65 * context.cosPhi - 3.98);
    return longitudinal * azimuthal * interfaceWeight * transmittance * activeBacklit;
}

vec3 EvaluateHairUeTRT(
    HairUeScatteringContext context,
    float roughness,
    vec3 baseColor,
    float area)
{
    // UE Legacy 固定 Shift=0.035，TRT 使用 +4*Shift。
    float alpha = 4.0 * HAIR_UE_SHIFT;
    float roughnessSquared = roughness * roughness;
    float width = area + 2.0 * roughnessSquared;
    float longitudinal = EvaluateHairUeGaussian(
        width,
        context.sinThetaL + context.sinThetaV - alpha);
    // Legacy TRT 的界面权重是 (1-F)^2*F：两次透射和一次内部反射。
    float fresnel = EvaluateHairUeFresnel(context.cosThetaD * 0.5);
    float interfaceWeight = (1.0 - fresnel) * (1.0 - fresnel) * fresnel;
    vec3 transmittance = pow(
        abs(baseColor),
        vec3(0.8 / context.cosThetaD));
    float azimuthal = exp(17.0 * context.cosPhi - 16.78);
    return longitudinal * azimuthal *
        interfaceWeight * transmittance;
}

#endif
