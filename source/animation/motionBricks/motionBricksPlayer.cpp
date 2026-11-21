#include "motionBricksPlayer.h"
#include "motionBricksSkeleton.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>

namespace VL::Animation::MotionBricks
{
namespace
{
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kPi = 3.14159265358979323846f;

Pose NormalizedPose(const Pose& pose, size_t expectedJointCount, const char* context)
{
    if (pose.size() != expectedJointCount)
        throw std::invalid_argument(std::string(context) + " joint count does not match");
    Pose normalized = pose;
    for (Transform& transform : normalized)
        transform = Normalized(transform);
    return normalized;
}

float NormalizeAngle(float angle)
{
    while (angle > kPi) angle -= kTwoPi;
    while (angle < -kPi) angle += kTwoPi;
    return angle;
}
}

PoseHistory::PoseHistory(size_t capacity)
{
    if (capacity == 0)
        throw std::invalid_argument("MotionBricks history capacity must be positive");
    poses_.resize(capacity);
    deltaSeconds_.assign(capacity, 0.0f);
}

void PoseHistory::Fill(const Pose& pose)
{
    if (pose.empty())
        throw std::invalid_argument("MotionBricks history pose cannot be empty");
    if (jointCount_ != 0 && pose.size() != jointCount_)
        throw std::invalid_argument("MotionBricks history pose shape cannot change");
    const Pose normalized = NormalizedPose(pose, pose.size(), "history pose");
    jointCount_ = pose.size();
    for (Pose& slot : poses_)
        slot = normalized;
    std::fill(deltaSeconds_.begin(), deltaSeconds_.end(), 0.0f);
}

void PoseHistory::Record(float deltaSeconds, const Pose& pose)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
        throw std::invalid_argument("MotionBricks history delta must be finite and positive");
    if (!HasPose())
        throw std::logic_error("MotionBricks history must be filled before recording");
    const Pose normalized = NormalizedPose(pose, jointCount_, "recorded pose");

    for (size_t index = 0; index + 1 < poses_.size(); ++index)
        poses_[index] = std::move(poses_[index + 1]);
    poses_.back() = normalized;

    // MotionRecorder.record(): write dt into the newest slot, shift left, then zero
    // the newest slot. The previous newest pose therefore keeps dt as its delta.
    deltaSeconds_.back() = deltaSeconds;
    for (size_t index = 0; index + 1 < deltaSeconds_.size(); ++index)
        deltaSeconds_[index] = deltaSeconds_[index + 1];
    deltaSeconds_.back() = 0.0f;
}

std::array<Pose, kContextPoses> PoseHistory::SampleFour() const
{
    if (!HasPose())
        throw std::logic_error("MotionBricks history has no pose");

    std::vector<float> poseTimes(poses_.size(), 0.0f);
    for (size_t index = poses_.size(); index-- > 1;)
        poseTimes[index - 1] = poseTimes[index] - deltaSeconds_[index - 1];

    std::array<Pose, kContextPoses> samples;
    for (size_t sample = 0; sample < samples.size(); ++sample)
    {
        const float sampleTime = -static_cast<float>(samples.size() - 1 - sample) /
            kSampleRate;
        const auto upperIt = std::upper_bound(poseTimes.begin(), poseTimes.end(), sampleTime);
        size_t upper = static_cast<size_t>(std::distance(poseTimes.begin(), upperIt));
        upper = std::min(upper, poses_.size() - 1);
        size_t lower = upper > 0 ? upper - 1 : 0;
        const float frameTime = poseTimes[upper] - poseTimes[lower];
        float blend = frameTime > 0.0f ? (sampleTime - poseTimes[lower]) / frameTime : 0.0f;
        if (frameTime <= 0.0f) lower = upper;
        blend = std::clamp(blend, 0.0f, 1.0f);

        Pose pose(jointCount_);
        for (size_t joint = 0; joint < jointCount_; ++joint)
            pose[joint] = Blend(poses_[lower][joint], poses_[upper][joint], blend);
        samples[sample] = std::move(pose);
    }
    return samples;
}

const Pose& PoseHistory::Latest() const
{
    if (!HasPose())
        throw std::logic_error("MotionBricks history has no pose");
    return poses_.back();
}

GeneratedMotionPlayer::GeneratedMotionPlayer(const SkeletonBinding& binding,
    size_t historyCapacity)
    : binding_(binding), history_(historyCapacity)
{
}

void GeneratedMotionPlayer::SeedHistory(const Pose& pose)
{
    if (pose.size() != binding_.jointNames.size())
        throw std::invalid_argument("MotionBricks seed pose joint count does not match the binding");
    history_.Fill(pose);
}

void GeneratedMotionPlayer::RecordPose(float deltaSeconds, const Pose& pose)
{
    if (pose.size() != binding_.jointNames.size())
        throw std::invalid_argument("MotionBricks recorded pose joint count does not match the binding");
    history_.Record(deltaSeconds, pose);
}

