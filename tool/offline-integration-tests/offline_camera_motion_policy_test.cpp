#include <gtest/gtest.h>

#include "render/offline/offlineCameraMotionPolicy.h"

namespace VL
{
namespace
{
using Clock = OfflineCameraMotionPolicy::Clock;

OfflineRenderSettings Settings()
{
    return {1280, 720, 32, 2, 1, 7};
}
}

TEST(OfflineCameraMotionPolicy, MotionUsesHalfResolutionOneSampleThenContinuesFullTarget)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    const auto full = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start);
    ASSERT_TRUE(full);
    EXPECT_EQ(full->samples, 32);
    EXPECT_EQ(full->width, 1280);
    EXPECT_EQ(full->height, 720);
    EXPECT_FALSE(full->interactivePreview);

    camera.position.x() = 1.0f;
    const auto moving = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10));
    ASSERT_TRUE(moving);
    EXPECT_EQ(moving->samples, 1);
    EXPECT_EQ(moving->width, 640);
    EXPECT_EQ(moving->height, 360);
    EXPECT_TRUE(moving->interactivePreview);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(20)));

    const auto settled = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(220));
    ASSERT_TRUE(settled);
    EXPECT_EQ(settled->samples, 32);
    EXPECT_EQ(settled->width, 1280);
    EXPECT_EQ(settled->height, 720);
    EXPECT_FALSE(settled->interactivePreview);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(230)));
}

TEST(OfflineCameraMotionPolicy, ChangingOnlyClipPlanesResubmitsCamera)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.clipNear = 0.5f;
    const auto near = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(1));
    ASSERT_TRUE(near);
    EXPECT_TRUE(near->interactivePreview);
    EXPECT_EQ(near->samples, 1);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(2)));
    camera.clipFar = 100.0f;
    const auto far = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(3));
    ASSERT_TRUE(far);
    EXPECT_TRUE(far->interactivePreview);
    EXPECT_EQ(far->samples, 1);
}

TEST(OfflineCameraMotionPolicy, PreviewFlagAloneChangesEffectiveTarget)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    requested.width = 1;
    requested.height = 1;
    requested.samples = 1;
    const auto full = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start);
    ASSERT_TRUE(full);
    EXPECT_FALSE(full->interactivePreview);

    camera.position.x() = 1.0f;
    const auto moving = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10));
    ASSERT_TRUE(moving);
    EXPECT_EQ(moving->width, full->width);
    EXPECT_EQ(moving->height, full->height);
    EXPECT_EQ(moving->samples, full->samples);
    EXPECT_TRUE(moving->interactivePreview);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(20)));

    const auto settled = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(210));
    ASSERT_TRUE(settled);
    EXPECT_FALSE(settled->interactivePreview);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(220)));
}

TEST(OfflineCameraMotionPolicy, FastCameraUpdatesWaitForCompleteOneSample)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(20)));
    camera.position.x() = 3.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(25)));
    const auto latest = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(30));
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->samples, 1);
    EXPECT_EQ(latest->width, 640);
    EXPECT_EQ(latest->height, 360);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(40)));
    camera.position.x() = 2.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(50)));
    EXPECT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(60)));
}

TEST(OfflineCameraMotionPolicy, OddAndSinglePixelExtentsRoundUpToAtLeastOne)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    requested.width = 1279;
    requested.height = 719;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    const auto odd = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10));
    ASSERT_TRUE(odd);
    EXPECT_EQ(odd->width, 640);
    EXPECT_EQ(odd->height, 360);
    EXPECT_EQ(odd->samples, 1);

    requested.width = 1;
    requested.height = 1;
    const auto smallest = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(20));
    ASSERT_TRUE(smallest);
    EXPECT_EQ(smallest->width, 1);
    EXPECT_EQ(smallest->height, 1);
    EXPECT_EQ(smallest->samples, 1);
    const auto settled = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(220));
    ASSERT_TRUE(settled);
    EXPECT_EQ(settled->width, 1);
    EXPECT_EQ(settled->height, 1);
    EXPECT_EQ(settled->samples, 32);
}

