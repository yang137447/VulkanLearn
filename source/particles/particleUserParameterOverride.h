#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "particles/particleAsset.h"

namespace VL
{

// 稳定的运行时参数签名：排序后的 key/value 做 FNV-1a，保证同一组参数
// 不受 unordered_map 遍历顺序影响。仅用于渲染器缓存失效判断。
inline uint64_t BuildParticleUserParameterSignature(
    const std::unordered_map<std::string, float>& parameters)
{
    std::vector<std::pair<std::string, float>> ordered(
        parameters.begin(), parameters.end());
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const auto& left, const auto& right)
        {
            return left.first < right.first;
        });

    uint64_t hash = 1469598103934665603ull;
    const auto appendByte = [&hash](uint8_t byte)
    {
        hash ^= static_cast<uint64_t>(byte);
        hash *= 1099511628211ull;
    };
    for (const auto& [key, value] : ordered)
    {
        for (const char character : key)
        {
            appendByte(static_cast<uint8_t>(character));
        }
        appendByte(0u);
        uint32_t bits = 0u;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        appendByte(static_cast<uint8_t>(bits & 0xffu));
        appendByte(static_cast<uint8_t>((bits >> 8u) & 0xffu));
        appendByte(static_cast<uint8_t>((bits >> 16u) & 0xffu));
        appendByte(static_cast<uint8_t>((bits >> 24u) & 0xffu));
    }
    return hash;
}

// 把运行时参数合并进作者 JSON 的根 userParameters。非有限值/空 key
// 由 World 拒绝；这里再做一次防御性过滤，避免把坏值送进 ParseJson。
inline nlohmann::json MergeParticleUserParameters(
    const nlohmann::json& authoringJson,
    const std::unordered_map<std::string, float>& runtimeParameters)
{
    nlohmann::json merged = authoringJson;
    if (!merged.is_object())
    {
        return merged;
    }
    auto parameters = merged.find("userParameters");
    if (parameters == merged.end() || !parameters->is_object())
    {
        merged["userParameters"] = nlohmann::json::object();
        parameters = merged.find("userParameters");
    }
    for (const auto& [key, value] : runtimeParameters)
    {
        if (key.empty() || !std::isfinite(value))
        {
            continue;
        }
        (*parameters)[key] = value;
    }
    return merged;
}

// 运行时重解析只包含作者 JSON 里的 emitter；World 运行时新增的 slot
// 不在作者 JSON 中。按 name 把解析结果套回快照顺序，未匹配项保留
// 快照资产，避免运行时发射器被覆盖丢失。
inline std::vector<ParticleEmitterAsset>
ReconcileParticleEmitterOverrides(
    const std::vector<ParticleEmitterAsset>& parsedEmitters,
    const std::vector<ParticleEmitterAsset>& snapshotEmitters)
{
    std::unordered_map<std::string, const ParticleEmitterAsset*>
        parsedByName;
    parsedByName.reserve(parsedEmitters.size());
    for (const ParticleEmitterAsset& emitter : parsedEmitters)
    {
        parsedByName.emplace(emitter.name, &emitter);
    }
    std::vector<ParticleEmitterAsset> reconciled;
    reconciled.reserve(snapshotEmitters.size());
    for (const ParticleEmitterAsset& emitter : snapshotEmitters)
    {
        const auto parsed = parsedByName.find(emitter.name);
        reconciled.push_back(
            parsed != parsedByName.end()
            ? *parsed->second
            : emitter);
    }
    return reconciled;
}

// 完整的运行时覆盖路径：合并根 userParameters -> ParseJson -> 按快照
// 顺序对齐运行时-only emitter。调用方负责缓存与异常处理。
inline ParticleSystemAsset ParseParticleAssetWithRuntimeParameters(
    const ParticleSystemAsset& base,
    const std::unordered_map<std::string, float>& runtimeParameters)
{
    if (!base.authoringJson || runtimeParameters.empty())
    {
        return base;
    }
    nlohmann::json merged = MergeParticleUserParameters(
        *base.authoringJson, runtimeParameters);
    ParticleSystemAsset parsed = ParticleAssetLoader::ParseJson(
        merged, base.sourcePath);
    parsed.emitters = ReconcileParticleEmitterOverrides(
        parsed.emitters, base.emitters);
    parsed.authoringJson = base.authoringJson;
    return parsed;
}

} // namespace VL
