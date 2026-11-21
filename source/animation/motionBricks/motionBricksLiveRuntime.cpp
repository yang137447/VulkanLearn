#include "motionBricksLiveRuntime.h"

#include "motionBricksMath.h"
#include "motionBricksPlayer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace VL::Animation::MotionBricks
{
namespace
{
OnnxTensor FloatTensor(std::string name, std::vector<int64_t> shape,
    std::vector<float> values)
{
    OnnxTensor tensor;
    tensor.name = std::move(name);
    tensor.dtype = TensorDataType::Float32;
    tensor.shape = std::move(shape);
    tensor.floats = std::move(values);
    return tensor;
}

OnnxTensor IntTensor(std::string name, std::vector<int64_t> shape,
    std::vector<int32_t> values)
{
    OnnxTensor tensor;
    tensor.name = std::move(name);
    tensor.dtype = TensorDataType::Int32;
    tensor.shape = std::move(shape);
    tensor.ints = std::move(values);
    return tensor;
}

std::vector<OnnxTensor> EncodeOnnxInputs(const EncodedInputs& encoded)
{
    std::vector<OnnxTensor> inputs;
    inputs.reserve(7);

    std::vector<float> global;
    global.reserve(kContextAndTargetPoses * 5);
    for (const auto& frame : encoded.globalRootValues)
        global.insert(global.end(), frame.begin(), frame.end());
    inputs.push_back(FloatTensor("global_root_values", {1, 8, 5}, std::move(global)));

    inputs.push_back(IntTensor("has_global_root_values", {1, 8},
        std::vector<int32_t>(encoded.hasGlobalRootValues.begin(),
            encoded.hasGlobalRootValues.end())));

    std::vector<float> localRoot;
    localRoot.reserve(kContextAndTargetPoses * 4);
    for (const auto& frame : encoded.localRootValues)
        localRoot.insert(localRoot.end(), frame.begin(), frame.end());
    inputs.push_back(FloatTensor("local_root_values", {1, 8, 4}, std::move(localRoot)));

    inputs.push_back(IntTensor("has_local_root_values", {1, 8},
        std::vector<int32_t>(encoded.hasLocalRootValues.begin(),
            encoded.hasLocalRootValues.end())));

    std::vector<float> localPoses;
    localPoses.reserve(kContextAndTargetPoses * 267);
    for (const auto& frame : encoded.localPoses)
        localPoses.insert(localPoses.end(), frame.begin(), frame.end());
    inputs.push_back(FloatTensor("local_poses", {1, 8, 267}, std::move(localPoses)));

    inputs.push_back(IntTensor("has_local_poses", {1, 8},
        std::vector<int32_t>(encoded.hasLocalPoses.begin(), encoded.hasLocalPoses.end())));
    inputs.push_back(IntTensor("allowed_pred_num_tokens", {1, 11},
        std::vector<int32_t>(encoded.allowedPredNumTokens.begin(),
            encoded.allowedPredNumTokens.end())));
    return inputs;
}

DecodedMotion RunOnnx(OnnxSession& session, const SomaSkeletonAsset& source,
    const LiveGenerationRequest& request)
{
    std::vector<OnnxTensor> inputs = EncodeOnnxInputs(request.encoded);
    std::vector<OnnxTensor> outputs;
    session.Run(inputs, outputs);

    const OnnxTensor* motions = nullptr;
    const OnnxTensor* tokens = nullptr;
    for (const OnnxTensor& tensor : outputs)
    {
        if (tensor.name == "pred_global_motions") motions = &tensor;
        if (tensor.name == "pred_num_tokens") tokens = &tensor;
    }
    if (!motions || !tokens)
        throw std::runtime_error("ONNX SOMA output is missing pred_global_motions/pred_num_tokens");
    if (motions->floats.size() != 64u * 366u || tokens->ints.size() != 1u)
        throw std::runtime_error("ONNX SOMA output has an unexpected shape");

    std::vector<std::array<float, 366>> prediction(64);
    for (size_t row = 0; row < prediction.size(); ++row)
        std::copy_n(motions->floats.data() + row * 366, 366, prediction[row].begin());
    const int tokenCount = tokens->ints.front();
    DecodedMotion decoded = DecodeSomaOutput(source.binding, request.encoded.alignment,
        Transform{}, prediction, tokenCount);
    CorrectEndpoints(source.binding, request.contextLast, request.targetFirst, decoded.frames);
    return decoded;
}
}

struct LiveMotionRuntime::Impl
{
    std::shared_ptr<const SomaSkeletonAsset> source;
    const SkeletonMesh& target;
    CompiledRetarget retarget;
    GeneratedMotionPlayer player;
    ModelContract contract;
    std::filesystem::path modelPath;
    OnnxSessionOptions options;
    LiveGenerationFunction generator;
    GenerationParams params;
    std::vector<LocomotionTarget> targets;
    // Distance advanced by one target pose. The model consumes four poses at
    // 30 Hz, so this is target speed / 30.
    std::atomic<float> targetStepMeters{0.06f};
    std::atomic<float> targetYawRadians{0.0f};
    std::atomic<uint32_t> controlVersion{0};
    uint32_t lastQueuedControlVersion = 0;

    std::thread worker;
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool stop = false;
    bool sessionReady = false;
    bool inFlight = false;
    std::optional<LiveGenerationRequest> request;
    std::optional<DecodedMotion> ready;
    Pose readyTarget;
    uint32_t readyControlVersion = 0;
    std::string provider = "pending";
    std::string error;
    double lastGenerationMs = 0;
    bool firstFrame = true;
    bool hasGeneration = false;

    Impl(std::shared_ptr<const SomaSkeletonAsset> sourceAsset,
        const SkeletonMesh& targetMesh,
        const RetargetProfile& profile,
        std::filesystem::path path,
        ModelContract modelContract,
        OnnxSessionOptions sessionOptions,
        LiveGenerationFunction generation,
        int transitionEarlyByFrames,
        float stepMeters, std::vector<LocomotionTarget> locomotionTargets)
        : source(std::move(sourceAsset)),
          target(targetMesh),
          retarget(source->mesh, target, profile),
          player(source->binding),
          contract(std::move(modelContract)),
          modelPath(std::move(path)),
          options(sessionOptions),
          generator(std::move(generation)),
          targets(std::move(locomotionTargets)),
          targetStepMeters(stepMeters)
    {
        params.transitionEarlyByFrames = transitionEarlyByFrames;
        params.numTargetPoses = 4;
        Pose seed = targets.empty() ? source->binding.referenceLocalPose :
            targets.front().poses.front();
        if (!targets.empty())
        {
            seed[0].rotation = Eigen::Quaternionf(Eigen::AngleAxisf(
                3.14159265359f - PoseHeading(source->binding, seed),
                Eigen::Vector3f::UnitY())) * seed[0].rotation;
            seed[0].position.x() = 0;
            seed[0].position.z() = 0;
        }
        player.SeedHistory(seed);
        worker = std::thread([this] { WorkerLoop(); });
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        condition.notify_all();
        if (worker.joinable()) worker.join();
    }

    void WorkerLoop()
    {
        std::optional<OnnxSession> session;
        if (!generator)
        {
            try
            {
                session.emplace(modelPath, &contract, options);
                std::lock_guard lock(mutex);
                provider = session->ProviderName();
                sessionReady = true;
            }
            catch (const std::exception& exception)
            {
                std::lock_guard lock(mutex);
                error = exception.what();
                sessionReady = false;
                std::cerr << "[MotionBricks] live session error: "
                          << error << '\n';
                return;
            }
        }
        else
        {
            std::lock_guard lock(mutex);
            provider = "fake";
            sessionReady = true;
        }

        while (true)
        {
            LiveGenerationRequest job;
            {
                std::unique_lock lock(mutex);
                condition.wait(lock, [this] { return stop || request.has_value(); });
                if (stop) return;
                job = std::move(*request);
                request.reset();
                inFlight = true;
            }

            try
            {
                const auto start = std::chrono::steady_clock::now();
                DecodedMotion decoded = generator ? generator(job) :
                    RunOnnx(*session, *source, job);
                const size_t frameCount = decoded.frames.size();
                const double milliseconds = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                {
                    std::lock_guard lock(mutex);
                    ready = std::move(decoded);
                    readyTarget = std::move(job.targetFirst);
                    readyControlVersion = job.controlVersion;
                    lastGenerationMs = milliseconds;
                    error.clear();
                    inFlight = false;
                }
                std::cout << "[MotionBricks] live provider=" << provider
                          << " ms=" << milliseconds << " frames="
                          << frameCount << '\n';
            }
            catch (const std::exception& exception)
            {
                std::lock_guard lock(mutex);
                error = exception.what();
                inFlight = false;
                std::cerr << "[MotionBricks] live error: " << exception.what() << '\n';
            }
            condition.notify_all();
        }
    }

    bool SessionReady() const
    {
        std::lock_guard lock(mutex);
        return sessionReady;
    }

    bool CanRequest() const
    {
        std::lock_guard lock(mutex);
        return sessionReady && !inFlight && !request.has_value() && !ready.has_value();
    }

    void QueueGeneration()
    {
        if (!CanRequest()) return;

        const std::array<Pose, kContextPoses> context = player.CurrentContext();
        const Pose& current = player.LatestRecordedPose();
        const float yaw = targetYawRadians.load(std::memory_order_relaxed);
        const float step = targetStepMeters.load(std::memory_order_relaxed);
        const float speed = step * kSampleRate;
        std::vector<Pose> targetPoses;
        targetPoses.reserve(static_cast<size_t>(params.numTargetPoses));
        size_t upper = 0;
        while (upper + 1 < targets.size() && targets[upper].speed < speed) ++upper;
        const size_t lower = upper > 0 ? upper - 1 : 0;
        const float blend = upper != lower ? std::clamp(
            (speed - targets[lower].speed) /
            (targets[upper].speed - targets[lower].speed), 0.0f, 1.0f) : 0.0f;
        std::array<Pose, kContextPoses> blended = context;
        if (!targets.empty())
            for (size_t frame = 0; frame < kContextPoses; ++frame)
            {
                blended[frame] = targets[lower].poses[frame];
                for (size_t joint = 0; joint < blended[frame].size(); ++joint)
                    blended[frame][joint] = Blend(targets[lower].poses[frame][joint],
                        targets[upper].poses[frame][joint], blend);
            }
        const Eigen::Quaternionf placement(Eigen::AngleAxisf(
            3.14159265359f + yaw - PoseHeading(source->binding, blended[0]),
            Eigen::Vector3f::UnitY()));
        const Eigen::Vector3f direction(-std::sin(yaw), 0.0f, -std::cos(yaw));
        for (int index = 0; index < params.numTargetPoses; ++index)
        {
            Pose pose = blended[static_cast<size_t>(index)];
            pose[0].rotation = (placement * pose[0].rotation).normalized();
            pose[0].position = current[0].position + direction *
                (speed + step * static_cast<float>(index));
            pose[0].position.y() = blended[static_cast<size_t>(index)][0].position.y();
            targetPoses.push_back(std::move(pose));
        }

        LiveGenerationRequest job;
        job.contextLast = context.back();
        job.targetFirst = targetPoses.front();
        job.transitionEarlyByFrames = params.transitionEarlyByFrames;
        job.controlVersion = controlVersion.load(std::memory_order_relaxed);
        job.encoded = player.BuildGenerationInput(targetPoses, params);
        {
            std::lock_guard lock(mutex);
            if (stop || !sessionReady || inFlight || request.has_value()) return;
            request = std::move(job);
        }
        condition.notify_all();
    }

    bool TakeReady(DecodedMotion& decoded, Pose& targetPose)
    {
        std::lock_guard lock(mutex);
        if (!ready) return false;
        if (readyControlVersion != controlVersion.load(std::memory_order_relaxed))
        {
            ready.reset();
            return false;
        }
        decoded = std::move(*ready);
        targetPose = std::move(readyTarget);
        ready.reset();
        return true;
    }

    ExternalPoseFrame MakeFrame(const Pose& sourcePose, const PlaybackFrame& frame,
        float deltaSeconds) const
    {
        ExternalPoseFrame output;
        output.locals = RetargetInPlace(*source, target, retarget, sourcePose);
        output.rootMotionDelta = frame.rootMotionDeltaMeters * 100.0f;
        output.rootMotionYawDelta = frame.rootMotionYawDelta;
        output.hardCut = frame.hardCut;
        output.deltaSeconds = deltaSeconds;
        output.sourceTime = frame.sourceTime;
        return output;
    }
};

LiveMotionRuntime::LiveMotionRuntime(std::shared_ptr<const SomaSkeletonAsset> source,
    const SkeletonMesh& target, const RetargetProfile& profile,
    const std::filesystem::path& modelPath, const ModelContract& contract,
    const OnnxSessionOptions& options, int transitionEarlyByFrames,
    float targetStepMeters, std::vector<LocomotionTarget> targets)
    : impl_(std::make_unique<Impl>(std::move(source), target, profile, modelPath,
        contract, options, LiveGenerationFunction{}, transitionEarlyByFrames,
        targetStepMeters, std::move(targets)))
{
}

LiveMotionRuntime::LiveMotionRuntime(std::shared_ptr<const SomaSkeletonAsset> source,
    const SkeletonMesh& target, const RetargetProfile& profile,
    LiveGenerationFunction generation, int transitionEarlyByFrames,
    float targetStepMeters, std::vector<LocomotionTarget> targets)
    : impl_(std::make_unique<Impl>(std::move(source), target, profile,
        std::filesystem::path{}, ModelContract{}, OnnxSessionOptions{},
        std::move(generation), transitionEarlyByFrames, targetStepMeters, std::move(targets)))
{
}

LiveMotionRuntime::~LiveMotionRuntime() = default;

ExternalPoseFrame LiveMotionRuntime::Tick(float deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
        throw std::invalid_argument("Live MotionBricks delta must be finite and nonnegative");
    if (!impl_) throw std::logic_error("Live MotionBricks runtime was moved from");
    if (deltaSeconds == 0.0f)
    {
        PlaybackFrame frame;
        frame.pose = impl_->player.LatestRecordedPose();
        return impl_->MakeFrame(frame.pose, frame, 0.0f);
    }

    DecodedMotion decoded;
    Pose targetPose;
    if (impl_->TakeReady(decoded, targetPose))
    {
        CorrectEndpoints(impl_->source->binding, impl_->player.LatestRecordedPose(),
            targetPose, decoded.frames);
        impl_->player.CommitGeneration(std::move(decoded), impl_->params);
        impl_->hasGeneration = true;
    }

    if (!impl_->hasGeneration)
    {
        impl_->QueueGeneration();
        impl_->lastQueuedControlVersion = impl_->controlVersion.load(
            std::memory_order_relaxed);
        PlaybackFrame rest;
        rest.pose = impl_->player.LatestRecordedPose();
        rest.hardCut = impl_->firstFrame;
        impl_->firstFrame = false;
        return impl_->MakeFrame(rest.pose, rest, 0.0f);
    }

    // Steering changes bypass the normal transition lead time: queue one
    // latest-wins generation with the new heading/stride as soon as the worker
    // is free. If it is busy, the next Tick retries.
    const uint32_t controlVersion = impl_->controlVersion.load(
        std::memory_order_relaxed);
    if (controlVersion != impl_->lastQueuedControlVersion && impl_->CanRequest())
    {
        impl_->QueueGeneration();
        impl_->lastQueuedControlVersion = controlVersion;
    }

    PlaybackFrame frame = deltaSeconds > 0.0f ? impl_->player.Step(deltaSeconds) :
        impl_->player.Sample(0.0f);
    if (deltaSeconds > 0.0f && impl_->player.TimeToTransition() <= 0.15f)
        impl_->QueueGeneration();
    impl_->firstFrame = false;
    return impl_->MakeFrame(frame.pose, frame, deltaSeconds);
}

size_t LiveMotionRuntime::FrameCount() const
{
    return impl_->player.FrameCount();
}

size_t LiveMotionRuntime::TargetJointCount() const
{
    return impl_->target.joints.size();
}

std::string LiveMotionRuntime::ProviderName() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->provider;
}

