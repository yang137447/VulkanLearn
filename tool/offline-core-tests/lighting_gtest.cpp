#include <algorithm>
#include <cmath>

#include <gtest/gtest.h>

#include "core/renderer.h"
#include "tableIIFixture.h"

namespace
{
using namespace VL::Offline;

Vec3 AreaReference(const Scene& scene, Hit hit, Vec3 view, int grid)
{
    Vec3 total{};
    const Vec3 normal = cross(scene.areaU, scene.areaV);
    for (int y = 0; y < grid; ++y)
    {
        for (int x = 0; x < grid; ++x)
        {
            const Vec3 delta = scene.lightCenter +
                scene.areaU * (((x + 0.5) / grid - 0.5) * scene.lightWidth) +
                scene.areaV * (((y + 0.5) / grid - 0.5) * scene.lightDepth) - hit.position;
            const double r2 = dot(delta, delta);
            const Vec3 wi = delta / std::sqrt(r2);
            // 独立面积求积用于校验几何 Jacobian 与 MIS，不复用采样器 PDF。
            const Vec3 response = hit.instance->model == SurfaceModel::Lambert
                ? hit.instance->baseColor * (std::max(0.0, dot(hit.normal, wi)) / kPi)
                : microcylinderResponse(hit, view, wi, *hit.instance->fabric);
            total += response * (std::max(0.0, dot(normal, -wi)) / r2);
        }
    }
    return total * (scene.areaRadiance * scene.lightWidth * scene.lightDepth /
                    (grid * grid));
}

TEST(OfflineLighting, OneSidedEmitterPdfAndMisPartition)
{
    Scene scene{};
    scene.hasAreaEmitter = true;
    scene.lightCenter = {0, 2, 0};
    scene.lightWidth = scene.lightDepth = 2;
    aimAreaLight(scene, {});
    double distance = 0;
    EXPECT_TRUE(intersectAreaLight(scene, {{0, 0, 0}, {0, 1, 0}}, distance));
    EXPECT_NEAR(distance, 2, 1e-12);
    EXPECT_NEAR(areaLightPdf(scene, {}, {0, 1, 0}), 1, 1e-12);
    EXPECT_EQ(areaLightPdf(scene, {0, 3, 0}, {0, -1, 0}), 0);
    EXPECT_FALSE(intersectAreaLight(scene, {{1.01, 0, 0}, {0, 1, 0}}, distance));
    EXPECT_FALSE(intersectAreaLight(scene, {{0, 0, 0}, {1, 0, 0}}, distance));
    for (double a : {0.01, 1.0, 100.0})
        for (double b : {0.01, 1.0, 100.0})
            EXPECT_NEAR(powerWeight(a, b) + powerWeight(b, a), 1, 1e-12);
}

TEST(OfflineLighting, CameraEmitterRadianceAndContinuationAov)
{
    Scene scene{};
    scene.hasAreaEmitter = true;
    scene.lightCenter = {0, 2, 0};
    scene.lightWidth = scene.lightDepth = 2;
    aimAreaLight(scene, {});
    const AlbedoTable albedo;
    Random rng{13u};
    const PathResult front = trace(scene, albedo, {{0, 1, 0}, {0, 1, 0}}, 4, 4, rng);
    EXPECT_NEAR(length(front.direct - Vec3{10, 10, 10}), 0, 1e-12);
    EXPECT_EQ(length(front.indirect), 0);
    const PathResult back = trace(scene, albedo, {{0, 3, 0}, {0, -1, 0}}, 4, 4, rng);
    EXPECT_EQ(length(back.direct), 0);
    EXPECT_EQ(length(back.indirect), 0);
    Mesh mesh;
    Triangle triangle;
    triangle.vertices = {{{-2, 0, -2}, {2, 0, -2}, {0, 0, 2}}};
    triangle.normals = {{{0, 1, 0}, {0, 1, 0}, {0, 1, 0}}};
    mesh.triangles.push_back(triangle);
    mesh.order.push_back(0);
    expand(mesh.bounds, triangleBounds(triangle));
    buildBvh(mesh, 0, 1);
    MeshInstance instance;
    instance.mesh = &mesh;
    instance.model = SurfaceModel::Lambert;
    scene.instances.push_back(instance);
    for (int i = 0; i < 200; ++i)
    {
        const PathResult bounced =
            trace(scene, albedo, {{0.2, 1, 0.2}, {0, -1, 0}}, 4, 4, rng);
        EXPECT_EQ(length(bounced.indirect), 0);
    }
}

TEST(OfflineLighting, AreaMisAndLightSamplingAgreeWithIndependentQuadrature)
{
    Scene scene{};
    scene.hasAreaEmitter = true;
    scene.lightCenter = {-0.8, 1.5, 1.2};
    scene.lightWidth = scene.lightDepth = 2.5;
    aimAreaLight(scene, {});
    Hit hit;
    hit.normal = hit.geometricNormal = {0, 1, 0};
    hit.dpdu = {1, 0, 0};
    hit.dpdv = {0, 0, 1};
    const Vec3 view = normalize(Vec3{0.3, 0.9, 0.1});
    const AlbedoTable albedo;
    MeshInstance instance;
    for (int surface = 0; surface < 2; ++surface)
    {
        instance.model = surface == 0 ? SurfaceModel::Lambert : SurfaceModel::ClothFiber;
        instance.baseColor = {0.22, 0.23, 0.24};
        instance.fabric = surface == 0 ? nullptr : &VL::Offline::Test::TableIIFabrics()[5];
        hit.instance = &instance;
        const Vec3 coarse = AreaReference(scene, hit, view, 64);
        const Vec3 reference = AreaReference(scene, hit, view, 128);
        Random rng{91937u};
        Vec3 mean{}, squares{};
        constexpr int count = 6000;
        for (int i = 1; i <= count; ++i)
        {
            const Vec3 sample = directLight(scene, albedo, hit, view, 4, rng);
            ASSERT_TRUE(std::isfinite(sample.x) && std::isfinite(sample.y) &&
                        std::isfinite(sample.z));
            const Vec3 delta = sample - mean;
            mean += delta / i;
            squares += delta * (sample - mean);
        }
        for (int channel = 0; channel < 3; ++channel)
        {
            const double expected = component(reference, channel);
            const double gridError = std::abs(component(coarse, channel) - expected);
            const double error = std::abs(component(mean, channel) - expected);
            const double se = std::sqrt(component(squares, channel) /
                                        (count * (count - 1.0)));
            EXPECT_LT(gridError, 0.002 * std::max(expected, 1e-6));
            EXPECT_LT(error, 6 * se + gridError + 0.002 * std::max(expected, 1e-6))
                << "surface=" << surface << " channel=" << channel;
        }
    }
}
} // namespace
