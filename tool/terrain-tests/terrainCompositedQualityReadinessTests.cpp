// 到达收敛与逐视角质量证据分离：转身/走动不能让加载黑帧和输入闸门重新生效。
#include "render/frame/terrainCompositedQualityValidation.h"

#include <Eigen/Geometry>
#include <gtest/gtest.h>

namespace
{
VL::TerrainQualityAbi::Counts PassingCounts()
{
    VL::TerrainQualityAbi::Counts counts;
    counts.visibleSamples = counts.validAtTargetPlusBias = 100;
    counts.complete = true;
    return counts;
}

class QualityReadiness : public testing::Test
{
protected:
    VL::TerrainQualityValidation state;
    Eigen::Matrix4f camera = Eigen::Matrix4f::Identity();
    VL::TerrainQualityAbi::Surface surface{};
    VL::TerrainQualityValidationStamp stamp;

    void SetUp() override
    {
        surface.width = surface.height = 8192;
        surface.maxMipAndRepeat = 13;
        surface.generationLow = 7;
        surface.revisionLow = 3;
        state.Begin(7, 9, camera, {}, 100);
        stamp = state.Resources(&surface, 1, 110);
    }

    void Accept()
    {
        ASSERT_TRUE(state.Observe(stamp, camera, 20, 20, 200, PassingCounts(), true));
    }
};

TEST_F(QualityReadiness, ArrivalNeedsOnePassingCapture)
{
    EXPECT_FALSE(state.ArrivalConverged(7, {}));
    Accept();
    EXPECT_TRUE(state.ArrivalConverged(7, {}));
    // 其它代不属于本次到达。
    EXPECT_FALSE(state.ArrivalConverged(8, {}));
}

TEST_F(QualityReadiness, RotationKeepsArrivalConvergedButRevokesViewReady)
{
    Accept();
    Eigen::Matrix4f turned = Eigen::Matrix4f::Identity();
    turned.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(1.0f, Eigen::Vector3f::UnitY()).toRotationMatrix();
    state.Begin(7, 9, turned, {}, 300);
    EXPECT_FALSE(state.Ready(7, turned, {}));
    EXPECT_TRUE(state.ArrivalConverged(7, {}));
}

TEST_F(QualityReadiness, ExplicitArrivalRevokesUntilNewCapture)
{
    Accept();
    const VL::TerrainQualityInvalidation teleport{7, 1, 250};
    const auto arrived = state.Begin(7, 9, camera, teleport, 900);
    EXPECT_FALSE(state.ArrivalConverged(7, teleport));
    EXPECT_TRUE(state.Observe(arrived, camera, 21, 21, 901, PassingCounts(), true));
    EXPECT_TRUE(state.ArrivalConverged(7, teleport));
}

TEST_F(QualityReadiness, WorldReplacementRevokesArrivalConvergence)
{
    Accept();
    state.Begin(8, 9, camera, {}, 300);
    EXPECT_FALSE(state.ArrivalConverged(8, {}));
    EXPECT_FALSE(state.ArrivalConverged(7, {}));
}

TEST_F(QualityReadiness, FailedCaptureDoesNotMarkArrivalConverged)
{
    auto failed = PassingCounts();
    failed.unresolved = 1;
    EXPECT_FALSE(state.Observe(stamp, camera, 20, 20, 200, failed, true));
    EXPECT_FALSE(state.ArrivalConverged(7, {}));
}
}
