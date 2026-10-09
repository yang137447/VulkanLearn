#include <cmath>

#include <gtest/gtest.h>
#include "core/renderer.h"

namespace
{
using namespace VL::Offline;

TEST(AshikhminVelvet, NormalizationAndReciprocity)
{
    const VelvetMaterial velvet = makeVelvetMaterial(0.5, 0.05, {0.3, 0.02, 0.04});
    EXPECT_NEAR(velvet.g.back(), velvet.normalMean, 1e-12);
    const Vec3 n{0, 0, 1};
    const Vec3 v = normalize(Vec3{0.8, 0, 0.6});
    const Vec3 l = normalize(Vec3{0.2, 0.8, 0.6});
    const Vec3 a = velvetBrdf(velvet, n, v, l);
    const Vec3 b = velvetBrdf(velvet, n, l, v);
    EXPECT_NEAR(a.x, b.x, 1e-12);
    EXPECT_NEAR(a.y, b.y, 1e-12);
    EXPECT_GT(velvetSpecular(velvet, n, v, l), 0.0);
    EXPECT_LT(velvetSpecular(velvet, n, v, l), a.x);
}

TEST(AshikhminVelvet, WhiteFurnaceAtObliqueAngles)
{
    const Vec3 n{0, 0, 1};
    constexpr int muSteps = 96, phiSteps = 96;
    for (double f0 : {0.05, 1.0})
    {
        const VelvetMaterial velvet = makeVelvetMaterial(0.5, f0, {1, 1, 1});
        for (double noV : {0.35, 0.7, 0.95})
        {
            const Vec3 v{std::sqrt(1.0 - noV * noV), 0, noV};
            double energy = 0.0;
            for (int i = 0; i < muSteps; ++i)
            {
                const double noL = (i + 0.5) / muSteps;
                const double sinL = std::sqrt(1.0 - noL * noL);
                for (int j = 0; j < phiSteps; ++j)
                {
                    const double phi = 2 * kPi * (j + 0.5) / phiSteps;
                    const Vec3 l{sinL * std::cos(phi), sinL * std::sin(phi), noL};
                    energy += velvetBrdf(velvet, n, v, l).x * noL;
                }
            }
            energy *= 2 * kPi / (muSteps * phiSteps);
            EXPECT_NEAR(energy, 1.0, 0.035) << "f0=" << f0 << " noV=" << noV;
        }
    }
}

TEST(AshikhminVelvet, CorrelatedVisibilityStrengthensRetroreflection)
{
    const VelvetMaterial velvet = makeVelvetMaterial(0.5, 0.05, {0, 0, 0});
    const Vec3 n{0, 0, 1};
    const Vec3 v = normalize(Vec3{0.8, 0, 0.6});
    const Vec3 l = normalize(Vec3{0.8, 0.001, 0.6});
    const Vec3 h = normalize(v + l);
    const double noH = dot(n, h);
    const double p = velvet.normalization *
        std::exp(-noH * noH /
                 (velvet.sigma * velvet.sigma * (1.0 - noH * noH)));
    const double fresnel = velvet.f0 +
        (1.0 - velvet.f0) * std::pow(1.0 - dot(v, h), 5.0);
    const double x = 0.6 * (kVelvetTableSize - 1);
    const int index = static_cast<int>(x);
    const double g = velvet.g[index] * (1.0 - (x - index)) +
                     velvet.g[index + 1] * (x - index);
    const double uncorrelated = p * velvet.normalMean * fresnel / (4.0 * g * g);
    EXPECT_GT(velvetSpecular(velvet, n, v, l), uncorrelated);
}

TEST(AshikhminVelvet, HemisphereSamplingPdfMatchesReturnedDensity)
{
    MeshInstance instance;
    instance.model = SurfaceModel::AshikhminVelvet;
    Hit hit;
    hit.instance = &instance;
    hit.normal = {0, 0, 1};
    Random random{1234};
    for (int i = 0; i < 1000; ++i)
    {
        const DirectionSample sample = sampleDirection(hit, random);
        EXPECT_DOUBLE_EQ(sample.pdf, directionPdf(hit, sample.direction));
        EXPECT_GT(sample.pdf, 0.0);
    }
    EXPECT_DOUBLE_EQ(directionPdf(hit, {0, 0, -1}), 0.0);
}
} // namespace