TEST(OfflineCameraMotionPolicy, AssetAndIntegratorChangesAreNotDeferred)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    auto changed = Settings();
    changed.seed = 9;
    const auto input = policy.Decide(camera, 1, nullptr, changed,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(20));
    ASSERT_TRUE(input);
    EXPECT_EQ(input->samples, 1);
    EXPECT_EQ(input->width, 640);
    EXPECT_EQ(input->height, 360);
    EXPECT_EQ(input->seed, 9u);
    OfflineWorldAssets asset;
    EXPECT_TRUE(policy.Decide(camera, 1, &asset, changed,
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(30)));
}

TEST(OfflineCameraMotionPolicy, WorldChangeAndResetStartFullTarget)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10))->samples, 1);
    const auto world = policy.Decide(camera, 2, nullptr, Settings(),
        Offline::SessionState::Idle, start + std::chrono::milliseconds(20));
    ASSERT_TRUE(world);
    EXPECT_EQ(world->samples, 32);
    EXPECT_EQ(world->width, 1280);
    EXPECT_EQ(world->height, 720);
    policy.Reset();
    const auto reset = policy.Decide(camera, 2, nullptr, Settings(),
        Offline::SessionState::Idle, start + std::chrono::milliseconds(30));
    ASSERT_TRUE(reset);
    EXPECT_EQ(reset->samples, 32);
    EXPECT_EQ(reset->width, 1280);
    EXPECT_EQ(reset->height, 720);
}

TEST(OfflineCameraMotionPolicy, SlowHalfPreviewDowngradesWhilePreparingAndCoalescesCamera)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10))->width, 640);
    camera.position.x() = 2.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(20)));
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(40)));
    const auto quarter = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(41));
    ASSERT_TRUE(quarter);
    EXPECT_EQ(quarter->width, 320);
    EXPECT_EQ(quarter->height, 180);
    EXPECT_EQ(quarter->samples, 1);
    camera.position.x() = 3.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(50)));
    const auto latest = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(61));
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->width, 320);
}

TEST(OfflineCameraMotionPolicy, CompletedSlowHalfPreviewDowngradesWithoutNewCamera)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    const auto quarter = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(41));
    ASSERT_TRUE(quarter);
    EXPECT_EQ(quarter->width, 320);
    EXPECT_EQ(quarter->height, 180);
    EXPECT_TRUE(quarter->interactivePreview);
}

TEST(OfflineCameraMotionPolicy, ExactThirtyMillisecondCompletionKeepsHalfPreview)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10))->width, 640);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(40)));
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(40)));
    camera.position.x() = 2.0f;
    const auto half = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(41));
    ASSERT_TRUE(half);
    EXPECT_EQ(half->width, 640);
    const auto quarter = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(72));
    ASSERT_TRUE(quarter);
    EXPECT_EQ(quarter->width, 320);
}

TEST(OfflineCameraMotionPolicy, ThreeFastQuarterCompletionsPromoteWithHysteresis)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(41))->width, 320);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(45)));
    // 同一张 Completed 重复读取不应把 1 次快预览当成 3 次。
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(46)));
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(47)));
    camera.position.x() = 2.0f;
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(48))->width, 320);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(52)));
    camera.position.x() = 3.0f;
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(53))->width, 320);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(56)));
    const auto half = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(57));
    ASSERT_TRUE(half);
    EXPECT_EQ(half->width, 640);
    EXPECT_EQ(half->height, 360);
}

TEST(OfflineCameraMotionPolicy, SettleDiscardsQuarterTimingAndRestoresFullTarget)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(41))->width, 320);
    const auto full = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(210));
    ASSERT_TRUE(full);
    EXPECT_EQ(full->width, 1280);
    EXPECT_EQ(full->height, 720);
    EXPECT_EQ(full->samples, 32);
    camera.position.x() = 2.0f;
    const auto half = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(220));
    ASSERT_TRUE(half);
    EXPECT_EQ(half->width, 640);
}

