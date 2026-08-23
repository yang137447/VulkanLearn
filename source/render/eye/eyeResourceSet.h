#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "render/eye/eyeAssets.h"

class Texture;

namespace VL
{

// 一个 World generation 对应一个不可变 EyeResourceSet。材质只从当前 set
// 解析 stable ID；candidate 未提交前不会污染 active World 的 descriptor 绑定。
class EyeResourceSet
{
public:
    uint32_t ResolveProfileId(std::string_view assetPath) const;
    const EyeProfileAsset& GetProfile(uint32_t profileId) const;

    std::string sourceDigest;
    std::string computeArtifactGenerationKey;
    EyeCausticLutMetadata lutMetadata;
    std::vector<EyeProfileAsset> profiles;
    std::unordered_map<std::string, uint32_t> profileIdsByAssetPath;
    std::unordered_map<uint32_t, size_t> profileIndicesById;
    std::shared_ptr<Texture> causticLutTexture;
};

} // namespace VL