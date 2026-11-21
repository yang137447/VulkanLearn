#include <gtest/gtest.h>

#include "engine/frame/terrainLoadingGate.h"
#include "ui/gameplayHudView.h"

namespace VL
{
namespace
{
uint64_t queriedGeneration = 0;
Eigen::Matrix4f queriedCamera = Eigen::Matrix4f::Zero();
bool providerReady = false;
unsigned queryCount = 0;

bool ReadQuality(uint64_t generation, const Eigen::Matrix4f& camera)
{
    queriedGeneration = generation;
    queriedCamera = camera;
    ++queryCount;
    return providerReady;
}

class TerrainLoadingGateTest : public ::testing::Test
{
    void SetUp() override
    {
        queriedGeneration = 0;
        queriedCamera.setZero();
        providerReady = false;
        queryCount = 0;
    }
};

TEST_F(TerrainLoadingGateTest, NonTerrainAndOfflineNeverQueryGpuReadiness)
{
    const Eigen::Matrix4f camera = Eigen::Matrix4f::Identity();
    EXPECT_FALSE(TerrainLoadingRequired(false, false, 23, camera, ReadQuality));
    EXPECT_FALSE(TerrainLoadingRequired(true, true, 23, camera, ReadQuality));
    EXPECT_EQ(queryCount, 0u);
}

TEST_F(TerrainLoadingGateTest, UnreadyTerrainForwardsActualWorldAndCamera)
{
    Eigen::Matrix4f camera = Eigen::Matrix4f::Identity();
    camera(0, 3) = 17.0f;
    EXPECT_TRUE(TerrainLoadingRequired(false, true, 23, camera, ReadQuality));
    EXPECT_EQ(queriedGeneration, 23u);
    EXPECT_TRUE(queriedCamera.isApprox(camera, 0.0f));
    EXPECT_EQ(queryCount, 1u);
}

TEST_F(TerrainLoadingGateTest, ContinuousMovementDoesNotLocallyRevokeProviderReadiness)
{
    providerReady = true;
    Eigen::Matrix4f camera = Eigen::Matrix4f::Identity();
    EXPECT_FALSE(TerrainLoadingRequired(false, true, 23, camera, ReadQuality));
    camera(0, 3) = 0.01f;
    EXPECT_FALSE(TerrainLoadingRequired(false, true, 23, camera, ReadQuality));
    EXPECT_TRUE(queriedCamera.isApprox(camera, 0.0f));
    providerReady = false;
    EXPECT_TRUE(TerrainLoadingRequired(false, true, 23, camera, ReadQuality));
}

TEST_F(TerrainLoadingGateTest, WorldReplacementCannotReuseLocalReadyLatch)
{
    providerReady = true;
    const Eigen::Matrix4f camera = Eigen::Matrix4f::Identity();
    EXPECT_FALSE(TerrainLoadingRequired(false, true, 23, camera, ReadQuality));
    providerReady = false;
    EXPECT_TRUE(TerrainLoadingRequired(false, true, 24, camera, ReadQuality));
    EXPECT_EQ(queriedGeneration, 24u);
}

TEST_F(TerrainLoadingGateTest, LoadingLabelWorksWithoutScriptHostOrPlay)
{
    ScriptRuntimeView frozen;
    frozen.worldGeneration = 23;
    ApplyTerrainLoadingHud(frozen, true);
    EXPECT_EQ(BuildGameplayHudText(frozen), "Loading terrain...");
    EXPECT_EQ(frozen.worldGeneration, 23u);
}

TEST_F(TerrainLoadingGateTest, LoadingLabelReplacesGameplayAndDoesNotMutateSource)
{
    ScriptRuntimeView source;
    source.available = true;
    source.state = ScriptPlayState::Playing;
    source.hudLines = {{"actor", "hud", "text", "Gameplay"}};
    auto frozen = source;
    ApplyTerrainLoadingHud(frozen, true);
    EXPECT_EQ(BuildGameplayHudText(frozen), "Loading terrain...");
    EXPECT_EQ(BuildGameplayHudText(source), "Gameplay");
    frozen = source;
    ApplyTerrainLoadingHud(frozen, false);
    EXPECT_EQ(BuildGameplayHudText(frozen), "Gameplay");
}
}
}
