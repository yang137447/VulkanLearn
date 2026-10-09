#include <cmath>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>
#include "core/renderSession.h"

namespace
{
using namespace VL::Offline;

const SlantedVelvetMaterial& PaperSlantedVelvet()
{
    static const auto material = makeSlantedVelvetMaterial(0.5, 40, 0.05, {1, 1, 1});
    return material;
}

Vec3 Direction(double mu, double phi)
{
    const double sine = std::sqrt(1.0 - mu * mu);
    return {sine * std::cos(phi), sine * std::sin(phi), mu};
}

TEST(AshikhminSlantedVelvet, RotatedDensityIsNormalizedOverItsWholeSupport)
{
    const auto& material = PaperSlantedVelvet();
    constexpr int muSteps = 256, phiSteps = 1024;
    double integral = 0, below = 0;
    for (int i = 0; i < muSteps; ++i)
        for (int j = 0; j < phiSteps; ++j)
        {
            const Vec3 h = Direction(-1.0 + 2.0 * (i + 0.5) / muSteps,
                                    2 * kPi * (j + 0.5) / phiSteps);
            const double density = slantedVelvetDensity(material, h);
            integral += density;
            if (h.z < 0) below += density;
        }
    EXPECT_NEAR(integral * 4 * kPi / (muSteps * phiSteps), 1, 0.002);
    EXPECT_GT(below, 0); // 非 height-field 分布不裁掉旋转后落在宏观下半球的法线。
}

TEST(AshikhminSlantedVelvet, BundleSlantIsNotTheSideNormalSlant)
{
    const auto& material = PaperSlantedVelvet();
    const double slant = 40 * kPi / 180;
    const Vec3 bundle{std::sin(slant), 0, std::cos(slant)};
    const Vec3 side{-std::cos(slant), 0, std::sin(slant)};
    EXPECT_NEAR(dot(side, bundle), 0, 1e-12);
    EXPECT_NEAR(slantedVelvetDensity(material, side), material.normalization, 1e-12);
    EXPECT_LT(slantedVelvetDensity(material, {0, 0, 1}),
              0.01 * slantedVelvetDensity(material, side));
}

TEST(AshikhminSlantedVelvet, TopContributionDoublesSignedMeanNotPositiveProjection)
{
    const auto& material = PaperSlantedVelvet();
    constexpr int muSteps = 256, phiSteps = 1024;
    double mean = 0;
    for (int i = 0; i < muSteps; ++i)
        for (int j = 0; j < phiSteps; ++j)
        {
            const Vec3 h = Direction(-1.0 + 2.0 * (i + 0.5) / muSteps,
                                    2 * kPi * (j + 0.5) / phiSteps);
            mean += h.z * slantedVelvetDensity(material, h);
        }
    mean *= 4 * kPi / (muSteps * phiSteps);
    EXPECT_NEAR(material.normalMean, 2 * mean, 0.002);
    EXPECT_GT(2 * slantedVelvetProjectedArea(material, {0, 0, 1}),
              material.normalMean + 1e-5);
}

TEST(AshikhminSlantedVelvet, ProjectedAreaMatchesIndependentFullSphereQuadrature)
{
    const auto& material = PaperSlantedVelvet();
    constexpr int muSteps = 256, phiSteps = 1024;
    for (Vec3 k : {Vec3{0, 0, 1}, normalize(Vec3{-0.8, 0.2, 0.6}),
                   normalize(Vec3{0.8, -0.2, 0.6})})
    {
        double g = 0;
        for (int i = 0; i < muSteps; ++i)
            for (int j = 0; j < phiSteps; ++j)
            {
                const Vec3 h = Direction(-1.0 + 2.0 * (i + 0.5) / muSteps,
                                        2 * kPi * (j + 0.5) / phiSteps);
                g += slantedVelvetDensity(material, h) * std::max(0.0, dot(h, k));
            }
        g *= 4 * kPi / (muSteps * phiSteps);
        EXPECT_NEAR(slantedVelvetProjectedArea(material, k), g, 0.005 * g + 1e-5);
    }
}

TEST(AshikhminSlantedVelvet, BrdfRemainsReciprocal)
{
    const auto& material = PaperSlantedVelvet();
    const Vec3 v = normalize(Vec3{-0.5, 0.2, 0.7});
    const Vec3 l = normalize(Vec3{-0.2, 0.7, 0.4});
    EXPECT_NEAR(slantedVelvetBrdf(material, v, l).x,
                slantedVelvetBrdf(material, l, v).x, 1e-12);
}

TEST(AshikhminSlantedVelvet, SlantedSidesHaveAsymmetricRetroreflection)
{
    const auto& material = PaperSlantedVelvet();
    const Vec3 left = normalize(Vec3{-0.8, 0, 0.6});
    const Vec3 right = normalize(Vec3{0.8, 0, 0.6});
    EXPECT_GT(slantedVelvetSpecular(material, left, left),
              100 * slantedVelvetSpecular(material, right, right));
}

TEST(AshikhminSlantedVelvet, FifteenDegreeCorrelationUsesEquationFourteen)
{
    const auto& material = PaperSlantedVelvet();
    const Vec3 v = Direction(0.6, kPi), l = Direction(0.65, kPi + 0.2);
    const Vec3 h = normalize(v + l);
    const double gV = slantedVelvetProjectedArea(material, v);
    const double gL = slantedVelvetProjectedArea(material, l);
    const double pV = v.z * material.normalMean / gV;
    const double pL = l.z * material.normalMean / gL;
    const double t = std::exp(-0.5 * std::pow(0.2 / (15 * kPi / 180), 2));
    const double fresnel = material.f0 +
        (1 - material.f0) * std::pow(1 - dot(v, h), 5);
    const double uncorrelated = slantedVelvetDensity(material, h) *
        material.normalMean * fresnel / (4 * gV * gL);
    EXPECT_NEAR(slantedVelvetSpecular(material, v, l),
                uncorrelated * ((1 - t) + t / std::max(pV, pL)), 1e-12);
}

TEST(AshikhminSlantedVelvet, PaperFresnelHasBoundedReflectanceAndCompensatedWhiteFurnace)
{
    const auto& material = PaperSlantedVelvet();
    for (double reflectance : material.reflectance)
    {
        EXPECT_TRUE(std::isfinite(reflectance));
        EXPECT_GE(reflectance, 0);
        EXPECT_LT(reflectance, 1);
    }
    constexpr int muSteps = 128, phiSteps = 512;
    for (double mu : {0.35, 0.7, 0.95})
        for (double phi : {0.0, kPi / 2, kPi})
        {
            const Vec3 v = Direction(mu, phi);
            double energy = 0;
            for (int i = 0; i < muSteps; ++i)
                for (int j = 0; j < phiSteps; ++j)
                {
                    const Vec3 l = Direction((i + 0.5) / muSteps,
                                            2 * kPi * (j + 0.5) / phiSteps);
                    energy += slantedVelvetBrdf(material, v, l).x * l.z;
                }
            energy *= 2 * kPi / (muSteps * phiSteps);
            EXPECT_NEAR(energy, 1, 0.025) << "mu=" << mu << " phi=" << phi;
        }
}

TEST(AshikhminSlantedVelvet, FiberRotationRotatesTheSignedDistribution)
{
    auto material = PaperSlantedVelvet();
    material.diffuse = {};
    MeshInstance instance;
    instance.model = SurfaceModel::AshikhminSlantedVelvet;
    instance.slantedVelvet = &material;
    Hit hit;
    hit.instance = &instance;
    hit.normal = {0, 0, 1};
    hit.dpdu = {1, 0, 0};
    const Vec3 v = normalize(Vec3{-0.8, 0, 0.6});
    const double original = evaluateSlantedVelvet(material, hit, v, v).x;
    instance.fiberRotationDegrees = 180;
    EXPECT_NEAR(evaluateSlantedVelvet(material, hit, v, v).x,
                slantedVelvetBrdf(material, {-v.x, 0, v.z}, {-v.x, 0, v.z}).x, 1e-12);
    EXPECT_GT(original, evaluateSlantedVelvet(material, hit, v, v).x);
}

TEST(AshikhminSlantedVelvet, HemisphereSamplerAndPdfAgree)
{
    MeshInstance instance;
    instance.model = SurfaceModel::AshikhminSlantedVelvet;
    Hit hit;
    hit.instance = &instance;
    hit.normal = {0, 0, 1};
    Random rng{1234};
    for (int i = 0; i < 1000; ++i)
    {
        const auto sample = sampleDirection(hit, rng);
        EXPECT_DOUBLE_EQ(sample.pdf, directionPdf(hit, sample.direction));
        EXPECT_GT(sample.pdf, 0);
    }
}

TEST(AshikhminSlantedVelvet, FactoryRejectsOutOfQuadratureContract)
{
    EXPECT_THROW(makeSlantedVelvetMaterial(0.01, 40, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSlantedVelvetMaterial(0.2, 40, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSlantedVelvetMaterial(0.5, 20, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSlantedVelvetMaterial(0.5, 0, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSlantedVelvetMaterial(0.5, 90, 0.05, {}), std::invalid_argument);
    EXPECT_THROW(makeSlantedVelvetMaterial(0.5, 40,
                 std::numeric_limits<double>::quiet_NaN(), {}), std::invalid_argument);
}

TEST(AshikhminSlantedVelvet, BoundaryParametersProduceFiniteFrozenTables)
{
    for (const auto& [sigma, slant] :
         {std::pair{0.35, 30.0}, std::pair{1.0, 80.0}})
    {
        const auto material = makeSlantedVelvetMaterial(sigma, slant, 0.05, {1, 1, 1});
        for (double g : material.g)
        {
            EXPECT_TRUE(std::isfinite(g));
            EXPECT_GT(g, 0);
        }
        for (double reflectance : material.reflectance)
            EXPECT_TRUE(std::isfinite(reflectance));
        EXPECT_TRUE(std::isfinite(material.diffuseScale));
    }
}

TEST(AshikhminSlantedVelvet, PerfectFresnelRetainsThePaperNonPhysicalEnergyLimit)
{
    const auto material = makeSlantedVelvetMaterial(0.5, 40, 1, {});
    EXPECT_GT(*std::max_element(material.reflectance.begin(), material.reflectance.end()), 1);
}

TEST(AshikhminSlantedVelvet, EvaluatorDispatchDoesNotFallBackToSheen)
{
    const auto& material = PaperSlantedVelvet();
    MeshInstance instance;
    instance.model = SurfaceModel::AshikhminSlantedVelvet;
    instance.slantedVelvet = &material;
    Hit hit;
    hit.instance = &instance;
    hit.normal = {0, 0, 1};
    hit.dpdu = {1, 0, 0};
    const Vec3 v = normalize(Vec3{-0.8, 0, 0.6});
    const AlbedoTable albedo;
    EXPECT_DOUBLE_EQ(evaluateBsdf(albedo, hit, v, v).x,
                     evaluateSlantedVelvet(material, hit, v, v).x);
}

TEST(AshikhminSlantedVelvet, SessionRejectsUnownedMaterial)
{
    RenderInput input;
    input.width = input.height = input.targetSamples = 1;
    auto mesh = std::make_shared<Mesh>();
    mesh->nodes.emplace_back();
    input.instanceMeshes.push_back(mesh);
    MeshInstance instance;
    instance.mesh = mesh.get();
    instance.model = SurfaceModel::AshikhminSlantedVelvet;
    instance.slantedVelvet = &PaperSlantedVelvet();
    input.scene.instances.push_back(instance);
    RenderSession session(1);
    EXPECT_THROW(session.Start(std::move(input)), std::invalid_argument);
}
} // namespace
