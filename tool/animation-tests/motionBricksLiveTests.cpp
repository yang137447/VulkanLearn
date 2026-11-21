#include "animation/motionBricks/motionBricksLiveRuntime.h"
#include "animation/motionBricks/motionBricksSkeleton.h"

#include <nlohmann/json.hpp>

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
using namespace VL::Animation;
using namespace VL::Animation::MotionBricks;
using json = nlohmann::json;

std::filesystem::path FixturePath(const char* name)
{
    return std::filesystem::path(VL_MOTIONBRICKS_FIXTURE_DIR) / name;
}

json LoadJson(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open fixture " + path.string());
    json value;
    input >> value;
    return value;
}

std::shared_ptr<const SomaSkeletonAsset> LoadSource()
{
    return LoadSomaSkeleton(FixturePath("SK_soma.json"), FixturePath("soma_n1.json"));
}

json MakeTargetSkeleton()
{
    struct Entry
    {
        const char* name;
        int parent;
        float z;
    };
    const std::array<Entry, 24> entries = {{
        {"root", -1, 0},
        {"pelvis", 0, 0},
        {"spine_01", 1, 10},
        {"spine_03", 2, 10},
        {"spine_05", 3, 10},
        {"neck_01", 4, 10},
        {"neck_02", 5, 5},
        {"head", 6, 5},
        {"clavicle_l", 4, 5},
        {"upperarm_l", 8, 10},
        {"lowerarm_l", 9, 10},
        {"hand_l", 10, 5},
        {"clavicle_r", 4, 5},
        {"upperarm_r", 12, 10},
        {"lowerarm_r", 13, 10},
        {"hand_r", 14, 5},
        {"thigh_l", 1, 10},
        {"calf_l", 16, 10},
        {"foot_l", 17, 5},
        {"ball_l", 18, 5},
        {"thigh_r", 1, 10},
        {"calf_r", 20, 10},
        {"foot_r", 21, 5},
        {"ball_r", 22, 5},
    }};
    json joints = json::array();
    for (const Entry& entry : entries)
    {
        std::array<float, 16> rest = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, entry.z,
            0, 0, 0, 1,
        };
        joints.push_back({{"name", entry.name}, {"parent", entry.parent},
            {"restLocal", rest}});
    }
    return {
        {"format", "vl.skeleton.v1"},
        {"skeleton", "SK_UEFN_Mannequin"},
        {"root", "root"},
        {"joints", joints},
        {"slots", json::array({"Default"})},
        {"curves", json::object()},
    };
}

SkeletonMesh MakeTargetMesh()
{
    const json skeleton = MakeTargetSkeleton();
    return BuildSkeletonMesh(ParseSkeleton(skeleton, "test-target"));
}

RetargetProfile LoadProfile()
{
    return ParseRetargetProfile(LoadJson(FixturePath("RT_UEFN_From_SOMA.json")), "fixture");
}

DecodedMotion MakeGeneratedMotion(const LiveGenerationRequest& request, size_t frameCount = 30)
{
    DecodedMotion motion;
    for (size_t frame = 0; frame < frameCount; ++frame)
    {
        Pose pose = request.targetFirst;
        pose[0].position += Eigen::Vector3f(0.03f * static_cast<float>(frame), 0.0f, 0.0f);
        motion.frames.push_back(std::move(pose));
        motion.footContacts.push_back({1.0f, 0.0f, 0.0f, 1.0f});
    }
    return motion;
}

std::filesystem::path ModelPathFromEnvironment()
{
    const char* value = std::getenv("VULKANLEARN_MOTIONBRICKS_SOMA_ONNX");
    if (value == nullptr || *value == '\0') return {};
    return std::filesystem::path(value);
}

