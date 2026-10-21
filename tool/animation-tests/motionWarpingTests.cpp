#include "animation/motionWarping.h"
#include "animation/animationAssets.h"

#include <gtest/gtest.h>
#include <ozz/animation/offline/animation_builder.h>
#include <cmath>

namespace
{
using namespace VL::Animation;

constexpr float Pi = 3.14159265358979323846f;

float Degrees(float radians)
{
    return radians * 180.0f / Pi;
}

// 把弧度差折回 (-180,180]；180° 目标在浮点上可能落到 ±π 的任一侧，朝向等价。
float WrappedDegrees(float radians)
{
    return Degrees(std::remainder(radians, 2.0f * Pi));
}

Eigen::Quaternionf YawQuaternion(float degrees)
{
    return Eigen::Quaternionf(Eigen::AngleAxisf(degrees * Pi / 180.0f, Eigen::Vector3f::UnitY()));
}

Clip WarpClip(float distance = 100, float duration = 2)
{
    Clip clip;
    clip.id = "warp";
    clip.raw.duration = duration;
    clip.raw.tracks.resize(1);
    auto& root = clip.raw.tracks[0];
    root.translations = {{0, {0, 0, 0}}, {duration, {0, 0, -distance}}};
    root.rotations = {{0, ozz::math::Quaternion::identity()}};
    root.scales = {{0, ozz::math::Float3::one()}};
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    return clip;
}

// 根 yaw 从 0 线性转到 yawDegrees；平移可按 distance 叠加，用于旋转/平移独立测试。
// 旋转键按 60 Hz 密采样：ozz 的轨道插值是 NLerp（raw_animation_utils.cc 的 LerpRotation），
// 两端稀疏键会让 1/60 s 的实际转角落到 NLerp 曲线上而不是线性角度；密键让测试采样点
// 正好落在键上，手算值可复现。
Clip WarpClipWithYaw(float yawDegrees, float duration = 2, float distance = 0)
{
    Clip clip = WarpClip(distance, duration);
    constexpr float sampleRate = 60.0f;
    const int steps = static_cast<int>(std::lround(duration * sampleRate));
    auto& rotations = clip.raw.tracks[0].rotations;
    rotations.clear();
    for (int step = 0; step <= steps; ++step)
    {
        const float fraction = static_cast<float>(step) / static_cast<float>(steps);
        rotations.push_back({duration * fraction,
            ozz::math::Quaternion::FromAxisAngle(ozz::math::Float3(0, 1, 0),
                yawDegrees * Pi / 180.0f * fraction)});
    }
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    return clip;
}

// Hurdle 的 WarpPointAnimProvider=Bone 夹具：root 是关节 0，attach 是它的恒等子骨，
// 带一个常量局部平移；两个关节都有轨道，与导入器“tracks 补齐到 joints.size()”一致。
SkeletonMesh WarpMesh(const Eigen::Vector3f& attachLocal = Eigen::Vector3f(0, 100, 0))
{
    SkeletonMesh mesh;
    Joint root;
    root.name = "root";
    root.parent = -1;
    Joint attach;
    attach.name = "attach";
    attach.parent = 0;
    attach.restLocal = Eigen::Matrix4f::Identity();
    attach.restLocal.block<3, 1>(0, 3) = attachLocal;
    mesh.joints = {root, attach};
    return mesh;
}

Clip WarpClipWithAttach(float distance, float duration, const Eigen::Vector3f& attachLocal)
{
    Clip clip = WarpClip(distance, duration);
    clip.raw.tracks.resize(2);
    auto& attach = clip.raw.tracks[1];
    attach.translations = {{0, {attachLocal.x(), attachLocal.y(), attachLocal.z()}},
        {duration, {attachLocal.x(), attachLocal.y(), attachLocal.z()}}};
    attach.rotations = {{0, ozz::math::Quaternion::identity()}};
    attach.scales = {{0, ozz::math::Float3::one()}};
    clip.runtime = ozz::animation::offline::AnimationBuilder()(clip.raw);
    return clip;
}

MotionWarpWindow FrontWindow(float start, float end, float targetZ, bool subtractRemaining = false)
{
    MotionWarpWindow window;
    window.targetName = "FrontLedge";
    window.targetLocation = Eigen::Vector3f(0, 0, targetZ);
    window.startTime = start;
    window.endTime = end;
    window.subtractRemainingRootMotion = subtractRemaining;
    return window;
}

// 旋转窗口：默认 Slerp + 1.0 时间缩放，目标世界 yaw = targetYawDegrees。
MotionWarpWindow RotationWindow(float start, float end, float targetYawDegrees,
    float timeMultiplier = 1.0f)
{
    MotionWarpWindow window = FrontWindow(start, end, 0);
    window.warpRotation = true;
    window.rotationTimeMultiplier = timeMultiplier;
    window.hasTargetRotation = true;
    window.targetRotation = YawQuaternion(targetYawDegrees);
    return window;
}

Eigen::Vector3f ApplyWindow(const MotionWarpRuntime& runtime, const Clip& clip,
    float previousPosition, float currentPosition, const Eigen::Vector3f& delta)
{
    return runtime.Apply(SkeletonMesh{}, clip, previousPosition, currentPosition, delta,
        Eigen::Matrix4f::Identity(), Eigen::Vector3f::Zero());
}

Eigen::Vector3f ApplyWindow(const MotionWarpRuntime& runtime, const SkeletonMesh& mesh,
    const Clip& clip, float previousPosition, float currentPosition, const Eigen::Vector3f& delta)
{
    return runtime.Apply(mesh, clip, previousPosition, currentPosition, delta,
        Eigen::Matrix4f::Identity(), Eigen::Vector3f::Zero());
}

// 旋转测试入口：yawDelta 由调用方先放入原始根 yaw，Apply 只在有旋转窗口时覆盖。
Eigen::Vector3f ApplyWindow(const MotionWarpRuntime& runtime, const Clip& clip,
    float previousPosition, float currentPosition, const Eigen::Vector3f& delta,
    float actorYaw, float& yawDelta)
{
    return runtime.Apply(SkeletonMesh{}, clip, previousPosition, currentPosition, delta,
        Eigen::Matrix4f::Identity(), Eigen::Vector3f::Zero(), actorYaw, &yawDelta);
}
}

