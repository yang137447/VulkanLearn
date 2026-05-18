#include "textureAssetLoader.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "commonFunction.h"

namespace
{
    // 纹理资产字段按大小写不敏感处理，避免配置书写差异影响解析。
    std::string ToLowerAscii(std::string value)
    {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    TextureIO::LoadOptions::Transfer ParseTextureColorSpace(
        std::string_view colorSpace,
        std::string_view textureAssetPath)
    {
        const std::string normalized = ToLowerAscii(std::string(colorSpace));
        if (normalized == "srgb")
        {
            return TextureIO::LoadOptions::Transfer::SRGB;
        }
        if (normalized == "linear")
        {
            return TextureIO::LoadOptions::Transfer::Linear;
        }
        throw std::runtime_error(
            "Unsupported texture colorSpace in " + std::string(textureAssetPath) + ": " + std::string(colorSpace));
    }

    vk::Filter ParseTextureFilter(std::string_view filter, std::string_view textureAssetPath)
    {
        const std::string normalized = ToLowerAscii(std::string(filter));
        if (normalized == "linear")
        {
            return vk::Filter::eLinear;
        }
        if (normalized == "nearest")
        {
            return vk::Filter::eNearest;
        }
        throw std::runtime_error(
            "Unsupported texture filter in " + std::string(textureAssetPath) + ": " + std::string(filter));
    }

    vk::SamplerAddressMode ParseTextureWrapMode(std::string_view wrapMode, std::string_view textureAssetPath)
    {
        const std::string normalized = ToLowerAscii(std::string(wrapMode));
        if (normalized == "repeat")
        {
            return vk::SamplerAddressMode::eRepeat;
        }
        if (normalized == "clamp")
        {
            return vk::SamplerAddressMode::eClampToEdge;
        }
        throw std::runtime_error(
            "Unsupported texture wrapMode in " + std::string(textureAssetPath) + ": " + std::string(wrapMode));
    }
}

void ValidateTextureAssetReference(
    std::string_view textureName,
    std::string_view textureAssetPath,
    std::string_view materialInstancePath)
{
    const std::filesystem::path path{std::string(textureAssetPath)};
    const std::string extension = ToLowerAscii(path.extension().string());
    const std::string filename = path.filename().string();
    if (extension != ".json" || filename.rfind("T_", 0) != 0)
    {
        throw std::runtime_error(
            "Material texture binding must reference a T_*.json texture asset. Material instance: " +
            std::string(materialInstancePath) +
            ", binding: " +
            std::string(textureName) +
            ", value: " +
            std::string(textureAssetPath));
    }
}

TextureBindingLoadDesc LoadTextureAssetDesc(std::string_view textureAssetPath)
{
    // 先读取纹理资产 JSON，再统一导出为 SceneLoader 可复用的加载描述。
    std::ifstream textureAssetFile(CommonFunction::Path(std::string(textureAssetPath)));
    if (!textureAssetFile.is_open())
    {
        throw std::runtime_error("Failed to open texture asset: " + std::string(textureAssetPath));
    }

    nlohmann::json textureAssetJson;
    textureAssetFile >> textureAssetJson;
    if (!textureAssetJson.is_object())
    {
        throw std::runtime_error("Texture asset must be a JSON object: " + std::string(textureAssetPath));
    }
    if (textureAssetJson.value("type", std::string()) != "texture")
    {
        throw std::runtime_error("Texture asset type must be \"texture\": " + std::string(textureAssetPath));
    }
    if (!textureAssetJson.contains("name") || !textureAssetJson["name"].is_string())
    {
        throw std::runtime_error("Texture asset is missing string field \"name\": " + std::string(textureAssetPath));
    }
    if (!textureAssetJson.contains("source") || !textureAssetJson["source"].is_string())
    {
        throw std::runtime_error("Texture asset is missing string field \"source\": " + std::string(textureAssetPath));
    }
    if (!textureAssetJson.contains("colorSpace") || !textureAssetJson["colorSpace"].is_string())
    {
        throw std::runtime_error("Texture asset is missing string field \"colorSpace\": " + std::string(textureAssetPath));
    }

    TextureBindingLoadDesc loadDesc;
    loadDesc.assetPath = textureAssetPath;
    loadDesc.source = textureAssetJson["source"].get<std::string>();
    loadDesc.transfer = ParseTextureColorSpace(textureAssetJson["colorSpace"].get<std::string>(), textureAssetPath);
    loadDesc.generateMipmaps = textureAssetJson.value("mipmaps", true);
    loadDesc.filter = ParseTextureFilter(textureAssetJson.value("filter", std::string("linear")), textureAssetPath);
    loadDesc.wrapMode = ParseTextureWrapMode(textureAssetJson.value("wrapMode", std::string("repeat")), textureAssetPath);
    return loadDesc;
}

Texture::CreateDesc ToTextureCreateDesc(const TextureBindingLoadDesc& loadDesc)
{
    // 只做字段搬运，不在这里引入额外默认规则，默认值由 TextureBindingLoadDesc 负责。
    Texture::CreateDesc createDesc;
    createDesc.semantic = loadDesc.semantic;
    createDesc.transfer = loadDesc.transfer;
    createDesc.generateMipmaps = loadDesc.generateMipmaps;
    createDesc.filter = loadDesc.filter;
    createDesc.wrapMode = loadDesc.wrapMode;
    createDesc.debugName = loadDesc.assetPath;
    return createDesc;
}

std::string BuildTextureCacheKey(const TextureBindingLoadDesc& loadDesc)
{
    // 将影响底层纹理创建结果的字段全部纳入 key，保证缓存命中语义明确。
    std::ostringstream stream;
    stream
        << loadDesc.assetPath
        << "|source=" << loadDesc.source
        << "|semantic=" << static_cast<int>(loadDesc.semantic)
        << "|transfer=" << static_cast<int>(loadDesc.transfer)
        << "|mipmaps=" << loadDesc.generateMipmaps
        << "|filter=" << static_cast<int>(loadDesc.filter)
        << "|wrapMode=" << static_cast<int>(loadDesc.wrapMode);
    return stream.str();
}
