#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace VL
{

inline constexpr const char* ParticlePreviewScenePath =
    "Maps/SC_particle_preview/SC_particle_preview.json";

// 资源编辑器只注入一个 particleSystem 对象；相机/环境/灯光来自固定预览场景。
inline nlohmann::json BuildParticlePreviewObjects(
    const std::string& particleAssetPath)
{
    return nlohmann::json::array({{
        {"name", "preview_particle"},
        {"type", "particleSystem"},
        {"particlePath", particleAssetPath},
        {"position", {0.0, 0.0, 0.0}},
        {"rotation", {0.0, 0.0, 0.0}},
        {"scale", {1.0, 1.0, 1.0}}
    }});
}

} // namespace VL