TEST(OfflineCameraMotionPolicy, InputChangesResetTimingAndOddQuarterRoundsUp)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    requested.width = 5;
    requested.height = 3;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    const auto half = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10));
    ASSERT_TRUE(half);
    EXPECT_EQ(half->width, 3);
    EXPECT_EQ(half->height, 2);
    const auto quarter = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(41));
    ASSERT_TRUE(quarter);
    EXPECT_EQ(quarter->width, 2);
    EXPECT_EQ(quarter->height, 1);

    OfflineWorldAssets asset;
    requested.seed = 8;
    requested.width = 1;
    requested.height = 1;
    const auto changed = policy.Decide(camera, 1, &asset, requested,
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(50));
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->width, 1);
    EXPECT_EQ(changed->height, 1);
    EXPECT_EQ(changed->samples, 1);
    EXPECT_EQ(changed->seed, 8u);
    EXPECT_FALSE(policy.Decide(camera, 1, &asset, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(60)));
    const auto world = policy.Decide(camera, 2, &asset, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(70));
    ASSERT_TRUE(world);
    EXPECT_EQ(world->samples, 32);
    policy.Reset();
    EXPECT_EQ(policy.Decide(camera, 2, &asset, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(80))->samples, 32);
}

TEST(OfflineCameraMotionPolicy, QuarterPreviewAtFastThresholdBreaksConsecutiveCompletions)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(41))->width, 320);
    for (int sample = 0; sample < 5; ++sample)
    {
        const int submittedAt = 41 + 10 * sample;
        const int completedAt = submittedAt + (sample == 1 ? 5 : 3);
        const auto completed = policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Completed, start + std::chrono::milliseconds(completedAt));
        if (sample == 4)
        {
            ASSERT_TRUE(completed);
            EXPECT_EQ(completed->width, 640);
        }
        else
        {
            EXPECT_FALSE(completed);
            camera.position.x() += 1.0f;
            const auto next = policy.Decide(camera, 1, nullptr, Settings(),
                Offline::SessionState::Completed,
                start + std::chrono::milliseconds(submittedAt + 10));
            ASSERT_TRUE(next);
            EXPECT_EQ(next->width, 320);
        }
    }
}

TEST(OfflineCameraMotionPolicy, RenderingQuarterWaitsForCompletionBeforeDownshift)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(41))->width, 320);
    camera.position.x() = 2.0f;
    for (int time = 50; time <= 80; time += 10)
        EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Rendering, start + std::chrono::milliseconds(time)));
    const auto latest = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(81));
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->width, 160);
}

TEST(OfflineCameraMotionPolicy, ResizeDiscardsOverBudgetTimingBeforeCompletion)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    requested.width = 801;
    requested.height = 601;
    camera.position.x() = 2.0f;
    const auto resized = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(100));
    ASSERT_TRUE(resized);
    EXPECT_EQ(resized->width, 401);
    EXPECT_EQ(resized->height, 301);
    EXPECT_EQ(resized->samples, 1);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(110)));
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(120)));
}

TEST(OfflineCameraMotionPolicy, AssetAndIntegratorChangesDiscardOldPreviewTiming)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    OfflineWorldAssets assets;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    const auto asset = policy.Decide(camera, 1, &assets, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(100));
    ASSERT_TRUE(asset);
    EXPECT_EQ(asset->width, 640);
    camera.position.x() = 3.0f;
    requested.bounces += 1;
    const auto integrator = policy.Decide(camera, 1, &assets, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(190));
    ASSERT_TRUE(integrator);
    EXPECT_EQ(integrator->width, 640);
    EXPECT_EQ(integrator->bounces, requested.bounces);
    EXPECT_FALSE(policy.Decide(camera, 1, &assets, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(210)));
}

