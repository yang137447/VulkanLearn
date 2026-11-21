#pragma once

#include "animation/motionBricks/motionBricksBakedRuntime.h"
#include "animation/motionBricks/motionBricksLiveRuntime.h"
#include "animation/motionBricks/motionBricksRuntimeInterface.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace VL::Animation::MotionBricks
{
struct MotionBricksMeshConfig
{
    std::string mode = "baked";
    std::string skeleton;
    std::string manifest;
    std::string retarget;
    std::string baked;
    std::string model;
    std::string provider = "auto";
    std::string locomotionTargets;
    int transitionEarlyByFrames = 6;
};

// Reads the optional motionBricks object on a scene mesh component. Returns false
// when the component has no motionBricks field; a present but malformed field sets
// error and also returns false.
bool TryReadMotionBricksConfig(const nlohmann::json& component,
    MotionBricksMeshConfig& config, std::string& error);

// Resolves paths under resourceRoot, loads/caches the SOMA source skeleton, and
// builds the selected baked or live runtime against the target SkeletonMesh.
std::shared_ptr<IMotionBricksRuntime> CreateMotionBricksRuntime(
    const MotionBricksMeshConfig& config,
    const std::filesystem::path& resourceRoot,
    const SkeletonMesh& target,
    std::map<std::string, std::shared_ptr<const SomaSkeletonAsset>>& skeletonCache);

// Kept for callers that explicitly require the baked path.
std::shared_ptr<BakedMotionRuntime> CreateBakedRuntime(
    const MotionBricksMeshConfig& config,
    const std::filesystem::path& resourceRoot,
    const SkeletonMesh& target,
    std::map<std::string, std::shared_ptr<const SomaSkeletonAsset>>& skeletonCache);
}
