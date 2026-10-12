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
        // 保留 previewmodel 已验证的尺度：单位球在约 630 cm 外的预览相机下可见。
        // 这不是按任意模型包围盒自动取景；name/type 同样必须满足场景对象校验契约。
        {"scale", {43.0, 43.0, 43.0}}
    }});
}

} // namespace VL
