#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/editorBuild.h"

static_assert(VL_WITH_EDITOR != 0,
    "editor/particles/particleModuleEdit.h requires VL_WITH_EDITOR=1");

namespace VL
{

// 编辑器模块列表的展示顺序与字段归属。运行时只消费展平后的 emitter，
// 因此这里不引入 Eigen 或运行时资产类型。
struct ParticleModuleSpec
{
    std::string type;
    std::string label;
    std::vector<std::string> fields;
};

inline const std::vector<ParticleModuleSpec>& ParticleModuleSpecs()
{
    static const std::vector<ParticleModuleSpec> specs = {
        {"emitter", "Emitter",
            {"maxParticles", "localSpace", "sortParticles", "sortOrder",
                "randomSeed", "deterministicRandom", "fixedTimeStep"}},
        {"renderer", "Renderer",
            {"renderMode", "billboardAlignment", "billboardNormal",
                "billboardUp", "ribbonWidth",
                "ribbonSegments", "ribbonFade", "mesh", "beam", "light",
                "decal",
                "opacity",
                "softness", "softParticles", "softParticleDistance",
                "distortionStrength",
                "additive", "meshLighting", "meshAmbient",
                "materialInstancePath", "billboardShadows",
                "billboardLighting", "billboardAmbient",
                "billboardLightingStrength",
                "beam.colorKeys", "beam.widthKeys", "beam.arcs",
                "beam.hitScan"}},
        {"spawn.rate", "Spawn Rate",
            {"spawnRate", "burst", "burstInterval"}},
        {"spawn.shapeSphere", "Sphere Location", {"shape"}},
        {"spawn.velocity", "Initial Velocity", {"velocity"}},
        {"update.lifetime", "Lifetime",
            {"lifetime", "lifetimeScaleKeys"}},
        {"update.color", "Color",
            {"colorStart", "colorEnd", "colorKeys"}},
        {"update.size", "Size", {"size", "sizeKeys"}},
        {"update.rotation", "Rotation",
            {"rotation", "rotationSpeed", "rotationRandom"}},
        {"forces.gravity", "Gravity Force", {"gravity"}},
        {"forces.drag", "Drag", {"drag"}},
        {"forces.curlNoise", "Curl Noise", {"curlNoise"}},
        {"forces.vortex", "Vortex", {"vortex"}},
        {"forces.attractor", "Point Attractor", {"attractor"}},
        {"forces.wind", "Wind", {"wind"}},
        {"collision.analytic", "Analytic Collision", {"collision"}},
        {"events", "Particle Events", {"events"}},
        {"flipbook", "Flipbook", {"texture", "flipbook"}},
        {"lod", "LOD",
            {"lod", "lodDistance", "lodSpawnRateScale"}},
        {"curves", "Curves", {}},
        {"bounds", "Fixed Bounds", {"useFixedBounds", "fixedBounds"}},
        {"culling", "Culling",
            {"maxDrawDistance", "fadeDistance", "minScreenSize",
                "maxScreenSize"}},
    };
    return specs;
}

inline const ParticleModuleSpec* FindParticleModuleSpec(
    std::string_view type)
{
    const std::vector<ParticleModuleSpec>& specs =
        ParticleModuleSpecs();
    for (const ParticleModuleSpec& spec : specs)
    {
        if (spec.type == type) return &spec;
    }
    return nullptr;
}

namespace ParticleModuleEditDetail
{

inline bool HasPrefix(
    std::string_view value,
    std::string_view prefix)
{
    return value.size() >= prefix.size() &&
        value.substr(0, prefix.size()) == prefix;
}

inline bool IsOneOf(
    std::string_view value,
    std::initializer_list<std::string_view> candidates)
{
    for (std::string_view candidate : candidates)
    {
        if (value == candidate) return true;
    }
    return false;
}

inline bool SplitFieldPath(
    std::string_view field,
    std::vector<std::string>& segments)
{
    if (field.empty()) return false;
    size_t start = 0;
    while (start <= field.size())
    {
        const size_t dot = field.find('.', start);
        const size_t end =
            dot == std::string_view::npos ? field.size() : dot;
        if (end == start) return false;
        segments.emplace_back(field.substr(start, end - start));
        if (dot == std::string_view::npos) break;
        start = dot + 1;
    }
    return !segments.empty();
}

inline bool ApplyFieldPath(
    nlohmann::json& target,
    const std::vector<std::string>& segments,
    const nlohmann::json& value,
    bool reset)
{
    if (!target.is_object() || segments.empty()) return false;
    nlohmann::json* cursor = &target;
    for (size_t index = 0; index + 1 < segments.size(); ++index)
    {
        const std::string& key = segments[index];
        if (reset)
        {
            const auto child = cursor->find(key);
            if (child == cursor->end() || !child->is_object())
                return true;
            cursor = &(*child);
            continue;
        }
        auto child = cursor->find(key);
        if (child == cursor->end() || !child->is_object())
        {
            (*cursor)[key] = nlohmann::json::object();
            child = cursor->find(key);
        }
        cursor = &(*child);
    }
    if (reset)
        cursor->erase(segments.back());
    else
        (*cursor)[segments.back()] = value;
    return true;
}

inline void MergeOverride(
    nlohmann::json& target,
    const nlohmann::json& source)
{
    for (auto field = source.begin(); field != source.end(); ++field)
    {
        auto existing = target.find(field.key());
        if (existing != target.end() &&
            existing->is_object() && field.value().is_object())
        {
            MergeOverride(*existing, field.value());
            continue;
        }
        target[field.key()] = field.value();
    }
}

inline nlohmann::json* ModuleArray(nlohmann::json& emitter)
{
    if (!emitter.is_object()) return nullptr;
    const auto modules = emitter.find("modules");
    if (modules == emitter.end() || !modules->is_array())
        return nullptr;
    return &(*modules);
}

inline bool ValidModules(const nlohmann::json& modules)
{
    if (!modules.is_array() || modules.size() > 64) return false;
    for (const nlohmann::json& module : modules)
    {
        if (!module.is_object()) return false;
        const auto type = module.find("type");
        if (type == module.end() || !type->is_string())
            return false;
        const auto enabled = module.find("enabled");
        if (enabled != module.end() && !enabled->is_boolean())
            return false;
        const auto fields = module.find("fields");
        if (fields != module.end() && !fields->is_object())
            return false;
    }
    return true;
}

inline bool FindModuleIndex(
    const nlohmann::json& modules,
    std::string_view type,
    size_t& index)
{
    for (size_t current = 0; current < modules.size(); ++current)
    {
        const nlohmann::json& module = modules.at(current);
        if (module.at("type").get_ref<const std::string&>() == type)
        {
            index = current;
            return true;
        }
    }
    return false;
}

} // namespace ParticleModuleEditDetail

inline std::string ParticleModuleTypeForField(std::string_view field)
{
    using namespace ParticleModuleEditDetail;
    if (IsOneOf(field,
        {"maxParticles", "localSpace", "sortParticles", "sortOrder",
            "randomSeed",
            "deterministicRandom", "fixedTimeStep"}))
        return "emitter";
    if (IsOneOf(field,
        {"renderMode", "billboardAlignment", "billboardNormal",
            "billboardUp", "ribbonWidth",
            "ribbonSegments", "ribbonFade", "mesh", "beam", "light",
            "decal",
            "opacity",
            "softness", "softParticles", "softParticleDistance",
            "distortionStrength",
            "additive", "meshLighting", "meshAmbient",
            "materialInstancePath", "billboardShadows",
            "billboardLighting", "billboardAmbient",
            "billboardLightingStrength",
            "beam.colorKeys", "beam.widthKeys", "beam.arcs"}) ||
        HasPrefix(field, "mesh.") ||
        HasPrefix(field, "beam.") ||
        HasPrefix(field, "light.") ||
        HasPrefix(field, "decal."))
        return "renderer";
    if (IsOneOf(field, {"spawnRate", "burst", "burstInterval"}))
        return "spawn.rate";
    if (field == "shape" || HasPrefix(field, "shape."))
        return "spawn.shapeSphere";
    if (field == "velocity" || HasPrefix(field, "velocity."))
        return "spawn.velocity";
    if (IsOneOf(field, {"lifetime", "lifetimeScaleKeys"}))
        return "update.lifetime";
    if (IsOneOf(field, {"colorStart", "colorEnd", "colorKeys"}))
        return "update.color";
    if (IsOneOf(field, {"size", "sizeKeys"})) return "update.size";
    if (IsOneOf(field,
        {"rotation", "rotationSpeed", "rotationRandom"}))
        return "update.rotation";
    if (field == "gravity") return "forces.gravity";
    if (field == "drag") return "forces.drag";
    if (field == "curlNoise" || HasPrefix(field, "curlNoise."))
        return "forces.curlNoise";
    if (field == "vortex" || HasPrefix(field, "vortex."))
        return "forces.vortex";
    if (field == "attractor" || HasPrefix(field, "attractor."))
        return "forces.attractor";
    if (field == "wind" || HasPrefix(field, "wind."))
        return "forces.wind";
    if (field == "collision" || HasPrefix(field, "collision."))
        return "collision.analytic";
    if (field == "events" || HasPrefix(field, "events."))
        return "events";
    if (field == "texture" || field == "flipbook" ||
        HasPrefix(field, "flipbook."))
        return "flipbook";
    if (IsOneOf(field,
        {"lod", "lodDistance", "lodSpawnRateScale"}) ||
        HasPrefix(field, "lod."))
        return "lod";
    if (field == "useFixedBounds" || field == "fixedBounds" ||
        HasPrefix(field, "fixedBounds."))
        return "bounds";
    if (IsOneOf(field,
        {"maxDrawDistance", "fadeDistance", "minScreenSize",
            "maxScreenSize"}))
        return "culling";
    return {};
}

// 旧文档没有 modules 时按字段归属一次性迁移。name/position 与未知字段
// 留在 base；所有模块都有显式 enabled，后续编辑器可以直接重排或禁用。
inline bool EnsureParticleModules(nlohmann::json& emitter)
{
    using namespace ParticleModuleEditDetail;
    if (!emitter.is_object()) return false;
    const auto existing = emitter.find("modules");
    if (existing != emitter.end()) return existing->is_array();

    nlohmann::json working = emitter;
    std::unordered_map<std::string, nlohmann::json> moduleFields;
    std::vector<std::string> migrated;
    for (auto field = working.begin(); field != working.end(); ++field)
    {
        const std::string type =
            ParticleModuleTypeForField(field.key());
        if (type.empty()) continue;
        moduleFields[type][field.key()] = field.value();
        migrated.push_back(field.key());
    }
    for (const std::string& field : migrated)
        working.erase(field);

    nlohmann::json modules = nlohmann::json::array();
    for (const ParticleModuleSpec& spec : ParticleModuleSpecs())
    {
        const auto fields = moduleFields.find(spec.type);
        if (spec.type != "emitter" && fields == moduleFields.end())
            continue;
        nlohmann::json module = {
            {"type", spec.type},
            {"enabled", true},
            {"fields", fields == moduleFields.end()
                ? nlohmann::json::object() : fields->second},
        };
        modules.push_back(std::move(module));
    }
    working["modules"] = std::move(modules);
    emitter = std::move(working);
    return true;
}

// 编辑投影允许后写覆盖先写；运行时 loader 仍会对同名字段冲突报错。
inline nlohmann::json FlattenParticleEmitter(
    const nlohmann::json& emitter)
{
    using namespace ParticleModuleEditDetail;
    nlohmann::json flattened = emitter;
    if (!emitter.is_object()) return flattened;
    const auto modules = emitter.find("modules");
    if (modules == emitter.end() || !modules->is_array())
        return flattened;
    if (!ValidModules(*modules)) return flattened;

    flattened.erase("modules");
    for (const nlohmann::json& module : *modules)
    {
        const bool enabled = module.value("enabled", true);
        if (!enabled) continue;
        const auto fields = module.find("fields");
        if (fields == module.end()) continue;
        MergeOverride(flattened, *fields);
    }
    return flattened;
}

inline bool SetParticleEmitterField(
    nlohmann::json& emitter,
    std::string_view field,
    const nlohmann::json& value,
    bool reset)
{
    using namespace ParticleModuleEditDetail;
    if (!emitter.is_object()) return false;
    std::vector<std::string> segments;
    if (!SplitFieldPath(field, segments)) return false;

    nlohmann::json working = emitter;
    const std::string type = ParticleModuleTypeForField(field);
    if (type.empty())
    {
        if (!ApplyFieldPath(working, segments, value, reset))
            return false;
        emitter = std::move(working);
        return true;
    }

    if (!EnsureParticleModules(working)) return false;
    nlohmann::json* modules = ModuleArray(working);
    if (modules == nullptr || !ValidModules(*modules)) return false;

    size_t moduleIndex = 0;
    if (!FindModuleIndex(*modules, type, moduleIndex))
    {
        if (modules->size() >= 64) return false;
        nlohmann::json module = {
            {"type", type},
            {"enabled", true},
            {"fields", nlohmann::json::object()},
        };
        modules->push_back(std::move(module));
        moduleIndex = modules->size() - 1;
    }
    nlohmann::json& target = modules->at(moduleIndex);
    if (!target.contains("fields"))
        target["fields"] = nlohmann::json::object();
    if (!target.at("fields").is_object()) return false;
    if (!ApplyFieldPath(target["fields"], segments, value, reset))
        return false;
    emitter = std::move(working);
    return true;
}

inline bool AddParticleModule(
    nlohmann::json& emitter,
    std::string_view type,
    size_t& index)
{
    using namespace ParticleModuleEditDetail;
    if (!emitter.is_object() ||
        FindParticleModuleSpec(type) == nullptr)
        return false;

    nlohmann::json working = emitter;
    if (!EnsureParticleModules(working)) return false;
    nlohmann::json* modules = ModuleArray(working);
    if (modules == nullptr || !ValidModules(*modules) ||
        modules->size() >= 64)
        return false;

    nlohmann::json module = {
        {"type", type},
        {"enabled", true},
        {"fields", nlohmann::json::object()},
    };
    index = modules->size();
    modules->push_back(std::move(module));
    emitter = std::move(working);
    return true;
}

inline bool RemoveParticleModule(
    nlohmann::json& emitter,
    size_t index)
{
    using namespace ParticleModuleEditDetail;
    nlohmann::json* modules = ModuleArray(emitter);
    if (modules == nullptr || !ValidModules(*modules) ||
        index >= modules->size())
        return false;
    modules->erase(modules->begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

inline bool MoveParticleModule(
    nlohmann::json& emitter,
    size_t index,
    int delta)
{
    using namespace ParticleModuleEditDetail;
    if (delta != -1 && delta != 1) return false;
    nlohmann::json* modules = ModuleArray(emitter);
    if (modules == nullptr || !ValidModules(*modules) ||
        index >= modules->size())
        return false;
    const std::ptrdiff_t target =
        static_cast<std::ptrdiff_t>(index) + delta;
    if (target < 0 ||
        target >= static_cast<std::ptrdiff_t>(modules->size()))
        return false;
    std::swap(
        modules->at(index),
        modules->at(static_cast<size_t>(target)));
    return true;
}

inline bool SetParticleModuleEnabled(
    nlohmann::json& emitter,
    size_t index,
    bool enabled)
{
    using namespace ParticleModuleEditDetail;
    nlohmann::json* modules = ModuleArray(emitter);
    if (modules == nullptr || !ValidModules(*modules) ||
        index >= modules->size())
        return false;
    modules->at(index)["enabled"] = enabled;
    return true;
}

} // namespace VL
