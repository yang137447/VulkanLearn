#pragma once

#include <filesystem>
#include <map>
#include <stdexcept>
#include "shader/build/contentHash.h"

namespace VL
{
// 资产发布使用内容摘要，不使用 mtime；几何、材质描述与贴图源在同一提交边界复核。
inline void ValidatePreparedAssetSources(const std::map<std::filesystem::path, ContentDigest>& sources)
{
    for (const auto& source : sources)
        if (ContentHasher::HashFile(source.first) != source.second)
            throw std::runtime_error("Prepared asset dependency changed: " + source.first.string());
}
}