TEST(OfflineCameraMotionPolicy, WorldAndResetDiscardSlowPreviewMeasurements)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    const auto world = policy.Decide(camera, 2, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(100));
    ASSERT_TRUE(world);
    EXPECT_EQ(world->width, 1280);
    camera.position.x() = 2.0f;
    const auto first = policy.Decide(camera, 2, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(110));
    ASSERT_TRUE(first);
    EXPECT_EQ(first->width, 640);
    policy.Reset();
    ASSERT_EQ(policy.Decide(camera, 2, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(200))->width, 1280);
    camera.position.x() = 3.0f;
    const auto afterReset = policy.Decide(camera, 2, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(210));
    ASSERT_TRUE(afterReset);
    EXPECT_EQ(afterReset->width, 640);
}

TEST(OfflineCameraMotionPolicy, PausedPreviewTimeDoesNotDowngradeAfterResume)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Paused, start + std::chrono::milliseconds(30)));
    camera.position.x() = 3.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Paused, start + std::chrono::milliseconds(150)));
    camera.position.x() = 4.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(250)));
    const auto resumed = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(260));
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->width, 640);
    EXPECT_EQ(resumed->height, 360);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(280)));
}

TEST(OfflineCameraMotionPolicy, PreviewSubmittedWhilePausedHasNoTimingMeasurement)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Paused, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(100)));
    const auto completed = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(110));
    ASSERT_TRUE(completed);
    EXPECT_EQ(completed->width, 640);
}

TEST(OfflineCameraMotionPolicy, SampleOnlyInputChangeDoesNotDeferLatestCamera)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    requested.samples = 64;
    const auto changed = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(20));
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->width, 640);
    EXPECT_EQ(changed->samples, 1);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(40)));
    const auto settled = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(220));
    ASSERT_TRUE(settled);
    EXPECT_EQ(settled->samples, 64);
}

TEST(OfflineCameraMotionPolicy, TinyResizeWithSamePreviewExtentDoesNotDeferLatestCamera)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    requested.width = 1;
    requested.height = 3;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    requested.width = 2;
    requested.height = 4;
    const auto resized = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(20));
    ASSERT_TRUE(resized);
    EXPECT_EQ(resized->width, 1);
    EXPECT_EQ(resized->height, 2);
    EXPECT_EQ(resized->samples, 1);
    camera.position.x() = 3.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(30)));
}

TEST(OfflineCameraMotionPolicy, UnchangedEffectiveTargetDoesNotStartNewMeasurement)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    requested.samples = 64;
    const auto notification = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(20));
    ASSERT_TRUE(notification);
    EXPECT_EQ(notification->samples, 1);
    EXPECT_TRUE(notification->interactivePreview);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(40)));
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(51)));
}

TEST(OfflineCameraMotionPolicy, PausedPreviewKeepsFlagWithoutRepeatedSubmission)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    const auto paused = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Paused, start + std::chrono::milliseconds(10));
    ASSERT_TRUE(paused);
    EXPECT_TRUE(paused->interactivePreview);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Paused, start + std::chrono::milliseconds(100)));
    camera.position.x() = 2.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(110)));
    const auto latest = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(120));
    ASSERT_TRUE(latest);
    EXPECT_TRUE(latest->interactivePreview);
    EXPECT_EQ(latest->width, 640);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(130)));
}

TEST(OfflineCameraMotionPolicy, RepeatedCompletedDoesNotPromoteOneQuarterPreview)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    ASSERT_EQ(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(41))->width, 320);
    for (int time = 45; time < 55; ++time)
        EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Completed, start + std::chrono::milliseconds(time)));
    camera.position.x() = 2.0f;
    const auto next = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(55));
    ASSERT_TRUE(next);
    EXPECT_EQ(next->width, 320);
}

