#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "particles/particleLimits.h"

namespace VL
{

enum class ParticleEmitterEditStatus
{
    Ok,
    InvalidDocument,
    MaxEmitters,
    MinEmitters,
    InvalidIndex,
    NoMove
};

namespace ParticleEmitterEditDetail
{

// 非法条目没有可信 name 时仍要占用一个唯一性槽位，避免复制后与默认名撞车。
inline std::string ExistingName(
    const nlohmann::json& emitter,
    size_t index)
{
    if (emitter.is_object())
    {
        const auto name = emitter.find("name");
        if (name != emitter.end() && name->is_string() &&
            !name->get_ref<const std::string&>().empty())
        {
            return name->get<std::string>();
        }
    }
    return "emitter" + std::to_string(index);
}

inline nlohmann::json* ExplicitEmitters(nlohmann::json& document)
{
    if (!document.is_object()) return nullptr;
    const auto emitters = document.find("emitters");
    if (emitters == document.end() || !emitters->is_array())
        return nullptr;
    return &(*emitters);
}

} // namespace ParticleEmitterEditDetail

// 旧文档的根字段就是一个隐式 emitter0；显式化时把它挪进数组，
// 根对象继续保留系统级 name/type。根 position 在隐式 emitter 中没有语义，
// 迁移时丢弃，避免升级后突然产生一个位移。
inline bool EnsureExplicitParticleEmitters(nlohmann::json& document)
{
    if (!document.is_object()) return false;
    if (ParticleEmitterEditDetail::ExplicitEmitters(document) != nullptr)
        return true;

    nlohmann::json emitter = document;
    emitter.erase("type");
    emitter.erase("emitters");
    emitter.erase("position");
    emitter["name"] = "emitter0";

    document.erase("emitters");
    document["emitters"] = nlohmann::json::array();
    document["emitters"].push_back(std::move(emitter));
    return true;
}

inline ParticleEmitterEditStatus AddParticleEmitter(
    nlohmann::json& document,
    size_t& selectedIndex)
{
    if (!document.is_object())
        return ParticleEmitterEditStatus::InvalidDocument;
    // 隐式旧文档在升级前只有 1 个可见 emitter；先按可见数量校验，
    // 失败路径（越界/超上限）不能把文档升级成显式数组。
    const size_t visibleCount =
        ParticleEmitterEditDetail::ExplicitEmitters(document) != nullptr
        ? document.at("emitters").size() : 1;
    if (visibleCount >= kMaxParticleEmitters)
        return ParticleEmitterEditStatus::MaxEmitters;
    if (selectedIndex >= visibleCount)
        return ParticleEmitterEditStatus::InvalidIndex;

    // 只在副本上完成 ensure/插入，最后整体交换回原文档。
    nlohmann::json working = document;
    if (!EnsureExplicitParticleEmitters(working))
        return ParticleEmitterEditStatus::InvalidDocument;
    nlohmann::json* emitters =
        ParticleEmitterEditDetail::ExplicitEmitters(working);
    if (emitters == nullptr)
        return ParticleEmitterEditStatus::InvalidDocument;

    std::vector<std::string> names;
    names.reserve(emitters->size());
    for (size_t index = 0; index < emitters->size(); ++index)
    {
        names.push_back(ParticleEmitterEditDetail::ExistingName(
            emitters->at(index), index));
    }

    const std::string base =
        ParticleEmitterEditDetail::ExistingName(
            emitters->at(selectedIndex), selectedIndex) + "_copy";
    std::string name = base;
    for (uint32_t suffix = 2; ; ++suffix)
    {
        bool taken = false;
        for (const std::string& existing : names)
        {
            if (existing == name)
            {
                taken = true;
                break;
            }
        }
        if (!taken) break;
        name = base + "_" + std::to_string(suffix);
    }

    // 复制原始条目后再改名；非对象条目按原样复制，交由加载校验报告。
    nlohmann::json copy = emitters->at(selectedIndex);
    if (copy.is_object())
        copy["name"] = name;
    const size_t inserted = selectedIndex + 1;
    emitters->insert(
        emitters->begin() + static_cast<std::ptrdiff_t>(inserted),
        std::move(copy));
    document = std::move(working);
    selectedIndex = inserted;
    return ParticleEmitterEditStatus::Ok;
}

inline ParticleEmitterEditStatus RemoveParticleEmitter(
    nlohmann::json& document,
    size_t& selectedIndex)
{
    if (!document.is_object())
        return ParticleEmitterEditStatus::InvalidDocument;
    nlohmann::json* emitters =
        ParticleEmitterEditDetail::ExplicitEmitters(document);
    // 隐式单发射器文档同样只有 1 项；删除只会破坏下限，不改写文档。
    const size_t count = emitters == nullptr ? 1 : emitters->size();
    if (count <= 1)
        return ParticleEmitterEditStatus::MinEmitters;
    if (selectedIndex >= count)
        return ParticleEmitterEditStatus::InvalidIndex;

    emitters->erase(
        emitters->begin() + static_cast<std::ptrdiff_t>(selectedIndex));
    selectedIndex = std::min(selectedIndex, emitters->size() - 1);
    return ParticleEmitterEditStatus::Ok;
}

inline ParticleEmitterEditStatus MoveParticleEmitter(
    nlohmann::json& document,
    size_t& selectedIndex,
    int delta)
{
    if (!document.is_object())
        return ParticleEmitterEditStatus::InvalidDocument;
    nlohmann::json* emitters =
        ParticleEmitterEditDetail::ExplicitEmitters(document);
    const size_t count = emitters == nullptr ? 1 : emitters->size();
    if (selectedIndex >= count)
        return ParticleEmitterEditStatus::InvalidIndex;
    if ((delta != -1 && delta != 1) || count <= 1)
        return ParticleEmitterEditStatus::NoMove;

    const std::ptrdiff_t target =
        static_cast<std::ptrdiff_t>(selectedIndex) + delta;
    if (target < 0 || target >= static_cast<std::ptrdiff_t>(count))
        return ParticleEmitterEditStatus::NoMove;

    std::swap(
        emitters->at(selectedIndex),
        emitters->at(static_cast<size_t>(target)));
    selectedIndex = static_cast<size_t>(target);
    return ParticleEmitterEditStatus::Ok;
}

} // namespace VL
