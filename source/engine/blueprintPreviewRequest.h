#pragma once

#include <cstdint>
#include <string>
#include <nlohmann/json.hpp>
#include "shader/build/contentHash.h"

namespace VL
{
inline std::string BlueprintPreviewIdentity(const std::string& path, uint64_t revision)
{
    return "~preview." + ContentHasher::HashString(path).ToHex() + "." + std::to_string(revision);
}

// 仅提取影响作者可视配置的字段；属性默认值/实例权限不触发重复的 World 资源准备。
inline std::string BlueprintPreviewVisualSignature(const nlohmann::json& draft)
{
    nlohmann::json visual;
    for (const char* field : {"class", "components", "rotation", "scale", "physics"})
        if (draft.contains(field)) visual[field] = draft.at(field);
    return visual.dump();
}

inline nlohmann::json BuildBlueprintPreviewObjects(const std::string& path, uint64_t requestedRevision,
    uint64_t acceptedRevision, const nlohmann::json& acceptedDraft)
{
    if (requestedRevision != acceptedRevision)
        throw std::invalid_argument("Blueprint preview document revision is stale");
    nlohmann::json actor{{"type", "blueprintInstance"}, {"id", BlueprintPreviewIdentity(path, acceptedRevision)},
        {"name", "Blueprint Preview"}, {"class", acceptedDraft.at("class")}, {"enabled", true},
        {"position", {0, 0, 0}}, {"rotation", acceptedDraft.value("rotation", nlohmann::json::array({0, 0, 0}))},
        {"scale", acceptedDraft.value("scale", nlohmann::json::array({1, 1, 1}))},
        {"components", acceptedDraft.value("components", nlohmann::json::array())},
        // 预览不运行脚本；资产默认值不能伪装成 inline 实例覆写绕过 Instance Editable。
        {"properties", nlohmann::json::object()}};
    if (acceptedDraft.contains("physics")) actor["physics"] = acceptedDraft.at("physics");
    return nlohmann::json::array({std::move(actor)});
}
}
