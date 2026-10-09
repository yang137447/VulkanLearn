#include "core/renderer.h"

#include <cmath>
#include <stdexcept>

namespace VL::Offline
{
namespace
{
double Density(double noH, double sigma)
{
    if (noH <= 0.0 || noH >= 1.0) return 0.0;
    const double sin2 = 1.0 - noH * noH;
    return std::exp(-noH * noH / (sigma * sigma * sin2));
}

double Lookup(const std::array<double, kVelvetTableSize>& values, double cosine)
{
    const double x = std::clamp(cosine, 0.0, 1.0) * (kVelvetTableSize - 1);
    const int i = static_cast<int>(x);
    const int next = std::min(i + 1, kVelvetTableSize - 1);
    return values[i] * (1.0 - (x - i)) + values[next] * (x - i);
}

double Fresnel(double f0, double voH)
{
    return f0 + (1.0 - f0) * std::pow(1.0 - voH, 5.0);
}
} // namespace

double velvetSpecular(const VelvetMaterial& material, Vec3 n, Vec3 v, Vec3 l)
{
    const double noV = dot(n, v), noL = dot(n, l);
    if (noV <= 0.0 || noL <= 0.0 || length(v + l) < 1e-12) return 0.0;
    const Vec3 h = normalize(v + l);
    const double density = Density(dot(n, h), material.sigma);
    if (density == 0.0) return 0.0;
    const double gV = Lookup(material.g, noV);
    const double gL = Lookup(material.g, noL);
    const double pV = noV * material.normalMean / gV;
    const double pL = noL * material.normalMean / gL;
    // 式(14)：两方向投影夹角决定可见性的相关程度；正入射投影退化时取无关极限。
    const Vec3 tv = v - n * noV, tl = l - n * noL;
    double t = 0.0;
    if (length(tv) > 1e-10 && length(tl) > 1e-10)
    {
        const double angle = std::acos(std::clamp(
            dot(tv, tl) / (length(tv) * length(tl)), -1.0, 1.0));
        const double width = 15.0 * kPi / 180.0;
        t = std::exp(-0.5 * angle * angle / (width * width));
    }
    // 式(11) 经式(25) 代入：相关项相对式(26) 为 (1-t)+t/max(Pv,Pl)。
    const double correlated = (1.0 - t) + t / std::max(pV, pL);
    return material.normalization * density * material.normalMean *
           Fresnel(material.f0, dot(v, h)) * correlated / (4.0 * gV * gL);
}

Vec3 velvetBrdf(const VelvetMaterial& material, Vec3 n, Vec3 v, Vec3 l)
{
    const double noV = dot(n, v), noL = dot(n, l);
    if (noV <= 0.0 || noL <= 0.0) return {};
    // 式(30)：漫反射率不是额外的 Lambert 权重，而是剩余的方向能量。
    const double diffuse = material.diffuseScale *
        (1.0 - Lookup(material.reflectance, noV)) *
        (1.0 - Lookup(material.reflectance, noL));
    const double specular = velvetSpecular(material, n, v, l);
    return material.diffuse * diffuse + Vec3{specular, specular, specular};
}

VelvetMaterial makeVelvetMaterial(double sigma, double f0, Vec3 diffuse)
{
    if (sigma <= 0.0 || f0 < 0.0 || f0 > 1.0 ||
        diffuse.x < 0.0 || diffuse.y < 0.0 || diffuse.z < 0.0 ||
        diffuse.x > 1.0 || diffuse.y > 1.0 || diffuse.z > 1.0)
        throw std::invalid_argument("invalid Ashikhmin Velvet parameters");
    VelvetMaterial material;
    material.sigma = sigma;
    material.f0 = f0;
    material.diffuse = diffuse;
    constexpr int normalSteps = 128;
    constexpr int azimuthSteps = 64;
    double densityIntegral = 0.0, projectedIntegral = 0.0;
    for (int i = 0; i < normalSteps; ++i)
    {
        const double mu = (i + 0.5) / normalSteps;
        const double p = Density(mu, sigma);
        densityIntegral += p;
        projectedIntegral += mu * p;
    }
    material.normalization = normalSteps / (2.0 * kPi * densityIntegral);
    material.normalMean = projectedIntegral / densityIntegral;
    // 式(18) 的半球积分按余弦与方位角作确定性求积，包含正入射和掠射端点。
    for (int index = 0; index < kVelvetTableSize; ++index)
    {
        const double noK = static_cast<double>(index) / (kVelvetTableSize - 1);
        const double sinK = std::sqrt(1.0 - noK * noK);
        double sum = 0.0;
        for (int i = 0; i < normalSteps; ++i)
        {
            const double noH = (i + 0.5) / normalSteps;
            const double sinH = std::sqrt(1.0 - noH * noH);
            const double p = Density(noH, sigma);
            for (int j = 0; j < azimuthSteps; ++j)
            {
                const double phi = 2.0 * kPi * (j + 0.5) / azimuthSteps;
                sum += p * std::max(0.0, noK * noH +
                    sinK * sinH * std::cos(phi));
            }
        }
        material.g[index] = material.normalization * 2.0 * kPi * sum /
                             (normalSteps * azimuthSteps);
    }
    // 式(29) 的 R 只依赖入射天顶角；与求值器共用相关遮蔽与分布。
    constexpr int lightSteps = 48;
    const Vec3 n{0, 0, 1};
    for (int index = 0; index < kVelvetTableSize; ++index)
    {
        const double noV = std::max(1e-6,
            static_cast<double>(index) / (kVelvetTableSize - 1));
        const Vec3 v{std::sqrt(1.0 - noV * noV), 0, noV};
        double sum = 0.0;
        for (int i = 0; i < lightSteps; ++i)
        {
            const double noL = (i + 0.5) / lightSteps;
            const double sinL = std::sqrt(1.0 - noL * noL);
            for (int j = 0; j < azimuthSteps; ++j)
            {
                const double phi = 2.0 * kPi * (j + 0.5) / azimuthSteps;
                const Vec3 l{sinL * std::cos(phi), sinL * std::sin(phi), noL};
                sum += velvetSpecular(material, n, v, l) * noL;
            }
        }
        material.reflectance[index] = sum * 2.0 * kPi / (lightSteps * azimuthSteps);
    }
    double remaining = 0.0;
    for (int i = 0; i < kVelvetTableSize - 1; ++i)
    {
        const double mu = (i + 0.5) / (kVelvetTableSize - 1);
        remaining += (1.0 - Lookup(material.reflectance, mu)) * mu;
    }
    material.diffuseScale = 1.0 / (2.0 * kPi * remaining /
                                   (kVelvetTableSize - 1));
    return material;
}
} // namespace VL::Offline
