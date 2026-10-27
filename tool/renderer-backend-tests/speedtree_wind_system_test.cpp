#include <gtest/gtest.h>

#include "render/foliage/speedTreeWindSystem.h"

namespace
{
using namespace VL;

WindResponse MakeWindResponse(float strengthResponse, float directionResponse)
{
    WindResponse config;
    config.common.strengthResponse = strengthResponse;
    config.common.directionResponse = directionResponse;
    return config;
}

WorldWind MakeTarget(float strength, const Eigen::Vector3f& direction)
{
    WorldWind wind;
    wind.authored = true;
    wind.direction = direction.normalized();
    wind.strength = strength;
    wind.gust.enabled = false;
    return wind;
}

void Configure(SpeedTreeWindSystem& system, const WindResponse& config)
{
    system.Configure(config, Eigen::Vector3f(-100.0f, -100.0f, 0.0f),
        Eigen::Vector3f(100.0f, 100.0f, 300.0f));
}
} // namespace

TEST(SpeedTreeWindSystem, ProfileStrengthResponseControlsSceneTargetEasing)
{
    SpeedTreeWindSystem slow;
    SpeedTreeWindSystem fast;
    Configure(slow, MakeWindResponse(1.0f, 1.0f));
    Configure(fast, MakeWindResponse(0.1f, 1.0f));

    const WorldWind full = MakeTarget(3270.0f, Eigen::Vector3f::UnitX());
    slow.AdvanceToSceneWind(0.0, full);
    fast.AdvanceToSceneWind(0.0, full);
    EXPECT_NEAR(slow.GetGpuState().windVector.w(), 1.0f, 1.0e-5f);
    EXPECT_NEAR(fast.GetGpuState().windVector.w(), 1.0f, 1.0e-5f);

    const WorldWind still = MakeTarget(0.0f, Eigen::Vector3f::UnitX());
    slow.AdvanceToSceneWind(0.5, still);
    fast.AdvanceToSceneWind(0.5, still);
    // 同一个场景目标，不同 profile 的 strengthResponse 产生不同强度的中间态。
    EXPECT_GT(slow.GetGpuState().windVector.w(), fast.GetGpuState().windVector.w());
    EXPECT_GT(slow.GetGpuState().windVector.w(), 0.0f);
    EXPECT_LT(fast.GetGpuState().windVector.w(), 0.005f);

    slow.AdvanceToSceneWind(2.0, still);
    fast.AdvanceToSceneWind(2.0, still);
    // LinearSigmoid 在两端保留极小尾巴，这是 SDK 公式本身的行为。
    EXPECT_LT(slow.GetGpuState().windVector.w(), 0.005f);
    EXPECT_LT(fast.GetGpuState().windVector.w(), 0.005f);
}

TEST(SpeedTreeWindSystem, AuthoredStrengthResponseSettlesAtAuthoredRate)
{
    SpeedTreeWindSystem system;
    Configure(system, MakeWindResponse(0.5f, 0.5f));
    system.AdvanceToSceneWind(0.0, MakeTarget(3270.0f, Eigen::Vector3f::UnitX()));

    // 60 Hz 逐帧推进，而不是一次跳到 0.5 s：逐帧 SetStrength 若以 currentStrength
    // 为 guard 会反复重起缓动，1 s 后仍几乎停在 1.0。
    const WorldWind still = MakeTarget(0.0f, Eigen::Vector3f::UnitX());
    for (int frame = 1; frame <= 60; ++frame)
        system.AdvanceToSceneWind(frame / 60.0, still);

    EXPECT_LT(system.GetGpuState().windVector.w(), 0.05f);
}

TEST(SpeedTreeWindSystem, ProfileDirectionResponseInterpolatesSceneTarget)
{
    SpeedTreeWindSystem system;
    Configure(system, MakeWindResponse(0.0f, 1.0f));
    system.AdvanceToSceneWind(0.0, MakeTarget(3270.0f, Eigen::Vector3f::UnitX()));

    system.AdvanceToSceneWind(0.5, MakeTarget(3270.0f, Eigen::Vector3f::UnitZ()));
    const Eigen::Vector3f midway = system.GetGpuState().windVector.head<3>();
    EXPECT_GT(midway.x(), 0.0f);
    EXPECT_GT(midway.z(), 0.0f);

    system.AdvanceToSceneWind(2.0, MakeTarget(3270.0f, Eigen::Vector3f::UnitZ()));
    const Eigen::Vector3f settled = system.GetGpuState().windVector.head<3>();
    EXPECT_NEAR(settled.x(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(settled.z(), 1.0f, 1.0e-5f);
}

TEST(SpeedTreeWindSystem, SceneGustAmplitudeScalesProfileGustWithoutChangingBaseStrength)
{
    WindResponse config = MakeWindResponse(0.5f, 0.0f);
    config.common.gustFrequency = 100.0f;
    config.common.gustStrengthMin = 0.4f;
    config.common.gustStrengthMax = 0.5f;
    config.common.gustDurationMin = 1.0f;
    config.common.gustDurationMax = 1.0f;
    config.common.gustRiseScalar = 0.5f;
    config.common.gustFallScalar = 0.5f;

    SpeedTreeWindSystem noGust;
    SpeedTreeWindSystem fullGust;
    Configure(noGust, config);
    Configure(fullGust, config);

    WorldWind target = MakeTarget(1635.0f, Eigen::Vector3f::UnitX()); // curve axis 0.5
    target.gust.enabled = true;
    WorldWind noGustTarget = target;
    noGustTarget.gust.amplitude = 0.0f;
    WorldWind fullGustTarget = target;
    fullGustTarget.gust.amplitude = 1.0f;
    noGust.AdvanceToSceneWind(0.0, noGustTarget);
    fullGust.AdvanceToSceneWind(0.0, fullGustTarget);

    // 让 profile 的离散 gust 事件启动并完成 rise；场景 amplitude 只缩放它的输出。
    noGust.AdvanceToSceneWind(0.1, noGustTarget);
    fullGust.AdvanceToSceneWind(0.1, fullGustTarget);
    noGust.AdvanceToSceneWind(0.5, noGustTarget);
    fullGust.AdvanceToSceneWind(0.5, fullGustTarget);

    EXPECT_NEAR(noGust.GetGpuState().windVector.w(), 0.5f, 1.0e-5f);
    EXPECT_GT(fullGust.GetGpuState().windVector.w(), 0.5f);
}
