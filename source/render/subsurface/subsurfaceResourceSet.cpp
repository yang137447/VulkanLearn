#include "render/subsurface/subsurfaceResourceSet.h"

#include <filesystem>
#include <stdexcept>

namespace VL
{
namespace
{

// path 是材质到 stable ID 的索引键，统一为 generic path 以避免分隔符差异改变绑定结果。
std::string NormalizeAssetPath(std::string_view assetPath)
{
    return std::filesystem::path(assetPath)
        .lexically_normal()
        .generic_string();
}

} // namespace

// Material loader 只从当前 World-local immutable set 解析派生 ID，不能从全局表猜测 profile。
uint32_t SubsurfaceResourceSet::ResolveProfileId(
    std::string_view assetPath) const
{
    const std::string normalizedPath = NormalizeAssetPath(assetPath);
    const auto it = profileIdsByAssetPath.find(normalizedPath);
    if (it == profileIdsByAssetPath.end())
    {
        throw std::runtime_error(
            "Subsurface profile asset is not active: " +
            normalizedPath);
    }
    return it->second;
}

uint32_t SubsurfaceResourceSet::ResolveSkinLutId(
    std::string_view assetPath) const
{
    const std::string normalizedPath = NormalizeAssetPath(assetPath);
    const auto it = skinLutIdsByAssetPath.find(normalizedPath);
    if (it == skinLutIdsByAssetPath.end())
    {
        throw std::runtime_error(
            "Preintegrated skin LUT asset is not active: " +
            normalizedPath);
    }
    return it->second;
}

// ID 到 asset 的反向索引供校验和诊断使用；发布后只读，生命周期跟随 World resource package。
const SubsurfaceProfileAsset& SubsurfaceResourceSet::GetProfile(
    uint32_t profileId) const
{
    const auto it = profileIndicesById.find(profileId);
    if (it == profileIndicesById.end())
    {
        throw std::runtime_error(
            "Subsurface profileId is not active: " +
            std::to_string(profileId));
    }
    return profiles[it->second];
}

// skin LUT 反向索引与 profile 使用同一 stable ID 语义，避免材质路径和 GPU tile 顺序脱钩。
const PreintegratedSkinLutAsset&
SubsurfaceResourceSet::GetSkinLut(uint32_t skinLutId) const
{
    const auto it = skinLutIndicesById.find(skinLutId);
    if (it == skinLutIndicesById.end())
    {
        throw std::runtime_error(
            "Preintegrated skinLutId is not active: " +
            std::to_string(skinLutId));
    }
    return skinLuts[it->second];
}

} // namespace VL
