#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>

#include "render/rendergraph/renderGraphCompiler.h"
#include "render/shadow/localShadowMath.h"
#include "sceneNode.h"
#include "world/localLightShadowSettings.h"

namespace
{

using VL::LightSnapshot;
using VL::LightSnapshotType;

Eigen::Vector4f TransformPoint(
    const Eigen::Matrix4f& matrix,
    const Eigen::Vector3f& point)
{
    return matrix * Eigen::Vector4f(point.x(), point.y(), point.z(), 1.0f);
}

} // namespace

TEST(LocalShadowMath, OctantOrderPinsSignBitLayout)
{
    const auto& octants = VL::LocalShadowOctantOrder();
    ASSERT_EQ(octants.size(), 8u);
    for (uint32_t index = 0; index < octants.size(); ++index)
    {
        EXPECT_FLOAT_EQ(
            octants[index].sx,
            (index & 1u) != 0 ? 1.0f : -1.0f);
        EXPECT_FLOAT_EQ(
            octants[index].sy,
            (index & 2u) != 0 ? 1.0f : -1.0f);
        EXPECT_FLOAT_EQ(
            octants[index].sz,
            (index & 4u) != 0 ? 1.0f : -1.0f);
    }
}

TEST(LocalShadowMath, OctahedralProjectionMatchesStandardEncoding)
{
    const Eigen::Vector3f light(10.0f, 20.0f, 30.0f);
    const Eigen::Vector3f point(13.0f, 17.0f, 23.0f);
    const Eigen::Vector3f delta = point - light;

    for (uint32_t index = 0;
         index < VL::LocalShadowOctantOrder().size();
         ++index)
    {
        const VL::LocalShadowOctant octant =
            VL::LocalShadowOctantOrder()[index];
        const float sx = octant.sx;
        const float sy = octant.sy;
        const float sz = octant.sz;

        const float expectedW =
            sx * delta.x() + sy * delta.y() + sz * delta.z();
        float expectedX = 0.0f;
        float expectedY = 0.0f;
        if (sz > 0.0f)
        {
            // 标准八面体上半球：p_xy = d_xy / L1。
            expectedX = delta.x();
            expectedY = delta.y();
        }
        else
        {
            // 下半球折叠：(dx - sx*dz)/L1，(dy - sy*dz)/L1。
            expectedX = delta.x() - sx * delta.z();
            expectedY = delta.y() - sy * delta.z();
        }

        const Eigen::Vector4f clip = TransformPoint(
            VL::BuildOctahedralProjectionMatrix(light, index),
            point);
        EXPECT_NEAR(clip.x(), expectedX, 1.0e-5f) << "octant " << index;
        EXPECT_NEAR(clip.y(), expectedY, 1.0e-5f) << "octant " << index;
        EXPECT_NEAR(clip.z(), 0.0f, 1.0e-6f) << "octant " << index;
        EXPECT_NEAR(clip.w(), expectedW, 1.0e-5f) << "octant " << index;
    }
}