EncodedInputs GeneratedMotionPlayer::BuildGenerationInput(
    const std::vector<Pose>& targetPoses, const GenerationParams& params) const
{
    if (!history_.HasPose())
        throw std::logic_error("MotionBricks player history must be seeded");
    if (params.numTargetPoses < 1 || params.numTargetPoses > kTargetCapacity)
        throw std::invalid_argument("MotionBricks target pose count must be 1 to 4");
    if (targetPoses.size() < static_cast<size_t>(params.numTargetPoses))
        throw std::invalid_argument("MotionBricks target pose list is too short");
    if (params.transitionEarlyByFrames < 0)
        throw std::invalid_argument("MotionBricks transition lead frames cannot be negative");

    const std::array<Pose, kContextPoses> context = history_.SampleFour();
    const std::vector<Pose> selected(targetPoses.begin(),
        targetPoses.begin() + params.numTargetPoses);
    return EncodeSomaInputs(binding_, context, selected, params.encode);
}

void GeneratedMotionPlayer::CommitGeneration(DecodedMotion output,
    const GenerationParams& params, bool hardCutOnFirstSample)
{
    if (!history_.HasPose())
        throw std::logic_error("MotionBricks player history must be seeded");
    const size_t minimumFrames = static_cast<size_t>(ValidFrameCount(kMinTokenCount));
    if (output.frames.size() < minimumFrames ||
        output.frames.size() > static_cast<size_t>(kMaxValidFrames))
        throw std::invalid_argument(
            "MotionBricks generation must contain 18 to 58 frames");
    if (output.footContacts.size() != output.frames.size())
        throw std::invalid_argument("MotionBricks foot contact count does not match frames");
    for (const std::array<float, 4>& contacts : output.footContacts)
        for (float contact : contacts)
            if (!std::isfinite(contact) || contact < 0.0f || contact > 1.0f)
                throw std::invalid_argument(
                    "MotionBricks foot contact must be finite and in [0,1]");
    if (params.transitionEarlyByFrames < 0)
        throw std::invalid_argument("MotionBricks transition lead frames cannot be negative");

    for (Pose& pose : output.frames)
        pose = NormalizedPose(pose, binding_.jointNames.size(), "generated pose");
    output_ = std::move(output);
    transitionEarlyByFrames_ = static_cast<size_t>(params.transitionEarlyByFrames);
    playbackTime_ = 0.0f;
    // The official character samples frame 0 immediately after generation and sets
    // its position there. Using frame 0 as the baseline avoids turning an
    // uncorrected first-frame jump into root motion.
    previousSample_ = output_.frames.front();
    hasGeneration_ = true;
    pendingHardCut_ = hardCutOnFirstSample;
}

PlaybackFrame GeneratedMotionPlayer::Sample(float deltaSeconds)
{
    if (!hasGeneration_)
        throw std::logic_error("MotionBricks player has no generation");
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
        throw std::invalid_argument("MotionBricks playback delta must be finite and nonnegative");

    playbackTime_ += deltaSeconds;
    PlaybackFrame frame;
    frame.pose = SamplePose(output_.frames, playbackTime_);
    frame.footContacts = SampleFootContacts(output_.footContacts, playbackTime_);
    if (previousSample_.size() == frame.pose.size() && !frame.pose.empty())
    {
        frame.rootMotionDeltaMeters = frame.pose[0].position - previousSample_[0].position;
        frame.rootMotionYawDelta = NormalizeAngle(
            PoseHeading(binding_, frame.pose) - PoseHeading(binding_, previousSample_));
    }
    frame.hardCut = pendingHardCut_;
    pendingHardCut_ = false;
    frame.sourceTime = playbackTime_;
    previousSample_ = frame.pose;
    return frame;
}

PlaybackFrame GeneratedMotionPlayer::Step(float deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds <= 0.0f)
        throw std::invalid_argument("MotionBricks step delta must be finite and positive");
    PlaybackFrame frame = Sample(deltaSeconds);
    history_.Record(deltaSeconds, frame.pose);
    return frame;
}

float GeneratedMotionPlayer::PlaybackDuration() const
{
    if (!hasGeneration_ || output_.frames.empty()) return 0.0f;
    const int lastFrame = static_cast<int>(output_.frames.size()) - 1;
    const int endFrame = std::max(0, lastFrame - static_cast<int>(transitionEarlyByFrames_));
    return static_cast<float>(endFrame) / kSampleRate;
}

float GeneratedMotionPlayer::TimeToTransition() const
{
    return std::max(0.0f, PlaybackDuration() - playbackTime_);
}

Eigen::Vector3f GeneratedMotionPlayer::CurrentRootPosition() const
{
    if (!hasGeneration_)
        throw std::logic_error("MotionBricks player has no generation");
    return SamplePose(output_.frames, playbackTime_)[0].position;
}
}
