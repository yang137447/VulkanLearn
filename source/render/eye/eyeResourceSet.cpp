#include "render/eye/eyeResourceSet.h"

#include <filesystem>
#include <stdexcept>

namespace VL
{
namespace
{
std::string NormalizeAssetPath(std::string_view assetPath)
{
    return std::filesystem::path(assetPath)
        .lexically_normal()
        .generic_string();
}
}

uint32_t EyeResourceSet::ResolveProfileId(std::string_view assetPath) const
{
    const std::string normalizedPath = NormalizeAssetPath(assetPath);
    const auto it = profileIdsByAssetPath.find(normalizedPath);
    if (it == profileIdsByAssetPath.end())
    {
        throw std::runtime_error(
            "Eye profile asset is not active: " + normalizedPath);
    }
    return it->second;
}

const EyeProfileAsset& EyeResourceSet::GetProfile(uint32_t profileId) const
{
    const auto it = profileIndicesById.find(profileId);
    if (it == profileIndicesById.end())
    {
        throw std::runtime_error(
            "Eye profileId is not active: " + std::to_string(profileId));
    }
    return profiles[it->second];
}

} // namespace VL