TEST(MotionWarping, IdentityTargetKeepsDelta)
{
    const Eigen::Vector3f delta(10, 0, 0);
    const Eigen::Vector3f total(100, 0, 0);
    const auto warped = WarpTranslationToTarget(Eigen::Vector3f::Zero(),
        Eigen::Quaternionf::Identity(), delta, total, Eigen::Vector3f(100, 0, 0));
    EXPECT_TRUE(warped.isApprox(delta, 1e-4f));
}

TEST(MotionWarping, DoubleTargetScalesAlongRoot)
{
    const Eigen::Vector3f delta(10, 0, 0);
    const Eigen::Vector3f total(100, 0, 0);
    const auto warped = WarpTranslationToTarget(Eigen::Vector3f::Zero(),
        Eigen::Quaternionf::Identity(), delta, total, Eigen::Vector3f(200, 0, 0));
    EXPECT_TRUE(warped.isApprox(Eigen::Vector3f(20, 0, 0), 1e-4f));
}

TEST(MotionWarping, PerpendicularDeltaIsPreserved)
{
    const Eigen::Vector3f delta(10, 5, 0);
    const Eigen::Vector3f total(100, 0, 0);
    const auto warped = WarpTranslationToTarget(Eigen::Vector3f::Zero(),
        Eigen::Quaternionf::Identity(), delta, total, Eigen::Vector3f(200, 0, 0));
    EXPECT_TRUE(warped.isApprox(Eigen::Vector3f(20, 5, 0), 1e-4f));
}

TEST(MotionWarping, LateralTargetAddsShear)
{
    const Eigen::Vector3f delta(10, 0, 0);
    const Eigen::Vector3f total(100, 0, 0);
    const auto warped = WarpTranslationToTarget(Eigen::Vector3f::Zero(),
        Eigen::Quaternionf::Identity(), delta, total, Eigen::Vector3f(100, 10, 0));
    EXPECT_TRUE(warped.isApprox(Eigen::Vector3f(10, 1, 0), 1e-4f));
}

TEST(MotionWarping, ZeroDeltaOrTotalReturnsZero)
{
    EXPECT_TRUE(WarpTranslationToTarget(Eigen::Vector3f::Zero(), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f::Zero(), Eigen::Vector3f(100, 0, 0), Eigen::Vector3f(100, 0, 0)).isZero());
    EXPECT_TRUE(WarpTranslationToTarget(Eigen::Vector3f::Zero(), Eigen::Quaternionf::Identity(),
        Eigen::Vector3f(10, 0, 0), Eigen::Vector3f::Zero(), Eigen::Vector3f(100, 0, 0)).isZero());
}