void PumpUntil(const std::function<bool()>& condition, LiveMotionRuntime& runtime,
    std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition() && std::chrono::steady_clock::now() < deadline)
    {
        runtime.Tick(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

TEST(MotionBricksLive, FakeGeneratorCommitsAndProducesRiggedPose)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();
    std::atomic<int> calls = 0;
    LiveMotionRuntime runtime(source, target, profile,
        [&calls](const LiveGenerationRequest& request)
        {
            ++calls;
            return MakeGeneratedMotion(request);
        });

    const ExternalPoseFrame first = runtime.Tick(1.0f / 60.0f);
    EXPECT_TRUE(first.hardCut);
    EXPECT_EQ(first.locals.size(), (target.joints.size() + 3) / 4);

    PumpUntil([&runtime] { return runtime.FrameCount() != 0; }, runtime,
        std::chrono::seconds(5));
    ASSERT_NE(runtime.FrameCount(), 0u) << runtime.LastError();
    EXPECT_EQ(runtime.ProviderName(), "fake");

    for (int frame = 0; frame < 10; ++frame)
    {
        const ExternalPoseFrame pose = runtime.Tick(1.0f / 60.0f);
        EXPECT_EQ(pose.locals.size(), (target.joints.size() + 3) / 4);
        EXPECT_TRUE(pose.rootMotionDelta.allFinite());
        EXPECT_TRUE(std::isfinite(pose.rootMotionYawDelta));
    }
    EXPECT_GE(calls.load(), 1);
    EXPECT_GE(runtime.LastGenerationMs(), 0.0f);
}

TEST(MotionBricksLive, SteeringUpdatesGenerationTargetAndStops)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();
    std::mutex mutex;
    std::vector<Eigen::Vector3f> offsets;
    LiveMotionRuntime runtime(source, target, profile,
        [&mutex, &offsets](const LiveGenerationRequest& request)
        {
            {
                std::lock_guard lock(mutex);
                offsets.push_back(request.targetFirst[0].position -
                    request.contextLast[0].position);
            }
            return MakeGeneratedMotion(request);
        });

    PumpUntil([&runtime] { return runtime.FrameCount() != 0; }, runtime,
        std::chrono::seconds(5));
    ASSERT_NE(runtime.FrameCount(), 0u) << runtime.LastError();

    runtime.SetTargetMotion(1.0f, 0.0f, 1.0f);
    PumpUntil([&mutex, &offsets] {
        std::lock_guard lock(mutex);
        return offsets.size() >= 2;
    }, runtime, std::chrono::seconds(5));

    runtime.SetTargetMotion(0.0f, 0.0f, 1.0f);
    PumpUntil([&mutex, &offsets] {
        std::lock_guard lock(mutex);
        return offsets.size() >= 3;
    }, runtime, std::chrono::seconds(5));

    std::lock_guard lock(mutex);
    ASSERT_GE(offsets.size(), 3u);
    const auto horizontal = [](const Eigen::Vector3f& offset)
    {
        return Eigen::Vector2f(offset.x(), offset.z());
    };
    const Eigen::Vector2f forward = horizontal(offsets[0]);
    const Eigen::Vector2f right = horizontal(offsets[1]);
    ASSERT_GT(forward.norm(), 0.005f);
    ASSERT_GT(right.norm(), 0.005f);
    // A 90-degree steering intent must rotate the generated target away from
    // the previous forward direction instead of advancing along it.
    EXPECT_LT(std::abs(forward.normalized().dot(right.normalized())), 0.2f)
        << offsets[1].transpose();
    EXPECT_LT(horizontal(offsets[2]).norm(), 0.2f) << offsets[2].transpose();
}

TEST(MotionBricksLive, RequestsAreThrottledAndNeverOverlap)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();
    std::atomic<int> calls = 0;
    std::atomic<int> active = 0;
    std::atomic<int> maxActive = 0;
    LiveMotionRuntime runtime(source, target, profile,
        [&](const LiveGenerationRequest& request)
        {
            const int now = ++active;
            int previous = maxActive.load();
            while (now > previous && !maxActive.compare_exchange_weak(previous, now))
            {
            }
            ++calls;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            --active;
            return MakeGeneratedMotion(request);
        });

    PumpUntil([&runtime] { return runtime.FrameCount() != 0; }, runtime,
        std::chrono::seconds(5));
    ASSERT_NE(runtime.FrameCount(), 0u) << runtime.LastError();
    const int callsAfterFirst = calls.load();
    EXPECT_EQ(callsAfterFirst, 1);

    for (int frame = 0; frame < 180; ++frame)
        runtime.Tick(1.0f / 60.0f);

    EXPECT_EQ(maxActive.load(), 1);
    EXPECT_GE(calls.load(), 2);
    EXPECT_LE(calls.load(), 8);
}

