#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <gtest/gtest.h>

#include "core/renderer.h"
#include "tableIIFixture.h"

namespace
{
using namespace VL::Offline;

void ExpectClose(Vec3 actual, Vec3 expected)
{
    EXPECT_LT(length(actual - expected), 1e-10 * (1.0 + length(expected)));
}

Hit PlanarHit()
{
    Hit hit;
    hit.normal = hit.geometricNormal = {0, 1, 0};
    hit.dpdu = {1, 0, 0};
    hit.dpdv = {0, 0, 1};
    return hit;
}

// 标量参考直接从投影长度计算 M/P，不调用生产求值器的角度/Fresnel helper。
Vec3 ReferenceKernel(const FabricMaterial& material, Vec3 wi, Vec3 wo)
{
    Vec3 numerator{};
    double q = (1 - material.yarns[0].coverage -
                material.yarns[1].coverage) * std::max(0.0, wo.y);
    const auto bell = [](double degrees, double theta) {
        const double sigma = degrees * kPi / 180.0;
        return std::exp(-theta * theta / (2 * sigma * sigma)) /
               (sigma * std::sqrt(2 * kPi));
    };
    const auto combine = [](double a, double b, double angle) {
        const double rho = std::exp(-angle * angle / (2 * std::pow(kPi / 9, 2)));
        return a * b * (1 - rho) + std::min(a, b) * rho;
    };
    const auto fresnel = [](double eta, double ci) {
        const double ct = std::sqrt(1 - (1 - ci * ci) / (eta * eta));
        return (std::pow((ci - eta * ct) / (ci + eta * ct), 2) +
                std::pow((eta * ci - ct) / (eta * ci + ct), 2)) / 2;
    };
    for (int family = 0; family < 2; ++family)
    {
        const auto& yarn = material.yarns[family];
        for (int sample = 0; sample < yarn.sampleCount; ++sample)
        {
            const double angle = yarn.samples[sample] * kPi / 180.0;
            const double c = std::cos(angle), s = std::sin(angle);
            const auto local = [&](Vec3 w) {
                const double axis = family == 0 ? w.x : w.z;
                return Vec3{axis * c + w.y * s, w.y * c - axis * s,
                            family == 0 ? w.z : -w.x};
            };
            const Vec3 i = local(wi), o = local(wo);
            const double cti = std::hypot(i.y, i.z), cto = std::hypot(o.y, o.z);
            const double ip = std::hypot(i.x, i.y), op = std::hypot(o.x, o.y);
            if (cti < 1e-8 || cto < 1e-8 || ip < 1e-8 || op < 1e-8)
                throw std::runtime_error("reference probe on angular singularity");
            const double cosPhi = std::clamp((i.y * o.y + i.z * o.z) / (cti * cto), -1.0, 1.0);
            const double cosPsi = std::clamp((i.x * o.x + i.y * o.y) / (ip * op), -1.0, 1.0);
            const double p = combine(std::max(0.0, i.y / ip), std::max(0.0, o.y / op),
                                     std::acos(cosPsi));
            const double m = combine(std::max(0.0, i.y / cti), std::max(0.0, o.y / cto),
                                     std::acos(cosPhi));
            q += p * yarn.coverage / yarn.sampleCount;
            const double ti = std::atan2(i.x, cti), to = std::atan2(o.x, cto);
            const double halfCos = std::sqrt(std::max(0.0, (1 + cosPhi) / 2));
            const double cd = std::cos((ti - to) / 2);
            const double fr = fresnel(material.eta, cd * halfCos);
            const double sr = fr * halfCos * bell(yarn.gammaS, (ti + to) / 2);
            const double vr = std::pow(1 - fr, 2) *
                ((1 - yarn.kd) * bell(yarn.gammaV, (ti + to) / 2) + yarn.kd) / (cti + cto);
            const Vec3 scatter = (Vec3{sr, sr, sr} + yarn.dye * vr) / (cd * cd);
            numerator += scatter * (m * p * cti * yarn.coverage / yarn.sampleCount);
        }
    }
    return q > 1e-8 ? numerator / q : Vec3{};
}

TEST(OfflineReference, DyeAndCoverageClosedFormAtNormalIncidence)
{
    const Hit hit = PlanarHit();
    const Vec3 n{0, 1, 0};
    const double f0 = std::pow((1.5 - 1) / (1.5 + 1), 2);
    for (double kd : {0.0, 0.3, 1.0})
    {
        const FabricMaterial flat{"analytic", "analytic", 1.5,
            {VL::Offline::Test::TableIIYarn(0.3, {0.02, 0.3, 0.7}, kd, 6, 12, {0, 0}, {1}),
             VL::Offline::Test::TableIIYarn(0.3, {0.02, 0.3, 0.7}, kd, 6, 12, {0, 0}, {1})}, ""};
        // 正入射 Q=1；期刊 Eq. 3 的染色作用于整个体散射项。
        const double surface = f0 / ((6 * kPi / 180) * std::sqrt(2 * kPi));
        const double volume = std::pow(1 - f0, 2) / 2 *
            ((1 - kd) / ((12 * kPi / 180) * std::sqrt(2 * kPi)) + kd);
        ExpectClose(microcylinderResponse(hit, n, n, flat),
                    (Vec3{surface, surface, surface} + Vec3{0.02, 0.3, 0.7} * volume) * 0.6);
        FabricMaterial black = flat;
        black.yarns[0].dye = black.yarns[1].dye = {};
        ExpectClose(microcylinderResponse(hit, n, n, black),
                    Vec3{surface, surface, surface} * 0.6);
    }
}

TEST(OfflineReference, IndependentJournalEquationsAcrossTableII)
{
    const Hit hit = PlanarHit();
    for (const auto& material : VL::Offline::Test::TableIIFabrics())
    {
        for (Vec3 wo : {Vec3{0.25, 0.94, 0.23}, Vec3{-0.75, 0.25, 0.55},
                        Vec3{0.03, 0.13, 0.99}})
        {
            wo = normalize(wo);
            for (Vec3 wi : {Vec3{0.43, 0.87, -0.22}, Vec3{-0.9, 0.3, 0.23},
                            Vec3{0.31, -0.76, 0.57}, Vec3{0.04, 0.98, 0.18}})
            {
                wi = normalize(wi);
                SCOPED_TRACE(material.caseName);
                ExpectClose(microcylinderResponse(hit, wo, wi, material),
                            ReferenceKernel(material, wi, wo));
            }
        }
    }
}

TEST(OfflineReference, UnequalFamilySampleCountsKeepCoverageWeightedQ)
{
    FabricMaterial material = VL::Offline::Test::TableIIFabrics()[2];
    resampleYarn(material.yarns[0], 8);
    resampleYarn(material.yarns[1], 64);
    const Vec3 wo = normalize(Vec3{0.2, 0.8, 0.3});
    const Vec3 wi = normalize(Vec3{-0.4, 0.7, 0.2});
    ExpectClose(microcylinderResponse(PlanarHit(), wo, wi, material),
                ReferenceKernel(material, wi, wo));
}

TEST(OfflineGroom, InterpolationWrapAndBarycentricUv)
{
    const Hit hit = PlanarHit();
    const GroomMap identity{1, 1, {{1, 0}}, "test"};
    const GroomMap turn{1, 1, {{0, 1}}, "test"};
    ExpectClose(orientYarns(hit, &identity, 0).dpdu, hit.dpdu);
    const Hit rotated = orientYarns(hit, &turn, 0);
    ExpectClose(rotated.dpdu, hit.dpdv);
    ExpectClose(rotated.dpdv, -hit.dpdu);
    ExpectClose(orientYarns(hit, nullptr, 90).dpdu, rotated.dpdu);
    ExpectClose(orientYarns(hit, nullptr, 90).dpdv, rotated.dpdv);

    const GroomMap seam{2, 1, {{-0.99995, 0.01}, {-0.99995, -0.01}}, "test"};
    const Vec2 sample = groomDirection(seam, {0.5, 0.5});
    EXPECT_LT(sample.x, -0.999);
    EXPECT_LT(std::abs(sample.y), 1e-10);
    const Vec2 repeat = groomDirection(seam, {-1.5, 2.5});
    EXPECT_LT(std::abs(sample.x - repeat.x), 1e-10);
    const GroomMap antipodal{2, 1, {{1, 0}, {-1, 0}}, "test"};
    const Vec2 boundary = groomDirection(antipodal, {0.5, 0.5});
    EXPECT_DOUBLE_EQ(std::hypot(boundary.x, boundary.y), 1.0);

    Triangle tri{};
    tri.vertices = {Vec3{0, 0, 0}, Vec3{0, 0, 1}, Vec3{1, 0, 0}};
    tri.normals = {hit.normal, hit.normal, hit.normal};
    tri.uv = {Vec2{0, 0}, Vec2{0, 1}, Vec2{1, 0}};
    tri.dpdu = hit.dpdu;
    tri.dpdv = hit.dpdv;
    Hit interpolated;
    intersectTriangle(tri, {{0.2, 1, 0.3}, {0, -1, 0}}, 0, interpolated);
    EXPECT_NEAR(interpolated.uv.x, 0.2, 1e-12);
    EXPECT_NEAR(interpolated.uv.y, 0.3, 1e-12);
}

TEST(OfflineGroom, KernelAndPdfRotateCovariantly)
{
    const Hit hit = PlanarHit();
    const GroomMap turn{1, 1, {{0, 1}}, "test"};
    const Hit rotated = orientYarns(hit, &turn, 0);
    const auto rotateWorld = [](Vec3 v) { return Vec3{-v.z, v.y, v.x}; };
    const Vec3 view = normalize(Vec3{0.2, 0.95, 0.23});
    const Vec3 light = normalize(Vec3{-0.4, 0.8, 0.24});
    for (const auto& material : VL::Offline::Test::TableIIFabrics())
    {
        SCOPED_TRACE(material.caseName);
        ExpectClose(microcylinderResponse(rotated, rotateWorld(view),
                                          rotateWorld(light), material),
                    microcylinderResponse(hit, view, light, material));
        EXPECT_NEAR(microcylinderDirectionPdf(rotated, rotateWorld(view),
                                              rotateWorld(light), material),
                    microcylinderDirectionPdf(hit, view, light, material), 1e-10);
    }
}
} // namespace
