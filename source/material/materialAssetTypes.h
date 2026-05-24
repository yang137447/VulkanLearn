#pragma once

#include <string>
#include <nlohmann/json.hpp>

// Result of resolving one MI_*.json against its referenced M_*.json.
// The effective JSON is intentionally shaped for existing reflection-based material loading.
struct MaterialInstanceResolveResult
{
    // Raw validated M_*.json contents.
    nlohmann::json materialJson;

    // M_ defaults plus MI_ overrides, with derived shaderName and shadingModel metadata.
    nlohmann::json effectiveMaterialInstanceJson;

    // Project-relative path to the referenced M_*.json.
    std::string materialPath;

    // Shader pair name inferred from the M_ file location, for example shader/glsl/pass/M_sky.json -> pass/sky.
    std::string shaderName;
};
