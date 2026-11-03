#include "editor/command/assetDocumentFields.h"
#include "editor/particles/particleModuleEdit.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace VL::Editor
{
namespace
{
using Json = nlohmann::json;
using K = EditorAssetDocumentKind;
enum class ValueKind {
    Text,
    Boolean,
    Number,
    Vector3,
    Strings,
    Channels,
    Object,
    Array,
    UserParameters
};
struct Field
{
    K kind;
    const char* scope;
    const char* name;
    ValueKind type;
    bool reset = false;
    const char* unit = "";
    const char* options = "";
    // 能力表默认公开每个字段；别名只参与写入校验，不重复暴露成第二行。
    bool hidden = false;
    // 数量型字段在协议边界就拒绝小数/负值，不能依赖运行时加载器兜底。
    bool integer = false;
    double minimum = std::numeric_limits<double>::quiet_NaN();
    double maximum = std::numeric_limits<double>::quiet_NaN();
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
    // 仅 pointLight 接受 pulse；其它灯型由 SceneAssetValidator 明确拒绝。
    {K::Scene, "pointLight", "pulse.period", ValueKind::Number, true, "seconds",
        "", false, false, 0.0001,
        std::numeric_limits<double>::quiet_NaN()},
    {K::Scene, "pointLight", "pulse.duration", ValueKind::Number, true, "seconds",
        "", false, false, 0.0001,
        std::numeric_limits<double>::quiet_NaN()},
    {K::Scene, "pointLight", "pulse.phase", ValueKind::Number, true, "seconds"},
    {K::Scene, "pointLight", "pulse.peakIntensity", ValueKind::Number, true, "cd",
        "", false, false, 0.0, 1.0e12},
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
    {K::Mesh, "", "importOptions.generateSmoothNormals", ValueKind::Boolean, true},
    {K::Mesh, "", "importOptions.includedSectionNames", ValueKind::Strings, true},
    {K::Mesh, "slot", "materialInstancePath", ValueKind::Text, false, "asset_path"},
    {K::Texture, "", "name", ValueKind::Text},
    {K::Texture, "", "source", ValueKind::Text, false, "source_path"},
    {K::Texture, "", "colorSpace", ValueKind::Text, false, "", "linear|srgb"},
    {K::Texture, "", "mipmaps", ValueKind::Boolean, true},
    {K::Texture, "", "filter", ValueKind::Text, true, "", "linear|nearest"},
    {K::Texture, "", "wrapMode", ValueKind::Text, true, "", "repeat|clamp"},
    {K::Texture, "", "channelsDescription", ValueKind::Channels, true},
    {K::ParticleSystem, "", "name", ValueKind::Text},
    {K::ParticleSystem, "", "version", ValueKind::Number},
    {K::ParticleSystem, "", "modules", ValueKind::Array},
    {K::ParticleSystem, "", "userParameters", ValueKind::UserParameters, true},
    {K::ParticleSystem, "", "position", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "localSpace", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "renderMode", ValueKind::Text, false, "", "billboard|ribbon|mesh|beam"},
    {K::ParticleSystem, "", "beam.start", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "beam.end", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "beam.segments", ValueKind::Number, true,
        "", "", false, true, 2.0, 64.0},
    {K::ParticleSystem, "", "beam.width", ValueKind::Number, true, "cm",
        "", false, false, 0.0001, std::numeric_limits<double>::quiet_NaN()},
    {K::ParticleSystem, "", "beam.count", ValueKind::Number, true,
        "", "", false, true, 1.0, 16.0},
    {K::ParticleSystem, "", "beam.noiseAmplitude", ValueKind::Number, true, "cm",
        "", false, false, 0.0, std::numeric_limits<double>::quiet_NaN()},
    {K::ParticleSystem, "", "beam.noiseFrequency", ValueKind::Number, true,
        "", "", false, false, 0.0001, std::numeric_limits<double>::quiet_NaN()},
    {K::ParticleSystem, "", "beam.noiseSpeed", ValueKind::Number, true,
        "", "", false, false, 0.0, std::numeric_limits<double>::quiet_NaN()},
    {K::ParticleSystem, "", "beam.endpointJitter", ValueKind::Number, true, "cm",
        "", false, false, 0.0, std::numeric_limits<double>::quiet_NaN()},
    {K::ParticleSystem, "", "beam.branchCount", ValueKind::Number, true,
        "", "", false, true, 0.0, 4.0},
    {K::ParticleSystem, "", "beam.branchStart", ValueKind::Number, true,
        "", "", false, false, 0.0, 1.0},
    {K::ParticleSystem, "", "beam.branchLength", ValueKind::Number, true,
        "", "", false, false, 0.0001, 1.0},
    {K::ParticleSystem, "", "beam.branchSpread", ValueKind::Number, true, "cm",
        "", false, false, 0.0, std::numeric_limits<double>::quiet_NaN()},
    {K::ParticleSystem, "", "billboardAlignment", ValueKind::Text, true, "", "camera|velocity|fixed"},
    {K::ParticleSystem, "", "billboardNormal", ValueKind::Vector3, true},
    {K::ParticleSystem, "", "billboardUp", ValueKind::Vector3, true},
    {K::ParticleSystem, "", "billboardShadows", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "billboardLighting", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "billboardAmbient", ValueKind::Number, true,
        "", "", false, false, 0.0, 1.0},
    {K::ParticleSystem, "", "ribbonWidth", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "ribbonSegments", ValueKind::Number, true},
    {K::ParticleSystem, "", "ribbonFade", ValueKind::Number, true},
    {K::ParticleSystem, "", "mesh.path", ValueKind::Text, true, "asset_path"},
    {K::ParticleSystem, "", "mesh.scale", ValueKind::Vector3, true},
    {K::ParticleSystem, "", "mesh.alignment", ValueKind::Text, true, "", "world|camera|velocity"},
    {K::ParticleSystem, "", "mesh.materialInstancePath", ValueKind::Text, true, "asset_path"},
    // 旧资产把材质实例路径放在 emitter 根；能力表只公开 mesh 路径，
    // 根别名仍可写入并由 SetParticleField 规范化到 mesh。
    {K::ParticleSystem, "", "materialInstancePath", ValueKind::Text, true, "asset_path", "", true},
    {K::ParticleSystem, "", "meshLighting", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "meshAmbient", ValueKind::Number, true},
    {K::ParticleSystem, "", "useFixedBounds", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "fixedBounds.center", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "fixedBounds.extent", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "maxParticles", ValueKind::Number},
    {K::ParticleSystem, "", "randomSeed", ValueKind::Number, true, "", "", false, true, 0.0,
        4294967295.0},
    {K::ParticleSystem, "", "deterministicRandom", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "fixedTimeStep", ValueKind::Number, true, "s", "", false, false,
        0.0, 0.25},
    {K::ParticleSystem, "", "burst", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "burstInterval", ValueKind::Number, true, "s"},
    {K::ParticleSystem, "", "spawnRate", ValueKind::Number},
    {K::ParticleSystem, "", "lodDistance", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "lodSpawnRateScale", ValueKind::Number, true},
    {K::ParticleSystem, "", "lod.tiers", ValueKind::Array, true},
    {K::ParticleSystem, "", "lifetime", ValueKind::Array},
    {K::ParticleSystem, "", "lifetimeScaleKeys", ValueKind::Array, true},
    {K::ParticleSystem, "", "shape.type", ValueKind::Text, false, "", "sphere|box|cone|cylinder|disc|ring"},
    {K::ParticleSystem, "", "shape.radius", ValueKind::Number, false, "cm"},
    {K::ParticleSystem, "", "shape.extent", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "shape.height", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "shape.innerRadius", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "velocity.min", ValueKind::Vector3, false, "cm/s"},
    {K::ParticleSystem, "", "velocity.max", ValueKind::Vector3, false, "cm/s"},
    {K::ParticleSystem, "", "gravity", ValueKind::Vector3, false, "cm/s^2"},
    {K::ParticleSystem, "", "drag", ValueKind::Number},
    {K::ParticleSystem, "", "curlNoise.strength", ValueKind::Number, true, "cm/s^2"},
    {K::ParticleSystem, "", "curlNoise.frequency", ValueKind::Number, true, "1/cm"},
    {K::ParticleSystem, "", "curlNoise.speed", ValueKind::Number, true, "1/s"},
    {K::ParticleSystem, "", "vortex.strength", ValueKind::Number, false, "cm/s^2"},
    {K::ParticleSystem, "", "vortex.radius", ValueKind::Number, false, "cm"},
    {K::ParticleSystem, "", "vortex.axis", ValueKind::Vector3, false},
    {K::ParticleSystem, "", "attractor.position", ValueKind::Vector3, false, "cm"},
    {K::ParticleSystem, "", "attractor.strength", ValueKind::Number, false, "cm/s^2"},
    {K::ParticleSystem, "", "attractor.radius", ValueKind::Number, false, "cm"},
    {K::ParticleSystem, "", "wind", ValueKind::Vector3, false, "cm/s^2"},
    {K::ParticleSystem, "", "size", ValueKind::Array, false, "cm"},
    {K::ParticleSystem, "", "rotation", ValueKind::Number, true, "degrees"},
    {K::ParticleSystem, "", "rotationSpeed", ValueKind::Number, true, "degrees/s"},
    {K::ParticleSystem, "", "rotationRandom", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "softness", ValueKind::Number},
    {K::ParticleSystem, "", "softParticles", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "softParticleDistance", ValueKind::Number, false, "cm"},
    {K::ParticleSystem, "", "sortParticles", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "opacity", ValueKind::Number},
    {K::ParticleSystem, "", "maxDrawDistance", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "fadeDistance", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "minScreenSize", ValueKind::Number, true, "px"},
    {K::ParticleSystem, "", "maxScreenSize", ValueKind::Number, true, "px"},
    {K::ParticleSystem, "", "collision.type", ValueKind::Text, true, "", "none|plane|box|capsule|sdf|mesh"},
    {K::ParticleSystem, "", "collision.bounce", ValueKind::Number, true},
    {K::ParticleSystem, "", "collision.friction", ValueKind::Number, true},
    {K::ParticleSystem, "", "collision.radius", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "collision.killOnCollision", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "collision.ccd", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "collision.plane.normal", ValueKind::Vector3, true},
    {K::ParticleSystem, "", "collision.plane.offset", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "collision.box.min", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "collision.box.max", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "collision.capsule.a", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "collision.capsule.b", ValueKind::Vector3, true, "cm"},
    {K::ParticleSystem, "", "collision.capsule.radius", ValueKind::Number, true, "cm"},
    {K::ParticleSystem, "", "collision.sdf.path", ValueKind::Text, true, "asset_path"},
    {K::ParticleSystem, "", "collision.mesh.path", ValueKind::Text, true, "asset_path"},
    {K::ParticleSystem, "", "events.spawn", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "events.death", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "events.collision", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "colorStart", ValueKind::Array},
    {K::ParticleSystem, "", "colorEnd", ValueKind::Array},
    {K::ParticleSystem, "", "texture", ValueKind::Text, true, "asset_path"},
    {K::ParticleSystem, "", "blend", ValueKind::Text, false, "", "additive|alpha"},
    {K::ParticleSystem, "", "flipbook.columns", ValueKind::Number},
    {K::ParticleSystem, "", "flipbook.rows", ValueKind::Number},
    {K::ParticleSystem, "", "flipbook.fps", ValueKind::Number},
    {K::ParticleSystem, "", "flipbook.startFrame", ValueKind::Number},
    {K::ParticleSystem, "", "flipbook.endFrame", ValueKind::Number},
    {K::ParticleSystem, "", "flipbook.loop", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "flipbook.randomStartFrame", ValueKind::Boolean, true},
    {K::ParticleSystem, "", "flipbook.frameDurations", ValueKind::Array, true},
    {K::ParticleSystem, "", "flipbook.framePivots", ValueKind::Array, true},
    {K::ParticleSystem, "", "flipbook.frameUVs", ValueKind::Array, true},
    {K::ParticleSystem, "", "flipbook.frameRotations", ValueKind::Array, true},
    {K::ParticleSystem, "", "colorKeys", ValueKind::Array, true},
    {K::ParticleSystem, "", "sizeKeys", ValueKind::Array, true},
    // 受控的结构字段：不属于任何编辑模块，只用于整条发射器数组增删/重排。
    {K::ParticleSystem, "", "emitters", ValueKind::Array},
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

struct ParticleFieldPath
{
    bool hasEmitter = false;
    size_t emitterIndex = 0;
    std::string field;
};

// PS 的多发射器路径是 emitters.<index>.<field>。这里只做语法与 v1 上限
// 检查；索引是否落在当前文档内由 ApplyAssetChanges 对照候选数组判断。
ParticleFieldPath ParseParticleFieldPath(const std::string& path)
{
    ParticleFieldPath result;
    result.field = path;
    if (path.rfind("emitters.", 0) != 0) return result;

    const size_t indexStart = std::string("emitters.").size();
    const size_t separator = path.find('.', indexStart);
    if (separator == std::string::npos || separator == indexStart ||
        separator + 1 == path.size())
        throw std::invalid_argument("Particle emitter path requires emitters.<index>.<field>");
    size_t index = 0;
    for (size_t cursor = indexStart; cursor < separator; ++cursor)
    {
        const unsigned char character = static_cast<unsigned char>(path[cursor]);
        if (!std::isdigit(character))
            throw std::invalid_argument("Particle emitter path index must be numeric");
        index = index * 10 + static_cast<size_t>(character - '0');
        if (index >= 16)
            throw std::invalid_argument("Particle emitter path index exceeds the 16-emitter limit");
    }
    result.hasEmitter = true;
    result.emitterIndex = index;
    result.field = path.substr(separator + 1);
    return result;
}

const Field& FindField(K kind, const EditorAssetFieldChange& change)
{
    if (change.objectIdentity && (kind != K::Scene || change.slotIndex ||
        change.expectedSlotName || change.expectedEmitterName))
        throw std::invalid_argument("objectIdentity is only valid for scene object fields");
    if (change.slotIndex.has_value() != change.expectedSlotName.has_value() ||
        (change.slotIndex && kind != K::Mesh) ||
        (change.expectedSlotName && change.expectedSlotName->empty()))
        throw std::invalid_argument("Mesh slots require slotIndex and non-empty expectedSlotName");
    ParticleFieldPath particlePath;
    if (kind == K::ParticleSystem)
        particlePath = ParseParticleFieldPath(change.field);
    if (change.expectedEmitterName &&
        (kind != K::ParticleSystem || !particlePath.hasEmitter ||
         change.expectedEmitterName->empty() || change.slotIndex ||
         change.expectedSlotName))
        throw std::invalid_argument(
            "Particle emitter identity requires emitters.<index>.<field> and a non-empty expectedEmitterName");
    const auto scope = Scope(change);
    const std::string fieldName = kind == K::ParticleSystem
        ? particlePath.field
        : change.field;
    for (const auto& field : fields)
        if (field.kind == kind && fieldName == field.name && MatchesScope(scope, field.scope))
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
    case ValueKind::UserParameters: return "object";
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
        if (value.is_number())
        {
            const double number = value.get<double>();
            if (!std::isfinite(number) ||
                !std::isfinite(static_cast<float>(number)))
                break;
            if (field.integer && std::floor(number) != number) break;
            if (std::isfinite(field.minimum) && number < field.minimum)
                break;
            if (std::isfinite(field.maximum) && number > field.maximum)
                break;
            return number;
        }
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
    case ValueKind::UserParameters:
        if (value.is_object() && value.size() <= 64)
        {
            bool valid = true;
            for (auto parameter = value.begin();
                 parameter != value.end(); ++parameter)
            {
                if (parameter.key().empty() ||
                    !parameter.value().is_number())
                {
                    valid = false;
                    break;
                }
                const double number = parameter.value().get<double>();
                if (!std::isfinite(number) ||
                    !std::isfinite(static_cast<float>(number)))
                {
                    valid = false;
                    break;
                }
            }
            if (valid) return value;
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

void SetDottedField(
    Json& target,
    const std::string& path,
    const EditorAssetFieldChange& change)
{
    Json* node = &target;
    size_t start = 0;
    while (true)
    {
        const size_t separator = path.find('.', start);
        const bool last = separator == std::string::npos;
        const std::string segment = path.substr(
            start, last ? std::string::npos : separator - start);
        if (segment.empty())
            throw std::invalid_argument("Particle field path contains an empty segment");
        if (!node->is_object())
            throw std::invalid_argument("Particle field path parent is not an object");
        if (last)
        {
            if (change.reset) node->erase(segment);
            else (*node)[segment] = ValueJson(change.value);
            return;
        }
        if (!node->contains(segment))
        {
            if (change.reset) return;
            (*node)[segment] = Json::object();
        }
        node = &node->at(segment);
        start = separator + 1;
    }
}

// materialInstancePath 的规范位置是 mesh 子对象；旧资产允许 emitter 根别名。
// 规范化写入后必须删除根/模块里的别名，否则 loader 的根字段会覆盖 mesh 值。
void RemoveParticleMaterialInstancePathAlias(Json& emitter)
{
    if (!emitter.is_object()) return;
    emitter.erase("materialInstancePath");
    const auto mesh = emitter.find("mesh");
    if (mesh != emitter.end() && mesh->is_object() && mesh->empty())
        emitter.erase(mesh);
    const auto modules = emitter.find("modules");
    if (modules == emitter.end() || !modules->is_array()) return;
    for (auto& module : *modules)
    {
        if (!module.is_object()) continue;
        const auto fields = module.find("fields");
        if (fields == module.end() || !fields->is_object()) continue;
        fields->erase("materialInstancePath");
        const auto moduleMesh = fields->find("mesh");
        if (moduleMesh != fields->end() && moduleMesh->is_object() &&
            moduleMesh->empty())
            fields->erase(moduleMesh);
    }
}

void SetParticleField(
    Json& target,
    const EditorAssetFieldChange& change,
    const std::map<size_t, std::string>* originalNames)
{
    const ParticleFieldPath path = ParseParticleFieldPath(change.field);
    const bool v2 = target.value("version", 1) == 2;
    if (change.field == "userParameters" && !v2)
        throw std::invalid_argument(
            "Particle userParameters requires version 2");
    if (path.hasEmitter && path.field == "userParameters")
        throw std::invalid_argument(
            "Particle userParameters is a system-level field");
    // 根别名与 mesh 路径共享同一个作者值；写入统一走规范路径。
    const std::string outputField =
        path.field == "materialInstancePath"
        ? "mesh.materialInstancePath"
        : path.field;
    if (!path.hasEmitter)
    {
        const bool explicitEmitters =
            target.is_object() && target.contains("emitters") &&
            target.at("emitters").is_array();
        if (explicitEmitters)
        {
            // 显式多发射器文档的根对象只拥有系统名和结构数组；
            // 其余字段写到根会被 ParseJson 忽略，必须明确拒绝。
            if (change.field != "name" && change.field != "emitters" &&
                change.field != "version" && change.field != "userParameters")
                throw std::invalid_argument(
                    "Particle field requires emitters.<index>.<field>: " +
                    change.field);
            SetField(target, change);
            return;
        }
        // 隐式 v2 emitter：根字段按模块 owner 写回 modules[].fields。
        if (v2 && change.field != "name" && change.field != "version" &&
            change.field != "modules" && change.field != "userParameters")
        {
            if (!SetParticleEmitterField(
                    target,
                    outputField,
                    ValueJson(change.value),
                    change.reset))
            {
                throw std::invalid_argument(
                    "Particle v2 field could not be written: " +
                    change.field);
            }
            if (outputField == "mesh.materialInstancePath")
                RemoveParticleMaterialInstancePathAlias(target);
            return;
        }
        // 旧资产没有 emitters 数组；根字段必须保持原有写入语义。
        // position 只属于显式 emitter，静默写到根对象会被解析器忽略。
        if (change.field == "position")
            throw std::invalid_argument("Particle position requires an explicit emitters array");
        // 旧文档的根字段同样可能有多级路径（例如 collision.plane.normal），
        // 不能退回只支持两级的 SetField。
        SetDottedField(target, outputField, change);
        if (outputField == "mesh.materialInstancePath")
            RemoveParticleMaterialInstancePathAlias(target);
        return;
    }
    if (!target.is_object() || !target.contains("emitters") ||
        !target.at("emitters").is_array())
        throw std::invalid_argument("Particle emitters array is missing");
    Json& emitters = target.at("emitters");
    if (path.emitterIndex >= emitters.size())
        throw std::invalid_argument("Particle emitter index is out of range");
    Json& emitter = emitters.at(path.emitterIndex);
    if (change.expectedEmitterName)
    {
        std::string identity = emitter.value(
            "name", "emitter" + std::to_string(path.emitterIndex));
        if (originalNames != nullptr)
        {
            const auto found =
                originalNames->find(path.emitterIndex);
            if (found != originalNames->end())
                identity = found->second;
        }
        if (identity != *change.expectedEmitterName)
            throw std::invalid_argument("Particle emitter identity changed");
    }
    if (v2)
    {
        if (!SetParticleEmitterField(
                emitter,
                outputField,
                ValueJson(change.value),
                change.reset))
        {
            throw std::invalid_argument(
                "Particle v2 field could not be written: " +
                path.field);
        }
        if (outputField == "mesh.materialInstancePath")
            RemoveParticleMaterialInstancePathAlias(emitter);
        return;
    }
    SetDottedField(emitter, outputField, change);
    if (outputField == "mesh.materialInstancePath")
        RemoveParticleMaterialInstancePathAlias(emitter);
}

Json FieldDescription(const Field& field)
{
    Json result{{"field", field.name}, {"type", TypeName(field.type)}, {"resettable", field.reset},
        {"unit", field.unit}};
    if (field.integer) result["integer"] = true;
    if (std::isfinite(field.minimum)) result["minimum"] = field.minimum;
    if (std::isfinite(field.maximum)) result["maximum"] = field.maximum;
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
        if (field.kind == kind && !field.hidden &&
            MatchesScope(scope, field.scope))
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
        if (field.kind != kind || field.hidden ||
            !MatchesScope(scope, field.scope)) continue;
        const std::string name(field.name);
        // 根 materialInstancePath 是旧别名；投影统一暴露在 mesh 下，
        // 编辑时再由写入路径规范化并删除根别名。
        const bool hasMeshMaterial =
            source.is_object() && source.contains("mesh") &&
            source.at("mesh").is_object() &&
            source.at("mesh").contains("materialInstancePath");
        if (name == "mesh.materialInstancePath" && !hasMeshMaterial &&
            source.contains("materialInstancePath"))
        {
            result["mesh"]["materialInstancePath"] =
                source.at("materialInstancePath");
            continue;
        }
        const auto dot = name.find('.');
        if (dot == std::string::npos)
        {
            if (source.contains(name)) result[name] = source.at(name);
        }
        else
        {
            const Json* value = &source;
            size_t start = 0;
            bool found = true;
            while (true)
            {
                const size_t separator = name.find('.', start);
                const bool last = separator == std::string::npos;
                const std::string segment = name.substr(
                    start,
                    last
                        ? std::string::npos
                        : separator - start);
                if (!value->is_object() || !value->contains(segment))
                {
                    found = false;
                    break;
                }
                value = &value->at(segment);
                if (last) break;
                start = separator + 1;
            }
            if (!found) continue;

            Json* target = &result;
            start = 0;
            while (true)
            {
                const size_t separator = name.find('.', start);
                const bool last = separator == std::string::npos;
                const std::string segment = name.substr(
                    start,
                    last
                        ? std::string::npos
                        : separator - start);
                if (last)
                {
                    (*target)[segment] = *value;
                    break;
                }
                if (!target->contains(segment))
                    (*target)[segment] = Json::object();
                target = &target->at(segment);
                start = separator + 1;
            }
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
        if (change.expectedEmitterName) item["expectedEmitterName"] = *change.expectedEmitterName;
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
                entry.key() != "objectIdentity" && entry.key() != "slotIndex" &&
                entry.key() != "expectedSlotName" && entry.key() != "expectedEmitterName")
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
        if (item.contains("expectedEmitterName")) change.expectedEmitterName = item.at("expectedEmitterName").get<std::string>();
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
    // 同一 batch 可能先改发射器名、再改其他字段；身份校验必须对照
    // 本 batch 开始前的名字，而不是已被前一条 change 改写的中间值。
    std::map<size_t, std::string> particleOriginalNames;
    if (kind == K::ParticleSystem &&
        candidate.contains("emitters") &&
        candidate.at("emitters").is_array())
    {
        for (const auto& change : changes)
        {
            if (!change.expectedEmitterName) continue;
            const auto path = ParseParticleFieldPath(change.field);
            if (!path.hasEmitter ||
                path.emitterIndex >= candidate.at("emitters").size())
                continue;
            particleOriginalNames[path.emitterIndex] =
                candidate.at("emitters").at(path.emitterIndex).value(
                    "name",
                    "emitter" + std::to_string(path.emitterIndex));
        }
    }
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
        if (kind == K::ParticleSystem)
            SetParticleField(target, change,
                particleOriginalNames.empty()
                    ? nullptr : &particleOriginalNames);
        else SetField(target, change);
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
    if (kind == K::Mesh)
    {
        result["slots"] = Json::array();
        const auto& slots = document.at("materialSlots");
        for (size_t index = 0; index < slots.size(); ++index)
            result["slots"].push_back({{"slotIndex", index}, {"expectedSlotName", slots[index].at("name")},
                {"fields", FieldList(kind, "slot")}});
    }
    if (kind == K::ParticleSystem)
    {
        const bool v2 = document.value("version", 1) == 2;
        const bool explicitEmitters =
            document.contains("emitters") &&
            document.at("emitters").is_array();
        const Json allFields = FieldList(kind, "");
        Json emitterFields = allFields;
        emitterFields.erase(
            std::remove_if(
                emitterFields.begin(), emitterFields.end(),
                [](const Json& field)
                {
                    return field.value("field", std::string()) ==
                        "emitters" ||
                        field.value("field", std::string()) ==
                        "userParameters";
                }),
            emitterFields.end());
        if (!explicitEmitters)
        {
            // 隐式 emitter 没有局部 position；能力表不能 advertise 一个
            // 会被 ParseJson 忽略的字段。
            result["fields"] = allFields;
            result["fields"].erase(
                std::remove_if(
                    result["fields"].begin(), result["fields"].end(),
                    [v2](const Json& field)
                    {
                        const std::string name =
                            field.value("field", std::string());
                        return name == "position" ||
                            (!v2 && name == "userParameters");
                    }),
                result["fields"].end());
        }
        else
        {
            // 显式多发射器文档的根只拥有系统名和结构数组；emitter 字段
            // 只能通过 emitters.<index>.<field> 写入。
            result["fields"] = Json::array();
            for (const auto& field : allFields)
            {
                const std::string name =
                    field.value("field", std::string());
                if (name == "name" || name == "emitters" ||
                    (v2 && name == "userParameters"))
                    result["fields"].push_back(field);
            }
        }
        result["emitters"] = Json::array();
        if (explicitEmitters)
        {
            const auto& emitters = document.at("emitters");
            for (size_t index = 0; index < emitters.size(); ++index)
            {
                const std::string name = emitters.at(index).value(
                    "name", "emitter" + std::to_string(index));
                result["emitters"].push_back({{"index", index}, {"name", name},
                    {"fields", emitterFields}});
            }
        }
        else
        {
            // 旧 PS 的隐式发射器仍需要出现在选择器里，但字段路径保持根路径。
            Json implicitFields = emitterFields;
            implicitFields.erase(
                std::remove_if(
                    implicitFields.begin(), implicitFields.end(),
                    [](const Json& field)
                    {
                        return field.value("field", std::string()) ==
                            "position";
                    }),
                implicitFields.end());
            result["emitters"].push_back({{"index", 0}, {"name", "emitter0"},
                {"fields", implicitFields}});
        }
    }
    return result;
}

Json ProjectEditableAsset(K kind, const Json& document)
{
    Json result = Projection(kind, "", document);
    if (kind == K::ParticleSystem &&
        document.contains("emitters") &&
        document.at("emitters").is_array())
    {
        // 系统级 name 来自根对象；每个 emitter 只投影自己的作者字段。
        result["emitters"] = Json::array();
        for (const auto& emitter : document.at("emitters"))
            result["emitters"].push_back(Projection(kind, "", emitter));
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
    if (kind == K::Mesh)
    {
        result["materialSlots"] = Json::array();
        for (const auto& slot : document.at("materialSlots"))
            result["materialSlots"].push_back(Projection(kind, "slot", slot));
    }
    return result;
}
} // namespace VL::Editor
