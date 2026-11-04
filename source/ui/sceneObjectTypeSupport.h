#pragma once

#include <string_view>

#include <nlohmann/json.hpp>

namespace VL
{

inline bool IsSupportedSceneObjectType(std::string_view type)
{
    // terrain 在 World 中仍落成 chunk mesh，但场景资产类型必须和普通对象一起
    // 通过 UI 选择器；否则该关卡不会进入 sceneOptions，LoadWorld 会拒绝打开。
    return type == "mesh" ||
        type == "terrain" ||
        type == "blueprintInstance" ||
        type == "directionalLight" ||
        type == "pointLight" ||
        type == "spotLight" ||
        type == "squareAreaLight" ||
        type == "camera" ||
        type == "environment";
}

inline bool HasSupportedSceneObjectTypes(const nlohmann::json& sceneJson)
{
    // 调用方已确认 objects 是数组；选择器只筛类型，完整资产合同仍由场景验证器负责。
    for (const nlohmann::json& objectJson : sceneJson["objects"])
    {
        if (!objectJson.is_object() ||
            !objectJson.contains("type") ||
            !objectJson["type"].is_string() ||
            !IsSupportedSceneObjectType(
                objectJson["type"].get<std::string>()))
        {
            return false;
        }
    }
    return true;
}

} // namespace VL
