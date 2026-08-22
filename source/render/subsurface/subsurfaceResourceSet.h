#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "render/subsurface/subsurfaceAssets.h"

class Texture;

namespace VL
{

// 持有一个 World generation 的已验证 SSS 资产表和 GPU lookup texture。
// loader 只在 candidate prepare 阶段填充它；发布到 RendererResourceCache 后以
// shared_ptr<const SubsurfaceResourceSet> 供 Material loader 和 lighting/profile pass 消费，
// 不创建 RenderGraph attachment，也不允许 active World 修改其中的数据。
class SubsurfaceResourceSet
{
public:
    uint32_t ResolveProfileId(std::string_view assetPath) const;
    uint32_t ResolveSkinLutId(std::string_view assetPath) const;
    const SubsurfaceProfileAsset& GetProfile(
        uint32_t profileId) const;
    const PreintegratedSkinLutAsset& GetSkinLut(
        uint32_t skinLutId) const;

    std::string sourceDigest;
    std::vector<SubsurfaceProfileAsset> profiles;
    std::vector<PreintegratedSkinLutAsset> skinLuts;
    std::unordered_map<std::string, uint32_t> profileIdsByAssetPath;
    std::unordered_map<std::string, uint32_t> skinLutIdsByAssetPath;
    std::unordered_map<uint32_t, size_t> profileIndicesById;
    std::unordered_map<uint32_t, size_t> skinLutIndicesById;
    std::shared_ptr<Texture> profileTableTexture;
    std::shared_ptr<Texture> skinLutTableTexture;
};

} // namespace VL