TEST(OfflineCameraMotionPolicy, SlowPreviewsDescendOneTierThroughSixteenthWhilePreparingOrCompleted)
{
    const Offline::SessionState states[]{
        Offline::SessionState::Preparing,
        Offline::SessionState::Completed};
    for (const auto state : states)
    {
        SCOPED_TRACE(static_cast<int>(state));
        OfflineCameraMotionPolicy policy;
        CameraSnapshot camera;
        const auto start = Clock::time_point{};
        ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Idle, start));
        camera.position.x() = 1.0f;
        const auto half = policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Rendering, start + std::chrono::milliseconds(10));
        ASSERT_TRUE(half);
        EXPECT_EQ(half->width, 640);
        for (int divisor = 4, submittedAt = 10; divisor <= 16; divisor *= 2)
        {
            camera.position.x() += 1.0f;
            const auto next = policy.Decide(camera, 1, nullptr, Settings(),
                state, start + std::chrono::milliseconds(submittedAt + 31));
            ASSERT_TRUE(next);
            EXPECT_EQ(next->width, 1280 / divisor);
            EXPECT_EQ(next->height, 720 / divisor);
            EXPECT_EQ(next->samples, 1);
            submittedAt += 31;
            EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
                Offline::SessionState::Preparing, start + std::chrono::milliseconds(submittedAt + 1)));
        }
        camera.position.x() += 1.0f;
        const auto lowest = policy.Decide(camera, 1, nullptr, Settings(),
            state, start + std::chrono::milliseconds(134));
        if (state == Offline::SessionState::Completed)
        {
            ASSERT_TRUE(lowest);
            EXPECT_EQ(lowest->width, 80);
            EXPECT_EQ(lowest->height, 45);
        }
        else
            EXPECT_FALSE(lowest);
    }
}

TEST(OfflineCameraMotionPolicy, LowestUnfinishedPreviewCoalescesUntilCompleted)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    for (int divisor = 4, time = 41; divisor <= 16; divisor *= 2, time += 31)
    {
        camera.position.x() += 1.0f;
        const auto next = policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Preparing, start + std::chrono::milliseconds(time));
        ASSERT_TRUE(next);
        EXPECT_EQ(next->width, 1280 / divisor);
    }
    for (int time = 134; time <= 165; time += 31)
    {
        camera.position.x() += 1.0f;
        EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Preparing, start + std::chrono::milliseconds(time)));
        EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Rendering, start + std::chrono::milliseconds(time + 1)));
    }
    const auto latest = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(176));
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->width, 80);
    EXPECT_EQ(latest->height, 45);
    EXPECT_EQ(latest->samples, 1);
}

TEST(OfflineCameraMotionPolicy, FastLowPreviewsAscendOneTierPerThreeCompletions)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    for (int divisor = 4, time = 41; divisor <= 16; divisor *= 2, time += 31)
    {
        camera.position.x() += 1.0f;
        const auto next = policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Preparing, start + std::chrono::milliseconds(time));
        ASSERT_TRUE(next);
        EXPECT_EQ(next->width, 1280 / divisor);
    }
    int submittedAt = 103;
    for (int divisor = 16; divisor > 2; divisor /= 2)
    {
        for (int sample = 0; sample < 3; ++sample)
        {
            submittedAt += 3;
            const auto completed = policy.Decide(camera, 1, nullptr, Settings(),
                Offline::SessionState::Completed, start + std::chrono::milliseconds(submittedAt));
            if (sample == 2)
            {
                ASSERT_TRUE(completed);
                EXPECT_EQ(completed->width, 1280 / (divisor / 2));
                EXPECT_EQ(completed->height, 720 / (divisor / 2));
                EXPECT_EQ(completed->samples, 1);
            }
            else
            {
                EXPECT_FALSE(completed);
                EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
                    Offline::SessionState::Completed, start + std::chrono::milliseconds(submittedAt)));
                camera.position.x() += 1.0f;
                submittedAt += 1;
                const auto next = policy.Decide(camera, 1, nullptr, Settings(),
                    Offline::SessionState::Completed, start + std::chrono::milliseconds(submittedAt));
                ASSERT_TRUE(next);
                EXPECT_EQ(next->width, 1280 / divisor);
            }
        }
    }
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(submittedAt + 3)));
    camera.position.x() += 1.0f;
    const auto capped = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(submittedAt + 4));
    ASSERT_TRUE(capped);
    EXPECT_EQ(capped->width, 640);
}

