#pragma once

#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/input/inputWidgets.h"

namespace VL::EditorUi
{
// 冻结的物理配置驱动相同的行表；面板只产生字段请求，GT 用完整候选校验组合。
struct ScriptPhysicsField
{
    const char* name;
    const char* label;
    float defaultValue;
};

inline const std::vector<ScriptPhysicsField>& ScriptPhysicsFields()
{
    static const std::vector<ScriptPhysicsField> fields{
        {"radius", "Radius (cm)", 30}, {"halfHeight", "Half Height (cm)", 60},
        {"mass", "Mass (kg)", 1}, {"friction", "Friction", 0.5f},
        {"restitution", "Restitution", 0.2f}, {"stepHeight", "Step Height (cm)", 30},
        {"jumpSpeed", "Jump Speed (cm/s)", 500}, {"maxSlopeAngle", "Max Slope (deg)", 45},
        {"collisionLayer", "Collision Layer", 0}, {"collisionMask", "Collision Mask", 65535}};
    return fields;
}

inline bool ScriptPhysicsFieldVisible(const nlohmann::json& physics, const std::string& field)
{
    if (physics.is_null()) return false;
    const auto shape = physics.value("shape", "box");
    if (field == "radius") return shape == "sphere" || shape == "capsule";
    if (field == "halfHeight") return shape == "capsule";
    if (field == "stepHeight" || field == "jumpSpeed" || field == "maxSlopeAngle")
        return physics.value("motion", "static") == "character";
    return true;
}

inline std::string ScriptConstraintControlId(const std::string& prefix, size_t index, const std::string& field, size_t axis = 0)
{
    return prefix + "physics-constraint-" + std::to_string(index) + "-" + field + "-" + std::to_string(axis);
}

inline std::string ScriptConstraintControls(const std::string& prefix, const nlohmann::json& physics)
{
    using namespace UiWidgets;
    auto body = PropertyControlRow("Constraint Target", Input(prefix + "physics-constraint-target",
        {}, "") + "<button id=\"" + prefix + "physics-constraint-add\" class=\"browser-icon\" data-action=\"" +
        prefix + "physics-constraint-add\" title=\"Add fixed constraint\"><img src=\"icons/plus.png\"/></button>");
    const auto joints = physics.value("constraints", nlohmann::json::array());
    for (size_t index = 0; index < joints.size(); ++index)
    {
        const auto& joint = joints[index];
        body += PropertyControlRow("Constraint " + joint.at("id").get<std::string>(),
            "<button id=\"" + prefix + "physics-constraint-remove-" + std::to_string(index) +
            "\" class=\"browser-icon\" data-action=\"" + prefix + "physics-constraint-remove\" data-index=\"" +
            std::to_string(index) + "\" title=\"Remove constraint\"><img src=\"icons/trash.png\"/></button>");
        body += PropertyControlRow("Type", Select(ScriptConstraintControlId(prefix, index, "type"),
            {prefix + "physics-constraint-type", index}, Option("fixed", "Fixed") + Option("hinge", "Hinge")));
        body += PropertyControlRow("Target", Input(ScriptConstraintControlId(prefix, index, "target"),
            {prefix + "physics-constraint-target", index}, joint.at("target").get<std::string>()));
        for (const char* field : {"anchor", "axis"})
        {
            if (std::string(field) == "axis" && joint.at("type") != "hinge") continue;
            if (std::string(field) == "axis")
            {
                std::string options;
                const std::vector<std::pair<std::string, std::string>> axes{
                    {"[1,0,0]", "X"}, {"[0,1,0]", "Y"}, {"[0,0,1]", "Z"},
                    {"[-1,0,0]", "-X"}, {"[0,-1,0]", "-Y"}, {"[0,0,-1]", "-Z"}};
                for (const auto& axis : axes) options += Option(axis.first, axis.second);
                options += Option(joint.at("axis").dump(), "Current Axis");
                body += PropertyControlRow("Axis", Select(ScriptConstraintControlId(prefix, index, "axis"),
                    {prefix + "physics-constraint-axis", index}, options));
                continue;
            }
            std::vector<NumericComponent> controls;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                NumericComponent control;
                control.id = ScriptConstraintControlId(prefix, index, field, axis);
                control.label = std::string(1, "XYZ"[axis]);
                control.value = joint.at(field)[axis].dump();
                control.change = {prefix + "physics-constraint-" + field, index, axis};
                controls.push_back(std::move(control));
            }
            body += PropertyControlRow(std::string(field) == "anchor" ? "Anchor (cm)" : "Axis",
                NumericComponents(controls));
        }
        if (joint.at("type") == "hinge")
            for (const char* field : {"minAngle", "maxAngle"})
                body += PropertyControlRow(field, Input(ScriptConstraintControlId(prefix, index, field),
                    {prefix + "physics-constraint-" + field, index}, joint.value(field, nlohmann::json()).dump()));
    }
    return body;
}

inline std::string ScriptPhysicsControls(const std::string& prefix = "script-",
    const nlohmann::json& physics = nlohmann::json::object())
{
    using namespace UiWidgets;
    std::string body = PropertyControlRow("Motion", Select(prefix + "physics-motion", {prefix + "physics-motion"},
        Option("none", "None") + Option("static", "Static") + Option("dynamic", "Dynamic") +
        Option("kinematic", "Kinematic") + Option("character", "Character")));
    body += "<div id=\"" + prefix + "physics-body\">";
    body += PropertyControlRow("Shape", Select(prefix + "physics-shape", {prefix + "physics-shape"},
        Option("box", "Box") + Option("sphere", "Sphere") + Option("capsule", "Capsule") +
        Option("compound", "Compound") + Option("mesh", "Static Mesh")));
    body += "<div id=\"" + prefix + "physics-component-row\">" +
        PropertyControlRow("Mesh Component", Input(prefix + "physics-component",
            {prefix + "physics-component"}, "")) + "</div>";
    body += PropertyControlRow("Trigger", "<input id=\"" + prefix + "physics-trigger\" type=\"checkbox\" "
        "data-action=\"" + prefix + "physics-trigger\"/>");
    std::vector<NumericComponent> extents;
    for (uint32_t axis = 0; axis < 3; ++axis)
    {
        NumericComponent control;
        control.id = prefix + "physics-extents-" + std::to_string(axis);
        control.label = std::string(1, "XYZ"[axis]);
        control.value = "50";
        control.change = {prefix + "physics-halfExtents", 0, axis};
        extents.push_back(std::move(control));
    }
    body += "<div id=\"" + prefix + "physics-halfExtents-row\">" +
        PropertyControlRow("Half Extents (cm)", NumericComponents(extents)) + "</div>";
    for (const auto& field : ScriptPhysicsFields())
        body += "<div id=\"" + prefix + "physics-" + std::string(field.name) + "-row\">" +
            PropertyControlRow(field.label, Input(prefix + "physics-" + std::string(field.name),
                {prefix + "physics-" + std::string(field.name)}, std::to_string(field.defaultValue))) + "</div>";
    body += ScriptConstraintControls(prefix, physics);
    return body + "</div>";
}
} // namespace VL::EditorUi
