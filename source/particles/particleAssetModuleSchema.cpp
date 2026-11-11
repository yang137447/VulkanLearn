#include "particles/particleAssetInternal.h"

#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace VL
{
namespace ParticleAssetDetail
{
// v2 模块只做作者层组织；运行时仍消费展平后的 flat emitter。
// 未知模块类型必须拒绝，避免拼写错误静默变成空模块。
const std::unordered_set<std::string>& ParticleModuleTypes()
{
    static const std::unordered_set<std::string> types = {
        "emitter",
        "renderer",
        "spawn.rate",
        "spawn.shapeSphere",
        "spawn.velocity",
        "update.lifetime",
        "update.color",
        "update.size",
        "update.rotation",
        "forces.gravity",
        "forces.drag",
        "forces.curlNoise",
        "forces.vortex",
        "forces.attractor",
        "forces.wind",
        "collision.analytic",
        "events",
        "flipbook",
        "lod",
        "curves",
        "bounds",
        "culling"};
    return types;
}

void MergeParticleModuleFields(
    nlohmann::json& target,
    const nlohmann::json& source,
    const std::string& moduleType,
    const std::string& path)
{
    for (auto field = source.begin(); field != source.end(); ++field)
    {
        const std::string fieldPath = path.empty()
            ? field.key()
            : path + "." + field.key();
        if (!target.contains(field.key()))
        {
            target[field.key()] = field.value();
            continue;
        }
        if (target.at(field.key()).is_object() &&
            field.value().is_object())
        {
            MergeParticleModuleFields(
                target[field.key()],
                field.value(),
                moduleType,
                fieldPath);
            continue;
        }
        if (target.at(field.key()) != field.value())
        {
            throw std::invalid_argument(
                "Particle v2 module field conflict: " + moduleType +
                " -> " + fieldPath);
        }
    }
}

nlohmann::json FlattenParticleEmitterV2(
    const nlohmann::json& emitter,
    size_t index)
{
    if (!emitter.is_object())
    {
        throw std::invalid_argument(
            "Particle v2 emitter must be an object: " +
            std::to_string(index));
    }
    if (!emitter.contains("modules") ||
        !emitter.at("modules").is_array())
    {
        throw std::invalid_argument(
            "Particle v2 emitter requires a modules array: " +
            std::to_string(index));
    }
    const nlohmann::json& modules = emitter.at("modules");
    if (modules.size() > 64)
    {
        throw std::invalid_argument(
            "Particle v2 emitter supports at most 64 modules: " +
            std::to_string(index));
    }

    nlohmann::json flattened = emitter;
    flattened.erase("modules");
    for (const nlohmann::json& module : modules)
    {
        if (!module.is_object())
        {
            throw std::invalid_argument(
                "Particle v2 module must be an object");
        }
        const std::string type =
            module.value("type", std::string());
        if (ParticleModuleTypes().find(type) ==
            ParticleModuleTypes().end())
        {
            throw std::invalid_argument(
                "Particle v2 module type is unknown: " + type);
        }
        if (!module.value("enabled", true))
        {
            continue;
        }
        if (!module.contains("fields") ||
            !module.at("fields").is_object())
        {
            throw std::invalid_argument(
                "Particle v2 module requires an object fields: " + type);
        }
        MergeParticleModuleFields(
            flattened,
            module.at("fields"),
            type,
            {});
    }
    return flattened;
}

std::unordered_map<std::string, float> ParseParticleUserParameters(
    const nlohmann::json& json)
{
    if (!json.contains("userParameters"))
    {
        return {};
    }
    const nlohmann::json& parameters = json.at("userParameters");
    if (!parameters.is_object() || parameters.size() > 64)
    {
        throw std::invalid_argument(
            "Particle userParameters requires an object with at most "
            "64 entries");
    }
    std::unordered_map<std::string, float> result;
    result.reserve(parameters.size());
    for (auto parameter = parameters.begin();
         parameter != parameters.end(); ++parameter)
    {
        if (parameter.key().empty())
        {
            throw std::invalid_argument(
                "Particle user parameter name must be non-empty");
        }
        if (!parameter.value().is_number() ||
            !std::isfinite(parameter.value().get<float>()))
        {
            throw std::invalid_argument(
                "Particle user parameter must be finite: " +
                parameter.key());
        }
        result.emplace(
            parameter.key(),
            parameter.value().get<float>());
    }
    return result;
}

} // namespace ParticleAssetDetail
} // namespace VL
