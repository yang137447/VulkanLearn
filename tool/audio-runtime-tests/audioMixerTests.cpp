#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

#include "audio/audioMixer.h"

namespace
{
using namespace VL::Audio;

ListenerState DefaultListener()
{
    ListenerState listener;
    listener.position = Eigen::Vector3f::Zero();
    listener.forward = Eigen::Vector3f(0.0f, 0.0f, 1.0f);
    listener.up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    return listener;
}

std::vector<float> ConstantSamples(std::size_t count, float value)
{
    return std::vector<float>(count, value);
}

float MixPeak(const std::vector<float>& stereo)
{
    float peak = 0.0f;
    for (float sample : stereo)
        peak = std::max(peak, std::abs(sample));
    return peak;
}
} // namespace

TEST(AudioSpatialization, EqualPowerPanPlacesSourcesLeftCenterAndRight)
{
    const ListenerState listener = DefaultListener();

    const SpatialParams center =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 100.0f));
    EXPECT_NEAR(center.pan, 0.0f, 1.0e-5f);
    EXPECT_NEAR(center.leftGain, 0.70710678f, 1.0e-5f);
    EXPECT_NEAR(center.rightGain, 0.70710678f, 1.0e-5f);

    const SpatialParams right =
        ComputeSpatialParams(listener, Eigen::Vector3f(100.0f, 0.0f, 0.0f));
    EXPECT_NEAR(right.pan, 1.0f, 1.0e-5f);
    EXPECT_NEAR(right.leftGain, 0.0f, 1.0e-5f);
    EXPECT_NEAR(right.rightGain, 1.0f, 1.0e-5f);

    const SpatialParams left =
        ComputeSpatialParams(listener, Eigen::Vector3f(-100.0f, 0.0f, 0.0f));
    EXPECT_NEAR(left.pan, -1.0f, 1.0e-5f);
    EXPECT_NEAR(left.leftGain, 1.0f, 1.0e-5f);
    EXPECT_NEAR(left.rightGain, 0.0f, 1.0e-5f);
}

TEST(AudioSpatialization, PanFollowsListenerOrientation)
{
    ListenerState listener = DefaultListener();
    listener.forward = Eigen::Vector3f(1.0f, 0.0f, 0.0f);
    listener.up = Eigen::Vector3f(0.0f, 1.0f, 0.0f);

    // 前向转到 +X 后，+Z 位于听者左手边。
    const SpatialParams params =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 120.0f));
    EXPECT_NEAR(params.pan, -1.0f, 1.0e-5f);
    EXPECT_NEAR(params.leftGain, 1.0f, 1.0e-5f);
}

TEST(AudioSpatialization, DistanceGainFallsOffAndStaysPositive)
{
    const ListenerState listener = DefaultListener();
    const SpatialParams nearParams =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 0.0f));
    const SpatialParams midParams =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 200.0f));
    const SpatialParams farParams =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 4000.0f));

    EXPECT_NEAR(nearParams.gain, 1.0f, 1.0e-5f);
    EXPECT_NEAR(midParams.gain, 0.5f, 1.0e-5f);
    EXPECT_LT(farParams.gain, midParams.gain);
    EXPECT_GE(farParams.gain, 0.04f);
}

TEST(AudioSpatialization, DistanceReducesLowpassCutoffAndAlpha)
{
    const ListenerState listener = DefaultListener();
    const SpatialParams nearParams =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 0.0f));
    const SpatialParams midParams =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 1500.0f));
    const SpatialParams farParams =
        ComputeSpatialParams(listener, Eigen::Vector3f(0.0f, 0.0f, 6000.0f));

    EXPECT_GT(nearParams.lowpassCutoffHz, midParams.lowpassCutoffHz);
    EXPECT_GT(midParams.lowpassCutoffHz, farParams.lowpassCutoffHz);
    EXPECT_GT(nearParams.lowpassAlpha, midParams.lowpassAlpha);
    EXPECT_GT(midParams.lowpassAlpha, farParams.lowpassAlpha);
    EXPECT_GT(farParams.lowpassAlpha, 0.0f);
    EXPECT_LE(nearParams.lowpassAlpha, 1.0f);
}

TEST(AudioSpatialization, LowpassAlphaIsBoundedForExtremeCutoffs)
{
    EXPECT_GT(LowpassAlphaForCutoff(-100.0f), 0.0f);
    EXPECT_LE(LowpassAlphaForCutoff(100000.0f), 1.0f);
    EXPECT_FLOAT_EQ(LowpassAlphaForCutoff(100000.0f),
        LowpassAlphaForCutoff(0.45f * SampleRate));
}

