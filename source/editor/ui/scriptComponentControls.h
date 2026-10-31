#pragma once

#include "editor/ui/editorSceneActorTypes.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/views/viewWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "script/scriptComponentOverrides.h"
#include "editor/ui/tressfxHairSimulationControls.h"

namespace VL::EditorUi
{
// 毛发组件只在蓝图资产页提供作者入口：它的绑定来源必须是同一 actor 里的 mesh
// 组件，Scene 侧通用组件面板没有“挂到哪个角色”的上下文，不暴露它。
inline std::string ScriptComponentOptions(bool includeTressFx = false)
{
    return UiWidgets::Option("scene", "Scene") + UiWidgets::Option("mesh", "Mesh") +
        (includeTressFx ? UiWidgets::Option("tressfx", "TressFX") : std::string()) +
        UiWidgets::Option("camera", "Camera") + UiWidgets::Option("springArm", "Spring Arm") +
        UiWidgets::Option("pointLight", "Point Light") +
        UiWidgets::Option("spotLight", "Spot Light") + UiWidgets::Option("directionalLight", "Directional Light");
}

inline nlohmann::json NewScriptComponent(const std::string& type)
{
    nlohmann::json node{{"type", type}};
    if (IsScriptLight(type)) node["intensityUnit"] = type == "directionalLight" ? "lux" : "cd";
    // 毛发权重默认 auto：加载期由父 mesh 组件派生，作者可在 Details 覆盖成 rootBone/mesh。
    if (type == "tressfx") node["weights"] = "auto";
    return node;
}

// 新 tressfx 组件的默认父级：从当前选择向上找最近的 mesh 组件。选择本身就是 mesh
// 时用它；Blueprint 根或没有 mesh 祖先返回空，调用方必须拒绝添加（加载期也会再拒绝）。
inline std::string ScriptComponentMeshParent(
    const nlohmann::json& components, const std::string& selectedId)
{
    auto id = selectedId;
    while (!id.empty())
    {
        const nlohmann::json* found = nullptr;
        for (const auto& node : components)
            if (ScriptComponentId(node) == id) { found = &node; break; }
        if (found == nullptr) return {};
        if (found->value("type", "") == "mesh") return id;
        id = found->value("parent", "");
    }
    return {};
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
        row.icon = IsScriptMeshComponent(node.at("type").get<std::string>()) ? "icons/mesh.png" :
            node.at("type") == "camera" ? "icons/camera.png" : "icons/blueprint.png";
        // 资产编辑页在左侧独立选组件；Scene Details 保留原有父链内联表单。
        if (separateDetails)
            markup += "<div id=\"" + prefix + "component-details-" + std::to_string(index) + "\">";
        else
            markup += UiWidgets::TreeRow(row);
        markup += "<div class=\"script-component-actions\"><button id=\"" + prefix + "component-remove-" +
            std::to_string(index) + "\" class=\"browser-icon\" data-action=\"" + prefix + "component-remove\" data-index=\"" +
            std::to_string(index) + "\" title=\"Remove component and descendants\"><img src=\"icons/trash.png\"/></button></div>";
        const bool tressfx = node.at("type") == "tressfx";
        // 毛发只允许挂到 mesh 组件（绑定来源就是它），所以不提供 "Actor" 空父级；
        // 其它类型保持原有“任意组件”的父链语义。
        std::string options = tressfx ? std::string() : UiWidgets::Option("", "Actor");
        for (const auto& candidate : actor.scriptComponents)
        {
            if (candidate.value("id", "root") == id) continue;
            const bool current = candidate.value("id", "root") == node.value("parent", "");
            if (tressfx && candidate.value("type", "") != "mesh" && !current) continue;
            options += UiWidgets::Option(candidate.value("id", "root"), candidate.value("id", "root"));
        }
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
        if (tressfx)
        {
            // 关卡编辑与资产编辑必须分开：毛发绑定字段只在蓝图资产页作者，
            // Scene Details 一律只读展示（含内联 actor），不得在关卡里生成
            // componentOverrides 或依赖“看起来能改”的控件。
            const bool sceneHairFieldsReadOnly = !separateDetails;
            if (sceneHairFieldsReadOnly)
            {
                const auto weights = node.value("weights", "auto");
                const std::string weightsLabel = weights == "rootBone" ? "Root Bone" :
                    weights == "mesh" ? "From Mesh" : "Auto (from parent mesh)";
                const auto rootBone = node.value("rootBone", "");
                markup += UiWidgets::PropertyInfo("Hair Asset", node.value("modelPath", ""));
                markup += UiWidgets::PropertyInfo("Root Bone",
                    rootBone.empty() ? "(required)" : rootBone);
                markup += UiWidgets::PropertyInfo("Root Weights", weightsLabel);
                markup += UiWidgets::PropertyInfo("Simulation",
                    node.value("simulation", nlohmann::json::object()).value("mode", "inherit"));
                markup += UiWidgets::PropertyInfo("Asset-Owned",
                    "TressFX binding is authored in the Blueprint asset. Edit it in the Blueprint tab; "
                    "level instances cannot override these fields.");
            }
            else
            {
                // rootBone 属于挂载，必须显式作者；TF 只拥有可复用默认值。
                // 实例白名单不暴露毛发字段：绑定/权重属于资产级编排，不是场景实例差异。
                markup += ScriptComponentProperty(actor, index, "modelPath", "Hair Asset", UiWidgets::Input(
                    ComponentControlId(index, "modelPath", 0, prefix), {prefix + "component-modelPath", index},
                    node.value("modelPath", "")), prefix, false);
                markup += ScriptComponentProperty(actor, index, "rootBone", "Root Bone", UiWidgets::Input(
                    ComponentControlId(index, "rootBone", 0, prefix), {prefix + "component-rootBone", index},
                    node.value("rootBone", "")), prefix, false);
                markup += ScriptComponentProperty(actor, index, "weights", "Root Weights", UiWidgets::Select(
                    ComponentControlId(index, "weights", 0, prefix), {prefix + "component-weights", index},
                    UiWidgets::Option("auto", "Auto (from parent mesh)") +
                    UiWidgets::Option("rootBone", "Root Bone") + UiWidgets::Option("mesh", "From Mesh")),
                    prefix, false);
                markup += UiWidgets::PropertyControlRow("Material Override", UiWidgets::Input(
                    ComponentControlId(index, "materialOverride", 0, prefix),
                    {prefix + "component-materialOverride", index}, node.value("materialOverride", "")));
                const auto simulation = node.value("simulation", nlohmann::json::object());
                const auto overrides = simulation.value("overrides", nlohmann::json::object());
                markup += UiWidgets::PropertyControlRow("Simulation", UiWidgets::Select(
                    ComponentControlId(index, "hair-mode", 0, prefix), {prefix + "hair-mode", index},
                    UiWidgets::Option("inherit", "Inherit") + UiWidgets::Option("off", "Off") +
                    UiWidgets::Option("on", "On")));
                std::size_t count = 0;
                const auto* controls = TressfxSimulationControls(count);
                for (std::size_t parameter = 0; parameter < count; ++parameter)
                {
                    const auto& control = controls[parameter];
                    const std::string field = control.field;
                    // 空值代表继承；向量作为完整 JSON 值编辑，不能把未改的轴烘成覆盖。
                    std::string input = UiWidgets::Input(
                        ComponentControlId(index, "hair-" + field, 0, prefix),
                        {prefix + "hair-value", index, static_cast<uint32_t>(parameter)},
                        overrides.contains(field) ? overrides.at(field).dump() : "");
                    input += "<button id=\"" +
                        ComponentControlId(index, "hair-reset-" + field, 0, prefix) +
                        "\" class=\"browser-icon\" data-action=\"" + prefix +
                        "hair-reset\" data-index=\"" + std::to_string(index) +
                        "\" data-component=\"" + std::to_string(parameter) +
                        "\" title=\"Restore inherited value\"><img src=\"icons/refresh.png\"/></button>";
                    input += "<span id=\"" + ComponentControlId(index, "hair-source-" + field, 0, prefix) +
                        "\">" + (overrides.contains(field) ? "Override" : "Inherited") + "</span>";
                    markup += UiWidgets::PropertyControlRow(control.label, input);
                }
            }
            markup += UiWidgets::PropertyInfo("Local Transform",
                "Follows the parent mesh object space; local TRS is disabled for TressFX components.");
        }
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
        // 毛发组件必须与父 mesh 同对象空间，局部 TRS 与图元碰撞都不适用：
        // 外部阻挡代理由挂载 hairCollision 作者，不产生 Jolt 图元碰撞体。
        if (!tressfx)
        {
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
        }
        if (!tressfx) markup += ScriptComponentCollisionControls(index, prefix);
        if (separateDetails) markup += "</div>";
        markup += ScriptComponentControls(actor, id, depth + 1, prefix, separateDetails);
    }
    return markup;
}
} // namespace VL::EditorUi
