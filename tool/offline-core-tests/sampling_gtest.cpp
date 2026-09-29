#include <cmath>

#include <gtest/gtest.h>

#include "core/renderer.h"
#include "tableIIFixture.h"

namespace
{
using namespace VL::Offline;

Hit YarnHit()
{
    Hit hit;
    hit.normal = hit.geometricNormal = {0, 1, 0};
    hit.dpdu = {1, 0, 0};
    hit.dpdv = {0, 0, 1};
    return hit;
}

TEST(OfflineSampling, TableIIDirectionPdfIntegratesOverFullSphere)
{
    const Hit hit = YarnHit();
    const Vec3 view = normalize(Vec3{0.25, 0.94, 0.23});
    constexpr int nMu = 192, nPhi = 384;
    for (const auto& material : VL::Offline::Test::TableIIFabrics())
    {
        double density = 0;
        for (int i = 0; i < nMu; ++i)
        {
            const double mu = 2.0 * (i + 0.5) / nMu - 1.0;
            const double radius = std::sqrt(1.0 - mu * mu);
            for (int j = 0; j < nPhi; ++j)
            {
                const double phi = 2 * kPi * (j + 0.5) / nPhi;
                density += microcylinderDirectionPdf(
                    hit, view, {radius * std::cos(phi), mu, radius * std::sin(phi)}, material);
            }
        }
        // B 臂继续方向覆盖整个球面，不能错误地截去下半球。
        EXPECT_NEAR(density * (4 * kPi / (nMu * nPhi)), 1.0, 0.02)
            << material.caseName;
    }
}

TEST(OfflineSampling, MixtureSamplerMatchesPdfAndSupportsLowerHemisphere)
{
    const Hit hit = YarnHit();
    const Vec3 view = normalize(Vec3{-0.75, 0.25, 0.55});
    for (const auto& material : VL::Offline::Test::TableIIFabrics())
    {
        Random rng{1781u};
        int lower = 0;
        double inversePdf = 0;
        constexpr int count = 15000;
        for (int i = 0; i < count; ++i)
        {
            const DirectionSample sample =
                sampleMicrocylinderDirection(hit, view, material, rng);
            lower += dot(hit.normal, sample.direction) < 0;
            EXPECT_TRUE(std::isfinite(sample.pdf));
            ASSERT_GT(sample.pdf, 0.5 / (4 * kPi));
            EXPECT_NEAR(sample.pdf,
                        microcylinderDirectionPdf(hit, view, sample.direction, material),
                        1e-12);
            EXPECT_NEAR(length(sample.direction), 1.0, 1e-10);
            inversePdf += 1 / sample.pdf;
        }
        EXPECT_GT(lower, count / 12) << material.caseName;
        EXPECT_NEAR(inversePdf / count / (4 * kPi), 1.0, 0.04)
            << material.caseName;
    }
}

TEST(OfflineSampling, SatinBackMonteCarloAgreesWithIndependentFullSphereIntegral)
{
    const Hit hit = YarnHit();
    const Vec3 view = normalize(Vec3{0.25, 0.94, 0.23});
    const auto& material = VL::Offline::Test::TableIIFabrics()[3];
    const auto integrate = [&](int nMu, int nPhi) {
        Vec3 total{};
        for (int i = 0; i < nMu; ++i)
        {
            const double mu = 2.0 * (i + 0.5) / nMu - 1.0;
            const double radius = std::sqrt(1.0 - mu * mu);
            for (int j = 0; j < nPhi; ++j)
            {
                const double phi = 2 * kPi * (j + 0.5) / nPhi;
                total += microcylinderResponse(
                    hit, view, {radius * std::cos(phi), mu, radius * std::sin(phi)},
                    material);
            }
        }
        return total * (4 * kPi / (nMu * nPhi));
    };
    const Vec3 coarse = integrate(96, 192);
    const Vec3 reference = integrate(192, 384);
    Random rng{1781u};
    Vec3 mean{}, squares{};
    constexpr int count = 150000;
    for (int i = 1; i <= count; ++i)
    {
        const DirectionSample sample =
            sampleMicrocylinderDirection(hit, view, material, rng);
        // B 臂的 K 已含纱线测度；估计量不能再乘宏观 NoL。
        const Vec3 contribution =
            microcylinderResponse(hit, view, sample.direction, material) / sample.pdf;
        const Vec3 delta = contribution - mean;
        mean += delta / i;
        squares += delta * (contribution - mean);
    }
    for (int channel = 0; channel < 3; ++channel)
    {
        const double expected = component(reference, channel);
        const double gridError = std::abs(component(coarse, channel) - expected);
        const double error = std::abs(component(mean, channel) - expected);
        const double se = std::sqrt(component(squares, channel) /
                                    (count * (count - 1.0)));
        ASSERT_GT(expected, 0);
        EXPECT_LT(gridError, expected * 0.01);
        EXPECT_LT(error, 5 * se + gridError + 0.002 * expected);
        EXPECT_LT(se, 0.02 * expected);
    }
}
} // namespace