TEST(MotionBricksLive, FailedGenerationRetriesAndRecovers)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();
    std::atomic<int> calls = 0;
    LiveMotionRuntime runtime(source, target, profile,
        [&calls](const LiveGenerationRequest& request)
        {
            if (++calls <= 2)
                throw std::runtime_error("synthetic inference failure");
            return MakeGeneratedMotion(request);
        });

    PumpUntil([&runtime] { return !runtime.LastError().empty(); }, runtime,
        std::chrono::seconds(5));
    EXPECT_FALSE(runtime.LastError().empty());
    EXPECT_EQ(runtime.FrameCount(), 0u);

    PumpUntil([&runtime] { return runtime.FrameCount() != 0; }, runtime,
        std::chrono::seconds(5));
    ASSERT_NE(runtime.FrameCount(), 0u) << runtime.LastError();
    EXPECT_GE(calls.load(), 3);
    EXPECT_TRUE(runtime.LastError().empty());
}

TEST(MotionBricksLive, DestructorJoinsAnInFlightGeneration)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();
    std::atomic<bool> started = false;
    const auto begin = std::chrono::steady_clock::now();
    {
        LiveMotionRuntime runtime(source, target, profile,
            [&started](const LiveGenerationRequest& request)
            {
                started = true;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                return MakeGeneratedMotion(request);
            });
        for (int frame = 0; frame < 500 && !started.load(); ++frame)
        {
            runtime.Tick(1.0f / 60.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_TRUE(started.load());
    }
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(elapsed, std::chrono::seconds(3));
}

TEST(MotionBricksLive, RejectsInvalidDelta)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();
    LiveMotionRuntime runtime(source, target, profile,
        [](const LiveGenerationRequest& request) { return MakeGeneratedMotion(request); });
    EXPECT_THROW(runtime.Tick(-0.01f), std::invalid_argument);
    EXPECT_THROW(runtime.Tick(std::nanf("")), std::invalid_argument);
}

TEST(MotionBricksLive, LocomotionTargetsPreserveGaitAndPlaceAbsoluteHeading)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const auto targets = LoadLocomotionTargets(
        FixturePath("soma_locomotion_targets.json"), source->binding);
    ASSERT_EQ(targets.size(), 4u);
    for (const auto& gait : targets)
        for (const auto& pose : gait.poses)
            EXPECT_GT(pose[0].position.y(), 0.7f);
    for (const Eigen::Vector2f direction : {Eigen::Vector2f(0, 1),
        Eigen::Vector2f(1, 0), Eigen::Vector2f(-1, 0), Eigen::Vector2f(0, -1)})
    {
        std::mutex mutex;
        std::vector<LiveGenerationRequest> requests;
        LiveMotionRuntime runtime(source, target, LoadProfile(),
            [&](const LiveGenerationRequest& request)
            {
                std::lock_guard lock(mutex);
                requests.push_back(request);
                return MakeGeneratedMotion(request);
            }, 6, 0.0f, targets);
        runtime.SetTargetMotion(direction.x(), direction.y(), 1.0f);
        PumpUntil([&] {
            std::lock_guard lock(mutex);
            return !requests.empty();
        }, runtime, std::chrono::seconds(5));
        std::lock_guard lock(mutex);
        ASSERT_FALSE(requests.empty());
        const auto& request = requests.front();
        Eigen::Vector3f offset = request.targetFirst[0].position - request.contextLast[0].position;
        offset.y() = 0;
        EXPECT_NEAR(offset.norm(), 1.8f, 1e-4f);
        EXPECT_NEAR(offset.x(), direction.x() * 1.8f, 1e-4f);
        EXPECT_NEAR(offset.z(), -direction.y() * 1.8f, 1e-4f);
        const float heading = PoseHeading(source->binding, request.targetFirst);
        EXPECT_NEAR(std::sin(heading), direction.x(), 1e-4f);
        EXPECT_NEAR(std::cos(heading), -direction.y(), 1e-4f);
        bool hasGait = false;
        for (size_t joint = 1; joint < request.targetFirst.size(); ++joint)
            hasGait = hasGait || request.targetFirst[joint].rotation.angularDistance(
                source->binding.referenceLocalPose[joint].rotation) > 0.1f;
        EXPECT_TRUE(hasGait);
    }
}