TEST(OfflineCameraMotionPolicy, LowestTierSettlesAfterTwoHundredMilliseconds)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    for (int divisor = 4, time = 41; divisor <= 16; divisor *= 2, time += 31)
    {
        camera.position.x() += 1.0f;
        const auto next = policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Preparing, start + std::chrono::milliseconds(time));
        ASSERT_TRUE(next);
        EXPECT_EQ(next->width, 1280 / divisor);
    }
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(302)));
    const auto full = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(303));
    ASSERT_TRUE(full);
    EXPECT_EQ(full->width, 1280);
    EXPECT_EQ(full->height, 720);
    EXPECT_EQ(full->samples, 32);
    camera.position.x() += 1.0f;
    const auto nextMotion = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(313));
    ASSERT_TRUE(nextMotion);
    EXPECT_EQ(nextMotion->width, 640);
    EXPECT_EQ(nextMotion->samples, 1);
}

TEST(OfflineCameraMotionPolicy, EveryTierRoundsUpOddExtents)
{
    const int widths[]{80, 81};
    const int heights[]{45, 47};
    const int expectedWidths[][4]{{40, 20, 10, 5}, {41, 21, 11, 6}};
    const int expectedHeights[][4]{{23, 12, 6, 3}, {24, 12, 6, 3}};
    for (int extent = 0; extent < 2; ++extent)
    {
        SCOPED_TRACE(extent);
        OfflineCameraMotionPolicy policy;
        CameraSnapshot camera;
        const auto start = Clock::time_point{};
        auto requested = Settings();
        requested.width = widths[extent];
        requested.height = heights[extent];
        ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
            Offline::SessionState::Idle, start));
        for (int tier = 0; tier < 4; ++tier)
        {
            camera.position.x() += 1.0f;
            const auto next = policy.Decide(camera, 1, nullptr, requested,
                tier == 0 ? Offline::SessionState::Rendering : Offline::SessionState::Preparing,
                start + std::chrono::milliseconds(10 + 31 * tier));
            ASSERT_TRUE(next);
            EXPECT_EQ(next->width, expectedWidths[extent][tier]);
            EXPECT_EQ(next->height, expectedHeights[extent][tier]);
            EXPECT_EQ(next->samples, 1);
        }
    }
}

TEST(OfflineCameraMotionPolicy, SinglePixelPreviewsNeverResubmitUnchangedDimensions)
{
    for (int extent = 1; extent <= 2; ++extent)
    {
        SCOPED_TRACE(extent);
        OfflineCameraMotionPolicy policy;
        CameraSnapshot camera;
        const auto start = Clock::time_point{};
        auto requested = Settings();
        requested.width = extent;
        requested.height = extent;
        ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
            Offline::SessionState::Idle, start));
        camera.position.x() = 1.0f;
        const auto first = policy.Decide(camera, 1, nullptr, requested,
            Offline::SessionState::Rendering, start + std::chrono::milliseconds(10));
        ASSERT_TRUE(first);
        EXPECT_EQ(first->width, 1);
        EXPECT_EQ(first->height, 1);
        for (int time = 41; time <= 134; time += 31)
        {
            camera.position.x() += 1.0f;
            EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
                Offline::SessionState::Preparing, start + std::chrono::milliseconds(time)));
            EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
                Offline::SessionState::Rendering, start + std::chrono::milliseconds(time + 1)));
        }
        const auto latest = policy.Decide(camera, 1, nullptr, requested,
            Offline::SessionState::Completed, start + std::chrono::milliseconds(144));
        ASSERT_TRUE(latest);
        EXPECT_EQ(latest->width, 1);
        EXPECT_EQ(latest->height, 1);
        for (int time = 154; time < 164; ++time)
            EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
                Offline::SessionState::Completed, start + std::chrono::milliseconds(time)));
    }
}

