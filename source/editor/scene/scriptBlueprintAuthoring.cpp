#include "editor/scene/scriptBlueprintAuthoring.h"

#include <random>
#include <sstream>
#include <iomanip>
#include "editor/service/sceneDocumentService.h"
#include "script/scriptRuntime.h"
#include "ui/uiAction.h"
#include "world/world.h"
#include "sceneNode.h"
#include "script/scriptBlueprintAsset.h"
#include "script/scriptCompleteDeclaration.h"
#include "editor/scene/scriptBlueprintPlacement.h"
#include "editor/scene/scriptBlueprintConfiguration.h"

namespace VL
{
namespace
{
std::string NewBlueprintId()
{
    std::random_device random;
    std::ostringstream text;
    for (int word = 0; word < 4; ++word)
        text << std::hex << std::setfill('0') << std::setw(8) << random();
    return text.str();
}

void EditPhysics(nlohmann::json& actor, const nlohmann::json& request)
{
    if (actor.contains("blueprintPath"))
        throw std::invalid_argument("Inherited physics is owned by the Blueprint asset");
    Editor::EditScriptPhysics(actor, request);
}

void EditComponent(nlohmann::json& actor, const nlohmann::json& request,
    Editor::SceneDocumentService& documents, const std::string& scenePath)
{
    const auto component = request.at("component").get<std::string>();
    const auto field = request.at("field").get<std::string>();
    auto effective = documents.ResolveBlueprint(actor, scenePath);
    const bool inherited = actor.contains("blueprintPath");
    if (inherited && !ScriptComponentAllowsInstance(
        effective.value("__componentInstanceEditable", nlohmann::json::object()), component, field))
        throw std::invalid_argument("Inherited component field is not Instance Editable: " + component + "." + field);
    if (field != "parent" && field != "position" && field != "rotation" && field != "scale" &&
        field != "modelPath" && field != "materialOverride" && field != "fov" && field != "near_clip" && field != "far_clip" &&
        field != "autoActivate" && field != "visible" && field != "color" && !IsScriptLightNumber(field) &&
        field != "collisionShape" && field != "collisionHalfExtents" &&
        field != "collisionRadius" && field != "collisionHalfHeight")
        throw std::invalid_argument("Unsupported component edit");
    for (const auto& node : effective.value("components", nlohmann::json::array()))
        if (ScriptComponentId(node) == component)
        {
            if (inherited && request.value("reset", false))
            {
                if (actor.contains("componentOverrides") && actor["componentOverrides"].contains(component))
                {
                    auto& overrides = actor["componentOverrides"];
                    overrides[component].erase(field);
                    if (overrides[component].empty()) overrides.erase(component);
                    if (overrides.empty()) actor.erase("componentOverrides");
                }
                return;
            }
            auto value = request.at("value");
            if (request.contains("axis") && field.rfind("collision", 0) != 0)
            {
                if (field != "position" && field != "rotation" && field != "scale" && field != "color")
                    throw std::invalid_argument("Component field is not a vector");
                value = ScriptComponentFieldValue(node, field);
                value.at(request.at("axis").get<size_t>()) = request.at("value");
            }
            if (inherited)
            {
                // 仅保存明确修改的字段；分量请求从最新草稿合成，不能把其它组件默认值展开到 Scene。
                if (!actor.contains("componentOverrides")) actor["componentOverrides"] = nlohmann::json::object();
                actor["componentOverrides"][component][field] = std::move(value);
            }
            else
                for (auto& target : actor.at("components"))
                    if (ScriptComponentId(target) == component)
                    {
                        if (field.rfind("collision", 0) == 0)
                            Editor::EditScriptComponentCollision(target, request);
                        else if (field == "materialOverride" && value == "") target.erase(field);
                        else target[field] = std::move(value);
                        break;
                    }
            return;
        }
    throw std::invalid_argument("Unknown component id");
}

const nlohmann::json& LogicPropertySchema(const ScriptRuntimeSubsystem& runtime,
    const nlohmann::json& node, const std::string& name)
{
    for (const auto& type : runtime.ComponentClasses())
        if (ScriptTypeNameMatches(type, node.at("class").get<std::string>()))
            for (const auto& property : type.at("properties"))
                if (property.at("name") == name) return property;
    throw std::invalid_argument("Unknown exposed C# component property: " + name);
}

const nlohmann::json& BlueprintPropertySchema(const ScriptRuntimeSubsystem& runtime,
    const nlohmann::json& node, const std::string& name)
{
    for (const auto& type : runtime.Classes())
        if (ScriptTypeNameMatches(type, node.at("class").get<std::string>()))
            for (const auto& property : type.at("properties"))
                if (property.at("name") == name) return property;
    throw std::invalid_argument("Unknown exposed Blueprint property: " + name);
}

// 旧 vector 分量协议只用于数学向量；容器和对象必须走带类型路径，不能把 List 当成 vecN。
nlohmann::json EditPropertyValue(const nlohmann::json& node, const nlohmann::json& schema,
    const nlohmann::json& authored, const nlohmann::json& request)
{
    auto value = CompleteScriptPropertyValue(node, schema);
    if (request.contains("axis"))
    {
        if (!ScriptPropertyVectorType(schema.at("type").get<std::string>()))
            throw std::invalid_argument("Script property is not a vector");
        value.at(ScriptPropertyArrayIndex(request.at("axis"), value.size())) = request.at("value");
        ValidateScriptPropertyValue(schema, value);
        return value;
    }
    return ApplyScriptPropertyEdit(schema, authored, value, request);
}

void EditLogicComponent(nlohmann::json& actor, ScriptRuntimeSubsystem& runtime,
    Editor::SceneDocumentService& documents, const std::string& scenePath,
    const nlohmann::json& request)
{
    const auto id = request.at("component").get<std::string>();
    const auto effective = documents.ResolveBlueprint(actor, scenePath);
    const bool inherited = actor.contains("blueprintPath");
    for (const auto& node : effective.value("scriptComponents", nlohmann::json::array()))
    {
        if (node.at("id") != id) continue;
        if (request.contains("field"))
        {
            if (inherited || request.at("field") != "enabled" ||
                !request.at("value").is_boolean())
                throw std::invalid_argument("Inherited C# component structure is owned by the Blueprint asset");
            for (auto& target : actor.at("scriptComponents"))
                if (target.at("id") == id) target["enabled"] = request.at("value");
            return;
        }
        const auto name = request.at("property").get<std::string>();
        const auto& schema = LogicPropertySchema(runtime, node, name);
        const auto permissions = node.value("instanceEditable", nlohmann::json::array());
        const bool allowed = inherited ? std::find(permissions.begin(), permissions.end(), name) != permissions.end() :
            schema.value("instanceEditable", false);
        if (!allowed) throw std::invalid_argument("C# component property is not Instance Editable: " + id + "." + name);
        if (request.value("reset", false))
        {
            if (inherited)
            {
                if (actor.contains("scriptComponentOverrides") && actor["scriptComponentOverrides"].contains(id))
                {
                    auto& overrides = actor["scriptComponentOverrides"];
                    overrides[id].erase(name);
                    if (overrides[id].empty()) overrides.erase(id);
                    if (overrides.empty()) actor.erase("scriptComponentOverrides");
                }
            }
            else
                for (auto& target : actor.at("scriptComponents"))
                    if (target.at("id") == id) target["properties"].erase(name);
            return;
        }
        auto layered = node;
        if (effective.contains("__logicPropertyDefaults"))
            layered["__propertyDefaults"] = effective.at("__logicPropertyDefaults")
                .value(id, nlohmann::json::object());
        if (inherited)
        {
            const auto overrides = actor.value("scriptComponentOverrides", nlohmann::json::object());
            const auto own = overrides.value(id, nlohmann::json::object());
            auto value = EditPropertyValue(layered, schema, own.value(name, nlohmann::json()), request);
            if (value.is_null())
            {
                if (actor.contains("scriptComponentOverrides") && actor["scriptComponentOverrides"].contains(id))
                {
                    actor["scriptComponentOverrides"][id].erase(name);
                    if (actor["scriptComponentOverrides"][id].empty()) actor["scriptComponentOverrides"].erase(id);
                    if (actor["scriptComponentOverrides"].empty()) actor.erase("scriptComponentOverrides");
                }
            }
            else actor["scriptComponentOverrides"][id][name] = std::move(value);
        }
        else
            for (auto& target : actor.at("scriptComponents"))
                if (target.at("id") == id)
                {
                    auto value = EditPropertyValue(layered, schema,
                        target.at("properties").value(name, nlohmann::json()), request);
                    if (value.is_null()) target["properties"].erase(name);
                    else target["properties"][name] = std::move(value);
                }
        return;
    }
    throw std::invalid_argument("Unknown C# component ID");
}

void ChangeLogicComponents(nlohmann::json& actor, UiActionType type, const nlohmann::json& request)
{
    if (actor.contains("blueprintPath"))
        throw std::invalid_argument("Inherited C# components are owned by the Blueprint asset");
    if (!actor.contains("scriptComponents")) actor["scriptComponents"] = nlohmann::json::array();
    auto& nodes = actor["scriptComponents"];
    if (type == UiActionType::AddScriptLogicComponent)
        nodes.push_back({{"id", "script-" + NewBlueprintId()}, {"class", request.at("class")},
            {"properties", nlohmann::json::object()}});
    else
    {
        const auto id = request.at("component").get<std::string>();
        bool found = false;
        for (auto node = nodes.begin(); node != nodes.end();)
            if (node->at("id") == id) { node = nodes.erase(node); found = true; }
            else ++node;
        if (!found) throw std::invalid_argument("Unknown C# component ID");
        if (nodes.empty()) actor.erase("scriptComponents");
    }
}
} // namespace

bool StageScriptBlueprintAction(World& world, ScriptRuntimeSubsystem& runtime,
    Editor::SceneDocumentService& documents, const UiAction& action,
    std::string& actorId, std::string& error)
{
    error.clear();
    try
    {
        if (action.actorProperty.worldGeneration != world.GetGeneration())
            throw std::invalid_argument("The scene changed since this Blueprint request");
        if (!runtime.IsEditing()) throw std::invalid_argument("Stop gameplay before editing Blueprints");
        if (action.type == UiActionType::UndoSceneEdit || action.type == UiActionType::RedoSceneEdit)
            return documents.Undo(world.GetScenePath(), action.type == UiActionType::RedoSceneEdit, error);
        if (action.type == UiActionType::DeleteScriptBlueprint)
            return documents.RemoveScriptBlueprint(world.GetScenePath(), action.actorProperty.identity, error);
        if (action.type == UiActionType::CreateScriptBlueprintAsset)
        {
            if (action.script.contains("newClass"))
            {
                auto types = runtime.Classes();
                for (const auto& type : runtime.ComponentClasses()) types.push_back(type);
                return documents.CreateScriptBlueprintSource(action.stringValue,
                    action.script.at("newClass").get<std::string>(),
                    action.script.at("name").get<std::string>(), types, error);
            }
            const auto source = documents.ScriptBlueprintDraft(world.GetScenePath(),
                action.actorProperty.identity, error);
            if (source.is_null()) return false;
            auto effective = CompleteBlueprintDeclaration(documents.ResolveBlueprint(source, world.GetScenePath()),
                runtime.Classes(), runtime.ComponentClasses());
            runtime.ValidateBlueprint(effective);
            return documents.CreateScriptBlueprintAsset(action.stringValue, effective, error);
        }
        nlohmann::json actor;
        bool create = action.type == UiActionType::AddScriptBlueprint;
        if (create)
        {
            actorId = NewBlueprintId();
            actor = {{"type", "blueprintInstance"}, {"id", actorId},
                {"name", action.script.value("name", std::string("Blueprint"))},
                {"position", {0, 0, 0}}, {"rotation", {0, 0, 0}},
                {"scale", {1, 1, 1}}, {"properties", nlohmann::json::object()}};
            if (action.script.contains("blueprintPath"))
            {
                actor["blueprintPath"] = action.script.at("blueprintPath");
                const auto defaults = documents.BlueprintAssetDefaults(actor.at("blueprintPath"), world.GetScenePath());
                actor["name"] = defaults.at("name");
                for (const char* field : {"rotation", "scale"})
                    if (defaults.contains(field)) actor[field] = defaults.at(field);
            }
            else
            {
                actor["class"] = action.script.at("class");
                actor["components"] = nlohmann::json::array();
                const auto mesh = action.script.value("modelPath", std::string());
                if (!mesh.empty()) actor["components"].push_back({{"type", "mesh"}, {"modelPath", mesh}});
            }
            if (action.script.contains("viewport"))
            {
                const auto& point = action.script.at("viewport");
                const auto& camera = world.GetRenderCamera();
                if (!camera) throw std::invalid_argument("Blueprint placement requires a camera");
                const auto position = ScriptBlueprintPlacement(camera->GetProjectionMatrix() * camera->GetViewMatrix(),
                    point.at(0).get<float>(), point.at(1).get<float>());
                actor["position"] = {position.x(), position.y(), position.z()};
            }
        }
        else
        {
            actor = documents.ScriptBlueprintDraft(world.GetScenePath(), action.actorProperty.identity, error);
            if (actor.is_null()) return false;
            actorId = action.actorProperty.identity.substr(std::string_view("blueprintInstance|").size());
            if (action.type == UiActionType::DuplicateScriptBlueprint)
            {
                create = true;
                actorId = NewBlueprintId();
                actor["id"] = actorId;
                actor["name"] = actor.at("name").get<std::string>() + " Copy";
            }
            else if (action.type == UiActionType::RenameScriptBlueprint)
                actor["name"] = action.script.at("name");
            else if (action.type == UiActionType::SetScriptPhysics)
                EditPhysics(actor, action.script);
            else if (action.type == UiActionType::AddScriptComponent ||
                action.type == UiActionType::RemoveScriptComponent)
            {
                if (actor.contains("blueprintPath"))
                    throw std::invalid_argument("Inherited components are owned by the Blueprint asset");
                if (!actor.contains("components")) actor["components"] = nlohmann::json::array();
                auto& nodes = actor.at("components");
                if (action.type == UiActionType::AddScriptComponent)
                {
                    for (auto& node : nodes)
                        if (!node.contains("id")) node["id"] = "root";
                    auto node = action.script.at("node");
                    node["id"] = "component-" + NewBlueprintId();
                    nodes.push_back(std::move(node));
                }
                else
                {
                    std::set<std::string> removed{action.script.at("component").get<std::string>()};
                    bool found = false;
                    for (const auto& node : nodes) if (removed.count(ScriptComponentId(node))) found = true;
                    if (!found) throw std::invalid_argument("Unknown component id");
                    size_t previous;
                    do
                    {
                        previous = removed.size();
                        for (const auto& node : nodes)
                            if (removed.count(node.value("parent", ""))) removed.insert(ScriptComponentId(node));
                    } while (previous != removed.size());
                    for (auto item = nodes.begin(); item != nodes.end();)
                        if (removed.count(ScriptComponentId(*item))) item = nodes.erase(item); else ++item;
                }
            }
            else if (action.type == UiActionType::SetScriptComponent)
                EditComponent(actor, action.script, documents, world.GetScenePath());
            else if (action.type == UiActionType::SetScriptLogicComponent)
                EditLogicComponent(actor, runtime, documents, world.GetScenePath(), action.script);
            else if (action.type == UiActionType::AddScriptLogicComponent ||
                action.type == UiActionType::RemoveScriptLogicComponent)
                ChangeLogicComponents(actor, action.type, action.script);
            else
            {
                const auto property = action.script.at("property").get<std::string>();
                // reset 也校验权限，旧 UI 或手工构造的请求不能绕过实例白名单。
                auto effective = documents.ResolveBlueprint(actor, world.GetScenePath());
                auto allowed = effective.value("__instanceEditable", nlohmann::json::array());
                if (!actor.contains("blueprintPath"))
                    for (const auto& type : runtime.Classes())
                        if (ScriptTypeNameMatches(type, actor.at("class").get<std::string>()))
                            for (const auto& field : type.at("properties"))
                                if (field.value("instanceEditable", false)) allowed.push_back(field.at("name"));
                if (std::find(allowed.begin(), allowed.end(), property) == allowed.end())
                    throw std::invalid_argument("Blueprint property is not Instance Editable: " + property);
                if (action.script.value("reset", false)) actor["properties"].erase(property);
                else
                {
                    auto value = EditPropertyValue(effective, BlueprintPropertySchema(runtime, effective, property),
                        actor.at("properties").value(property, nlohmann::json()), action.script);
                    if (value.is_null()) actor["properties"].erase(property);
                    else actor["properties"][property] = std::move(value);
                }
            }
        }
        runtime.ValidateBlueprint(documents.ResolveBlueprint(actor, world.GetScenePath()));
        return documents.StageScriptBlueprint(world.GetScenePath(), actor, create, error);
    }
    catch (const std::exception& exception) { error = exception.what(); return false; }
}
} // namespace VL
