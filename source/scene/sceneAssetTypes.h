#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <Eigen/Dense>

#include "../mesh/loader/common/meshAssetLoader.h"

// One validated scene asset object entry used by WorldBuilder and renderer loaders.
// Mesh objects carry an already built mesh load request so runtime code does
// not rebuild it.
struct SceneAssetObjectPlan
{
    size_t objectIndex = 0;
    std::string objectName;
    std::string objectType;
    std::optional<MeshAssetLoadRequest> meshLoadRequest;
};

// Scene-level validated loading plan consumed by WorldLoader, WorldBuilder, and
// renderer resource loaders.
struct SceneAssetBuildPlan
{
    std::string scenePath;
    std::vector<SceneAssetObjectPlan> objectPlans;
};

// One renderable mesh section expanded into World-owned object data. Renderer
// resource loading produces these entries while creating renderable/material
// resources; WorldBuilder consumes only this pure data plan, not renderer cache
// binding tables.
struct MeshObjectBuildPlan
{
    std::string objectName;
    std::string debugName;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    Eigen::Vector3f localBoundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f localBoundsMax = Eigen::Vector3f::Zero();
    std::string meshKey;
    std::string materialKey;
    std::string materialInstanceKey;
    std::string speedTreeWindProfileKey;
};
