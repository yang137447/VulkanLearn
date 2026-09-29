#include "render/offline/offlineGroomAsset.h"

#include <fstream>
#include <iterator>
#include <stdexcept>
#include <unordered_set>
#include "commonFunction.h"
#include "shader/build/contentHash.h"
#include "textureAssetLoader.h"

namespace VL
{
void FreezeOfflineMaterialTextures(OfflineWorldAssets& assets,
    const nlohmann::json& effectiveMaterial)
{
    const std::string model = effectiveMaterial.value("offlineModel", std::string());
    for (const auto& [slot, referenceJson] : effectiveMaterial.at("textures").items())
    {
        const bool groom = model == "ClothFiber" && slot == "groomMap";
        const bool color = (model == "ClothSheen" || model == "Lambert") &&
            (slot == "baseColorMap" || (model == "ClothSheen" && slot == "sheenColorMap"));
        if (!groom && !color) continue;
        const std::string reference = referenceJson.get<std::string>();
        ValidateTextureAssetReference(slot, reference,
            effectiveMaterial.at("material").get<std::string>());
        const auto desc = LoadTextureAssetDesc(reference);
        const std::string extension = std::filesystem::path(desc.source).extension().string();
        if (groom &&
            (extension != ".pfm" ||
             desc.transfer != TextureIO::LoadOptions::Transfer::Linear ||
             desc.filter != vk::Filter::eLinear ||
             desc.wrapMode != vk::SamplerAddressMode::eRepeat ||
             desc.generateMipmaps))
            throw std::runtime_error(
                "ClothFiber groomMap requires linear RGB PFM, repeat/linear, mipmaps=false: " +
                reference);
        if (color &&
            (extension != ".png" && extension != ".tga" &&
             extension != ".hdr" && extension != ".exr"))
            throw std::runtime_error("Unsupported offline color image format: " + reference);
        if (color && (extension == ".hdr" || extension == ".exr") &&
            desc.transfer != TextureIO::LoadOptions::Transfer::Linear)
            throw std::runtime_error("HDR/EXR offline texture must be linear: " + reference);
        const auto descriptorPath = std::filesystem::path(CommonFunction::Path(reference));
        const auto sourcePath = std::filesystem::path(CommonFunction::Path(desc.source));
        std::ifstream file(sourcePath, std::ios::binary);
        if (!file) throw std::runtime_error("Cannot open offline image: " + desc.source);
        auto bytes = std::make_shared<const std::string>(
            std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>{});
        if (file.bad()) throw std::runtime_error("Cannot read offline image: " + desc.source);
        const std::string descriptorDigest = ContentHasher::HashFile(descriptorPath).ToHex();
        const std::string sourceDigest = ContentHasher::HashString(*bytes).ToHex();
        if (groom)
            assets.groomAssets.insert_or_assign(reference, OfflineWorldAssets::GroomAsset{
                descriptorPath, sourcePath, descriptorDigest, sourceDigest, std::move(bytes)});
        else
            assets.colorAssets.insert_or_assign(reference, OfflineWorldAssets::ColorAsset{
                descriptorPath, sourcePath, descriptorDigest, sourceDigest, std::move(bytes),
                extension, desc.transfer == TextureIO::LoadOptions::Transfer::SRGB,
                desc.wrapMode == vk::SamplerAddressMode::eRepeat,
                desc.filter == vk::Filter::eLinear});
    }
}

void ValidateOfflineTexturesStillCurrent(const OfflineWorldAssets& assets)
{
    const auto current = [](const auto& asset)
    {
        return std::filesystem::is_regular_file(asset.descriptorPath) &&
            std::filesystem::is_regular_file(asset.sourcePath) &&
            ContentHasher::HashFile(asset.descriptorPath).ToHex() == asset.descriptorDigest &&
            ContentHasher::HashFile(asset.sourcePath).ToHex() == asset.sourceDigest;
    };
    for (const auto& [reference, groom] : assets.groomAssets)
        if (!current(groom))
            throw std::runtime_error("groom asset changed before commit: " + reference);
    for (const auto& [reference, color] : assets.colorAssets)
        if (!current(color))
            throw std::runtime_error("color asset changed before commit: " + reference);
}

void PruneUnusedOfflineMaterialAssets(OfflineWorldAssets& assets)
{
    std::unordered_set<std::string> profiles;
    std::unordered_set<std::string> textures;
    for (const auto& [key, material] : assets.effectiveMaterialInstances)
    {
        if (material.contains("modelProfile"))
            profiles.insert(material.at("modelProfile").get<std::string>());
        for (const auto& [slot, reference] : material.at("textures").items())
            textures.insert(reference.get<std::string>());
    }
    // 草稿替换后只保留仍被有效材质引用的字节与摘要；旧 session 自己持有旧包。
    for (auto it = assets.modelProfiles.begin(); it != assets.modelProfiles.end();)
        if (!profiles.count(it->first)) it = assets.modelProfiles.erase(it);
        else ++it;
    for (auto it = assets.modelProfilePaths.begin(); it != assets.modelProfilePaths.end();)
        if (!profiles.count(it->first)) it = assets.modelProfilePaths.erase(it);
        else ++it;
    for (auto it = assets.modelProfileDigests.begin(); it != assets.modelProfileDigests.end();)
        if (!profiles.count(it->first)) it = assets.modelProfileDigests.erase(it);
        else ++it;
    for (auto it = assets.groomAssets.begin(); it != assets.groomAssets.end();)
        if (!textures.count(it->first)) it = assets.groomAssets.erase(it);
        else ++it;
    for (auto it = assets.colorAssets.begin(); it != assets.colorAssets.end();)
        if (!textures.count(it->first)) it = assets.colorAssets.erase(it);
        else ++it;
}
} // namespace VL
