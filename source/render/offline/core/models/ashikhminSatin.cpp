#include "core/renderer.h"

#include <cmath>
#include <stdexcept>

namespace VL::Offline
{
namespace
{
constexpr int kSlopeSteps = 192;
struct CylindricalDistribution
{
    double normalization = 0.0;
    std::array<double, kSlopeSteps> sinBeta{}, cosBeta{}, weight{};
};

CylindricalDistribution BuildCylinder(double sigma)
{
    CylindricalDistribution result;
    double integral = 0.0;
    // 式(34) 的 sigma_y=∞：p 只依赖 h.x/h.z。令 h.y=s、
    // h.x=sqrt(1-s²)sinβ，h.z=sqrt(1-s²)cosβ，则 dω=ds dβ。
    // 用 tanβ=sigma*t 积分，窄纤维分布不会被均匀方位网格漏掉。
    for (int i = 0; i < kSlopeSteps; ++i)
    {
        const double t = -8.0 + 16.0 * (i + 0.5) / kSlopeSteps;
        const double slope = sigma * t;
        result.cosBeta[i] = 1.0 / std::sqrt(1.0 + slope * slope);
        result.sinBeta[i] = slope * result.cosBeta[i];
        result.weight[i] = std::exp(-t * t) * sigma /
                           (1.0 + slope * slope) * 16.0 / kSlopeSteps;
        integral += result.weight[i];
    }
    result.normalization = 1.0 / (2.0 * integral);
    for (double& weight : result.weight) weight /= integral;
    return result;
}

double ProjectedArea(const CylindricalDistribution& distribution, Vec3 k)
{
    double sum = 0.0;
    for (int i = 0; i < kSlopeSteps; ++i)
    {
        const double A = k.x * distribution.sinBeta[i] +
                         k.z * distribution.cosBeta[i];
        const double B = std::abs(k.y);
        // 式(18) 对 s 的正投影可解析积分：
        // (1/2)∫max(A sqrt(1-s²)+B s,0)ds
        // = [B+A(π/2+atan2(A,B))]/4，覆盖 A<0 的自遮蔽区。
        sum += distribution.weight[i] *
               (B + A * (kPi / 2.0 + std::atan2(A, B))) / 4.0;
    }
    return sum;
}

double Lookup(const std::array<double, kSatinMuSize * kSatinPhiSize>& table,
              Vec3 direction)
{
    // p 关于 x/y 翻转对称；g/R 仅冻结上半球第一象限。
    const double mu = std::clamp(direction.z, 0.0, 1.0) * (kSatinMuSize - 1);
    const double phi = std::atan2(std::abs(direction.y), std::abs(direction.x)) *
                       (kSatinPhiSize - 1) / (kPi / 2.0);
    const int i = static_cast<int>(mu), j = static_cast<int>(phi);
    const int nextI = std::min(i + 1, kSatinMuSize - 1);
    const int nextJ = std::min(j + 1, kSatinPhiSize - 1);
    const double a = table[j * kSatinMuSize + i] * (1.0 - (mu - i)) +
                     table[j * kSatinMuSize + nextI] * (mu - i);
    const double b = table[nextJ * kSatinMuSize + i] * (1.0 - (mu - i)) +
                     table[nextJ * kSatinMuSize + nextI] * (mu - i);
    return a * (1.0 - (phi - j)) + b * (phi - j);
}

Vec3 Direction(double mu, double phi)
{
    const double sinTheta = std::sqrt(1.0 - mu * mu);
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), mu};
}
} // namespace

double satinDensity(const SatinMaterial& material, Vec3 h)
{
    if (h.z <= 0.0) return 0.0;
    const double slope = h.x / h.z;
    const double flat = material.normalizationFlat *
        std::exp(-slope * slope / (material.sigmaFlat * material.sigmaFlat));
    const double ends = material.normalizationEnds *
        std::exp(-slope * slope / (material.sigmaEnds * material.sigmaEnds));
    // §6.3 的 70/30 是面积比例：先归一化两个 p，再混合；不是相加两个 BRDF。
    return material.flatWeight * flat + (1.0 - material.flatWeight) * ends;
}

double satinProjectedArea(const SatinMaterial& material, Vec3 direction)
{
    return Lookup(material.g, direction);
}

double satinSpecular(const SatinMaterial& material, Vec3 v, Vec3 l)
{
    if (v.z <= 0.0 || l.z <= 0.0) return 0.0;
    const Vec3 h = normalize(v + l);
    const double fresnel = material.f0 +
        (1.0 - material.f0) * std::pow(1.0 - dot(v, h), 5.0);
    // Satin 使用式(13)/(26) 无关遮蔽；式(14) 的回射相关增强只用于 Velvet。
    return satinDensity(material, h) * material.normalMean * fresnel /
           (4.0 * satinProjectedArea(material, v) * satinProjectedArea(material, l));
}

Vec3 satinBrdf(const SatinMaterial& material, Vec3 v, Vec3 l)
{
    if (v.z <= 0.0 || l.z <= 0.0) return {};
    const double diffuse = material.diffuseScale *
        (1.0 - Lookup(material.reflectance, v)) *
        (1.0 - Lookup(material.reflectance, l));
    const double specular = satinSpecular(material, v, l);
    return material.diffuse * diffuse + Vec3{specular, specular, specular};
}

