#pragma once

#include "motionBricksContract.h"
#include "motionBricksMath.h"
#include "animation/animationAssets.h"
#include "animation/animationSkeleton.h"
#include "animation/animationRetarget.h"

#include <filesystem>
#include <memory>

namespace VL::Animation::MotionBricks
{
// SOMA SK JSON converted into the engine's runtime skeleton. The mesh has no
// sections; it is a retarget source, not a renderable mesh. Positions are cm.
struct SomaSkeletonAsset
{
    SkeletonAsset skeleton;
    SkeletonMesh mesh;
    SkeletonBinding binding;
    ModelContract contract;
};

// Builds the ozz runtime and SkeletonMesh from a validated SK asset. The mesh is
// reordered to ozz's depth-first joint order; names and rest locals are preserved.
SkeletonMesh BuildSkeletonMesh(const SkeletonAsset& skeleton);

// Reads SK and manifest, builds the mesh, and binds the 30 SOMA model joints to
// its 77-joint hierarchy. The binding reference is in meters.
std::shared_ptr<const SomaSkeletonAsset> LoadSomaSkeleton(
    const std::filesystem::path& skeletonPath,
    const std::filesystem::path& manifestPath);

// MotionBricks poses are meters. The engine animation pipeline is centimeters.
LocalPose PoseToLocalPoseCm(const Pose& pose);
Pose LocalPoseToPoseMeters(const LocalPose& pose);

struct LocomotionTarget
{
    float speed = 0;
    std::array<Pose, kContextPoses> poses;
};

std::vector<LocomotionTarget> LoadLocomotionTargets(
    const std::filesystem::path& path, const SkeletonBinding& binding);
float PoseHeading(const SkeletonBinding& binding, const Pose& pose);
LocalPose RetargetInPlace(const SomaSkeletonAsset& source,
    const SkeletonMesh& target, const CompiledRetarget& retarget, Pose pose);
}