float LiveMotionRuntime::LastGenerationMs() const
{
    std::lock_guard lock(impl_->mutex);
    return static_cast<float>(impl_->lastGenerationMs);
}

void LiveMotionRuntime::SetTargetMotion(float directionX, float directionY,
    float speedScale)
{
    if (!impl_) throw std::logic_error("Live MotionBricks runtime was moved from");
    if (!std::isfinite(directionX) || !std::isfinite(directionY) ||
        !std::isfinite(speedScale) || speedScale <= 0.0f || speedScale > 4.0f)
        throw std::invalid_argument("MotionBricks target motion is invalid");

    const float length = std::sqrt(directionX * directionX + directionY * directionY);
    float yaw = 0.0f;
    float distance = 0.0f;
    if (length > 0.05f)
    {
        yaw = std::atan2(-directionX, directionY);
        // 0.06 m per 30 Hz pose is 1.8 m/s. Shift (1.8x) reaches ~3.2 m/s.
        distance = std::min(length, 1.0f) * speedScale * 0.06f;
    }

    const float previousYaw = impl_->targetYawRadians.load(std::memory_order_relaxed);
    if (length <= 0.05f) yaw = previousYaw;
    const float previousStep =
        impl_->targetStepMeters.load(std::memory_order_relaxed);
    const float yawDelta = std::abs(std::remainder(yaw - previousYaw,
        6.28318530718f));
    if (yawDelta > 0.15f || std::abs(distance - previousStep) > 0.01f)
    {
        impl_->targetYawRadians.store(yaw, std::memory_order_relaxed);
        impl_->targetStepMeters.store(distance, std::memory_order_relaxed);
        impl_->controlVersion.fetch_add(1, std::memory_order_relaxed);
    }
}

std::string LiveMotionRuntime::LastError() const
{
    std::lock_guard lock(impl_->mutex);
    return impl_->error;
}
}
