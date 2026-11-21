#pragma once

#include "animation/externalPose.h"

#include <cstddef>
#include <string>

namespace VL::Animation::MotionBricks
{
// Common GT-facing surface for baked and live MotionBricks sources. World owns the
// runtime and submits the returned frame to the target SkeletalPlayback.
class IMotionBricksRuntime
{
public:
    virtual ~IMotionBricksRuntime() = default;
    virtual ExternalPoseFrame Tick(float deltaSeconds) = 0;
    virtual size_t FrameCount() const = 0;
    virtual size_t TargetJointCount() const = 0;
    virtual const char* Mode() const = 0;
    virtual std::string ProviderName() const = 0;
    virtual float LastGenerationMs() const = 0;
    // GT-facing steering intent. directionX/Y is the fixed ground move vector
    // (+X/-Z for right/forward), speedScale scales the generated target stride. Baked
    // playback ignores it; live generation turns it into the next target pose.
    virtual void SetTargetMotion(float /*directionX*/, float /*directionY*/,
        float /*speedScale*/) {}
};
}