TEST(AudioVoiceBudget, RejectsIncomingVoiceWhenAllSlotsHoldHigherPriority)
{
    VoiceMixer mixer(MaxVoices);
    for (int index = 0; index < MaxVoices; ++index)
    {
        const VoiceSubmitResult result = mixer.Submit(
            VoicePriority::Impact, Eigen::Vector3f::Zero(),
            ConstantSamples(256, 0.1f));
        EXPECT_TRUE(result.accepted);
    }
    ASSERT_EQ(mixer.ActiveVoiceCount(), MaxVoices);

    const VoiceSubmitResult rejected = mixer.Submit(
        VoicePriority::Landing, Eigen::Vector3f::Zero(),
        ConstantSamples(256, 0.1f));
    EXPECT_FALSE(rejected.accepted);
    EXPECT_EQ(rejected.stolenSlot, -1);
    EXPECT_EQ(mixer.ActiveVoiceCount(), MaxVoices);
}

TEST(AudioVoiceBudget, StealsFirstSlotWhenOnlyLowerPriorityVoicesExist)
{
    VoiceMixer mixer(MaxVoices);
    for (int index = 0; index < MaxVoices; ++index)
    {
        mixer.Submit(VoicePriority::Landing, Eigen::Vector3f::Zero(),
            ConstantSamples(256, 0.1f));
    }

    const VoiceSubmitResult result = mixer.Submit(
        VoicePriority::Leak, Eigen::Vector3f::Zero(),
        ConstantSamples(256, 0.1f));
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.stolenSlot, 0);
    EXPECT_EQ(mixer.GetVoiceStatus(0).priority, VoicePriority::Leak);
    EXPECT_EQ(mixer.ActiveVoiceCount(), MaxVoices);
}

TEST(AudioVoiceBudget, ImpactOutranksLeak)
{
    VoiceMixer mixer(MaxVoices);
    for (int index = 0; index < MaxVoices; ++index)
    {
        mixer.Submit(VoicePriority::Leak, Eigen::Vector3f::Zero(),
            ConstantSamples(256, 0.1f));
    }

    const VoiceSubmitResult result = mixer.Submit(
        VoicePriority::Impact, Eigen::Vector3f::Zero(),
        ConstantSamples(256, 0.1f));
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.stolenSlot, 0);
    EXPECT_EQ(mixer.GetVoiceStatus(0).priority, VoicePriority::Impact);
}

TEST(AudioVoiceBudget, LandingCannotStealLeak)
{
    VoiceMixer mixer(MaxVoices);
    for (int index = 0; index < MaxVoices; ++index)
    {
        mixer.Submit(VoicePriority::Leak, Eigen::Vector3f::Zero(),
            ConstantSamples(256, 0.1f));
    }

    const VoiceSubmitResult result = mixer.Submit(
        VoicePriority::Landing, Eigen::Vector3f::Zero(),
        ConstantSamples(256, 0.1f));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(mixer.ActiveVoiceCount(), MaxVoices);
}

TEST(AudioVoiceBudget, SamePriorityStealsFarthestVoice)
{
    VoiceMixer mixer(4);
    for (int index = 0; index < 4; ++index)
    {
        mixer.Submit(VoicePriority::Landing,
            Eigen::Vector3f(static_cast<float>(index * 100), 0.0f, 0.0f),
            ConstantSamples(256, 0.1f));
    }

    const VoiceSubmitResult result = mixer.Submit(
        VoicePriority::Landing, Eigen::Vector3f(10.0f, 0.0f, 0.0f),
        ConstantSamples(256, 0.1f));
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.stolenSlot, 3);
}

TEST(AudioVoiceBudget, SamePriorityTieUsesShortestRemainingVoice)
{
    VoiceMixer mixer(4);
    mixer.Submit(VoicePriority::Landing, Eigen::Vector3f(200.0f, 0.0f, 0.0f),
        ConstantSamples(2048, 0.1f));
    mixer.Submit(VoicePriority::Landing, Eigen::Vector3f(200.0f, 0.0f, 0.0f),
        ConstantSamples(128, 0.1f));
    mixer.Submit(VoicePriority::Landing, Eigen::Vector3f::Zero(),
        ConstantSamples(2048, 0.1f));
    mixer.Submit(VoicePriority::Landing, Eigen::Vector3f::Zero(),
        ConstantSamples(2048, 0.1f));

    const VoiceSubmitResult result = mixer.Submit(
        VoicePriority::Landing, Eigen::Vector3f(200.0f, 0.0f, 0.0f),
        ConstantSamples(2048, 0.1f));
    ASSERT_TRUE(result.accepted);
    EXPECT_EQ(result.stolenSlot, 1);
}