TEST(MotionWarping, WindowGatesOnPreviousPosition)
{
    const auto clip = WarpClip();
    MotionWarpRuntime runtime;
    runtime.SetWindows({FrontWindow(0.5f, 1.0f, -100)});

    // 窗口开始前：保持片段原根位移。
    EXPECT_TRUE(ApplyWindow(runtime, clip, 0.25f, 0.5f, Eigen::Vector3f(0, 0, -12.5f))
        .isApprox(Eigen::Vector3f(0, 0, -12.5f), 1e-4f));
    // 窗口内：目标为窗口内总位移的两倍，delta 被放大。
    EXPECT_TRUE(ApplyWindow(runtime, clip, 0.5f, 0.75f, Eigen::Vector3f(0, 0, -12.5f))
        .isApprox(Eigen::Vector3f(0, 0, -50), 1e-3f));
    // PreviousPosition 已到达 EndTime：UE 标记窗口结束，delta 原样返回。
    EXPECT_TRUE(ApplyWindow(runtime, clip, 1.0f, 1.25f, Eigen::Vector3f(0, 0, -12.5f))
        .isApprox(Eigen::Vector3f(0, 0, -12.5f), 1e-4f));
}

TEST(MotionWarping, WindowTotalComesFromPreviousPositionToEndTime)
{
    const auto clip = WarpClip(100, 2);
    MotionWarpRuntime runtime;
    runtime.SetWindows({FrontWindow(0, 1, -200)});

    // Total = Root(1)-Root(0) = -50，而不是整段 -100；target -200 因此 scale=4。
    EXPECT_TRUE(ApplyWindow(runtime, clip, 0, 0.5f, Eigen::Vector3f(0, 0, -25))
        .isApprox(Eigen::Vector3f(0, 0, -100), 1e-3f));
}

TEST(MotionWarping, SubtractRemainingRootMotionSetsTargetBackToClipEnd)
{
    const auto clip = WarpClip(100, 2);
    MotionWarpRuntime keepTarget;
    keepTarget.SetWindows({FrontWindow(0, 1, -100)});
    MotionWarpRuntime subtractTarget;
    subtractTarget.SetWindows({FrontWindow(0, 1, -100, true)});

    const Eigen::Vector3f delta(0, 0, -25);
    EXPECT_TRUE(ApplyWindow(keepTarget, clip, 0, 0.5f, delta)
        .isApprox(Eigen::Vector3f(0, 0, -50), 1e-3f));
    // 减去 EndTime 之后剩余 -50：目标变为 -50，窗口内只走一半。
    EXPECT_TRUE(ApplyWindow(subtractTarget, clip, 0, 0.5f, delta)
        .isApprox(Eigen::Vector3f(0, 0, -25), 1e-3f));
}

TEST(MotionWarping, DebugStateTracksWindowProgress)
{
    MotionWarpRuntime runtime;
    runtime.SetWindows({FrontWindow(0.5f, 1.0f, -100)});
    EXPECT_EQ(runtime.DebugState(0.25f), "FrontLedge:waiting");
    EXPECT_EQ(runtime.DebugState(0.5f), "FrontLedge:active");
    EXPECT_EQ(runtime.DebugState(1.0f), "FrontLedge:done");
}

TEST(MotionWarping, MultipleActiveWindowsUseLastQueuedTarget)
{
    const auto clip = WarpClip(100, 2);
    MotionWarpRuntime runtime;
    // UE 的平移 modifier 每个都从动画重新取 Delta/Total 并覆盖 InRootMotion，
    // 因此同一 tick 多个窗口激活时，队列里最后一个窗口的结果生效。
    runtime.SetWindows({FrontWindow(0, 1, -100), FrontWindow(0, 1, -200)});
    EXPECT_TRUE(ApplyWindow(runtime, clip, 0, 0.5f, Eigen::Vector3f(0, 0, -25))
        .isApprox(Eigen::Vector3f(0, 0, -100), 1e-3f));
}