TEST(OfflineCameraMotionPolicy, IdenticalSmallExtentsDoNotAdvanceHiddenTier)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    requested.width = 5;
    requested.height = 3;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    camera.position.x() = 2.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(41)));
    camera.position.x() = 3.0f;
    const auto smallest = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(72));
    ASSERT_TRUE(smallest);
    EXPECT_EQ(smallest->width, 1);
    EXPECT_EQ(smallest->height, 1);
    camera.position.x() = 4.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Preparing, start + std::chrono::milliseconds(103)));
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(110)));
    for (int sample = 0; sample < 3; ++sample)
    {
        const int completedAt = 113 + 4 * sample;
        const auto completed = policy.Decide(camera, 1, nullptr, requested,
            Offline::SessionState::Completed, start + std::chrono::milliseconds(completedAt));
        if (sample == 2)
        {
            ASSERT_TRUE(completed);
            EXPECT_EQ(completed->width, 2);
            EXPECT_EQ(completed->height, 1);
        }
        else
        {
            EXPECT_FALSE(completed);
            camera.position.x() += 1.0f;
            const auto next = policy.Decide(camera, 1, nullptr, requested,
                Offline::SessionState::Completed, start + std::chrono::milliseconds(completedAt + 1));
            ASSERT_TRUE(next);
            EXPECT_EQ(next->width, 1);
        }
    }
}

TEST(OfflineCameraMotionPolicy, LowestTierInputChangeImmediatelyResetsTiming)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    auto requested = Settings();
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    for (int time = 41; time <= 103; time += 31)
    {
        camera.position.x() += 1.0f;
        ASSERT_TRUE(policy.Decide(camera, 1, nullptr, requested,
            Offline::SessionState::Preparing, start + std::chrono::milliseconds(time)));
    }
    camera.position.x() += 1.0f;
    requested.seed = 8;
    const auto changed = policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(134));
    ASSERT_TRUE(changed);
    EXPECT_EQ(changed->width, 640);
    EXPECT_EQ(changed->seed, 8u);
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, requested,
        Offline::SessionState::Completed, start + std::chrono::milliseconds(144)));
}

TEST(OfflineCameraMotionPolicy, PauseDiscardsLowTierTimingAndFastStreak)
{
    OfflineCameraMotionPolicy policy;
    CameraSnapshot camera;
    const auto start = Clock::time_point{};
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Idle, start));
    camera.position.x() = 1.0f;
    ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(10)));
    for (int time = 41; time <= 103; time += 31)
    {
        camera.position.x() += 1.0f;
        ASSERT_TRUE(policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Preparing, start + std::chrono::milliseconds(time)));
    }
    for (int sample = 0; sample < 2; ++sample)
    {
        const int completedAt = 106 + 4 * sample;
        EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Completed, start + std::chrono::milliseconds(completedAt)));
        camera.position.x() += 1.0f;
        const auto next = policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Completed, start + std::chrono::milliseconds(completedAt + 1));
        ASSERT_TRUE(next);
        EXPECT_EQ(next->width, 80);
    }
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Paused, start + std::chrono::milliseconds(115)));
    camera.position.x() += 1.0f;
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Paused, start + std::chrono::milliseconds(200)));
    EXPECT_FALSE(policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Rendering, start + std::chrono::milliseconds(210)));
    const auto resumed = policy.Decide(camera, 1, nullptr, Settings(),
        Offline::SessionState::Completed, start + std::chrono::milliseconds(220));
    ASSERT_TRUE(resumed);
    EXPECT_EQ(resumed->width, 80);
    for (int sample = 0; sample < 3; ++sample)
    {
        const int completedAt = 223 + 4 * sample;
        const auto completed = policy.Decide(camera, 1, nullptr, Settings(),
            Offline::SessionState::Completed, start + std::chrono::milliseconds(completedAt));
        if (sample == 2)
        {
            ASSERT_TRUE(completed);
            EXPECT_EQ(completed->width, 160);
        }
        else
        {
            EXPECT_FALSE(completed);
            camera.position.x() += 1.0f;
            const auto next = policy.Decide(camera, 1, nullptr, Settings(),
                Offline::SessionState::Completed, start + std::chrono::milliseconds(completedAt + 1));
            ASSERT_TRUE(next);
            EXPECT_EQ(next->width, 80);
        }
    }
}
} // namespace VL
