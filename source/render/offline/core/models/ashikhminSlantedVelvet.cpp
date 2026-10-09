#include "core/renderer.h"

#include <cmath>
#include <stdexcept>

namespace VL::Offline
{
namespace
{
constexpr int kSlopeSteps = 256;

struct CylindricalDistribution
{
    double normalization = 0.0;
    std::array<double, kSlopeSteps> sinBeta{}, cosBeta{}, weight{};
};

CylindricalDistribution BuildCylinder(double sigma)
{
    CylindricalDistribution result;
    double integral = 0.0;
    // §6.4 的 sigma_y=∞ 只约束 hx/hz；未倾斜时令 hy=s、
    // hx=sqrt(1-s²)sin(beta)、hz=sqrt(1-s²)cos(beta)，dω=ds dβ。
    // tan(beta)=sigma*t 将窄分布积分变成带雅可比的平滑高斯积分。
    for (int i = 0; i < kSlopeSteps; ++i)
    {
        const double t = -10.0 + 20.0 * (i + 0.5) / kSlopeSteps;
        const double slope = sigma * t;
        result.cosBeta[i] = 1.0 / std::sqrt(1.0 + slope * slope);
        result.sinBeta[i] = slope * result.cosBeta[i];
        result.weight[i] = std::exp(-t * t) * sigma /
                           (1.0 + slope * slope) * 20.0 / kSlopeSteps;
        integral += result.weight[i];
    }
    result.normalization = 1.0 / (2.0 * integral);
    for (double& weight : result.weight) weight /= integral;
    return result;
}

Vec3 RotateToSlantedLocal(Vec3 direction, double angle)
{
    const double cosine = std::cos(angle);
    const double sine = std::sin(angle);
    // global = Ry(angle) * local；把全局方向变回未倾斜圆柱的局部坐标。
    return {cosine * direction.x - sine * direction.z,
            direction.y,
            sine * direction.x + cosine * direction.z};
}

double ProjectedArea(const CylindricalDistribution& distribution, Vec3 local)
{
    double sum = 0.0;
    for (int i = 0; i < kSlopeSteps; ++i)
    {
        const double a = local.x * distribution.sinBeta[i] +
                         local.z * distribution.cosBeta[i];
        const double b = std::abs(local.y);
        // 对横向参数 s 的 max(a*sqrt(1-s²)+b*s, 0) 做解析积分。
        // 该形式允许倾斜分布的平均法线偏离宏观几何法线。
        sum += distribution.weight[i] *
               (b + a * (kPi / 2.0 + std::atan2(a, b))) / 4.0;
    }
    return sum;
}

double Lookup(const std::array<double,
               kSlantedVelvetMuSize * kSlantedVelvetPhiSize>& table,
              Vec3 direction)
{
    const double mu = std::clamp(direction.z, 0.0, 1.0) *
                      (kSlantedVelvetMuSize - 1);
    // Ry 倾斜仍关于 y 翻转对称，但 x 必须带符号；端点准确落在 ±U，
    // 避免把方向最弱一侧跨过方位接缝插值成较强反射。
    const double phi = std::atan2(std::abs(direction.y), direction.x) /
                       kPi * (kSlantedVelvetPhiSize - 1);
    const int i = static_cast<int>(mu);
    const int i1 = std::min(i + 1, kSlantedVelvetMuSize - 1);
    const int j = static_cast<int>(phi);
    const int j1 = std::min(j + 1, kSlantedVelvetPhiSize - 1);
    const double tx = mu - i;
    const double ty = phi - std::floor(phi);
    const double a = table[j * kSlantedVelvetMuSize + i] * (1.0 - tx) +
                     table[j * kSlantedVelvetMuSize + i1] * tx;
    const double b = table[j1 * kSlantedVelvetMuSize + i] * (1.0 - tx) +
                     table[j1 * kSlantedVelvetMuSize + i1] * tx;
    return a * (1.0 - ty) + b * ty;
}

double Fresnel(double f0, double voH)
{
    return f0 + (1.0 - f0) * std::pow(1.0 - voH, 5.0);
}

double CorrelatedVisibility(double normalMean, double gV, double gL,
                            Vec3 v, Vec3 l)
{
    const double pV = v.z * normalMean / gV;
    const double pL = l.z * normalMean / gL;
    const Vec3 tv{v.x, v.y, 0.0};
    const Vec3 tl{l.x, l.y, 0.0};
    double correlation = 0.0;
    if (length(tv) > 1.0e-10 && length(tl) > 1.0e-10)
    {
        const double angle = std::acos(std::clamp(
            dot(tv, tl) / (length(tv) * length(tl)), -1.0, 1.0));
        constexpr double width = 15.0 * kPi / 180.0;
        correlation = std::exp(-0.5 * angle * angle / (width * width));
    }
    return (1.0 - correlation) +
           correlation / std::max(pV, pL);
}

double SpecularWithProjectedAreas(const SlantedVelvetMaterial& material,
                                  Vec3 v, Vec3 l, double gV, double gL)
{
    const Vec3 h = normalize(v + l);
    const double density = slantedVelvetDensity(material, h);
    if (density <= 0.0) return 0.0;
    return density * material.normalMean *
           Fresnel(material.f0, dot(v, h)) *
           CorrelatedVisibility(material.normalMean, gV, gL, v, l) /
           (4.0 * gV * gL);
}
} // namespace

double slantedVelvetDensity(const SlantedVelvetMaterial& material, Vec3 h)
{
    // slant 是纤维束轴相对 n 的角度；侧面微法线与束轴垂直，
    // 平均微法线从 n 向 -U 倾斜 90°-slant，而不是直接旋转 slant。
    const double angle = (material.slantDegrees - 90.0) * kPi / 180.0;
    const Vec3 local = RotateToSlantedLocal(h, angle);
    if (local.z <= 0.0) return 0.0;
    const double slope = local.x / local.z;
    return material.normalization *
           std::exp(-slope * slope / (material.sigma * material.sigma));
}

double slantedVelvetProjectedArea(const SlantedVelvetMaterial& material,
                                  Vec3 direction)
{
    return Lookup(material.g, direction);
}

double slantedVelvetSpecular(const SlantedVelvetMaterial& material,
                             Vec3 v, Vec3 l)
{
    if (v.z <= 0.0 || l.z <= 0.0) return 0.0;
    return SpecularWithProjectedAreas(material, v, l,
                                     Lookup(material.g, v), Lookup(material.g, l));
}

Vec3 slantedVelvetBrdf(const SlantedVelvetMaterial& material, Vec3 v, Vec3 l)
{
    if (v.z <= 0.0 || l.z <= 0.0) return {};
    const double diffuse = material.diffuseScale *
        (1.0 - Lookup(material.reflectance, v)) *
        (1.0 - Lookup(material.reflectance, l));
    const double specular = slantedVelvetSpecular(material, v, l);
    return material.diffuse * diffuse + Vec3{specular, specular, specular};
}

Vec3 evaluateSlantedVelvet(const SlantedVelvetMaterial& material,
                           const Hit& hit, Vec3 v, Vec3 l)
{
    const Vec3 u = normalize(hit.dpdu - hit.normal * dot(hit.normal, hit.dpdu));
    const Vec3 crossU = cross(hit.normal, u);
    const double rotation = hit.instance->fiberRotationDegrees * kPi / 180.0;
    const Vec3 x = u * std::cos(rotation) + crossU * std::sin(rotation);
    const Vec3 y = cross(hit.normal, x);
    return slantedVelvetBrdf(material, {dot(v, x), dot(v, y), dot(v, hit.normal)},
                             {dot(l, x), dot(l, y), dot(l, hit.normal)});
}

SlantedVelvetMaterial makeSlantedVelvetMaterial(
    double sigma, double slantDegrees, double f0, Vec3 diffuse)
{
    if (!std::isfinite(sigma) || !std::isfinite(slantDegrees) ||
        !std::isfinite(f0) || !std::isfinite(diffuse.x) ||
        !std::isfinite(diffuse.y) || !std::isfinite(diffuse.z) ||
        sigma < 0.35 || sigma > 1.0 || slantDegrees < 30.0 || slantDegrees > 80.0 ||
        f0 < 0.0 || f0 > 1.0 || diffuse.x < 0.0 || diffuse.y < 0.0 ||
        diffuse.z < 0.0 || diffuse.x > 1.0 || diffuse.y > 1.0 ||
        diffuse.z > 1.0)
        throw std::invalid_argument("invalid Ashikhmin slanted Velvet parameters");

    SlantedVelvetMaterial material;
    material.sigma = sigma;
    material.slantDegrees = slantDegrees;
    material.f0 = f0;
    material.diffuse = diffuse;

    const auto distribution = BuildCylinder(sigma);
    material.normalization = distribution.normalization;
    const double angle = (slantDegrees - 90.0) * kPi / 180.0;
    double signedMean = 0.0;
    for (int i = 0; i < kSlopeSteps; ++i)
        signedMean += distribution.weight[i] * distribution.cosBeta[i] *
                      (kPi / 4.0) * std::cos(angle);
    // §6.4 把 <h·n> 加倍近似束顶贡献；这是带符号的整分布均值，
    // 不是 max(h·n,0) 的 g(n)。保留下半球法线，不把非 height-field 分布修成物理模型。
    material.normalMean = 2.0 * signedMean;

    for (int j = 0; j < kSlantedVelvetPhiSize; ++j)
        for (int i = 0; i < kSlantedVelvetMuSize; ++i)
        {
            const double mu = static_cast<double>(i) /
                              (kSlantedVelvetMuSize - 1);
            const double phi = kPi * j / (kSlantedVelvetPhiSize - 1);
            const Vec3 direction{std::sqrt(std::max(0.0, 1.0 - mu * mu)) *
                                     std::cos(phi),
                                 std::sqrt(std::max(0.0, 1.0 - mu * mu)) *
                                     std::sin(phi), mu};
            material.g[j * kSlantedVelvetMuSize + i] =
                ProjectedArea(distribution,
                    RotateToSlantedLocal(direction, angle));
        }

    constexpr int lightSteps = 64;
    constexpr int azimuthSteps = 256;
    std::vector<Vec3> directions;
    std::vector<double> projected;
    directions.reserve(lightSteps * azimuthSteps);
    projected.reserve(lightSteps * azimuthSteps);
    for (int i = 0; i < lightSteps; ++i)
        for (int j = 0; j < azimuthSteps; ++j)
        {
            const double mu = (i + 0.5) / lightSteps;
            const double phi = 2.0 * kPi * (j + 0.5) / azimuthSteps;
            const Vec3 l{std::sqrt(1.0 - mu * mu) * std::cos(phi),
                         std::sqrt(1.0 - mu * mu) * std::sin(phi), mu};
            directions.push_back(l);
            projected.push_back(Lookup(material.g, l));
        }
    for (int j = 0; j < kSlantedVelvetPhiSize; ++j)
        for (int i = 0; i < kSlantedVelvetMuSize; ++i)
        {
            const double noV = std::max(1.0e-6,
                static_cast<double>(i) / (kSlantedVelvetMuSize - 1));
            const double phi = kPi * j / (kSlantedVelvetPhiSize - 1);
            const Vec3 v{std::sqrt(std::max(0.0, 1.0 - noV * noV)) *
                             std::cos(phi),
                         std::sqrt(std::max(0.0, 1.0 - noV * noV)) *
                             std::sin(phi), noV};
            double sum = 0.0;
            const double gV = Lookup(material.g, v);
            for (std::size_t index = 0; index < directions.size(); ++index)
            {
                const Vec3 l = directions[index];
                sum += SpecularWithProjectedAreas(material, v, l,
                                                  gV, projected[index]) * l.z;
            }
            material.reflectance[j * kSlantedVelvetMuSize + i] =
                sum * 2.0 * kPi / (lightSteps * azimuthSteps);
        }

    double remaining = 0.0;
    for (int i = 0; i < lightSteps; ++i)
        for (int j = 0; j < azimuthSteps; ++j)
        {
            const double mu = (i + 0.5) / lightSteps;
            const double phi = 2.0 * kPi * (j + 0.5) / azimuthSteps;
            const Vec3 direction{std::sqrt(1.0 - mu * mu) * std::cos(phi),
                                 std::sqrt(1.0 - mu * mu) * std::sin(phi), mu};
            remaining += (1.0 - Lookup(material.reflectance, direction)) * mu;
        }
    remaining *= 2.0 * kPi / (lightSteps * azimuthSteps);
    // 不夹 R 到 1：论文明确说明 f0=1 的非物理分布可能超能量。
    material.diffuseScale = 1.0 / remaining;
    return material;
}
} // namespace VL::Offline