TEST(MotionWarping, FixedStepConvergesToTargetAt30_60_120Hz)
{
    const auto clip = WarpClip(100, 2);
    MotionWarpRuntime runtime;
    runtime.SetWindows({FrontWindow(0, 1, -100)});
    Eigen::Vector3f finalByRate[3];
    const float rates[3] = {30.0f, 60.0f, 120.0f};
    for (int rateIndex = 0; rateIndex < 3; ++rateIndex)
    {
        const float step = 1.0f / rates[rateIndex];
        const int count = static_cast<int>(rates[rateIndex]);
        Eigen::Vector3f actor = Eigen::Vector3f::Zero();
        float previous = 0;
        for (int i = 1; i <= count; ++i)
        {
            const float current = i * step;
            const Eigen::Vector3f rawDelta(0, 0, -50.0f * step);
            actor += runtime.Apply(SkeletonMesh{}, clip, previous, current, rawDelta,
                Eigen::Matrix4f::Identity(), actor);
            previous = current;
        }
        finalByRate[rateIndex] = actor;
        EXPECT_NEAR(actor.z(), -100.0f, 0.5f) << "rate=" << rates[rateIndex];
    }
    EXPECT_NEAR(finalByRate[0].z(), finalByRate[1].z(), 0.5f);
    EXPECT_NEAR(finalByRate[1].z(), finalByRate[2].z(), 0.5f);
}

TEST(MotionWarping, BoneWarpPointPlacesBoneInsteadOfRoot)
{
    // UE WarpPointAnimProvider=Bone：目标点是骨骼位置，不是根位置。attach 在根前方
    // 50 cm 且整个片段固定，所以根必须比无骨骼目标时少走 50 cm 才把 attach 送到 -100。
    const Eigen::Vector3f attachLocal(0, 0, -50);
    const auto mesh = WarpMesh(attachLocal);
    const auto clip = WarpClipWithAttach(100, 2, attachLocal);
    MotionWarpRuntime runtime;
    MotionWarpWindow window = FrontWindow(0, 1, -100);
    window.warpPointBone = "attach";
    runtime.SetWindows({window});
    // 无骨骼：总位移 -50，目标 -100 -> scale 2 -> 本 tick -50。
    EXPECT_TRUE(ApplyWindow(runtime, mesh, clip, 0, 0.5f, Eigen::Vector3f(0, 0, -25))
        .isApprox(Eigen::Vector3f(0, 0, -25), 1e-3f));
    MotionWarpRuntime rootOnly;
    rootOnly.SetWindows({FrontWindow(0, 1, -100)});
    EXPECT_TRUE(ApplyWindow(rootOnly, mesh, clip, 0, 0.5f, Eigen::Vector3f(0, 0, -25))
        .isApprox(Eigen::Vector3f(0, 0, -50), 1e-3f));
}

TEST(MotionWarping, UnknownWarpPointBoneKeepsRootTarget)
{
    const auto mesh = WarpMesh(Eigen::Vector3f(0, 100, 0));
    const auto clip = WarpClipWithAttach(100, 2, Eigen::Vector3f(0, 100, 0));
    MotionWarpRuntime runtime;
    MotionWarpWindow window = FrontWindow(0, 1, -100);
    window.warpPointBone = "missing";
    runtime.SetWindows({window});
    EXPECT_TRUE(ApplyWindow(runtime, mesh, clip, 0, 0.5f, Eigen::Vector3f(0, 0, -25))
        .isApprox(Eigen::Vector3f(0, 0, -50), 1e-3f));
}

TEST(MotionWarping, SampleBoneModelTransformComposesParentChain)
{
    const Eigen::Vector3f attachLocal(0, 100, 0);
    const auto mesh = WarpMesh(attachLocal);
    const auto clip = WarpClipWithAttach(100, 2, attachLocal);
    Eigen::Matrix4f transform;
    ASSERT_TRUE(SampleBoneModelTransform(mesh, clip, "attach", 1.0f, transform));
    // 根在 t=1 走到 -50，attach 再叠加局部 +100 高度。
    const Eigen::Vector3f attachModel = transform.block<3, 1>(0, 3);
    EXPECT_TRUE(attachModel.isApprox(Eigen::Vector3f(0, 100, -50), 1e-3f))
        << "attachModel=" << attachModel.transpose();
    EXPECT_FALSE(SampleBoneModelTransform(mesh, clip, "missing", 1.0f, transform));
}

