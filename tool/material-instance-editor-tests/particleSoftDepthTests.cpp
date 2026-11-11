#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "particles/particleSoftDepthCpu.h"

namespace
{

using Json = nlohmann::json;
using namespace VL;

} // namespace

TEST(ParticleSoftDepthCpu, LinearizesDeviceDepthWithInverseProjection)
{
    Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();
    EXPECT_NEAR(
        ParticleViewDepthFromDeviceDepthCpu(0.25f, identity),
        -0.25f,
        1.0e-6f);
    EXPECT_NEAR(
        ParticleViewDepthFromDeviceDepthCpu(1.0f, identity),
        -1.0f,
        1.0e-6f);
}

TEST(ParticleSoftDepthCpu, ThicknessCapsFadeWithoutChangingDefault)
{
    EXPECT_NEAR(ParticleSoftFadeCpu(12.5f, 25.0f, 0.0f), 0.5f, 1.0e-5f);
    EXPECT_NEAR(ParticleSoftFadeCpu(25.0f, 25.0f, 0.0f), 1.0f, 1.0e-5f);
    // thickness=50 时，深度差 12.5 的 fade 被限制到 0.25。
    EXPECT_NEAR(ParticleSoftFadeCpu(12.5f, 25.0f, 50.0f), 0.25f, 1.0e-5f);
    EXPECT_NEAR(ParticleSoftFadeCpu(50.0f, 25.0f, 50.0f), 1.0f, 1.0e-5f);
}
