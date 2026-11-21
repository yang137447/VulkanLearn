#pragma once

#include "motionBricksSkeleton.h"
#include "motionBricksPlayer.h"
#include "motionBricksRuntimeInterface.h"
#include "animation/animationRetarget.h"
#include "animation/externalPose.h"

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace VL::Animation::MotionBricks
{
struct BakedMotion
{
    int tokenCount = 0;
    int transitionEarlyByFrames = 0;
    Transform alignment;
    std::vector<std::array<float, 366>> prediction;
};

// The baked probe path: decode a stored model prediction once, retarget the
// source poses to a target SkeletalPlayback, and expose one ExternalPoseFrame per
// GT tick. No ONNX session or worker thread is involved.
BakedMotion LoadBakedMotion(const std::filesystem::path& path);

class BakedMotionRuntime : public IMotionBricksRuntime
{
public:
    BakedMotionRuntime(std::shared_ptr<const SomaSkeletonAsset> source,
        const SkeletonMesh& target, const RetargetProfile& profile, BakedMotion baked);

    ExternalPoseFrame Tick(float deltaSeconds) override;

    size_t SourceJointCount() const { return source_->mesh.joints.size(); }
    size_t TargetJointCount() const override { return target_.joints.size(); }
    size_t FrameCount() const override { return decoded_.frames.size(); }
    const char* Mode() const override { return "baked"; }
    std::string ProviderName() const override { return "baked"; }
    float LastGenerationMs() const override { return 0.0f; }
    float PlaybackTime() const { return player_.PlaybackTime(); }
    float PlaybackDuration() const { return player_.PlaybackDuration(); }

private:
    std::shared_ptr<const SomaSkeletonAsset> source_;
    const SkeletonMesh& target_;
    CompiledRetarget retarget_;
    GeneratedMotionPlayer player_;
    DecodedMotion decoded_;
    GenerationParams params_;
};
}