TEST(MotionWarping, DisjointWindowsEachReachTheirOwnTarget)
{
    // UE modifier 各自从动画重新取剩余根位移，窗口之间不累计“未达成量”。
    const auto mesh = WarpMesh();
    const auto clip = WarpClip(200, 2);
    MotionWarpRuntime runtime;
    runtime.SetWindows({FrontWindow(0, 1, -100), FrontWindow(1.2f, 2.0f, -150)});
    Eigen::Vector3f actor = Eigen::Vector3f::Zero();
    const Eigen::Vector3f rawDelta(0, 0, -200.0f / 120.0f);
    float previous = 0;
    float firstWindowEnd = 0;
    for (int tick = 1; tick <= 120; ++tick)
    {
        const float current = tick / 60.0f;
        actor += runtime.Apply(mesh, clip, previous, current, rawDelta,
            Eigen::Matrix4f::Identity(), actor);
        previous = current;
        if (tick == 60) firstWindowEnd = actor.z();
    }
    EXPECT_NEAR(firstWindowEnd, -100.0f, 0.5f);
    EXPECT_NEAR(actor.z(), -150.0f, 0.5f);
}

TEST(MotionWarping, BoneWarpPointConvergesToAttractorAtWindowEnd)
{
    // Mantle/Vault 的 FrontLedge 窗口是 WarpPointAnimProvider=Bone：目标是“骨应该到达
    // 的位置”，根的收敛点 = target + (root - warpPoint)。夹具让 attach 恒在根前方
    // 50 cm（沿根运动轴），窗口 target=(0,0,-300) 时 attractor=(0,0,-350)：60 个 tick
    // 后根必须正好停在那里（目标与 total 同轴时 SkewWarp 的 scale 是精确的）。
    const Eigen::Vector3f attachLocal(0, 0, 50);
    const auto mesh = WarpMesh(attachLocal);
    const auto clip = WarpClipWithAttach(100, 2, attachLocal);
    MotionWarpRuntime runtime;
    MotionWarpWindow window = FrontWindow(0, 1, -300);
    window.warpPointBone = "attach";
    runtime.SetWindows({window});

    Eigen::Vector3f actor = Eigen::Vector3f::Zero();
    const float step = 1.0f / 60.0f;
    const Eigen::Vector3f rawDelta(0, 0, -50.0f * step);
    for (int tick = 1; tick <= 60; ++tick)
        actor += runtime.Apply(mesh, clip, (tick - 1) * step, tick * step, rawDelta,
            Eigen::Matrix4f::Identity(), actor);

    EXPECT_NEAR(actor.x(), 0.0f, 0.5f);
    EXPECT_NEAR(actor.y(), 0.0f, 0.5f);
    EXPECT_NEAR(actor.z(), -350.0f, 0.5f);
}

TEST(MotionWarping, RotationWarpReaches180DegreeTargetAt60Hz)
{
    // 根 yaw 90°/2s，窗口 [0,1] 内总根旋转 45°；目标世界 yaw 180°、Actor 起始 0°。
    // UE 公式每 tick：targetDelta = 180 - actor，total = 剩余根旋转；
    // Alpha = dt / (End - previous)，deltaOut = Alpha*(targetDelta-total)，
    // 最终 yaw = deltaOut + 原始根 yaw。该夹具下 diff/剩余 tick 恒为 2.25°，
    // 加原始 0.75° 后每 tick 3°，60 tick 后精确到达 180°。
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    runtime.SetWindows({RotationWindow(0, 1, 180)});

    const float step = 1.0f / 60.0f;
    float actorYaw = 0;
    for (int tick = 1; tick <= 60; ++tick)
    {
        float yawDelta = 0;
        ApplyWindow(runtime, clip, (tick - 1) * step, tick * step, Eigen::Vector3f::Zero(),
            actorYaw, yawDelta);
        EXPECT_NEAR(Degrees(yawDelta), 3.0f, 1e-3f) << "tick=" << tick;
        actorYaw += yawDelta;
    }
    EXPECT_NEAR(Degrees(actorYaw), 180.0f, 1e-3f);
}

TEST(MotionWarping, RotationWarpReaches90DegreeTargetAt60Hz)
{
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    runtime.SetWindows({RotationWindow(0, 1, 90)});

    const float step = 1.0f / 60.0f;
    float actorYaw = 0;
    for (int tick = 1; tick <= 60; ++tick)
    {
        float yawDelta = 0;
        ApplyWindow(runtime, clip, (tick - 1) * step, tick * step, Eigen::Vector3f::Zero(),
            actorYaw, yawDelta);
        actorYaw += yawDelta;
    }
    EXPECT_NEAR(Degrees(actorYaw), 90.0f, 1e-3f);
}

