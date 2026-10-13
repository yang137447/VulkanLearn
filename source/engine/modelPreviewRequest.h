#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace VL
{

inline constexpr const char* ModelPreviewScenePath =
    "Maps/SC_model_preview/SC_model_preview.json";

// UI 与控制台共用值语义注入参数；实际资产校验、加载和失败回滚仍由 LoadWorld 事务负责。
inline nlohmann::json BuildModelPreviewObjects(const std::string& modelPath)
{
    return nlohmann::json::array({{
        {"name", "preview_model"},
        {"type", "mesh"},
        {"modelPath", modelPath},
        {"position", {0.0, 0.0, 0.0}},
        {"rotation", {0.0, 0.0, 0.0}},
        // 保留预览注入尺度；发布后相机按变换后的包围盒取景，不改资产 transform。
        {"scale", {43.0, 43.0, 43.0}}
    }});
}

// 复用正式球资产，只覆盖本次场景实例的材质；不生成临时 SM，也不写入 MI 草稿。
inline nlohmann::json BuildMaterialPreviewObjects(const std::string& materialPath)
{
    auto objects = BuildModelPreviewObjects("Common/Meshes/SM_material_preview_ball.json");
    objects[0]["name"] = "preview_material";
    objects[0]["materialOverride"] = materialPath;
    return objects;
}

} // namespace VL
