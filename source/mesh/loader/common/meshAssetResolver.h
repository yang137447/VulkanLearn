#pragma once

#include <string_view>
#include <nlohmann/json.hpp>
#include "../../meshAssetTypes.h"

// Resolves raw mesh asset JSON into the effective mesh asset JSON consumed by validation.
// The first version keeps the parsed SM_*.json unchanged, but this is the boundary for future
// defaults, presets, or platform overrides. It does not read model files, import geometry, create
// material instances, or allocate Vulkan resources.
class MeshAssetResolver
{
public:
    static MeshAssetResolveResult Resolve(
        std::string_view meshAssetPath,
        const nlohmann::json& meshAssetJson);
};