TEST(MotionWarping, RotationTimeMultiplierScalesUeAlphaFormula)
{
    // UE RootMotionModifier.cpp:595-598 手算：片段根 yaw 90°/2s，窗口 [0,1] 的
    // total=45°；target=180°；multiplier=0.8、previous=0、current=1/60：
    //   Alpha = (1/60)/(1*0.8) = 1/48
    //   targetThis = 45 + (180-45)/48 = 47.8125°，deltaOut = 2.8125°
    //   raw = 0.75°（45°/s × 1/60）
    //   final = deltaOut + raw = 3.5625°
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    runtime.SetWindows({RotationWindow(0, 1, 180, 0.8f)});
    float yawDelta = 0;
    ApplyWindow(runtime, clip, 0.0f, 1.0f / 60.0f, Eigen::Vector3f::Zero(), 0.0f, yawDelta);
    EXPECT_NEAR(Degrees(yawDelta), 3.5625f, 1e-3f);
}

TEST(MotionWarping, RotationWarpOnlyInsideWindow)
{
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    runtime.SetWindows({RotationWindow(0.5f, 1.0f, 180)});

    // 窗口开始前：Apply 不动 outYawDelta，调用方保留原始根 yaw。
    float before = 0.75f;
    ApplyWindow(runtime, clip, 0.25f, 0.5f, Eigen::Vector3f::Zero(), 0.0f, before);
    EXPECT_FLOAT_EQ(before, 0.75f);

    // 窗口内最后一个 tick：Alpha=1，total=11.25°，deltaOut=180-11.25=168.75°，
    // 加原始 11.25° 后正好 180°。
    float inside = 0;
    ApplyWindow(runtime, clip, 0.75f, 1.0f, Eigen::Vector3f::Zero(), 0.0f, inside);
    EXPECT_NEAR(std::abs(WrappedDegrees(inside)), 180.0f, 1e-3f);

    // PreviousPosition 到达 EndTime：窗口结束，不再施加旋转。
    float after = 0.5f;
    ApplyWindow(runtime, clip, 1.0f, 1.25f, Eigen::Vector3f::Zero(), 0.0f, after);
    EXPECT_FLOAT_EQ(after, 0.5f);
}

TEST(MotionWarping, MultipleActiveWindowsUseLastRotationTarget)
{
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    // 与平移一致：UE 逐个 modifier 覆盖 rotation，队列最后一个旋转窗口生效。
    runtime.SetWindows({RotationWindow(0, 1, 90), RotationWindow(0, 1, 180)});
    float yawDelta = 0;
    ApplyWindow(runtime, clip, 0.0f, 1.0f / 60.0f, Eigen::Vector3f::Zero(), 0.0f, yawDelta);
    EXPECT_NEAR(Degrees(yawDelta), 3.0f, 1e-3f);

    MotionWarpRuntime reversed;
    reversed.SetWindows({RotationWindow(0, 1, 180), RotationWindow(0, 1, 90)});
    float reversedYaw = 0;
    ApplyWindow(reversed, clip, 0.0f, 1.0f / 60.0f, Eigen::Vector3f::Zero(), 0.0f, reversedYaw);
    EXPECT_NEAR(Degrees(reversedYaw), 1.5f, 1e-3f);
}

TEST(MotionWarping, RotationAndTranslationWarpIndependently)
{
    // 同一窗口：平移走 SkewWarp（target -100、窗口总位移 -50 => 60 tick 后根到 -100），
    // 旋转同时写 yaw 通道（180° 目标 => 60 tick 后 180°），两者互不覆盖。
    const auto clip = WarpClipWithYaw(90.0f, 2, 100);
    MotionWarpRuntime translationRuntime;
    translationRuntime.SetWindows({FrontWindow(0, 1, -100)});

    MotionWarpWindow combined = RotationWindow(0, 1, 180);
    combined.targetLocation = Eigen::Vector3f(0, 0, -100);
    MotionWarpRuntime combinedRuntime;
    combinedRuntime.SetWindows({combined});

    const float step = 1.0f / 60.0f;
    const Eigen::Vector3f rawDelta(0, 0, -100.0f * step);
    Eigen::Vector3f translated = Eigen::Vector3f::Zero();
    Eigen::Vector3f combinedTranslation = Eigen::Vector3f::Zero();
    float actorYaw = 0;
    for (int tick = 1; tick <= 60; ++tick)
    {
        const float previous = (tick - 1) * step;
        const float current = tick * step;
        // 平移 target 的模型空间换算依赖当前 Actor 位置，必须把累计位移传进去；
        // 否则 target 每 tick 都留在 -100，SkewWarp 会按剩余窗口放大成调和级数。
        translated += translationRuntime.Apply(SkeletonMesh{}, clip, previous, current, rawDelta,
            Eigen::Matrix4f::Identity(), translated);
        float yawDelta = 0;
        combinedTranslation += combinedRuntime.Apply(SkeletonMesh{}, clip, previous, current,
            rawDelta, Eigen::Matrix4f::Identity(), combinedTranslation, actorYaw, &yawDelta);
        actorYaw += yawDelta;
    }

    EXPECT_TRUE(combinedTranslation.isApprox(translated, 1e-4f))
        << "combined=" << combinedTranslation.transpose() << " translated=" << translated.transpose();
    EXPECT_NEAR(translated.z(), -100.0f, 0.5f);
    EXPECT_NEAR(Degrees(actorYaw), 180.0f, 1e-3f);
}

