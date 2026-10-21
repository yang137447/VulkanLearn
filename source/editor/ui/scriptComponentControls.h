#pragma once

#include "editor/ui/editorSceneActorTypes.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/views/viewWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "script/scriptComponentOverrides.h"

namespace VL::EditorUi
{
inline std::string ScriptComponentOptions()
{
    return UiWidgets::Option("scene", "Scene") + UiWidgets::Option("mesh", "Mesh") +
        UiWidgets::Option("camera", "Camera") + UiWidgets::Option("springArm", "Spring Arm") +
        UiWidgets::Option("pointLight", "Point Light") +
        UiWidgets::Option("spotLight", "Spot Light") + UiWidgets::Option("directionalLight", "Directional Light");
}

inline nlohmann::json NewScriptComponent(const std::string& type)
{
    nlohmann::json node{{"type", type}};
    if (IsScriptLight(type)) node["intensityUnit"] = type == "directionalLight" ? "lux" : "cd";
    return node;
}

inline std::string ComponentControlId(size_t index, const std::string& field, size_t axis = 0,
    const std::string& prefix = "script-")
{
    return prefix + "component-" + std::to_string(index) + "-" + field + "-" + std::to_string(axis);
}

inline std::string ComponentInstanceControlId(size_t index, const std::string& field,
    const std::string& prefix = "script-")
{
    return prefix + "component-instance-" + std::to_string(index) + "-" + field;
}

inline std::string ComponentResetControlId(size_t index, const std::string& field)
{
    return "script-component-reset-" + std::to_string(index) + "-" + field;
}

inline std::string ScriptComponentCollisionControls(size_t index, const std::string& prefix)
{
    using namespace UiWidgets;
    auto body = PropertyControlRow("Collision", Select(ComponentControlId(index, "collisionShape", 0, prefix),
        {prefix + "component-collisionShape", index},
        Option("none", "None") + Option("box", "Box") + Option("sphere", "Sphere") + Option("capsule", "Capsule")));
    std::vector<NumericComponent> extents;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        NumericComponent control;
        control.id = ComponentControlId(index, "collisionHalfExtents", axis, prefix);
        control.label = std::string(1, "XYZ"[axis]);
        control.value = "50";
        control.change = {prefix + "component-collisionHalfExtents", index, axis};
        extents.push_back(std::move(control));
    }
    body += "<div id=\"" + ComponentControlId(index, "collisionHalfExtents-row", 0, prefix) + "\">" +
        PropertyControlRow("Half Extents (cm)", NumericComponents(extents)) + "</div>";
    for (const auto& field : std::vector<std::pair<std::string, std::string>>{
        {"collisionRadius", "Radius (cm)"}, {"collisionHalfHeight", "Half Height (cm)"}})
        body += "<div id=\"" + ComponentControlId(index, field.first + "-row", 0, prefix) + "\">" +
            PropertyControlRow(field.second, Input(ComponentControlId(index, field.first, 0, prefix),
                {prefix + "component-" + field.first, index}, field.first == "collisionRadius" ? "30" : "60")) + "</div>";
    return body;
}

inline std::string ScriptComponentProperty(const EditorSceneActorSnapshot& actor, size_t index,
    const std::string& field, const std::string& label, std::string controls,
    const std::string& prefix, bool permissions)
{
    std::string permission;
    if (permissions)
        permission = "<label class=\"script-component-instance\" title=\"Allow scene instances to override " + UiWidgets::Text(field) +
            "\"><input id=\"" + ComponentInstanceControlId(index, field, prefix) +
            "\" type=\"checkbox\" data-action=\"" + prefix + "component-instance\" data-index=\"" +
            std::to_string(index) + "\" data-field=\"" + UiWidgets::Text(field) + "\"/><span>Instance Editable</span></label>";
    else if (!actor.scriptBlueprintPath.empty())
        controls += "<button id=\"" + ComponentResetControlId(index, field) +
            "\" class=\"browser-icon\" data-action=\"script-component-reset\" data-index=\"" +
            std::to_string(index) + "\" data-field=\"" + UiWidgets::Text(field) +
            "\" title=\"Restore inherited component value\"><img src=\"icons/refresh.png\"/></button>";
    return UiWidgets::PropertyControlRow(label, controls) + permission;
}