TEST(MotionBricksLive, InPlaceRetargetPreservesPelvisHeightAndTilt)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    CompiledRetarget retarget(source->mesh, target, LoadProfile());
    Pose pose = source->binding.referenceLocalPose;
    pose[0].position.y() += 0.05f;
    pose[0].rotation = Eigen::Quaternionf(Eigen::AngleAxisf(0.1f,
        Eigen::Vector3f::UnitZ())) * pose[0].rotation;
    const Pose result = LocalPoseToPoseMeters(RetargetInPlace(*source, target, retarget, pose));
    const Pose reference = LocalPoseToPoseMeters(retarget.TargetReference());
    EXPECT_NEAR(result[0].position.norm(), reference[0].position.norm(), 1e-5f);
    EXPECT_NEAR(result[0].rotation.angularDistance(reference[0].rotation), 0, 1e-5f);
    const auto pelvis = std::find_if(target.joints.begin(), target.joints.end(),
        [](const auto& joint) { return joint.name == "pelvis"; });
    ASSERT_NE(pelvis, target.joints.end());
    const size_t index = static_cast<size_t>(pelvis - target.joints.begin());
    EXPECT_NEAR(result[index].position.y() - reference[index].position.y(), 0.05f, 1e-4f);
    EXPECT_GT(result[index].rotation.angularDistance(reference[index].rotation), 0.05f);
}

TEST(MotionBricksLive, OfficialIdleRetargetKeepsHandsOutsideTheTorso)
{
    const auto source = LoadSource();
    SkeletonMesh target = BuildSkeletonMesh(ParseSkeleton(
        LoadJson(FixturePath("SK_UEFN_Mannequin.json")), "fixture"));
    CompiledRetarget retarget(source->mesh, target, LoadProfile());
    const auto targets = LoadLocomotionTargets(
        FixturePath("soma_locomotion_targets.json"), source->binding);
    const auto models = LocalToModels(target,
        RetargetInPlace(*source, target, retarget, targets.front().poses.front()));
    const auto find = [&](const char* name)
    {
        const auto joint = std::find_if(target.joints.begin(), target.joints.end(),
            [&](const auto& value) { return value.name == name; });
        return static_cast<size_t>(joint - target.joints.begin());
    };
    EXPECT_GT(models[find("hand_l")](0, 3), 10);
    EXPECT_LT(models[find("hand_r")](0, 3), -10);
    EXPECT_LT(models[find("hand_l")](1, 3), models[find("upperarm_l")](1, 3) - 20);
    EXPECT_LT(models[find("hand_r")](1, 3), models[find("upperarm_r")](1, 3) - 20);
}

TEST(MotionBricksLive, ZeroDeltaFreezesPoseAndDoesNotReplayRootMotion)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    LiveMotionRuntime runtime(source, target, LoadProfile(),
        [](const auto& request) { return MakeGeneratedMotion(request); });
    PumpUntil([&] { return runtime.FrameCount() != 0; }, runtime, std::chrono::seconds(5));
    ASSERT_NE(runtime.FrameCount(), 0u);
    const auto before = runtime.Tick(1.0f / 60.0f);
    const Pose reference = LocalPoseToPoseMeters(before.locals);
    for (int frame = 0; frame < 10; ++frame)
    {
        const auto paused = runtime.Tick(0);
        EXPECT_FLOAT_EQ(paused.rootMotionDelta.norm(), 0);
        EXPECT_FLOAT_EQ(paused.rootMotionYawDelta, 0);
        const Pose pose = LocalPoseToPoseMeters(paused.locals);
        ASSERT_EQ(pose.size(), reference.size());
        for (size_t joint = 0; joint < pose.size(); ++joint)
        {
            EXPECT_LT((pose[joint].position - reference[joint].position).norm(), 1e-5f);
            EXPECT_LT(pose[joint].rotation.angularDistance(reference[joint].rotation), 1e-5f);
        }
    }
}