TEST(LocalShadowMath, SpotMatricesMapAxisToVulkanDepthRange)
{
    const Eigen::Vector3f position(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f direction(0.0f, 0.0f, -1.0f);
    const float outerConeRadians = 30.0f * 3.14159265358979323846f / 180.0f;
    const VL::LocalShadowSpotMatrices matrices =
        VL::BuildSpotShadowMatrices(
            position,
            direction,
            outerConeRadians,
            1.0f,
            100.0f);

    const Eigen::Vector4f nearClip = TransformPoint(
        matrices.projection * matrices.view,
        Eigen::Vector3f(0.0f, 0.0f, -1.0f));
    ASSERT_GT(nearClip.w(), 0.0f);
    EXPECT_NEAR(nearClip.x() / nearClip.w(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(nearClip.y() / nearClip.w(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(nearClip.z() / nearClip.w(), 0.0f, 1.0e-5f);

    const Eigen::Vector4f farClip = TransformPoint(
        matrices.projection * matrices.view,
        Eigen::Vector3f(0.0f, 0.0f, -100.0f));
    ASSERT_GT(farClip.w(), 0.0f);
    EXPECT_NEAR(farClip.z() / farClip.w(), 1.0f, 1.0e-5f);

    // 灯位置在 view 空间应为原点。
    const Eigen::Vector4f origin = TransformPoint(
        matrices.view,
        position);
    EXPECT_NEAR(origin.x(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(origin.y(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(origin.z(), 0.0f, 1.0e-5f);
}

TEST(LocalShadowMath, SpotOuterConeEdgesLandOnNdcSquare)
{
    const Eigen::Vector3f position(0.0f, 0.0f, 0.0f);
    const Eigen::Vector3f direction(0.0f, 0.0f, -1.0f);
    const float outerConeRadians = 25.0f * 3.14159265358979323846f / 180.0f;
    const VL::LocalShadowSpotMatrices matrices =
        VL::BuildSpotShadowMatrices(
            position,
            direction,
            outerConeRadians,
            1.0f,
            120.0f);

    const float distance = 50.0f;
    const float tangent = std::tan(outerConeRadians);
    const Eigen::Vector3f edge(
        distance * tangent,
        0.0f,
        -distance);
    const Eigen::Vector4f clip = TransformPoint(
        matrices.projection * matrices.view,
        edge);
    ASSERT_GT(clip.w(), 0.0f);
    // 采样端把 Camera::SetProjection 展开成 -1/tan 的 x 比例；
    // 世界 +X 的锥外缘因此落在 ndc.x = -1。
    EXPECT_NEAR(clip.x() / clip.w(), -1.0f, 1.0e-5f);
}

TEST(LocalShadowMath, RankStableByScoreThenLightId)
{
    auto makeLight = [](LightSnapshotType type, bool castShadows,
                        VL::RuntimeId lightId, float intensity,
                        float radius, const Eigen::Vector3f& position)
    {
        LightSnapshot light;
        light.type = type;
        light.castShadows = castShadows;
        light.lightId = lightId;
        light.intensity = intensity;
        light.radius = radius;
        light.position = position;
        return light;
    };

    std::vector<LightSnapshot> lights;
    lights.push_back(makeLight(
        LightSnapshotType::Point, false, 1, 10000.0f, 100.0f,
        Eigen::Vector3f(100.0f, 0.0f, 0.0f)));
    lights.push_back(makeLight(
        LightSnapshotType::Spot, true, 3, 100.0f, 1000.0f,
        Eigen::Vector3f(500.0f, 0.0f, 0.0f)));
    lights.push_back(makeLight(
        LightSnapshotType::Point, true, 9, 10.0f, 100.0f,
        Eigen::Vector3f(100.0f, 0.0f, 0.0f)));
    lights.push_back(makeLight(
        LightSnapshotType::Point, true, 7, 10.0f, 100.0f,
        Eigen::Vector3f(100.0f, 0.0f, 0.0f)));
    lights.push_back(makeLight(
        LightSnapshotType::Directional, true, 2, 100000.0f, 0.0f,
        Eigen::Vector3f::Zero()));

    const std::vector<size_t> ranked = VL::RankLocalShadowLights(
        lights,
        Eigen::Vector3f::Zero(),
        3);
    ASSERT_EQ(ranked.size(), 3u);
    EXPECT_EQ(ranked[0], 1u);
    // 后两盏灯同分，按 lightId 升序：7 在 9 前。
    EXPECT_EQ(ranked[1], 3u);
    EXPECT_EQ(ranked[2], 2u);

    EXPECT_TRUE(VL::RankLocalShadowLights(
        lights, Eigen::Vector3f::Zero(), 0).empty());
}

TEST(LocalShadowMath, FallbackFarIsFiniteAndDeterministic)
{
    EXPECT_FLOAT_EQ(
        VL::ComputeLocalShadowFar(0.0f),
        VL::kLocalShadowFallbackFarCm);
    EXPECT_FLOAT_EQ(VL::ComputeLocalShadowFar(-1.0f), 2000.0f);
    EXPECT_FLOAT_EQ(VL::ComputeLocalShadowFar(100.0f), 105.0f);
    EXPECT_FLOAT_EQ(VL::ComputeLocalShadowFar(0.5f), 2.0f);
}

TEST(LocalLightShadowSettings, ParsesDefaultsAndOverrides)
{
    const nlohmann::json none = nlohmann::json::object();
    const VL::LocalLightShadowSettings defaults =
        VL::ReadLocalLightShadowSettings(none, "pointLight|Key");
    EXPECT_FALSE(defaults.castShadows);
    EXPECT_FLOAT_EQ(defaults.bias, 2.0f);

    const nlohmann::json enabled = {
        { "shadow", { { "castShadows", true }, { "bias", 3.5f } } }
    };
    const VL::LocalLightShadowSettings parsed =
        VL::ReadLocalLightShadowSettings(enabled, "spotLight|Key");
    EXPECT_TRUE(parsed.castShadows);
    EXPECT_FLOAT_EQ(parsed.bias, 3.5f);
}

TEST(LocalLightShadowSettings, RejectsInvalidAuthorData)
{
    const nlohmann::json notObject = {{ "shadow", 7 }};
    EXPECT_THROW(
        VL::ReadLocalLightShadowSettings(notObject, "pointLight|Key"),
        std::invalid_argument);

    const nlohmann::json badFlag = {
        { "shadow", { { "castShadows", 1 } } }
    };
    EXPECT_THROW(
        VL::ReadLocalLightShadowSettings(badFlag, "pointLight|Key"),
        std::invalid_argument);

    const nlohmann::json negativeBias = {
        { "shadow", { { "bias", -0.1f } } }
    };
    EXPECT_THROW(
        VL::ReadLocalLightShadowSettings(negativeBias, "pointLight|Key"),
        std::invalid_argument);

    const nlohmann::json hugeBias = {
        { "shadow", { { "bias", 1001.0f } } }
    };
    EXPECT_THROW(
        VL::ReadLocalLightShadowSettings(hugeBias, "pointLight|Key"),
        std::invalid_argument);
}

TEST(LocalLightShadowSettings, SceneNodeDefaultsAndSetters)
{
    PointLight point;
    EXPECT_FALSE(point.GetCastShadows());
    EXPECT_FLOAT_EQ(point.GetShadowBias(), 2.0f);
    point.SetCastShadows(true);
    point.SetShadowBias(1.25f);
    EXPECT_TRUE(point.GetCastShadows());
    EXPECT_FLOAT_EQ(point.GetShadowBias(), 1.25f);

    SpotLight spot;
    EXPECT_FALSE(spot.GetCastShadows());
    spot.SetCastShadows(true);
    spot.SetShadowBias(0.0f);
    EXPECT_TRUE(spot.GetCastShadows());
    EXPECT_FLOAT_EQ(spot.GetShadowBias(), 0.0f);
}

TEST(LocalShadowGraph, CompilesLocalShadowProducersAndSamplingBarrier)
{
    const std::string configPath =
        std::string(VL_TEST_SOURCE_ROOT) + "/config/renderGraphConfig.json";
    std::ifstream file(configPath);
    ASSERT_TRUE(file.is_open()) << configPath;
    nlohmann::json config;
    file >> config;

    VL::RenderGraphCompiler compiler;
    const auto result = compiler.Compile(config);
    ASSERT_TRUE(result.IsSuccess()) << result.Error().code;
    const VL::CompiledRenderGraph& graph = result.Value();

    uint32_t localShadowWriters = 0;
    const VL::CompiledRenderGraphPass* firstLocalShadow = nullptr;
    const VL::CompiledRenderGraphPass* deferred = nullptr;
    for (const VL::CompiledRenderGraphPass& pass : graph.passes)
    {
        if (pass.type == VL::RenderGraphPassType::LocalShadow)
        {
            ++localShadowWriters;
            if (firstLocalShadow == nullptr)
            {
                firstLocalShadow = &pass;
            }
            ASSERT_FALSE(pass.outputResources.empty());
            EXPECT_EQ(pass.outputResources.front().resource, "localShadowMap");
        }
        if (pass.name == "deferredLighting")
        {
            deferred = &pass;
        }
    }
    EXPECT_EQ(localShadowWriters, 4u);
    ASSERT_NE(firstLocalShadow, nullptr);
    ASSERT_NE(deferred, nullptr);

    bool hasInitialTransition = false;
    for (const VL::CompiledRenderGraphBarrier& barrier :
         firstLocalShadow->barriersBeforePass)
    {
        if (barrier.resource == "localShadowMap" &&
            barrier.type ==
                VL::CompiledRenderGraphBarrierType::UndefinedToAttachment)
        {
            hasInitialTransition = true;
        }
    }
    EXPECT_TRUE(hasInitialTransition);

    bool hasTransition = false;
    for (const VL::CompiledRenderGraphBarrier& barrier :
         deferred->barriersBeforePass)
    {
        if (barrier.resource == "localShadowMap" &&
            barrier.type ==
                VL::CompiledRenderGraphBarrierType::AttachmentToShaderRead)
        {
            hasTransition = true;
        }
    }
    EXPECT_TRUE(hasTransition);
}
