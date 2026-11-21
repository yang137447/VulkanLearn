#pragma once

#include "motionBricksMath.h"

#include <array>
#include <cstddef>
#include <vector>

namespace VL::Animation::MotionBricks
{
// Recent poses recorded at the engine frame rate, resampled to the model's 30 Hz
// context. The delta-time shift matches MotionRecorder.record(): the newest slot is
// always zero and each earlier value is the time to the next recorded pose.
class PoseHistory
{
public:
    explicit PoseHistory(size_t capacity = 32);

    void Fill(const Pose& pose);
    void Record(float deltaSeconds, const Pose& pose);
    // Four poses at -3/30, -2/30, -1/30 and 0 seconds relative to the latest pose.
    // Samples before the available history clamp to the oldest pose.
    std::array<Pose, kContextPoses> SampleFour() const;

    const Pose& Latest() const;
    bool HasPose() const { return jointCount_ != 0; }
    size_t Capacity() const { return poses_.size(); }

private:
    std::vector<Pose> poses_;
    std::vector<float> deltaSeconds_;
    size_t jointCount_ = 0;
};

struct GenerationParams
{
    int numTargetPoses = 1;
    int transitionEarlyByFrames = 0;
    EncodeConfig encode;
};

// One GT sample. rootMotionDeltaMeters is the full 3D runtime-space delta in
// meters; a ground consumer drops the Y component (the engine is Y-up) before
// converting to centimeters. hardCut means the caller must not treat the delta as
// continuous motion.
struct PlaybackFrame
{
    Pose pose;
    std::array<float, 4> footContacts{};
    Eigen::Vector3f rootMotionDeltaMeters = Eigen::Vector3f::Zero();
    float rootMotionYawDelta = 0;
    bool hardCut = false;
    float sourceTime = 0;
};

// Pure GT scheduler for one generated motion. It owns no ONNX session, World, Jolt
// or render state. BuildGenerationInput() is the synchronous hook for P2 inference;
// CommitGeneration() installs the decoded, endpoint-corrected result.
class GeneratedMotionPlayer
{
public:
    explicit GeneratedMotionPlayer(const SkeletonBinding& binding,
        size_t historyCapacity = 32);

    void SeedHistory(const Pose& pose);
    void RecordPose(float deltaSeconds, const Pose& pose);

    // Samples the four context poses from history and encodes them with the selected
    // target poses. targetPoses must hold at least params.numTargetPoses poses.
    EncodedInputs BuildGenerationInput(const std::vector<Pose>& targetPoses,
        const GenerationParams& params) const;
    // Four 30 Hz context poses ending at the latest recorded pose. Live generation
    // uses the last one for endpoint correction while BuildGenerationInput encodes it.
    std::array<Pose, kContextPoses> CurrentContext() const { return history_.SampleFour(); }

    // Installs a decoded motion. The first Sample() reports hardCutOnFirstSample and
    // then clears it. Playback starts at time zero and the root-delta baseline is
    // frame 0, matching the official generate -> sample(0) sequence. params supplies
    // the transition lead frames; production output must contain 18..58 frames.
    void CommitGeneration(DecodedMotion output, const GenerationParams& params,
        bool hardCutOnFirstSample = false);

    PlaybackFrame Sample(float deltaSeconds);
    // GT integration entry: samples the current playback time, then records the
    // displayed pose into history. This is the required order; callers must not
    // substitute separate Sample/RecordPose calls.
    PlaybackFrame Step(float deltaSeconds);

    bool HasGeneration() const { return hasGeneration_; }
    float PlaybackTime() const { return playbackTime_; }
    float PlaybackDuration() const;
    float TimeToTransition() const;
    size_t FrameCount() const { return output_.frames.size(); }
    const DecodedMotion& Generation() const { return output_; }
    const SkeletonBinding& Binding() const { return binding_; }
    const Pose& LatestRecordedPose() const { return history_.Latest(); }
    Eigen::Vector3f CurrentRootPosition() const;

private:
    SkeletonBinding binding_;
    PoseHistory history_;
    DecodedMotion output_;
    size_t transitionEarlyByFrames_ = 0;
    float playbackTime_ = 0;
    Pose previousSample_;
    bool hasGeneration_ = false;
    bool pendingHardCut_ = false;
};
}
