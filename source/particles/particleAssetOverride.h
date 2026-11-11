#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include "particles/particleAsset.h"

namespace VL
{

// 场景加载时解析 particlePath：命中预览草稿就完全跳过磁盘 PS 文件，
// 未命中仍走正式 ParticleAssetLoader::Prepare。
inline ParticleSystemAsset PrepareParticleAssetWithOverride(
    const std::string& path,
    const std::unordered_map<std::string, nlohmann::json>& documents,
    const std::filesystem::path& resourceRoot = {})
{
    const auto document = documents.find(path);
    return document != documents.end()
        ? ParticleAssetLoader::ParseJson(
            document->second, path, resourceRoot)
        : ParticleAssetLoader::Prepare(path);
}

} // namespace VL
