#include "meshAssetResolver.h"

#include <stdexcept>
#include <string>

MeshAssetResolveResult MeshAssetResolver::Resolve(
    std::string_view meshAssetPath,
    const nlohmann::json& meshAssetJson)
{
    if (!meshAssetJson.is_object())
    {
        throw std::runtime_error("Mesh asset must be a JSON object: " + std::string(meshAssetPath));
    }

    MeshAssetResolveResult result;
    result.effectiveMeshAssetJson = meshAssetJson;
    return result;
}