TEST(MotionBricksLive, DiscardsGenerationForAnObsoleteSteeringCommand)
{
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    std::atomic<bool> started = false;
    std::atomic<bool> release = false;
    LiveMotionRuntime runtime(source, target, LoadProfile(),
        [&](const LiveGenerationRequest& request)
        {
            started = true;
            while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return MakeGeneratedMotion(request);
        });
    PumpUntil([&] { return started.load(); }, runtime, std::chrono::seconds(5));
    runtime.SetTargetMotion(0, 0, 1);
    release = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    const auto frame = runtime.Tick(1.0f / 60.0f);
    EXPECT_EQ(runtime.FrameCount(), 0u);
    EXPECT_FLOAT_EQ(frame.rootMotionDelta.norm(), 0);
    PumpUntil([&] { return runtime.FrameCount() != 0; }, runtime, std::chrono::seconds(5));
    EXPECT_NE(runtime.FrameCount(), 0u);
}

TEST(MotionBricksLive, ExecutionProviderSelection)
{
    const std::filesystem::path modelPath = ModelPathFromEnvironment();
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
        GTEST_SKIP() << "VULKANLEARN_MOTIONBRICKS_SOMA_ONNX is not set";
    const ModelContract contract = ParseModelManifest(FixturePath("soma_n1.json"));

    {
        OnnxSession session(modelPath, &contract,
            OnnxSessionOptions{OnnxExecutionProvider::Cpu, 0});
        EXPECT_EQ(session.ProviderName(), "CPU");
    }
    {
        OnnxSession session(modelPath, &contract,
            OnnxSessionOptions{OnnxExecutionProvider::Auto, 0});
        EXPECT_FALSE(session.ProviderName().empty());
        RecordProperty("autoProvider", session.ProviderName());
    }

    for (const OnnxExecutionProvider provider :
        {OnnxExecutionProvider::DirectML, OnnxExecutionProvider::Cuda})
    {
        const char* name = provider == OnnxExecutionProvider::DirectML ? "DirectML" : "CUDA";
        try
        {
            OnnxSession session(modelPath, &contract, OnnxSessionOptions{provider, 0});
            RecordProperty(std::string(name) + "Provider", session.ProviderName());
        }
        catch (const std::exception& exception)
        {
            RecordProperty(std::string(name) + "Error", exception.what());
        }
    }
}

TEST(MotionBricksLive, RealModelGeneratesWithinDesktopBudget)
{
    const std::filesystem::path modelPath = ModelPathFromEnvironment();
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
        GTEST_SKIP() << "VULKANLEARN_MOTIONBRICKS_SOMA_ONNX is not set";
    const ModelContract contract = ParseModelManifest(FixturePath("soma_n1.json"));
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();

    LiveMotionRuntime runtime(source, target, profile, modelPath, contract,
        OnnxSessionOptions{OnnxExecutionProvider::Cpu, 0});
    PumpUntil([&runtime] { return runtime.FrameCount() != 0; }, runtime,
        std::chrono::seconds(120));
    ASSERT_NE(runtime.FrameCount(), 0u) << runtime.LastError();
    EXPECT_EQ(runtime.ProviderName(), "CPU");
    EXPECT_GT(runtime.LastGenerationMs(), 0.0f);
    EXPECT_LT(runtime.LastGenerationMs(), 2000.0f);

    for (int frame = 0; frame < 120; ++frame)
    {
        const ExternalPoseFrame pose = runtime.Tick(1.0f / 60.0f);
        EXPECT_EQ(pose.locals.size(), (target.joints.size() + 3) / 4);
        EXPECT_TRUE(pose.rootMotionDelta.allFinite());
    }
}