TEST(MotionWarping, MissingTargetRotationKeepsRawYaw)
{
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime noRotation;
    MotionWarpWindow window = RotationWindow(0, 1, 180);
    window.hasTargetRotation = false;
    noRotation.SetWindows({window});
    float yawDelta = 0.75f * Pi / 180.0f;
    ApplyWindow(noRotation, clip, 0.0f, 1.0f / 60.0f, Eigen::Vector3f::Zero(), 0.0f, yawDelta);
    EXPECT_NEAR(Degrees(yawDelta), 0.75f, 1e-4f);

    // warpRotation=false 时即使提交了目标旋转也不执行。
    MotionWarpRuntime disabled;
    MotionWarpWindow disabledWindow = RotationWindow(0, 1, 180);
    disabledWindow.warpRotation = false;
    disabled.SetWindows({disabledWindow});
    float disabledYaw = 0.75f * Pi / 180.0f;
    ApplyWindow(disabled, clip, 0.0f, 1.0f / 60.0f, Eigen::Vector3f::Zero(), 0.0f, disabledYaw);
    EXPECT_NEAR(Degrees(disabledYaw), 0.75f, 1e-4f);
}

TEST(MotionWarping, SubtractRemainingRootMotionOffsetsRotationTarget)
{
    // UE RootMotionRemainingAfterNotify：subtractRemaining=true 时目标 yaw 再扣掉
    // EndTime 之后剩余根旋转。该片段 90°/2s，窗口 [0,1] 的剩余根旋转 45°，
    // 因此目标 180° 在窗口末尾只要求 Actor 到 135°；剩余 45° 由窗口外的原始根旋转补齐。
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    MotionWarpWindow window = RotationWindow(0, 1, 180);
    window.subtractRemainingRootMotion = true;
    runtime.SetWindows({window});

    const float step = 1.0f / 60.0f;
    float actorYaw = 0;
    float yawAtWindowEnd = 0;
    for (int tick = 1; tick <= 120; ++tick)
    {
        // 与播放器一致：先放本 tick 的原始根 yaw；窗口内由 Apply 覆盖。
        float yawDelta = 0.75f * Pi / 180.0f;
        ApplyWindow(runtime, clip, (tick - 1) * step, tick * step, Eigen::Vector3f::Zero(),
            actorYaw, yawDelta);
        actorYaw += yawDelta;
        if (tick == 60) yawAtWindowEnd = actorYaw;
    }
    EXPECT_NEAR(Degrees(yawAtWindowEnd), 135.0f, 1e-3f);
    EXPECT_NEAR(Degrees(actorYaw), 180.0f, 1e-3f);
}

TEST(MotionWarping, IgnoreVerticalDoesNotAffectYawWarp)
{
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    MotionWarpWindow window = RotationWindow(0, 1, 180);
    window.ignoreVertical = true;
    runtime.SetWindows({window});
    float yawDelta = 0;
    ApplyWindow(runtime, clip, 0.0f, 1.0f / 60.0f, Eigen::Vector3f::Zero(), 0.0f, yawDelta);
    EXPECT_NEAR(Degrees(yawDelta), 3.0f, 1e-3f);
}

TEST(MotionWarping, UnsupportedRotationMethodKeepsRawYaw)
{
    // BuildMontageWarpWindows 会拒绝非 Slerp；直接构造窗口时 Apply 保守跳过旋转，
    // 保持调用方传入的原始根 yaw。
    const auto clip = WarpClipWithYaw(90.0f);
    MotionWarpRuntime runtime;
    MotionWarpWindow window = RotationWindow(0, 1, 180);
    window.rotationMethod = "ConstantRate";
    runtime.SetWindows({window});
    float yawDelta = 0.75f * Pi / 180.0f;
    ApplyWindow(runtime, clip, 0.0f, 1.0f / 60.0f, Eigen::Vector3f::Zero(), 0.0f, yawDelta);
    EXPECT_NEAR(Degrees(yawDelta), 0.75f, 1e-4f);
}

