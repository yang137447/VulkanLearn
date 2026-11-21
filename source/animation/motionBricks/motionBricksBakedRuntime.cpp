#include "motionBricksBakedRuntime.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <fstream>
#include <stdexcept>

namespace VL::Animation::MotionBricks
{
BakedMotion LoadBakedMotion(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::invalid_argument("Could not open baked MotionBricks motion: " + path.string());
    nlohmann::json json;
    input >> json;

    BakedMotion baked;
    baked.tokenCount = json.at("token_count").get<int>();
    baked.transitionEarlyByFrames = json.value("transition_early_by_frames", 0);
    ValidFrameCount(baked.tokenCount);
    if (baked.transitionEarlyByFrames < 0)
        throw std::invalid_argument("Baked MotionBricks transition lead frames cannot be negative");

    const auto alignment = json.at("alignment").get<std::vector<float>>();
    if (alignment.size() != 7)
        throw std::invalid_argument("Baked MotionBricks alignment must have 7 values");
    baked.alignment.position = Eigen::Vector3f(alignment[0], alignment[1], alignment[2]);
    baked.alignment.rotation = Eigen::Quaternionf(alignment[6], alignment[3],
        alignment[4], alignment[5]);
    baked.alignment = Normalized(baked.alignment);

    const auto rows = json.at("pred_global_motions");
    if (!rows.is_array() || rows.size() != 64)
        throw std::invalid_argument("Baked MotionBricks prediction must contain 64 frames");
    baked.prediction.resize(64);
    for (size_t row = 0; row < rows.size(); ++row)
    {
        const auto values = rows[row].get<std::vector<float>>();
        if (values.size() != 366)
            throw std::invalid_argument("Baked MotionBricks prediction row must have 366 values");
        for (size_t value = 0; value < values.size(); ++value)
        {
            if (!std::isfinite(values[value]))
                throw std::invalid_argument("Baked MotionBricks prediction contains non-finite data");
            baked.prediction[row][value] = values[value];
        }
    }
    return baked;
}

BakedMotionRuntime::BakedMotionRuntime(std::shared_ptr<const SomaSkeletonAsset> source,
    const SkeletonMesh& target, const RetargetProfile& profile, BakedMotion baked)
    : source_(std::move(source)),
      target_(target),
      retarget_(source_->mesh, target, profile),
      player_(source_->binding),
      decoded_(DecodeSomaOutput(source_->binding, baked.alignment, Transform{},
          baked.prediction, baked.tokenCount))
{
    params_.transitionEarlyByFrames = baked.transitionEarlyByFrames;
    if (decoded_.frames.empty())
        throw std::invalid_argument("Baked MotionBricks decode produced no frames");
    player_.SeedHistory(decoded_.frames.front());
    player_.CommitGeneration(decoded_, params_);
}

ExternalPoseFrame BakedMotionRuntime::Tick(float deltaSeconds)
{
    if (!std::isfinite(deltaSeconds) || deltaSeconds < 0.0f)
        throw std::invalid_argument("Baked MotionBricks delta must be finite and nonnegative");

    PlaybackFrame frame;
    if (deltaSeconds > 0.0f)
    {
        frame = player_.Step(deltaSeconds);
        const float duration = static_cast<float>(player_.FrameCount() - 1) / kSampleRate;
        if (player_.PlaybackTime() >= duration)
        {
            // The probe loops the same baked generation. The generation boundary is a
            // hard cut so the caller does not integrate the loop jump as root motion.
            player_.CommitGeneration(decoded_, params_, true);
            frame = player_.Sample(0.0f);
        }
    }
    else
    {
        frame = player_.Sample(0.0f);
    }

    ExternalPoseFrame output;
    output.locals = RetargetInPlace(*source_, target_, retarget_, frame.pose);
    output.rootMotionDelta = frame.rootMotionDeltaMeters * 100.0f;
    output.rootMotionYawDelta = frame.rootMotionYawDelta;
    output.hardCut = frame.hardCut;
    output.deltaSeconds = deltaSeconds;
    output.sourceTime = frame.sourceTime;
    return output;
}
}
