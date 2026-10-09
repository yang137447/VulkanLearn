#include <cmath>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>
#include "core/renderSession.h"

namespace
{
using namespace VL::Offline;

const SatinMaterial& PaperSatin()
{
    static const SatinMaterial material =
        makeSatinMaterial(0.1, 0.3, 0.7, 0.05, {1, 1, 1});
    return material;
}

Vec3 Direction(double mu, double phi)
{
    const double sinTheta = std::sqrt(1.0 - mu * mu);
    return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), mu};
}

TEST(AshikhminSatin, MixtureDensityIntegratesToOne)
{
    const auto& satin = PaperSatin();
    constexpr int muSteps = 256, phiSteps = 1024;
    double integral = 0.0;
    for (int i = 0; i < muSteps; ++i)
        for (int j = 0; j < phiSteps; ++j)
            integral += satinDensity(satin, Direction(
                (i + 0.5) / muSteps, 2 * kPi * (j + 0.5) / phiSteps));
    EXPECT_NEAR(integral * 2 * kPi / (muSteps * phiSteps), 1.0, 0.003);
}

TEST(AshikhminSatin, AreaWeightsMixNormalizedDensitiesBeforeScattering)
{
    const auto& satin = PaperSatin();
    const Vec3 h = normalize(Vec3{0.12, 0.5, 0.6});
    const double slope2 = h.x * h.x / (h.z * h.z);
    const double flat = satin.normalizationFlat * std::exp(-slope2 / 0.01);
    const double ends = satin.normalizationEnds * std::exp(-slope2 / 0.09);
    EXPECT_NEAR(satinDensity(satin, h), 0.7 * flat + 0.3 * ends, 1e-12);
}

TEST(AshikhminSatin, ProjectedAreaMatchesIndependentHemisphereQuadrature)
{
    const auto& satin = PaperSatin();
    constexpr int muSteps = 256, phiSteps = 1024;
    for (Vec3 k : {Vec3{0, 0, 1}, normalize(Vec3{0.9, 0.1, 0.2}),
                   normalize(Vec3{0.1, 0.9, 0.2}), Vec3{1, 0, 0}})
    {
        double g = 0.0;
        for (int i = 0; i < muSteps; ++i)
            for (int j = 0; j < phiSteps; ++j)
            {
                const Vec3 h = Direction((i + 0.5) / muSteps,
                                        2 * kPi * (j + 0.5) / phiSteps);
                g += satinDensity(satin, h) * std::max(0.0, dot(h, k));
            }
        g *= 2 * kPi / (muSteps * phiSteps);
        EXPECT_NEAR(satinProjectedArea(satin, k), g, 0.003 * g + 1e-5);
    }
    EXPECT_NEAR(satinProjectedArea(satin, {0, 0, 1}), satin.normalMean, 1e-12);
}

TEST(AshikhminSatin, BrdfIsReciprocal)
{
    const auto& satin = PaperSatin();
    const Vec3 v = normalize(Vec3{0.15, 0.7, 0.4});
    const Vec3 l = normalize(Vec3{-0.25, 0.8, 0.6});
    EXPECT_NEAR(satinBrdf(satin, v, l).x, satinBrdf(satin, l, v).x, 1e-12);
}

TEST(AshikhminSatin, CylindricalNormalsProduceDirectionalHighlight)
{
    const auto& satin = PaperSatin();
    const Vec3 across = normalize(Vec3{0, 0.8, 0.6});
    const Vec3 along = normalize(Vec3{0.8, 0, 0.6});
    EXPECT_GT(satinSpecular(satin, across, across),
              100.0 * satinSpecular(satin, along, along));
}

TEST(AshikhminSatin, FiberRotationRotatesTheBrdfInsteadOfTheGeometry)
{
    const auto& satin = PaperSatin();
    MeshInstance instance;
    instance.model = SurfaceModel::AshikhminSatin;
    instance.satin = &satin;
    Hit hit;
    hit.instance = &instance;
    hit.normal = {0, 0, 1};
    hit.dpdu = {1, 0, 0};
    const Vec3 v = normalize(Vec3{0, 0.8, 0.6});
    const double original = evaluateSatin(satin, hit, v, v).x;
    instance.fiberRotationDegrees = 90.0;
    EXPECT_NEAR(evaluateSatin(satin, hit, v, v).x,
                satinBrdf(satin, {v.y, 0, v.z}, {v.y, 0, v.z}).x, 1e-12);
    EXPECT_GT(original, evaluateSatin(satin, hit, v, v).x);
}

TEST(AshikhminSatin, CompensatedWhiteFurnaceAtMultipleAzimuths)
{
    const auto& satin = PaperSatin();
    constexpr int muSteps = 128, phiSteps = 512;
    for (double mu : {0.35, 0.7, 0.95})
        for (double phi : {0.0, kPi / 4, kPi / 2})
        {
            const Vec3 v = Direction(mu, phi);
            double energy = 0.0;
            for (int i = 0; i < muSteps; ++i)
                for (int j = 0; j < phiSteps; ++j)
                {
                    const Vec3 l = Direction((i + 0.5) / muSteps,
                                            2 * kPi * (j + 0.5) / phiSteps);
                    energy += satinBrdf(satin, v, l).x * l.z;
                }
            energy *= 2 * kPi / (muSteps * phiSteps);
            EXPECT_NEAR(energy, 1.0, 0.015) << "mu=" << mu << "phi=" << phi;
        }
}

TEST(AshikhminSatin, HemisphereSamplingPdfMatchesReturnedDensity)
{
    MeshInstance instance;
    instance.model = SurfaceModel::AshikhminSatin;
    Hit hit;
    hit.instance = &instance;
    hit.normal = {0, 0, 1};
    Random random{1234};
    for (int i = 0; i < 1000; ++i)
    {
        const auto sample = sampleDirection(hit, random);
        EXPECT_DOUBLE_EQ(sample.pdf, directionPdf(hit, sample.direction));
        EXPECT_GT(sample.pdf, 0.0);
    }
}

TEST(AshikhminSatin, FactoryRejectsInvalidNumericalInputs)
{
    EXPECT_THROW(makeSatinMaterial(0, 0.3, 0.7, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSatinMaterial(0.1, 0.02, 0.7, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSatinMaterial(0.1, 0.3, 1.1, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSatinMaterial(0.1, 0.3, 0.7,
                 std::numeric_limits<double>::quiet_NaN(), {}), std::invalid_argument);
}

TEST(AshikhminSatin, SessionRejectsAnUnownedFrozenMaterial)
{
    RenderInput input;
    input.width = input.height = input.targetSamples = 1;
    auto mesh = std::make_shared<Mesh>();
    mesh->nodes.emplace_back();
    input.instanceMeshes.push_back(mesh);
    MeshInstance instance;
    instance.mesh = mesh.get();
    instance.model = SurfaceModel::AshikhminSatin;
    instance.satin = &PaperSatin();
    input.scene.instances.push_back(instance);
    RenderSession session(1);
    EXPECT_THROW(session.Start(std::move(input)), std::invalid_argument);
}
} // namespace
