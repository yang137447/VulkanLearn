#include "core/renderer.h"
#include <algorithm>
#include <cmath>

namespace VL::Offline
{

// B 臂保留论文辐射核的方向测度；路径吞吐量只除立体角 PDF。
double dielectricFresnel(double eta, double cosine)
{
    const double ci = std::clamp(cosine, 0.0, 1.0);
    const double sin2t = (1.0 - ci * ci) / (eta * eta);
    if (sin2t >= 1.0) return 1.0;
    const double ct = std::sqrt(1.0 - sin2t);
    const double rs = (ci - eta * ct) / (ci + eta * ct);
    const double rp = (eta * ci - ct) / (eta * ci + ct);
    return 0.5 * (rs * rs + rp * rp);
}

double gaussian(double widthDegrees, double angle)
{
    const double width = widthDegrees * kPi / 180.0;
    return std::exp(-0.5 * angle * angle / (width * width)) /
           (width * std::sqrt(2.0 * kPi));
}

double correlated(double a, double b, double delta)
{
    const double sigma = 20.0 * kPi / 180.0;
    const double u = std::exp(-0.5 * delta * delta / (sigma * sigma));
    return (1.0 - u) * a * b + u * std::min(a, b);
}

double angleDifference(double a, double b)
{
    double delta = std::abs(a - b);
    return std::min(delta, 2.0 * kPi - delta);
}

Vec2 groomDirection(const GroomMap& map, Vec2 uv)
{
    const double x = (uv.x - std::floor(uv.x)) * map.width - 0.5;
    const double y = (uv.y - std::floor(uv.y)) * map.height - 0.5;
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    const double fx = x - ix, fy = y - iy;
    const auto texel = [&](int u, int v) {
        u = (u % map.width + map.width) % map.width;
        v = (v % map.height + map.height) % map.height;
        return map.directions[static_cast<std::size_t>(v) * map.width + u];
    };
    const Vec2 a = texel(ix, iy), b = texel(ix + 1, iy);
    const Vec2 c = texel(ix, iy + 1), d = texel(ix + 1, iy + 1);
    Vec2 value{
        (a.x * (1.0 - fx) + b.x * fx) * (1.0 - fy) +
            (c.x * (1.0 - fx) + d.x * fx) * fy,
        (a.y * (1.0 - fx) + b.y * fx) * (1.0 - fy) +
            (c.y * (1.0 - fx) + d.y * fx) * fy};
    const double norm = std::hypot(value.x, value.y);
    // 对跖方向恰好抵消时取最近样本，避免凭空产生一个零向量。
    if (norm < 1e-8) return texel(ix + (fx >= 0.5), iy + (fy >= 0.5));
    return {value.x / norm, value.y / norm};
}

Hit orientYarns(Hit hit, const GroomMap* groom, double degrees)
{
    if (!groom && degrees == 0.0) return hit;
    const Vec3 u = normalize(hit.dpdu - hit.normal * dot(hit.normal, hit.dpdu));
    Vec3 v = hit.dpdv - hit.normal * dot(hit.normal, hit.dpdv);
    v = normalize(v - u * dot(u, v));
    const Vec2 direction = groom ? groomDirection(*groom, hit.uv) : Vec2{1, 0};
    const double angle = degrees * kPi / 180.0;
    const double c = direction.x * std::cos(angle) - direction.y * std::sin(angle);
    const double s = direction.y * std::cos(angle) + direction.x * std::sin(angle);
    hit.dpdu = u * c + v * s;
    hit.dpdv = v * c - u * s;
    return hit;
}

std::array<Vec3, 2 * kMaxTangentSamples> yarnTangents(
    const Hit& hit, const FabricMaterial& material)
{
    const Vec3 u = normalize(hit.dpdu - hit.normal * dot(hit.normal, hit.dpdu));
    Vec3 v = hit.dpdv - hit.normal * dot(hit.normal, hit.dpdv);
    v = normalize(v - u * dot(u, v));
    const Vec3 axes[2] = {u, v};
    std::array<Vec3, 2 * kMaxTangentSamples> tangents{};
    for (int family = 0; family < 2; ++family) {
        for (int sample = 0; sample < material.yarns[family].sampleCount; ++sample) {
            const Vec2 rotation = material.yarns[family].rotated[sample];
            tangents[family * kMaxTangentSamples + sample] =
                normalize(axes[family] * rotation.x +
                          hit.normal * rotation.y);
        }
    }
    return tangents;
}

Vec3 microcylinderResponse(Hit hit, Vec3 view, Vec3 light,
                           const FabricMaterial& material)
{
    Vec3 numerator{};
    const double gap = std::max(0.0, 1.0 - material.yarns[0].coverage -
                                      material.yarns[1].coverage);
    // 正式版 Eq. (15)：按覆盖率归一化；空隙只计入投影面积，不添加反射。
    double denominator = gap * std::max(dot(hit.normal, view), 0.0);
    const auto tangents = yarnTangents(hit, material);
    for (int family = 0; family < 2; ++family) {
        const YarnMaterial& yarn = material.yarns[family];
        Vec3 familySum{};
        for (int sample = 0; sample < yarn.sampleCount; ++sample) {
            const Vec3 t = tangents[family * kMaxTangentSamples + sample];
            const Vec3 n = normalize(hit.normal - t * dot(hit.normal, t));
            const Vec3 b = cross(t, n);
            const double ti = std::clamp(dot(light, t), -1.0, 1.0);
            const double tr = std::clamp(dot(view, t), -1.0, 1.0);
            const double cosTi = std::sqrt(std::max(0.0, 1.0 - ti * ti));
            const double cosTr = std::sqrt(std::max(0.0, 1.0 - tr * tr));
            const double psiI = std::atan2(dot(light, t), dot(light, n));
            const double psiR = std::atan2(dot(view, t), dot(view, n));
            const double projectedI = std::hypot(dot(light, t), dot(light, n));
            const double projectedR = std::hypot(dot(view, t), dot(view, n));
            const double projection = correlated(
                projectedI < 1e-8 ? 0.0 : std::max(std::cos(psiI), 0.0),
                projectedR < 1e-8 ? 0.0 : std::max(std::cos(psiR), 0.0),
                angleDifference(psiI, psiR));
            denominator += yarn.coverage * projection / yarn.sampleCount;
            if (cosTi < 1e-6 || cosTr < 1e-6) continue;
            const double phiI = std::atan2(dot(light, b), dot(light, n));
            const double phiR = std::atan2(dot(view, b), dot(view, n));
            const double phiD = angleDifference(phiI, phiR);
            const double thetaI = std::asin(ti);
            const double thetaR = std::asin(tr);
            const double thetaH = 0.5 * (thetaI + thetaR);
            const double cosThetaD = std::cos(0.5 * (thetaI - thetaR));
            const double cosHalfPhi = std::cos(0.5 * phiD);
            const double fresnel = dielectricFresnel(material.eta,
                                                     cosThetaD * cosHalfPhi);
            const double surface = fresnel * cosHalfPhi *
                                   gaussian(yarn.gammaS, thetaH);
            const double transmission = (1.0 - fresnel) * (1.0 - fresnel);
            // Eq. (3)：A 染色整个体散射瓣，不能只染色 kd 常数项。
            const Vec3 volume = yarn.dye *
                                ((1.0 - yarn.kd) *
                                 gaussian(yarn.gammaV, thetaH) + yarn.kd) *
                                (transmission / std::max(cosTi + cosTr, 1e-4));
            const Vec3 fs = (Vec3{surface, surface, surface} + volume) /
                            std::max(cosThetaD * cosThetaD, 1e-4);
            const double mask = correlated(std::max(std::cos(phiI), 0.0),
                                           std::max(std::cos(phiR), 0.0), phiD);
            familySum += fs * (mask * projection * cosTi / yarn.sampleCount);
        }
        numerator += familySum * yarn.coverage;
    }
    // Eq. (5) 的未覆盖比例不附加 Lambert 填光；孔隙透射不在本模型中。
    return numerator / std::max(denominator, 1e-6);
}

double truncatedGaussianPdf(double angle, double mean, double width)
{
    const double inv = 1.0 / (std::sqrt(2.0) * width);
    const double normalization = 0.5 *
        (std::erf((0.5 * kPi - mean) * inv) -
         std::erf((-0.5 * kPi - mean) * inv));
    const double x = (angle - mean) / width;
    return std::exp(-0.5 * x * x) /
           (width * std::sqrt(2.0 * kPi) * normalization);
}

double microcylinderDirectionPdf(const Hit& hit, Vec3 view, Vec3 direction,
                                 const FabricMaterial& material)
{
    // 全球方向的混合 proposal：均匀球面和两族纱线截断高斯锥共用一个立体角 PDF。
    const auto tangents = yarnTangents(hit, material);
    double lobePdf = 0.0;
    for (int family = 0; family < 2; ++family) {
        const YarnMaterial& yarn = material.yarns[family];
        for (int sample = 0; sample < yarn.sampleCount; ++sample) {
            const Vec3 t = tangents[family * kMaxTangentSamples + sample];
            const double thetaR = std::asin(std::clamp(dot(view, t), -1.0, 1.0));
            const double axial = std::clamp(dot(direction, t), -1.0, 1.0);
            const double thetaI = std::asin(axial);
            const double cosThetaI = std::sqrt(std::max(1e-16, 1.0 - axial * axial));
            const double surfaceWidth = 2.0 * yarn.gammaS * kPi / 180.0;
            const double volumeWidth = 2.0 * yarn.gammaV * kPi / 180.0;
            lobePdf += (truncatedGaussianPdf(thetaI, -thetaR, surfaceWidth) +
                        truncatedGaussianPdf(thetaI, -thetaR, volumeWidth)) /
                       (2.0 * kPi * cosThetaI * 4.0 * yarn.sampleCount);
        }
    }
    return 0.5 / (4.0 * kPi) +
           0.5 * lobePdf;
}

DirectionSample sampleMicrocylinderDirection(const Hit& hit, Vec3 view,
                                             const FabricMaterial& material,
                                             Random& rng)
{
    Vec3 direction;
    if (rng.next() < 0.5) {
        const double phi = 2.0 * kPi * rng.next();
        const double axial = 2.0 * rng.next() - 1.0;
        const double radius = std::sqrt(std::max(0.0, 1.0 - axial * axial));
        const Vec3 u = normalize(cross(
            std::abs(hit.normal.z) < 0.9 ? Vec3{0.0, 0.0, 1.0}
                                         : Vec3{0.0, 1.0, 0.0}, hit.normal));
        const Vec3 v = cross(hit.normal, u);
        direction = u * (radius * std::cos(phi)) +
                    v * (radius * std::sin(phi)) + hit.normal * axial;
    } else {
        const double selection = rng.next() * 2.0;
        const int family = std::min(static_cast<int>(selection), 1);
        const int count = material.yarns[family].sampleCount;
        const int index = family * kMaxTangentSamples +
            std::min(static_cast<int>((selection - family) * count), count - 1);
        const bool surface = rng.next() < 0.5;
        const auto tangents = yarnTangents(hit, material);
        const Vec3 t = tangents[index];
        const Vec3 n = normalize(hit.normal - t * dot(hit.normal, t));
        const Vec3 b = cross(t, n);
        const double thetaR = std::asin(std::clamp(dot(view, t), -1.0, 1.0));
        const double width = 2.0 * (surface ? material.yarns[family].gammaS
                                            : material.yarns[family].gammaV) *
                             kPi / 180.0;
        double thetaI;
        do {
            const double u = std::max(rng.next(), 1e-12);
            const double v = rng.next();
            thetaI = -thetaR + width *
                std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
        } while (std::abs(thetaI) >= 0.5 * kPi);
        const double phi = 2.0 * kPi * rng.next();
        direction = t * std::sin(thetaI) +
                    (n * std::cos(phi) + b * std::sin(phi)) * std::cos(thetaI);
    }
    return {direction, microcylinderDirectionPdf(hit, view, direction, material)};
}

} // namespace VL::Offline
