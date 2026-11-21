#pragma once

#include "animation/animationRetarget.h"
#include "motionBricksRuntimeInterface.h"
#include "motionBricksSkeleton.h"
#include "onnxRuntimeApi.h"

#include <filesystem>
#include <functional>
#include <memory>

namespace VL::Animation::MotionBricks
{
struct LiveGenerationRequest
{
    EncodedInputs encoded;
    Pose contextLast;
    Pose targetFirst;
    int transitionEarlyByFrames = 0;
    uint32_t controlVersion = 0;
};

using LiveGenerationFunction = std::function<DecodedMotion(const LiveGenerationRequest&)>;

// GT scheduler plus one inference worker. The worker owns the OnnxSession and the
// encoded input/decoded output; GT only consumes completed generations. The class is
// safe to use without a GPU; Auto falls back to CPU.
class LiveMotionRuntime : public IMotionBricksRuntime
{
public:
    LiveMotionRuntime(std::shared_ptr<const SomaSkeletonAsset> source,
        const SkeletonMesh& target,
        const RetargetProfile& profile,
        const std::filesystem::path& modelPath,
        const ModelContract& contract,
        const OnnxSessionOptions& options,
        int transitionEarlyByFrames = 6,
        float targetStepMeters = 0.06f,
        std::vector<LocomotionTarget> targets = {});

    // Test seam: the function runs on the worker thread and replaces ONNX inference.
    LiveMotionRuntime(std::shared_ptr<const SomaSkeletonAsset> source,
        const SkeletonMesh& target,
        const RetargetProfile& profile,
        LiveGenerationFunction generation,
        int transitionEarlyByFrames = 6,
        float targetStepMeters = 0.06f,
        std::vector<LocomotionTarget> targets = {});

    ~LiveMotionRuntime() override;
    LiveMotionRuntime(const LiveMotionRuntime&) = delete;
    LiveMotionRuntime& operator=(const LiveMotionRuntime&) = delete;

    ExternalPoseFrame Tick(float deltaSeconds) override;
    size_t FrameCount() const override;
    size_t TargetJointCount() const override;
    const char* Mode() const override { return "live"; }
    std::string ProviderName() const override;
    float LastGenerationMs() const override;
    void SetTargetMotion(float directionX, float directionY,
        float speedScale) override;
    std::string LastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