TEST(AudioMixing, BlockSumsSimultaneousVoices)
{
    const std::vector<float> samples = ConstantSamples(64, 0.25f);

    VoiceMixer single(2);
    single.SetListener(DefaultListener());
    single.Submit(VoicePriority::Impact, Eigen::Vector3f(0.0f, 0.0f, 100.0f),
        samples);
    std::vector<float> singleBlock(MixBlockFrames * MixChannels, 0.0f);
    single.MixBlock(singleBlock.data(), MixBlockFrames);

    VoiceMixer doubled(2);
    doubled.SetListener(DefaultListener());
    doubled.Submit(VoicePriority::Impact, Eigen::Vector3f(0.0f, 0.0f, 100.0f),
        samples);
    doubled.Submit(VoicePriority::Impact, Eigen::Vector3f(0.0f, 0.0f, 100.0f),
        samples);
    std::vector<float> doubledBlock(MixBlockFrames * MixChannels, 0.0f);
    doubled.MixBlock(doubledBlock.data(), MixBlockFrames);

    for (int frame = 0; frame < 64; ++frame)
    {
        EXPECT_NEAR(doubledBlock[frame * 2], singleBlock[frame * 2] * 2.0f, 1.0e-5f);
        EXPECT_NEAR(doubledBlock[frame * 2 + 1], singleBlock[frame * 2 + 1] * 2.0f, 1.0e-5f);
    }
    // 一个 voice 播完后，后续帧必须是静音，而不是继续复制最后一个样本。
    EXPECT_NEAR(doubledBlock[100 * 2], 0.0f, 1.0e-6f);
    EXPECT_EQ(doubled.ActiveVoiceCount(), 0);
}

TEST(AudioMixing, SaturatesFullVoicePoolWithoutOverflowOrNaN)
{
    VoiceMixer mixer(MaxVoices);
    mixer.SetListener(DefaultListener());
    for (int index = 0; index < MaxVoices; ++index)
    {
        mixer.Submit(VoicePriority::Impact, Eigen::Vector3f::Zero(),
            ConstantSamples(4096, (index % 2 == 0) ? 1.0f : -1.0f));
    }

    for (int block = 0; block < 8; ++block)
    {
        std::vector<float> mixed(MixBlockFrames * MixChannels, 0.0f);
        mixer.MixBlock(mixed.data(), MixBlockFrames);
        ASSERT_EQ(mixed.size(), static_cast<std::size_t>(MixBlockFrames * MixChannels));
        EXPECT_LE(MixPeak(mixed), 1.0f);
        for (float sample : mixed) EXPECT_TRUE(std::isfinite(sample));
    }
}

TEST(AudioMixing, SanitizesNonFiniteSourceSamples)
{
    VoiceMixer mixer(2);
    mixer.SetListener(DefaultListener());
    std::vector<float> samples = ConstantSamples(64, 0.25f);
    samples[0] = std::numeric_limits<float>::quiet_NaN();
    samples[1] = std::numeric_limits<float>::infinity();
    mixer.Submit(VoicePriority::Impact, Eigen::Vector3f::Zero(), samples);

    std::vector<float> mixed(MixBlockFrames * MixChannels, 0.0f);
    mixer.MixBlock(mixed.data(), MixBlockFrames);
    for (float sample : mixed)
    {
        EXPECT_TRUE(std::isfinite(sample));
        EXPECT_LE(std::abs(sample), 1.0f);
    }
}

TEST(AudioMixing, VoiceEndsAfterLastSample)
{
    VoiceMixer mixer(2);
    mixer.SetListener(DefaultListener());
    mixer.Submit(VoicePriority::Leak, Eigen::Vector3f::Zero(),
        ConstantSamples(32, 0.2f));
    ASSERT_EQ(mixer.ActiveVoiceCount(), 1);

    std::vector<float> mixed(MixBlockFrames * MixChannels, 0.0f);
    mixer.MixBlock(mixed.data(), MixBlockFrames);
    EXPECT_EQ(mixer.ActiveVoiceCount(), 0);
    EXPECT_EQ(mixer.GetVoiceStatus(0).remainingFrames, 0u);
    EXPECT_FALSE(mixer.GetVoiceStatus(0).active);
}