// 只消费冻结 JSON 并生成控件地址；父链与矩阵的正确性由 GT 作者入口校验。
inline std::string ScriptComponentControls(const EditorSceneActorSnapshot& actor,
    const std::string& parent = {}, uint32_t depth = 0, const std::string& prefix = "script-",
    bool separateDetails = false)
{
    std::string markup;
    for (size_t index = 0; index < actor.scriptComponents.size(); ++index)
    {
        const auto& node = actor.scriptComponents[index];
        if (node.value("parent", "") != parent) continue;
        const auto id = node.value("id", "root");
        UiWidgets::TreeRowData row;
        row.label = id;
        row.depth = depth;
        row.icon = node.at("type") == "mesh" ? "icons/mesh.png" :
            node.at("type") == "camera" ? "icons/camera.png" : "icons/blueprint.png";
        // 资产编辑页在左侧独立选组件；Scene Details 保留原有父链内联表单。
        if (separateDetails)
            markup += "<div id=\"" + prefix + "component-details-" + std::to_string(index) + "\">";
        else
            markup += UiWidgets::TreeRow(row);
        markup += "<div class=\"script-component-actions\"><button id=\"" + prefix + "component-remove-" +
            std::to_string(index) + "\" class=\"browser-icon\" data-action=\"" + prefix + "component-remove\" data-index=\"" +
            std::to_string(index) + "\" title=\"Remove component and descendants\"><img src=\"icons/trash.png\"/></button></div>";
        std::string options = UiWidgets::Option("", "Actor");
        for (const auto& candidate : actor.scriptComponents)
            if (candidate.value("id", "root") != id)
                options += UiWidgets::Option(candidate.value("id", "root"), candidate.value("id", "root"));
        markup += UiWidgets::PropertyControlRow("Parent", UiWidgets::Select(
            ComponentControlId(index, "parent", 0, prefix), {prefix + "component-parent", index}, options));
        if (node.at("type") == "mesh")
            markup += ScriptComponentProperty(actor, index, "modelPath", "Mesh", UiWidgets::Input(
                ComponentControlId(index, "modelPath", 0, prefix), {prefix + "component-modelPath", index},
                node.value("modelPath", "")), prefix, separateDetails);
        if (node.at("type") == "mesh")
            markup += ScriptComponentProperty(actor, index, "materialOverride", "Material Override", UiWidgets::Input(
                ComponentControlId(index, "materialOverride", 0, prefix), {prefix + "component-materialOverride", index},
                node.value("materialOverride", "")), prefix, separateDetails);
        if (node.at("type") == "camera")
        {
            for (const auto& field : std::vector<std::pair<std::string, float>>{
                {"fov", 60}, {"near_clip", 1}, {"far_clip", 10000}, {"priority", 0}})
                markup += ScriptComponentProperty(actor, index, field.first, field.first, UiWidgets::Input(
                    ComponentControlId(index, field.first, 0, prefix), {prefix + "component-" + field.first, index},
                    node.contains(field.first) ? node.at(field.first).dump() : std::to_string(field.second)),
                    prefix, separateDetails);
            markup += ScriptComponentProperty(actor, index, "autoActivate", "Auto Activate",
                UiWidgets::Select(ComponentControlId(index, "autoActivate", 0, prefix), {prefix + "component-autoActivate", index},
                    UiWidgets::Option("false", "False") + UiWidgets::Option("true", "True")), prefix, separateDetails);
        }
        if (node.at("type") == "springArm")
        {
            for (const auto& field : std::vector<std::pair<std::string, float>>{
                {"targetArmLength", 300}, {"probeRadius", 12}, {"collisionMask", 65535},
                {"cameraLagSpeed", 10}, {"cameraLagMaxDistance", 0}, {"cameraRotationLagSpeed", 10},
                {"cameraLagMaxTimeStep", 1.0f / 60}})
                markup += ScriptComponentProperty(actor, index, field.first, field.first, UiWidgets::Input(
                    ComponentControlId(index, field.first, 0, prefix), {prefix + "component-" + field.first, index},
                    node.contains(field.first) ? node.at(field.first).dump() : std::to_string(field.second)),
                    prefix, separateDetails);
            for (const auto& vector : std::vector<std::pair<std::string, std::string>>{
                {"socketOffset", "Socket Offset (cm)"}, {"targetOffset", "Target Offset (world cm)"},
                {"rotationOffset", "Rotation Offset (deg)"}})
            {
                std::vector<UiWidgets::NumericComponent> offsets;
                for (uint32_t axis = 0; axis < 3; ++axis)
                {
                    UiWidgets::NumericComponent control;
                    control.id = ComponentControlId(index, vector.first, axis, prefix);
                    control.label = std::string(1, "XYZ"[axis]);
                    control.value = node.contains(vector.first) ? node.at(vector.first)[axis].dump() : "0";
                    control.change = {prefix + "component-" + vector.first, index, axis};
                    offsets.push_back(std::move(control));
                }
                markup += ScriptComponentProperty(actor, index, vector.first, vector.second,
                    UiWidgets::NumericComponents(offsets), prefix, separateDetails);
            }
            for (const auto& toggle : std::vector<std::pair<std::string, std::string>>{
                {"doCollisionTest", "Do Collision Test"}, {"enableCameraLag", "Enable Camera Lag"},
                {"enableCameraRotationLag", "Enable Camera Rotation Lag"},
                {"useCameraLagSubstepping", "Camera Lag Substepping"},
                {"usePawnControlRotation", "Use Pawn Control Rotation"},
                {"inheritPitch", "Inherit Pitch"}, {"inheritYaw", "Inherit Yaw"},
                {"inheritRoll", "Inherit Roll"}, {"absoluteRotation", "Absolute Rotation"},
                {"drawDebug", "Draw Debug"}})
                markup += ScriptComponentProperty(actor, index, toggle.first, toggle.second,
                    UiWidgets::Select(ComponentControlId(index, toggle.first, 0, prefix),
                        {prefix + "component-" + toggle.first, index},
                        UiWidgets::Option("false", "False") + UiWidgets::Option("true", "True")),
                    prefix, separateDetails);
        }
        if (IsScriptLight(node.at("type")))
        {
            markup += UiWidgets::PropertyInfo("Intensity Unit", node.at("type") == "directionalLight" ? "lux" : "cd");
            for (const char* field : {"intensity", "coneAngleInner", "coneAngleOuter"})
            {
                if (node.at("type") != "spotLight" && std::string(field) != "intensity") continue;
                markup += ScriptComponentProperty(actor, index, field, field, UiWidgets::Input(
                    ComponentControlId(index, field, 0, prefix), {prefix + "component-" + field, index},
                    node.value(field, nlohmann::json(ScriptLightDefault(field))).dump()), prefix, separateDetails);
            }
            markup += ScriptComponentProperty(actor, index, "visible", "Visible",
                UiWidgets::Select(ComponentControlId(index, "visible", 0, prefix), {prefix + "component-visible", index},
                    UiWidgets::Option("false", "False") + UiWidgets::Option("true", "True")), prefix, separateDetails);
        }
        for (const char* field : {"position", "rotation", "scale", "color"})
        {
            if (std::string(field) == "color" && !IsScriptLight(node.at("type"))) continue;
            std::vector<UiWidgets::NumericComponent> controls;
            for (size_t axis = 0; axis < 3; ++axis)
            {
                UiWidgets::NumericComponent control;
                control.id = ComponentControlId(index, field, axis, prefix);
                control.label = std::string(1, std::string(field) == "color" ? "RGB"[axis] : "XYZ"[axis]);
                control.value = node.contains(field) ? node.at(field)[axis].dump() :
                    (std::string(field) == "scale" || std::string(field) == "color" ? "1" : "0");
                control.change = {prefix + "component-" + field, index, static_cast<uint32_t>(axis)};
                controls.push_back(std::move(control));
            }
            markup += "<div class=\"actor-property-row\">" +
                ScriptComponentProperty(actor, index, field,
                    std::string(field) == "color" ? "Linear Color" : std::string("Local ") + field,
                    UiWidgets::NumericComponents(controls), prefix, separateDetails) + "</div>";
        }
        markup += ScriptComponentCollisionControls(index, prefix);
        if (separateDetails) markup += "</div>";
        markup += ScriptComponentControls(actor, id, depth + 1, prefix, separateDetails);
    }
    return markup;
}
} // namespace VL::EditorUi
