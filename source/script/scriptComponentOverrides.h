#pragma once

#include <algorithm>
#include <set>
#include <string>
#include <vector>
#include "script/scriptComponents.h"

namespace VL
{
// 共享作者权限表供资产校验、Scene 合成和只读 UI 使用；不允许实例改变组件身份或父链。
inline std::vector<std::string> ScriptComponentInstanceFields(const nlohmann::json& component)
{
    std::vector<std::string> result{"position", "rotation", "scale"};
    const auto type = component.at("type").get<std::string>();
    if (type == "mesh") result.insert(result.end(), {"modelPath", "materialOverride"});
    if (type == "camera") result.insert(result.end(), {"fov", "near_clip", "far_clip", "autoActivate", "priority"});
    if (type == "springArm") result.insert(result.end(), {"targetArmLength", "socketOffset", "probeRadius",
        "doCollisionTest", "collisionMask", "enableCameraLag", "cameraLagSpeed", "cameraLagMaxDistance",
        "enableCameraRotationLag", "cameraRotationLagSpeed", "usePawnControlRotation", "inheritPitch",
        "inheritYaw", "inheritRoll", "absoluteRotation", "rotationOffset", "drawDebug", "targetOffset",
        "useCameraLagSubstepping", "cameraLagMaxTimeStep"});
    if (IsScriptLight(type)) result.insert(result.end(), {"color", "intensity", "visible"});
    if (type == "spotLight") result.insert(result.end(), {"coneAngleInner", "coneAngleOuter"});
    return result;
}

inline nlohmann::json ScriptComponentFieldValue(const nlohmann::json& component, const std::string& field)
{
    if (component.contains(field)) return component.at(field);
    if (field == "position" || field == "rotation") return nlohmann::json::array({0, 0, 0});
    if (field == "scale" || field == "color") return nlohmann::json::array({1, 1, 1});
    if (field == "autoActivate") return false;
    if (field == "priority") return 0;
    if (field == "visible") return true;
    if (field == "fov") return 60;
    if (field == "near_clip") return 1;
    if (field == "far_clip") return 10000;
    if (field == "targetArmLength") return 300;
    if (field == "socketOffset" || field == "rotationOffset" || field == "targetOffset") return nlohmann::json::array({0, 0, 0});
    if (field == "probeRadius") return 12;
    if (field == "doCollisionTest" || field == "inheritPitch" || field == "inheritYaw" || field == "inheritRoll")
        return true;
    if (field == "collisionMask") return 65535;
    if (field == "enableCameraLag" || field == "enableCameraRotationLag" || field == "usePawnControlRotation" ||
        field == "absoluteRotation" || field == "drawDebug") return false;
    if (field == "cameraLagSpeed" || field == "cameraRotationLagSpeed") return 10;
    if (field == "cameraLagMaxDistance") return 0;
    if (field == "useCameraLagSubstepping") return true;
    if (field == "cameraLagMaxTimeStep") return 1.0f / 60;
    if (IsScriptLightNumber(field)) return ScriptLightDefault(field);
    return "";
}

inline bool ScriptComponentAllowsInstance(const nlohmann::json& permissions,
    const std::string& component, const std::string& field)
{
    if (!permissions.contains(component)) return false;
    const auto& fields = permissions.at(component);
    return std::find(fields.begin(), fields.end(), field) != fields.end();
}

inline void ValidateScriptComponentOverrideShape(const nlohmann::json& overrides)
{
    if (!overrides.is_object() || overrides.size() > 128)
        throw std::invalid_argument("componentOverrides requires at most 128 component objects");
    for (const auto& entry : overrides.items())
    {
        if (entry.key().empty() || entry.key().find_first_of("/\\|") != std::string::npos ||
            entry.key().find('\0') != std::string::npos || !entry.value().is_object() || entry.value().size() > 32)
            throw std::invalid_argument("Invalid componentOverrides component");
        for (const auto& field : entry.value().items())
        {
            const auto& name = field.key();
            const auto& value = field.value();
            if (name == "position" || name == "rotation" || name == "scale" || name == "color" || name == "socketOffset" ||
                name == "targetOffset" || name == "rotationOffset")
                ValidateScriptVector(value, name == "scale");
            else if (name == "modelPath" || name == "materialOverride")
            {
                if (!value.is_string() || value.get<std::string>().find('\0') != std::string::npos ||
                    (name == "modelPath" && value.get<std::string>().empty()))
                    throw std::invalid_argument("Component path override requires a string");
            }
            else if (name == "autoActivate" || name == "visible" || name == "doCollisionTest" ||
                name == "enableCameraLag" || name == "enableCameraRotationLag" || name == "usePawnControlRotation" ||
                name == "inheritPitch" || name == "inheritYaw" || name == "inheritRoll" ||
                name == "absoluteRotation" || name == "drawDebug" || name == "useCameraLagSubstepping")
            {
                if (!value.is_boolean()) throw std::invalid_argument("Component override requires a boolean");
            }
            else if (name == "fov" || name == "near_clip" || name == "far_clip" || name == "targetArmLength" ||
                name == "probeRadius" || name == "collisionMask" || name == "priority" ||
                name == "cameraLagSpeed" || name == "cameraLagMaxDistance" || name == "cameraRotationLagSpeed" ||
                name == "cameraLagMaxTimeStep" || IsScriptLightNumber(name))
            {
                if (!value.is_number() || !std::isfinite(value.get<float>()))
                    throw std::invalid_argument("Component override requires a finite float");
            }
            else throw std::invalid_argument("Component override cannot modify identity, parent or unsupported fields");
        }
    }
}

inline void ValidateScriptComponentPermissions(const nlohmann::json& components,
    const nlohmann::json& permissions)
{
    if (!permissions.is_object() || permissions.size() > 128)
        throw std::invalid_argument("componentInstanceEditable requires at most 128 component entries");
    for (const auto& entry : permissions.items())
    {
        const nlohmann::json* component = nullptr;
        for (const auto& candidate : components)
            if (ScriptComponentId(candidate) == entry.key()) component = &candidate;
        if (!component || !entry.value().is_array() || entry.value().size() > 32)
            throw std::invalid_argument("Unknown component or invalid componentInstanceEditable fields");
        const auto supported = ScriptComponentInstanceFields(*component);
        std::set<std::string> unique;
        for (const auto& field : entry.value())
        {
            if (!field.is_string() || std::find(supported.begin(), supported.end(), field.get<std::string>()) == supported.end() ||
                !unique.insert(field.get<std::string>()).second)
                throw std::invalid_argument("componentInstanceEditable requires unique supported field names");
        }
    }
}

inline void ApplyScriptComponentOverrides(nlohmann::json& components,
    const nlohmann::json& permissions, const nlohmann::json& overrides)
{
    ValidateScriptComponentOverrideShape(overrides);
    for (const auto& entry : overrides.items())
    {
        nlohmann::json* component = nullptr;
        for (auto& candidate : components)
            if (ScriptComponentId(candidate) == entry.key()) component = &candidate;
        if (!component) throw std::invalid_argument("Unknown component override id: " + entry.key());
        for (const auto& field : entry.value().items())
        {
            if (!ScriptComponentAllowsInstance(permissions, entry.key(), field.key()))
                throw std::invalid_argument("Component field is not Instance Editable: " + entry.key() + "." + field.key());
            // 空材质地址明确清除资产的可选绑定，其余缺省字段通过删除 Scene 覆写来继承。
            if (field.key() == "materialOverride" && field.value() == "") component->erase(field.key());
            else (*component)[field.key()] = field.value();
        }
    }
    ValidateScriptComponents(components);
}
} // namespace VL
