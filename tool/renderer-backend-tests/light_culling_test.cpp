#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "render/lightCulling/lightCulling.h"
#include "render/rendergraph/renderGraphPassType.h"

namespace
{

std::string ReadSource(const std::string& relativePath)
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

bool Contains(const std::string& source, const std::string& needle)
{
    return source.find(needle) != std::string::npos;
}

// 与 Camera::SetProjection + ndcMatrix 相同的约定：
// view 前向 -Z，最终 projection 把 near 映射到 0、far 映射到 1。
Eigen::Matrix4f BuildProjection(
    float verticalFovRadians,
    float aspect,
    float nearCm,
    float farCm)
{
    const float n = -nearCm;
    const float f = -farCm;
    const float k = -1.0f / std::tan(verticalFovRadians * 0.5f);
    Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
    projection <<
        -k, 0.0f, 0.0f, 0.0f,
        0.0f, -aspect * k, 0.0f, 0.0f,
        0.0f, 0.0f, -(n + f) / (n - f), 2.0f * n * f / (n - f),
        0.0f, 0.0f, -1.0f, 0.0f;
    Eigen::Matrix4f ndc = Eigen::Matrix4f::Identity();
    ndc(1, 1) = -1.0f;
    ndc(2, 2) = -0.5f;
    ndc(2, 3) = 0.5f;
    return ndc * projection;
}

VL::LightCullingCamera MakeCamera(
    uint32_t width = 1920,
    uint32_t height = 1080,
    float nearCm = 1.0f,
    float farCm = 8000.0f)
{
    VL::LightCullingCamera camera;
    camera.view = Eigen::Matrix4f::Identity();
    camera.projection = BuildProjection(
        60.0f * 3.14159265358979323846f / 180.0f,
        static_cast<float>(width) / static_cast<float>(height),
        nearCm,
        farCm);
    camera.width = width;
    camera.height = height;
    camera.nearCm = nearCm;
    camera.farCm = farCm;
    return camera;
}

VL::LightSnapshot MakePoint(
    const Eigen::Vector3f& position,
    float radius)
{
    VL::LightSnapshot light;
    light.type = VL::LightSnapshotType::Point;
    light.position = position;
    light.radius = radius;
    light.intensity = 10.0f;
    return light;
}

VL::LightSnapshot MakeDirectional()
{
    VL::LightSnapshot light;
    light.type = VL::LightSnapshotType::Directional;
    light.direction = Eigen::Vector3f(0.0f, -1.0f, 0.0f);
    light.intensity = 1.0f;
    return light;
}

bool AccumulatorHas(
    const VL::LightTileAccumulator& accumulator,
    uint32_t index)
{
    for (uint32_t value : accumulator.indices)
    {
        if (value == index)
        {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(LightCullingMath, TileGridUsesCeilDivision)
{
    uint32_t tilesX = 0;
    uint32_t tilesY = 0;
    VL::ComputeLightTileGrid(1920, 1080, tilesX, tilesY);
    EXPECT_EQ(tilesX, 120u);
    EXPECT_EQ(tilesY, 68u);
    VL::ComputeLightTileGrid(1, 1, tilesX, tilesY);
    EXPECT_EQ(tilesX, 1u);
    EXPECT_EQ(tilesY, 1u);
    VL::ComputeLightTileGrid(0, 0, tilesX, tilesY);
    EXPECT_EQ(tilesX, 1u);
    EXPECT_EQ(tilesY, 1u);
}

TEST(LightCullingMath, LayoutMatchesShaderConstants)
{
    EXPECT_EQ(sizeof(VL::LightGridParams), 64u);
    EXPECT_EQ(offsetof(VL::LightGridParams, tileSize), 0u);
    EXPECT_EQ(offsetof(VL::LightGridParams, enabled), 16u);
    EXPECT_EQ(offsetof(VL::LightGridParams, pointLightOffset), 32u);
    EXPECT_EQ(VL::kLightCullingTileSize, 16u);
    EXPECT_EQ(VL::kLightCullingMaxLightsPerTile, 64u);
    EXPECT_EQ(VL::LightGridBytes(3), 16u + 3u * 16u);
    EXPECT_EQ(VL::LightIndexListBytes(3), 3u * 64u * 4u);
}

TEST(LightCullingMath, PointLightInFrontCoversCenterTile)
{
    const VL::LightCullingCamera camera = MakeCamera();
    const VL::LightScreenBounds bounds =
        VL::ComputeLightScreenBounds(
            Eigen::Vector3f(0.0f, 0.0f, -500.0f),
            50.0f,
            camera);
    ASSERT_TRUE(bounds.visible);
    // 前方 500cm、半径 50cm 的灯应覆盖屏幕中心附近的 tile。
    EXPECT_TRUE(VL::TileOverlapsLightBounds(bounds, 60, 33, 16, 1920, 1080));
    // 左上角远离灯，必须被裁掉。
    EXPECT_FALSE(VL::TileOverlapsLightBounds(bounds, 0, 0, 16, 1920, 1080));
}

TEST(LightCullingMath, BehindCameraAndBeyondFarAreCulled)
{
    const VL::LightCullingCamera camera = MakeCamera();
    const VL::LightScreenBounds behind =
        VL::ComputeLightScreenBounds(
            Eigen::Vector3f(0.0f, 0.0f, 500.0f),
            10.0f,
            camera);
    EXPECT_FALSE(behind.visible);

    const VL::LightScreenBounds beyondFar =
        VL::ComputeLightScreenBounds(
            Eigen::Vector3f(0.0f, 0.0f, -9000.0f),
            10.0f,
            camera);
    EXPECT_FALSE(beyondFar.visible);
}

TEST(LightCullingMath, UnboundedRadiusCoversWholeScreen)
{
    const VL::LightCullingCamera camera = MakeCamera();
    const VL::LightScreenBounds bounds =
        VL::ComputeLightScreenBounds(
            Eigen::Vector3f(0.0f, 0.0f, -500.0f),
            0.0f,
            camera);
    ASSERT_TRUE(bounds.visible);
    for (uint32_t tileY = 0; tileY < 68; tileY += 17)
    {
        for (uint32_t tileX = 0; tileX < 120; tileX += 30)
        {
            EXPECT_TRUE(VL::TileOverlapsLightBounds(
                bounds, tileX, tileY, 16, 1920, 1080));
        }
    }
}

TEST(LightCullingMath, SphereCrossingNearPlaneCoversWholeScreen)
{
    const VL::LightCullingCamera camera = MakeCamera();
    // 灯在相机后方但半径跨过相机平面：clip.w <= 0，必须保守覆盖全屏，
    // 否则会漏掉近处溢出的光。
    const VL::LightScreenBounds bounds =
        VL::ComputeLightScreenBounds(
            Eigen::Vector3f(0.0f, 0.0f, 50.0f),
            500.0f,
            camera);
    ASSERT_TRUE(bounds.visible);
    EXPECT_TRUE(VL::TileOverlapsLightBounds(bounds, 0, 0, 16, 1920, 1080));
    EXPECT_TRUE(VL::TileOverlapsLightBounds(bounds, 119, 67, 16, 1920, 1080));
}

TEST(LightCullingMath, DirectionalLightsEnterEveryTile)
{
    std::vector<VL::LightSnapshot> lights;
    lights.push_back(MakeDirectional());
    const VL::LightCullingCamera camera = MakeCamera();
    for (uint32_t tileY : {0u, 30u, 67u})
    {
        VL::LightTileAccumulator accumulator;
        VL::BuildLightTileReference(
            lights, camera, 0, tileY, 16, accumulator);
        ASSERT_EQ(accumulator.indices.size(), 1u);
        EXPECT_EQ(accumulator.indices[0], 0u);
        EXPECT_EQ(accumulator.overflow, 0u);
    }
}

TEST(LightCullingMath, PackingOrderIsDirectionalThenPointThenSpot)
{
    std::vector<VL::LightSnapshot> lights;
    lights.push_back(MakePoint(Eigen::Vector3f(0.0f, 0.0f, -500.0f), 50.0f));
    lights.push_back(MakeDirectional());
    VL::LightSnapshot spot;
    spot.type = VL::LightSnapshotType::Spot;
    spot.position = Eigen::Vector3f(0.0f, 0.0f, -500.0f);
    spot.direction = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    spot.radius = 800.0f;
    spot.coneAngleOuter = 0.4f;
    spot.coneAngleInner = 0.2f;
    lights.push_back(spot);

    const VL::LightCullingCamera camera = MakeCamera();
    VL::LightTileAccumulator accumulator;
    VL::BuildLightTileReference(lights, camera, 60, 33, 16, accumulator);
    // 方向光先（索引 0），点光次（1），聚光最后（2）。
    ASSERT_GE(accumulator.indices.size(), 3u);
    EXPECT_EQ(accumulator.indices[0], 0u);
    EXPECT_EQ(accumulator.indices[1], 1u);
    EXPECT_EQ(accumulator.indices[2], 2u);
}

TEST(LightCullingMath, OverflowTruncatesDeterministically)
{
    std::vector<VL::LightSnapshot> lights;
    lights.push_back(MakeDirectional());
    lights.push_back(MakeDirectional());
    lights.push_back(MakeDirectional());
    const VL::LightCullingCamera camera = MakeCamera();
    VL::LightTileAccumulator accumulator;
    accumulator.capacity = 1;
    VL::BuildLightTileReference(lights, camera, 0, 0, 16, accumulator);
    ASSERT_EQ(accumulator.indices.size(), 1u);
    EXPECT_EQ(accumulator.indices[0], 0u);
    EXPECT_EQ(accumulator.overflow, 2u);
}

TEST(LightCullingMath, SpotBoundingSphereContainsApexAndBase)
{
    VL::LightSnapshot spot;
    spot.type = VL::LightSnapshotType::Spot;
    spot.position = Eigen::Vector3f(100.0f, 200.0f, 300.0f);
    spot.direction = Eigen::Vector3f(0.0f, 0.0f, -2.0f);
    spot.radius = 400.0f;
    spot.coneAngleOuter = 0.5f;
    const Eigen::Vector3f center =
        VL::ComputeSpotCullingCenter(spot);
    const float sphereRadius =
        VL::ComputeSpotCullingRadius(spot);
    ASSERT_GT(sphereRadius, 0.0f);
    // 顶点与底面边缘都必须落在包围球内。
    EXPECT_LE(
        (spot.position - center).norm(),
        sphereRadius + 1.0e-3f);
    const Eigen::Vector3f baseCenter =
        spot.position + spot.direction.normalized() * spot.radius;
    const float baseRadius =
        spot.radius * std::tan(spot.coneAngleOuter);
    Eigen::Vector3f baseRim =
        baseCenter + Eigen::Vector3f(1.0f, 0.0f, 0.0f) * baseRadius;
    EXPECT_LE(
        (baseRim - center).norm(),
        sphereRadius + 1.0e-3f);
}

TEST(LightCullingGraph, PassTypeIsRegisteredAndOrderedBeforeGeometry)
{
    EXPECT_EQ(
        VL::ParseRenderGraphPassType("lightCulling").value(),
        VL::RenderGraphPassType::LightCulling);
    const std::string config = ReadSource("config/renderGraphConfig.json");
    const size_t culling = config.find("\"name\": \"lightCulling\"");
    const size_t geometry = config.find("\"name\": \"geometry\"");
    ASSERT_NE(culling, std::string::npos);
    ASSERT_NE(geometry, std::string::npos);
    EXPECT_LT(culling, geometry);
    // 必须在局部光阴影之后，保证本帧 shadow slice 已就绪。
    const size_t localShadow7 = config.find("\"name\": \"localShadow7\"");
    ASSERT_NE(localShadow7, std::string::npos);
    EXPECT_LT(localShadow7, culling);
}

TEST(LightCullingShaderContract, ComputeWritesGridAndIndices)
{
    const std::string compute =
        ReadSource("shader/glsl/pass/lightCulling.comp");
    EXPECT_TRUE(Contains(compute, "directionalLightCount"));
    EXPECT_TRUE(Contains(compute, "atomicAdd(lightGrid.header.y"));
    EXPECT_TRUE(Contains(compute, "ComputeLightScreenBounds"));
    EXPECT_TRUE(Contains(compute, "TileOverlapsBounds"));
    EXPECT_TRUE(Contains(compute, "uvec4(count, overflow, 0u, 0u)"));
    EXPECT_TRUE(Contains(compute, "lightGrid.tiles[tileIndex]"));
}

TEST(LightCullingShaderContract, MaterialLightingHasGridBranchAndFallback)
{
    const std::string lighting =
        ReadSource("shader/glsl/common/lighting.glsl");
    EXPECT_TRUE(Contains(lighting, "VL_LIGHT_GRID_BINDING 5"));
    EXPECT_TRUE(Contains(lighting, "VL_LIGHT_INDEX_BINDING 6"));
    EXPECT_TRUE(Contains(lighting, "VL_LIGHT_GRID_PARAMS_BINDING 7"));
    EXPECT_TRUE(Contains(lighting, "if (LightGridEnabled())"));
    EXPECT_TRUE(Contains(lighting, "AccumulateSceneLightByIndex"));
    // 回退分支必须保留三段连续循环（方向光/点光/聚光各一次）。
    EXPECT_TRUE(Contains(lighting, "uboLight.directionalLightOffset"));
    EXPECT_TRUE(Contains(lighting, "uboLight.pointLightOffset"));
    EXPECT_TRUE(Contains(lighting, "uboLight.spotLightOffset"));
    // 粒子灯不参与聚类。
    EXPECT_TRUE(Contains(lighting, "CalculateParticleLightLobes"));
}

TEST(LightCullingShaderContract, ParticleLightingUsesDedicatedBindings)
{
    const std::string particle =
        ReadSource("shader/glsl/particle/particleSceneLighting.glsl");
    EXPECT_TRUE(Contains(particle, "VL_LIGHT_GRID_BINDING 10"));
    EXPECT_TRUE(Contains(particle, "VL_LIGHT_INDEX_BINDING 11"));
    EXPECT_TRUE(Contains(particle, "VL_LIGHT_GRID_PARAMS_BINDING 12"));
    EXPECT_TRUE(Contains(particle, "AccumulateParticleMeshLightByIndex"));
    EXPECT_TRUE(Contains(particle, "AccumulateParticleBillboardLightByIndex"));
    EXPECT_TRUE(Contains(particle, "ParticleBillboardPointContribution"));
}

TEST(LightCullingShaderContract, SharedSamplingHandleTileEdges)
{
    const std::string sampling =
        ReadSource("shader/glsl/common/lightGridSampling.glsl");
    EXPECT_TRUE(Contains(sampling, "LightGridTileIndexFromFragCoord"));
    EXPECT_TRUE(Contains(sampling, "LightGridVisibleLightCount"));
    EXPECT_TRUE(Contains(sampling, "min("));
    EXPECT_TRUE(Contains(sampling, "extentParams.x != 0u"));
}
