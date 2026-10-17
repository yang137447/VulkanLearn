#pragma once

#include <cmath>
#include <set>
#include <string>
#include <nlohmann/json.hpp>
#include "script/scriptComponentOverrides.h"
#include "script/scriptLogicComponents.h"

namespace VL
{
// Blueprint 作者数据在 scene 校验入口收口；网格组件转换后仍由正式 mesh loader 导入。
inline void ValidateScriptBlueprint(const nlohmann::json& blueprint)
{
    if (blueprint.value("type", "") != "blueprintInstance" || blueprint.contains("actorPath"))
        throw std::invalid_argument("Blueprint instances require blueprintInstance type and blueprintPath references");
    const bool assetInstance = blueprint.contains("blueprintPath");
    for (const char* field : {"id", assetInstance ? "blueprintPath" : "class"})
        if (!blueprint.contains(field) || !blueprint.at(field).is_string() ||
            blueprint.at(field).get<std::string>().empty() ||
            blueprint.at(field).get<std::string>().find('\0') != std::string::npos)
            throw std::invalid_argument(std::string("Blueprint requires non-empty string: ") + field);
    if (assetInstance && (blueprint.contains("class") || blueprint.contains("components") ||
        blueprint.contains("physics") || blueprint.contains("scriptComponents")))
        throw std::invalid_argument("Blueprint asset instances cannot redefine class or components");
    if (blueprint.contains("componentInstanceEditable"))
        throw std::invalid_argument("Component instance permissions belong to the Blueprint asset");
    if (blueprint.contains("componentOverrides"))
    {
        if (!assetInstance) throw std::invalid_argument("componentOverrides requires a Blueprint asset reference");
        ValidateScriptComponentOverrideShape(blueprint.at("componentOverrides"));
    }
    if (blueprint.contains("scriptComponentOverrides"))
    {
        if (!assetInstance) throw std::invalid_argument("scriptComponentOverrides requires a Blueprint asset reference");
        ValidateScriptLogicOverrideShape(blueprint.at("scriptComponentOverrides"));
    }
    if (blueprint.contains("scriptComponents")) ValidateScriptLogicComponents(blueprint.at("scriptComponents"));
    if (blueprint.at("id").get<std::string>().find_first_of("/\\|") != std::string::npos)
        throw std::invalid_argument("Blueprint id cannot contain path separators or |");
    if (blueprint.contains("enabled") && !blueprint.at("enabled").is_boolean())
        throw std::invalid_argument("Blueprint enabled must be boolean");
    if (!blueprint.contains("properties"))
        throw std::invalid_argument("Blueprint properties must be an object");
    ValidateScriptPropertyObject(blueprint.at("properties"));
    if (blueprint.contains("components")) ValidateScriptComponents(blueprint.at("components"));
    for (const char* field : {"position", "rotation", "scale"})
        if (blueprint.contains(field)) ValidateScriptVector(blueprint.at(field), std::string(field) == "scale");
    if (blueprint.contains("physics"))
    {
        ValidateScriptPhysics(blueprint.at("physics"));
        if (blueprint.at("physics").value("motion", "static") == "character" && blueprint.contains("rotation") &&
            (ScriptVector(blueprint, "rotation").x() != 0 || ScriptVector(blueprint, "rotation").z() != 0))
            throw std::invalid_argument("Character requires yaw-only rotation");
        // 根刚体与 Blueprint 共用刚性位姿；首版拒绝非统一/负缩放，组件视觉仍可独立缩放。
        if (blueprint.contains("scale"))
        {
            const auto scale = ScriptVector(blueprint, "scale");
            if (scale.minCoeff() <= 0 || scale.x() != scale.y() || scale.x() != scale.z())
                throw std::invalid_argument("Rigid body requires positive uniform Blueprint scale");
        }
    }
    ValidateScriptPhysicsComponents(blueprint);
}

inline std::string ScriptBlueprintMeshIdentity(const std::string& id) { return "__blueprint/" + id; }

inline nlohmann::json ScriptBlueprintMeshObject(const nlohmann::json& blueprint, size_t index = 0)
{
    const auto& component = blueprint.at("components").at(index);
    auto mesh = component;
    mesh["name"] = ScriptMeshIdentity(blueprint.at("id").get<std::string>(), component);
    // 先导入单位根，候选 World 再施加完整组件世界矩阵；旧单根格式继续保留原行为。
    for (const char* field : {"position", "rotation", "scale"})
        mesh[field] = !component.contains("id") ? blueprint.at(field) :
            nlohmann::json::array({std::string(field) == "scale" ? 1 : 0,
                std::string(field) == "scale" ? 1 : 0, std::string(field) == "scale" ? 1 : 0});
    return mesh;
}
} // namespace VL
