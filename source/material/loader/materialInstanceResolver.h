#pragma once

#include <string_view>
#include <nlohmann/json.hpp>
#include "../materialAssetTypes.h"

// Resolves disk material assets into the effective material-instance JSON used by runtime loading.
// It owns the M_ + MI_ merge policy: shaderName comes from the M_ file location, render states
// and macros start from M_ defaults, and MI_ may only override declared fields. It does not create
// Vulkan materials, textures, descriptors, or shader modules; RendererMaterialLoader
// and downstream renderer systems do that.
class MaterialInstanceResolver
{
public:
    // Loads and validates an M_*.json material definition from a project-relative path.
    static nlohmann::json LoadDefinition(std::string_view materialPath);

    // Combines a parsed MI_*.json with its referenced M_*.json into one runtime-facing JSON object.
    static MaterialInstanceResolveResult Resolve(
        std::string_view materialInstancePath,
        const nlohmann::json& materialInstanceJson);
};