Vec3 evaluateSatin(const SatinMaterial& material, const Hit& hit, Vec3 v, Vec3 l)
{
    const Vec3 u = normalize(hit.dpdu - hit.normal * dot(hit.normal, hit.dpdu));
    const Vec3 crossU = cross(hit.normal, u);
    const double angle = hit.instance->fiberRotationDegrees * kPi / 180.0;
    const Vec3 x = u * std::cos(angle) + crossU * std::sin(angle);
    const Vec3 y = cross(hit.normal, x);
    // 双面与镜像 UV 不翻转纤维轴的含义；分布对 x/y 的符号对称。
    return satinBrdf(material, {dot(v, x), dot(v, y), dot(v, hit.normal)},
                              {dot(l, x), dot(l, y), dot(l, hit.normal)});
}

SatinMaterial makeSatinMaterial(double sigmaFlat, double sigmaEnds,
                               double flatWeight, double f0, Vec3 diffuse)
{
    if (!std::isfinite(sigmaFlat) || !std::isfinite(sigmaEnds) ||
        !std::isfinite(flatWeight) || !std::isfinite(f0) ||
        !std::isfinite(diffuse.x) || !std::isfinite(diffuse.y) ||
        !std::isfinite(diffuse.z) ||
        sigmaFlat < 0.05 || sigmaFlat > 1.0 ||
        sigmaEnds < 0.05 || sigmaEnds > 1.0 ||
        flatWeight < 0.0 || flatWeight > 1.0 || f0 < 0.0 || f0 > 1.0 ||
        diffuse.x < 0.0 || diffuse.y < 0.0 || diffuse.z < 0.0 ||
        diffuse.x > 1.0 || diffuse.y > 1.0 || diffuse.z > 1.0)
        throw std::invalid_argument("invalid Ashikhmin Satin parameters");
    SatinMaterial material;
    material.sigmaFlat = sigmaFlat;
    material.sigmaEnds = sigmaEnds;
    material.flatWeight = flatWeight;
    material.f0 = f0;
    material.diffuse = diffuse;
    const auto flat = BuildCylinder(sigmaFlat);
    const auto ends = BuildCylinder(sigmaEnds);
    material.normalizationFlat = flat.normalization;
    material.normalizationEnds = ends.normalization;
    material.normalMean = flatWeight * ProjectedArea(flat, {0, 0, 1}) +
                         (1.0 - flatWeight) * ProjectedArea(ends, {0, 0, 1});
    for (int j = 0; j < kSatinPhiSize; ++j)
        for (int i = 0; i < kSatinMuSize; ++i)
        {
            const Vec3 k = Direction(static_cast<double>(i) / (kSatinMuSize - 1),
                                    (kPi / 2.0) * j / (kSatinPhiSize - 1));
            material.g[j * kSatinMuSize + i] =
                flatWeight * ProjectedArea(flat, k) +
                (1.0 - flatWeight) * ProjectedArea(ends, k);
        }
    // 式(29)：直接积分统一混合 BRDF；入射网格覆盖 sigma_x≥0.05，
    // 更窄分布需先提高求积精度，不能只在 MI 放宽参数范围。
    constexpr int muSteps = 64, phiSteps = 256;
    std::array<Vec3, muSteps * phiSteps> directions{};
    std::array<double, muSteps * phiSteps> projected{};
    for (int i = 0; i < muSteps; ++i)
        for (int j = 0; j < phiSteps; ++j)
        {
            const int index = i * phiSteps + j;
            directions[index] = Direction((i + 0.5) / muSteps,
                                         2.0 * kPi * (j + 0.5) / phiSteps);
            projected[index] = satinProjectedArea(material, directions[index]);
        }
    for (int j = 0; j < kSatinPhiSize; ++j)
        for (int i = 0; i < kSatinMuSize; ++i)
        {
            const Vec3 v = Direction(std::max(1e-6,
                static_cast<double>(i) / (kSatinMuSize - 1)),
                (kPi / 2.0) * j / (kSatinPhiSize - 1));
            const double gV = satinProjectedArea(material, v);
            double sum = 0.0;
            for (std::size_t index = 0; index < directions.size(); ++index)
            {
                const Vec3 l = directions[index];
                const Vec3 h = normalize(v + l);
                const double fresnel = f0 +
                    (1.0 - f0) * std::pow(1.0 - dot(v, h), 5.0);
                sum += satinDensity(material, h) * material.normalMean * fresnel *
                       l.z / (4.0 * gV * projected[index]);
            }
            material.reflectance[j * kSatinMuSize + i] =
                sum * 2.0 * kPi / (muSteps * phiSteps);
        }
    double remaining = 0.0;
    for (int j = 0; j < kSatinPhiSize - 1; ++j)
        for (int i = 0; i < kSatinMuSize - 1; ++i)
        {
            const double mu = (i + 0.5) / (kSatinMuSize - 1);
            const Vec3 k = Direction(mu, (kPi / 2.0) * (j + 0.5) /
                                        (kSatinPhiSize - 1));
            remaining += (1.0 - Lookup(material.reflectance, k)) * mu;
        }
    // 式(30) 对整个剩余半球归一化，不能给每个纤维族各加一次 Lambert。
    material.diffuseScale = 1.0 / (2.0 * kPi * remaining /
        ((kSatinMuSize - 1) * (kSatinPhiSize - 1)));
    return material;
}
} // namespace VL::Offline
