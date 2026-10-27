#include "editor/command/assetDocumentFields.h"

#include <cmath>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace VL::Editor
{
namespace
{
using Json = nlohmann::json;
using K = EditorAssetDocumentKind;
enum class ValueKind { Text, Boolean, Number, Vector3, Strings, Channels, Object, Array };
struct Field
{
    K kind;
    const char* scope;
    const char* name;
    ValueKind type;
    bool reset = false;
    const char* unit = "";
    const char* options = "";
};
// scope 只列真正可编辑的目标；slot 的顺序身份不由字段名推导。
const Field fields[] = {
    {K::Scene, "", "name", ValueKind::Text}, {K::Scene, "", "description", ValueKind::Text},
    {K::Scene, "mesh", "position", ValueKind::Vector3, false, "cm"},
    {K::Scene, "mesh", "rotation", ValueKind::Vector3, false, "degrees"},
    {K::Scene, "mesh", "scale", ValueKind::Vector3},
    {K::Scene, "mesh", "materialOverride", ValueKind::Text, true, "asset_path"},
    {K::Scene, "nodeLight", "position", ValueKind::Vector3, false, "cm"},
    {K::Scene, "nodeLight", "rotation", ValueKind::Vector3, false, "degrees"},
    {K::Scene, "light", "color", ValueKind::Vector3, false, "linear_rgb"},
    {K::Scene, "light", "intensity", ValueKind::Number, false, "intensityUnit"},
    {K::Scene, "light", "intensityUnit", ValueKind::Text, false, "", "lux|cd|lm"},
    {K::Scene, "spotLight", "cone_angle_inner", ValueKind::Number, false, "degrees"},
    {K::Scene, "spotLight", "cone_angle_outer", ValueKind::Number, false, "degrees"},
    {K::Scene, "squareAreaLight", "position", ValueKind::Vector3, false, "cm"},
    {K::Scene, "squareAreaLight", "normal", ValueKind::Vector3},
    {K::Scene, "squareAreaLight", "size", ValueKind::Number, false, "cm"},
    {K::Scene, "camera", "position", ValueKind::Vector3, false, "cm"},
    {K::Scene, "camera", "rotation", ValueKind::Vector3, false, "degrees"},
    {K::Scene, "camera", "look_at", ValueKind::Vector3, true, "cm"},
    {K::Scene, "camera", "fov", ValueKind::Number, false, "degrees"},
    {K::Scene, "camera", "near_clip", ValueKind::Number, false, "cm"},
    {K::Scene, "camera", "far_clip", ValueKind::Number, false, "cm"},
    {K::Scene, "camera", "exposure.mode", ValueKind::Text, false, "", "ev100|physical|auto"},
    {K::Scene, "camera", "exposure.ev100", ValueKind::Number, false, "EV100"},
    {K::Scene, "camera", "exposure.compensation", ValueKind::Number, false, "EV"},
    {K::Scene, "camera", "exposure.aperture", ValueKind::Number},
    {K::Scene, "camera", "exposure.shutterSeconds", ValueKind::Number, false, "seconds"},
    {K::Scene, "camera", "exposure.iso", ValueKind::Number},
    {K::Scene, "camera", "exposure.autoMinEv100", ValueKind::Number, true, "EV100"},
    {K::Scene, "camera", "exposure.autoMaxEv100", ValueKind::Number, true, "EV100"},
    {K::Scene, "camera", "exposure.autoSpeedUp", ValueKind::Number, true, "1/s"},
    {K::Scene, "camera", "exposure.autoSpeedDown", ValueKind::Number, true, "1/s"},
    {K::Scene, "camera", "exposure.autoTargetGray", ValueKind::Number, true, "linear"},
    {K::Mesh, "", "name", ValueKind::Text},
    {K::Mesh, "", "modelDataPath", ValueKind::Text, false, "source_path"},
    {K::Tressfx, "", "name", ValueKind::Text},
    {K::Tressfx, "", "modelDataPath", ValueKind::Text, false, "source_path"},
    // TressFX 默认模拟标量由独立文档拥有；放置覆盖不写回这里。作者值由
    // MeshAssetValidator 做值域与封闭对象校验；实时预览另走运行期命令。
    {K::Tressfx, "", "simulationDefaults.gravity", ValueKind::Vector3, false, "cm/s^2"},
    {K::Tressfx, "", "simulationDefaults.wind", ValueKind::Vector3},
    {K::Tressfx, "", "simulationDefaults.damping", ValueKind::Number},
    {K::Tressfx, "", "simulationDefaults.iterations", ValueKind::Number},
    {K::Tressfx, "", "simulationDefaults.bendStiffness", ValueKind::Number},
    {K::Tressfx, "", "simulationDefaults.globalShapeStiffness", ValueKind::Number},
    {K::Tressfx, "", "simulationDefaults.globalShapeEffectiveRange", ValueKind::Number},
    {K::Tressfx, "", "simulationDefaults.vspCoefficient", ValueKind::Number},
    {K::Tressfx, "", "simulationDefaults.vspAccelThresholdMax", ValueKind::Number, false, "cm"},
    // 分组整体作为数组写入：形状、名称与区间由 MeshAssetValidator 在 modify/save 时校验。
    {K::Tressfx, "", "groups", ValueKind::Array, true},
    {K::Mesh, "", "importOptions.generateSmoothNormals", ValueKind::Boolean, true},
    {K::Mesh, "", "importOptions.includedSectionNames", ValueKind::Strings, true},
    {K::Mesh, "slot", "materialInstancePath", ValueKind::Text, false, "asset_path"},
    {K::Tressfx, "slot", "materialInstancePath", ValueKind::Text, false, "asset_path"},
    {K::Texture, "", "name", ValueKind::Text},
    {K::Texture, "", "source", ValueKind::Text, false, "source_path"},
    {K::Texture, "", "colorSpace", ValueKind::Text, false, "", "linear|srgb"},
    {K::Texture, "", "mipmaps", ValueKind::Boolean, true},
    {K::Texture, "", "filter", ValueKind::Text, true, "", "linear|nearest"},
    {K::Texture, "", "wrapMode", ValueKind::Text, true, "", "repeat|clamp"},
    {K::Texture, "", "channelsDescription", ValueKind::Channels, true},
    {K::Blueprint, "", "name", ValueKind::Text},
    {K::Blueprint, "", "description", ValueKind::Text, true},
    {K::Blueprint, "", "class", ValueKind::Text},
    {K::Blueprint, "", "properties", ValueKind::Object},
    {K::Blueprint, "", "instanceEditable", ValueKind::Array, true},
    {K::Blueprint, "", "componentInstanceEditable", ValueKind::Object, true},
    {K::Blueprint, "", "components", ValueKind::Array, true},
    {K::Blueprint, "", "scriptComponents", ValueKind::Array, true},
    {K::Blueprint, "", "physics", ValueKind::Object, true},
    {K::Blueprint, "", "rotation", ValueKind::Vector3, true, "degrees"},
    {K::Blueprint, "", "scale", ValueKind::Vector3, true}
};

bool MatchesScope(const std::string& type, const std::string& scope)
{
    if (type == scope) return true;
    const bool node = type == "directionalLight" || type == "pointLight" || type == "spotLight";
    return (scope == "nodeLight" && node) || (scope == "light" && (node || type == "squareAreaLight"));
}

std::string Scope(const EditorAssetFieldChange& change)
{
    if (change.objectIdentity)
    {
        const auto separator = change.objectIdentity->find('|');
        if (separator == std::string::npos || separator == 0 || separator + 1 == change.objectIdentity->size())
            throw std::invalid_argument("objectIdentity must be type|name");
        return change.objectIdentity->substr(0, separator);
    }
    return change.slotIndex ? "slot" : "";
}

const Field& FindField(K kind, const EditorAssetFieldChange& change)
{
    if (change.objectIdentity && (kind != K::Scene || change.slotIndex || change.expectedSlotName))
        throw std::invalid_argument("objectIdentity is only valid for scene object fields");
    if (change.slotIndex.has_value() != change.expectedSlotName.has_value() ||
        (change.slotIndex && kind != K::Mesh && kind != K::Tressfx) ||
        (change.expectedSlotName && change.expectedSlotName->empty()))
        throw std::invalid_argument("Mesh slots require slotIndex and non-empty expectedSlotName");
    const auto scope = Scope(change);
    for (const auto& field : fields)
        if (field.kind == kind && change.field == field.name && MatchesScope(scope, field.scope))
            return field;
    throw std::invalid_argument("Unsupported asset field: " + scope + "." + change.field);
}

const char* TypeName(ValueKind type)
{
    switch (type)
    {
    case ValueKind::Text: return "string"; case ValueKind::Boolean: return "boolean";
    case ValueKind::Number: return "number"; case ValueKind::Vector3: return "vec3";
    case ValueKind::Strings: return "string_array"; case ValueKind::Channels: return "channel_descriptions";
    case ValueKind::Object: return "object"; case ValueKind::Array: return "array";
    }
    return "";
}

Json ValueJson(const EditorAssetFieldValue& value)
{
    return std::visit([](const auto& item) { return Json(item); }, value);
}

EditorAssetFieldValue DecodeValue(const Json& value, const Field& field)
{
    switch (field.type)
    {
    case ValueKind::Object: if (value.is_object()) return value; break;
    case ValueKind::Array: if (value.is_array()) return value; break;
    case ValueKind::Text:
        if (value.is_string() && !value.get<std::string>().empty())
        {
            const std::string text = value;
            if (text.find('\0') != std::string::npos) break;
            if (*field.options && ("|" + std::string(field.options) + "|").find("|" + text + "|") == std::string::npos)
                break;
            return text;
        }
        break;
    case ValueKind::Boolean: if (value.is_boolean()) return value.get<bool>(); break;
    case ValueKind::Number:
        if (value.is_number() && std::isfinite(value.get<double>()) &&
            std::isfinite(static_cast<float>(value.get<double>()))) return value.get<double>();
        break;
    case ValueKind::Vector3:
        if (value.is_array() && value.size() == 3)
        {
            std::array<double, 3> vector{};
            for (size_t index = 0; index < 3; ++index)
            {
                if (!value[index].is_number() || !std::isfinite(value[index].get<double>()) ||
                    !std::isfinite(static_cast<float>(value[index].get<double>())))
                    throw std::invalid_argument("Vector components must be finite: " + std::string(field.name));
                vector[index] = value[index].get<double>();
            }
            return vector;
        }
        break;
    case ValueKind::Strings:
        if (value.is_array() && !value.empty())
        {
            std::vector<std::string> strings;
            for (const auto& item : value)
            {
                if (!item.is_string() || item.get<std::string>().empty() ||
                    item.get<std::string>().find('\0') != std::string::npos)
                    throw std::invalid_argument("Section names must be non-empty strings");
                strings.push_back(item.get<std::string>());
            }
            return strings;
        }
        break;
    case ValueKind::Channels:
        if (value.is_object())
        {
            std::map<std::string, std::string> channels;
            for (const auto& entry : value.items())
            {
                if ((entry.key() != "r" && entry.key() != "g" && entry.key() != "b" && entry.key() != "a") ||
                    !entry.value().is_string())
                    throw std::invalid_argument("Channel descriptions require r/g/b/a string entries");
                channels[entry.key()] = entry.value().get<std::string>();
            }
            return channels;
        }
        break;
    }
    throw std::invalid_argument("Invalid " + std::string(TypeName(field.type)) + " value for " + field.name);
}

Json* Target(Json& document, const EditorAssetFieldChange& change)
{
    if (change.objectIdentity)
    {
        Json* found = nullptr;
        for (auto& object : document.at("objects"))
            if (object.at("type").get<std::string>() + "|" + object.at("name").get<std::string>() == *change.objectIdentity)
            {
                if (found) throw std::invalid_argument("Ambiguous objectIdentity");
                found = &object;
            }
        if (!found) throw std::invalid_argument("Scene objectIdentity was not found: " + *change.objectIdentity);
        return found;
    }
    if (change.slotIndex)
    {
        auto& slots = document.at("materialSlots");
        if (*change.slotIndex >= slots.size() ||
            slots.at(*change.slotIndex).at("name") != *change.expectedSlotName)
            throw std::invalid_argument("Mesh slot identity changed");
        return &slots.at(*change.slotIndex);
    }
    return &document;
}

void SetField(Json& target, const EditorAssetFieldChange& change)
{
    const auto dot = change.field.find('.');
    const std::string parent = change.field.substr(0, dot);
    const std::string leaf = dot == std::string::npos ? parent : change.field.substr(dot + 1);
    if (change.reset)
    {
        if (dot == std::string::npos) target.erase(leaf);
        else if (target.contains(parent))
        {
            target.at(parent).erase(leaf);
            if (target.at(parent).empty()) target.erase(parent);
        }
    }
    else if (dot == std::string::npos) target[leaf] = ValueJson(change.value);
    else target[parent][leaf] = ValueJson(change.value);
}

Json FieldDescription(const Field& field)
{
    Json result{{"field", field.name}, {"type", TypeName(field.type)}, {"resettable", field.reset},
        {"unit", field.unit}};
    if (*field.options)
    {
        result["enum"] = Json::array();
        std::string options(field.options);
        size_t start = 0;
        for (size_t end = options.find('|'); ; end = options.find('|', start))
        {
            result["enum"].push_back(options.substr(start, end - start));
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }
    return result;
}

Json FieldList(K kind, const std::string& scope)
{
    Json result = Json::array();
    for (const auto& field : fields)
        if (field.kind == kind && MatchesScope(scope, field.scope))
        {
            auto description = FieldDescription(field);
            if (std::string(field.name) == "intensityUnit")
                description["enum"] = scope == "directionalLight" ? Json::array({"lux"}) : Json::array({"cd", "lm"});
            result.push_back(std::move(description));
        }
    return result;
}

Json Projection(K kind, const std::string& scope, const Json& source)
{
    Json result = Json::object();
    for (const char* key : {"name", "type"})
        if (source.contains(key)) result[key] = source.at(key);
    for (const auto& field : fields)
    {
        if (field.kind != kind || !MatchesScope(scope, field.scope)) continue;
        const std::string name(field.name);
        const auto dot = name.find('.');
        if (dot == std::string::npos)
        {
            if (source.contains(name)) result[name] = source.at(name);
        }
        else
        {
            const auto parent = name.substr(0, dot), leaf = name.substr(dot + 1);
            if (source.contains(parent) && source.at(parent).contains(leaf))
                result[parent][leaf] = source.at(parent).at(leaf);
        }
    }
    return result;
}
} // namespace

Json EncodeAssetChanges(const std::vector<EditorAssetFieldChange>& changes, K kind)
{
    if (changes.empty() || changes.size() > 128) throw std::invalid_argument("changes requires 1..128 entries");
    Json result = Json::array();
    std::set<std::string> targets;
    for (const auto& change : changes)
    {
        const auto& field = FindField(kind, change);
        Json item{{"field", change.field}};
        if (change.objectIdentity) item["objectIdentity"] = *change.objectIdentity;
        if (change.slotIndex) item["slotIndex"] = *change.slotIndex;
        if (change.expectedSlotName) item["expectedSlotName"] = *change.expectedSlotName;
        if (!targets.insert(item.dump()).second) throw std::invalid_argument("Duplicate target field");
        if (change.reset)
        {
            if (!field.reset) throw std::invalid_argument("Required field cannot be reset: " + change.field);
            item["reset"] = true;
        }
        else
        {
            item["value"] = ValueJson(change.value);
            const auto decoded = DecodeValue(item.at("value"), field);
            if (decoded.index() != change.value.index()) throw std::invalid_argument("Typed asset field mismatch");
        }
        result.push_back(std::move(item));
    }
    return result;
}

std::vector<EditorAssetFieldChange> DecodeAssetChanges(const Json& changes, K kind)
{
    if (!changes.is_array() || changes.empty() || changes.size() > 128)
        throw std::invalid_argument("changes requires 1..128 entries");
    std::vector<EditorAssetFieldChange> result;
    for (const auto& item : changes)
    {
        if (!item.is_object()) throw std::invalid_argument("Asset change must be object");
        for (const auto& entry : item.items())
            if (entry.key() != "field" && entry.key() != "value" && entry.key() != "reset" &&
                entry.key() != "objectIdentity" && entry.key() != "slotIndex" && entry.key() != "expectedSlotName")
                throw std::invalid_argument("Unknown change property: " + entry.key());
        EditorAssetFieldChange change;
        change.field = item.at("field").get<std::string>();
        if (item.contains("objectIdentity")) change.objectIdentity = item.at("objectIdentity").get<std::string>();
        if (item.contains("slotIndex"))
        {
            const auto& index = item.at("slotIndex");
            if (!index.is_number_integer() || index.get<double>() < 0 || index.get<double>() > UINT32_MAX)
                throw std::invalid_argument("slotIndex must be uint32");
            change.slotIndex = index.get<uint32_t>();
        }
        if (item.contains("expectedSlotName")) change.expectedSlotName = item.at("expectedSlotName").get<std::string>();
        if (item.contains("reset"))
        {
            if (item.at("reset") != true || item.contains("value"))
                throw std::invalid_argument("reset must be true and omit value");
            change.reset = true;
        }
        const auto& field = FindField(kind, change);
        if (!change.reset) change.value = DecodeValue(item.at("value"), field);
        result.push_back(std::move(change));
    }
    EncodeAssetChanges(result, kind);
    return result;
}

Json ApplyAssetChanges(Json& candidate, K kind, const std::vector<EditorAssetFieldChange>& changes)
{
    const auto wire = EncodeAssetChanges(changes, kind);
    Json changed = Json::array();
    size_t changeIndex = 0;
    for (const auto& change : changes)
    {
        Json& target = *Target(candidate, change);
        const Json before = target;
        if (kind == K::Scene && change.field == "rotation" && target.at("type") == "camera")
        {
            for (const auto& other : changes)
                if (other.objectIdentity == change.objectIdentity && other.field == "look_at")
                    throw std::invalid_argument("Camera rotation and look_at cannot be submitted together");
            // 作者 rotation 优先选择显式 TRS，不能被旧 look_at 在下次加载时覆盖。
            target.erase("look_at");
        }
        SetField(target, change);
        if (target != before) changed.push_back(wire.at(changeIndex));
        if (kind == K::Scene && target.at("type") == "camera" && change.field == "rotation" &&
            before.contains("look_at"))
            changed.push_back({{"objectIdentity", *change.objectIdentity}, {"field", "look_at"}, {"reset", true}});
        ++changeIndex;
    }
    return changed;
}

Json DescribeAssetCapabilities(K kind, const Json& document)
{
    Json result{{"fields", FieldList(kind, "")}, {"maxChanges", 128},
        {"application", "save_and_reload"}, {"validationLevel", "document_and_references"},
        {"contentPreflight", false}, {"modifyWritesFile", false}};
    if (kind == K::Scene)
    {
        result["objects"] = Json::array();
        for (const auto& object : document.at("objects"))
        {
            const std::string type = object.at("type");
            result["objects"].push_back({{"objectIdentity", type + "|" + object.at("name").get<std::string>()},
                {"fields", FieldList(kind, type)}});
        }
        result["cameraRotationClearsLookAt"] = true;
        result["intensitySemantics"] = "authored_value_in_final_intensityUnit";
    }
    if (kind == K::Mesh || kind == K::Tressfx)
    {
        result["slots"] = Json::array();
        const auto& slots = document.at("materialSlots");
        for (size_t index = 0; index < slots.size(); ++index)
            result["slots"].push_back({{"slotIndex", index}, {"expectedSlotName", slots[index].at("name")},
                {"fields", FieldList(kind, "slot")}});
    }
    return result;
}

Json ProjectEditableAsset(K kind, const Json& document)
{
    Json result = Projection(kind, "", document);
    if (kind == K::Tressfx && document.contains("simulationDefaults"))
    {
        // enabled/backend 不开放写能力，但 UI 必须用同一作者投影判断 live/Reset。
        // 保留对象存在性与原始缺省，不能在投影时把运行期缺省值烘成作者字段。
        if (!result.contains("simulationDefaults"))
            result["simulationDefaults"] = Json::object();
        const auto& defaults = document.at("simulationDefaults");
        for (const char* field : {"enabled", "backend"})
            if (defaults.contains(field))
                result["simulationDefaults"][field] = defaults.at(field);
    }
    if (kind == K::Scene)
    {
        result["worldUnit"] = document.at("worldUnit");
        result["objects"] = Json::array();
        for (const auto& object : document.at("objects"))
        {
            auto projected = Projection(kind, object.at("type"), object);
            if (object.contains("modelPath")) projected["modelPath"] = object.at("modelPath");
            result["objects"].push_back(std::move(projected));
        }
    }
    if (kind == K::Mesh || kind == K::Tressfx)
    {
        result["materialSlots"] = Json::array();
        for (const auto& slot : document.at("materialSlots"))
            result["materialSlots"].push_back(Projection(kind, "slot", slot));
    }
    return result;
}
} // namespace VL::Editor