TEST(MotionBricksLive, RootMotionStaysBoundedAcrossLongRun)
{
    const std::filesystem::path modelPath = ModelPathFromEnvironment();
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
        GTEST_SKIP() << "VULKANLEARN_MOTIONBRICKS_SOMA_ONNX is not set";
    const ModelContract contract = ParseModelManifest(FixturePath("soma_n1.json"));
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    const RetargetProfile profile = LoadProfile();

    LiveMotionRuntime runtime(source, target, profile, modelPath, contract,
        OnnxSessionOptions{OnnxExecutionProvider::Cpu, 0});
    PumpUntil([&runtime] { return runtime.FrameCount() != 0; }, runtime,
        std::chrono::seconds(120));
    ASSERT_NE(runtime.FrameCount(), 0u) << runtime.LastError();

    constexpr float deltaSeconds = 1.0f / 60.0f;
    float maxSpeedCmPerSecond = 0.0f;
    float maxSpeedTime = 0.0f;
    for (int frame = 0; frame < 1200; ++frame)
    {
        const ExternalPoseFrame pose = runtime.Tick(deltaSeconds);
        const float speedCmPerSecond =
            pose.rootMotionDelta.norm() / deltaSeconds;
        if (speedCmPerSecond > maxSpeedCmPerSecond)
        {
            maxSpeedCmPerSecond = speedCmPerSecond;
            maxSpeedTime = pose.sourceTime;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_LT(maxSpeedCmPerSecond, 2000.0f)
        << "max root-motion speed cm/s at source time " << maxSpeedTime;
}

TEST(MotionBricksLive, RealModelWalkRunTurnAndStop)
{
    const auto modelPath = ModelPathFromEnvironment();
    if (modelPath.empty() || !std::filesystem::exists(modelPath))
        GTEST_SKIP() << "VULKANLEARN_MOTIONBRICKS_SOMA_ONNX is not set";
    const auto source = LoadSource();
    SkeletonMesh target = MakeTargetMesh();
    auto targets = LoadLocomotionTargets(FixturePath("soma_locomotion_targets.json"),
        source->binding);
    LiveMotionRuntime runtime(source, target, LoadProfile(), modelPath, source->contract,
        OnnxSessionOptions{OnnxExecutionProvider::Cpu, 0}, 10, 0.0f, std::move(targets));
    PumpUntil([&] { return runtime.FrameCount() != 0; }, runtime, std::chrono::seconds(120));
    ASSERT_NE(runtime.FrameCount(), 0u) << runtime.LastError();
    auto measure = [&](float directionX, float directionY, float speedScale)
    {
        runtime.SetTargetMotion(directionX, directionY, speedScale);
        Eigen::Vector3f travel = Eigen::Vector3f::Zero();
        float yaw = 0;
        float maxSpeed = 0;
        for (int frame = 0; frame < 240; ++frame)
        {
            const auto pose = runtime.Tick(1.0f / 60.0f);
            EXPECT_TRUE(pose.rootMotionDelta.allFinite());
            EXPECT_TRUE(runtime.LastError().empty()) << runtime.LastError();
            if (frame >= 120)
            {
                travel += pose.rootMotionDelta;
                yaw += pose.rootMotionYawDelta;
                maxSpeed = std::max(maxSpeed, pose.rootMotionDelta.norm() * 60.0f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        travel.y() = 0;
        EXPECT_LT(maxSpeed, 1000.0f);
        EXPECT_LT(std::abs(yaw), 0.5f);
        return travel / 2.0f;
    };
    const Eigen::Vector3f walk = measure(0, 1, 1);
    RecordProperty("walkSpeedCm", walk.norm());
    EXPECT_GT(walk.norm(), 80.0f);
    EXPECT_LT(walk.norm(), 350.0f);
    EXPECT_LT(walk.z(), -0.8f * walk.norm());
    const Eigen::Vector3f run = measure(0, 1, 1.8f);
    RecordProperty("runSpeedCm", run.norm());
    EXPECT_GT(run.norm(), walk.norm() * 1.15f);
    EXPECT_LT(run.norm(), 600.0f);
    const Eigen::Vector3f right = measure(1, 0, 1);
    RecordProperty("rightSpeedCm", right.norm());
    EXPECT_GT(right.x(), 0.7f * right.norm());
    EXPECT_GT(right.norm(), 80.0f);
    const Eigen::Vector3f left = measure(-1, 0, 1);
    RecordProperty("leftSpeedCm", left.norm());
    EXPECT_LT(left.x(), -0.7f * left.norm());
    EXPECT_GT(left.norm(), 80.0f);
    const Eigen::Vector3f back = measure(0, -1, 1);
    RecordProperty("backSpeedCm", back.norm());
    EXPECT_GT(back.z(), 0.7f * back.norm());
    EXPECT_GT(back.norm(), 80.0f);
    const Eigen::Vector3f stop = measure(0, 0, 1);
    RecordProperty("stopSpeedCm", stop.norm());
    EXPECT_LT(stop.norm(), 25.0f);
}
}