// ---- WarpPointAnimProvider=Static ----
// UE 在 RootMotionModifier.cpp:432-435 用 CalculateRootTransformRelativeToWarpPointAtTime
// （MotionWarpingComponent.cpp:245-251）得到 CachedOffsetFromWarpPoint，其中
// FTransform::GetRelativeTransform 是 this*Other^-1，即 offset = Root(EndTime) * Pose^-1。
// 这里用“Static pose == 某根轨迹上的 Bone 模型变换”对比，锁定位姿顺序与旋转参与方式。

TEST(MotionWarping, StaticWarpPointMatchesEquivalentBoneWarpPoint)
{
    // attach 相对根只有 yaw 90° 旋转、无平移；EndTime=1 时根在 (0,0,-50)。
    // Bone 的模型变换 = Root * attach = (yaw90, (0,0,-50))，与下面的 staticPose 完全一致，
    // 因此两条 provider 路径必须得到同一 offset（含旋转项）和同一 warp 结果。
    SkeletonMesh mesh = WarpMesh(Eigen::Vector3f(0, 0, 0));
    mesh.joints[1].restLocal.block<3, 3>(0, 0) =
        YawQuaternion(90.0f).toRotationMatrix();
    const auto clip = WarpClip(100, 2);

    MotionWarpWindow boneWindow = FrontWindow(0, 1, -100);
    boneWindow.warpPointBone = "attach";
    MotionWarpRuntime boneRuntime;
    boneRuntime.SetWindows({boneWindow});

    MotionWarpWindow staticWindow = FrontWindow(0, 1, -100);
    staticWindow.hasStaticWarpPoint = true;
    staticWindow.staticWarpPointLocation = Eigen::Vector3f(0, 0, -50);
    staticWindow.staticWarpPointRotation = YawQuaternion(90.0f);
    MotionWarpRuntime staticRuntime;
    staticRuntime.SetWindows({staticWindow});

    const Eigen::Vector3f delta(0, 0, -25);
    const Eigen::Vector3f bone = ApplyWindow(boneRuntime, mesh, clip, 0.0f, 0.5f, delta);
    const Eigen::Vector3f stat = ApplyWindow(staticRuntime, mesh, clip, 0.0f, 0.5f, delta);
    EXPECT_TRUE(stat.isApprox(bone, 1e-4f)) << "static=" << stat.transpose()
        << " bone=" << bone.transpose();
}

TEST(MotionWarping, StaticWarpPointOffsetsTargetByRootMinusPose)
{
    // 窗口 [0,1] 内根从 0 走到 -50；staticPose 在 mesh 原点（平移 0、旋转 Identity），
    // offset = Root(EndTime) * Pose^-1 = -50，于是根的目标 = target(-100) + (-50) = -150。
    // 没有 offset（provider=None）时收敛到 -100；差异正是 Static pose 的平移语义。
    const auto mesh = WarpMesh();
    const auto clip = WarpClip(100, 2);
    MotionWarpWindow staticWindow = FrontWindow(0, 1, -100);
    staticWindow.hasStaticWarpPoint = true;
    staticWindow.staticWarpPointLocation = Eigen::Vector3f::Zero();
    MotionWarpRuntime staticRuntime;
    staticRuntime.SetWindows({staticWindow});
    MotionWarpRuntime noneRuntime;
    noneRuntime.SetWindows({FrontWindow(0, 1, -100)});

    Eigen::Vector3f staticActor = Eigen::Vector3f::Zero();
    Eigen::Vector3f noneActor = Eigen::Vector3f::Zero();
    const float step = 1.0f / 60.0f;
    const Eigen::Vector3f rawDelta(0, 0, -50.0f * step);
    for (int tick = 1; tick <= 60; ++tick)
    {
        staticActor += staticRuntime.Apply(mesh, clip, (tick - 1) * step, tick * step,
            rawDelta, Eigen::Matrix4f::Identity(), staticActor);
        noneActor += noneRuntime.Apply(mesh, clip, (tick - 1) * step, tick * step,
            rawDelta, Eigen::Matrix4f::Identity(), noneActor);
    }
    EXPECT_NEAR(staticActor.z(), -150.0f, 0.5f);
    EXPECT_NEAR(noneActor.z(), -100.0f, 0.5f);
}
