#include "core/renderer.h"
#include <algorithm>
#include <cmath>

namespace VL::Offline
{

// A 臂按论文 BRDF 和 absCos 测度求值；方向反照率表使用未柔化的几何项。
double logLambda(double noX, double r)
{
    const double t = (1.0 - r) * (1.0 - r);
    const double a = 21.5473 * (1.0 - t) + 25.3245 * t;
    const double b = 3.82987 * (1.0 - t) + 3.32435 * t;
    const double c = 0.19823 * (1.0 - t) + 0.16801 * t;
    const double d = -1.97760 * (1.0 - t) - 1.27393 * t;
    const double e = -4.32054 * (1.0 - t) - 4.85967 * t;
    const auto curve = [=](double x) {
        return a / (1.0 + b * std::pow(x, c)) + d * x + e;
    };
    const double x = std::clamp(noX, 1e-4, 1.0);
    return x < 0.5 ? curve(x) : 2.0 * curve(0.5) - curve(1.0 - x);
}

double charlieD(double noH, double r)
{
    if (noH <= 0.0 || noH >= 1.0) return 0.0;
    return (2.0 + 1.0 / r) *
           std::pow(std::max(0.0, 1.0 - noH * noH), 0.5 / r) /
           (2.0 * kPi);
}

double sheenBrdf(Vec3 n, Vec3 v, Vec3 l, double r, bool soften)
{
    const double noV = dot(n, v);
    const double noL = dot(n, l);
    if (noV <= 0.0 || noL <= 0.0) return 0.0;
    const Vec3 h = normalize(v + l);
    const double d = charlieD(dot(n, h), r);
    const double lightSoftening = soften ? 1.0 + 2.0 * std::pow(1.0 - noL, 8.0) : 1.0;
    const double g = 1.0 / (1.0 + std::exp(logLambda(noV, r)) +
                           std::exp(lightSoftening * logLambda(noL, r)));
    return d * g / (4.0 * noV * noL);
}

double directionalAlbedo(double noV, double r)
{
    // 反照率只积分未柔化的 Eq. (4)；柔化仅作用于实时光照侧，不能污染分层保能项。
    constexpr int nMu = 64;
    constexpr int nPhi = 48;
    const Vec3 n{0.0, 0.0, 1.0};
    const Vec3 v{std::sqrt(std::max(0.0, 1.0 - noV * noV)), 0.0, noV};
    double sum = 0.0;
    for (int m = 0; m < nMu; ++m) {
        const double mu = (m + 0.5) / nMu;
        const double sinTheta = std::sqrt(1.0 - mu * mu);
        for (int p = 0; p < nPhi; ++p) {
            const double phi = 2.0 * kPi * (p + 0.5) / nPhi;
            const Vec3 l{sinTheta * std::cos(phi), sinTheta * std::sin(phi), mu};
            sum += sheenBrdf(n, v, l, r, false) * mu;
        }
    }
    return sum * 2.0 * kPi / (nMu * nPhi);
}

} // namespace VL::Offline
