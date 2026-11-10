#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <nlohmann/json.hpp>

// 文件 loader 与隔离资产候选共用同一字段/枚举合同，不读取或解码 source。
inline void ValidateTextureAssetJson(const nlohmann::json& asset)
{
    if (!asset.is_object()) throw std::invalid_argument("Texture asset must be an object");
    if (!asset.contains("type") || !asset.at("type").is_string())
        throw std::invalid_argument("Texture type must be texture or textureArray");
    const std::string type = asset.at("type").get<std::string>();
    const bool isArray = type == "textureArray";
    if (type != "texture" && !isArray)
        throw std::invalid_argument("Texture type must be texture or textureArray");
    for (const auto& entry : asset.items())
        if (entry.key() != "name" && entry.key() != "type" && entry.key() != "source" &&
            entry.key() != "sources" &&
            entry.key() != "displacementSources" &&
            entry.key() != "colorSpace" && entry.key() != "mipmaps" && entry.key() != "filter" &&
            entry.key() != "wrapMode" && entry.key() != "channelsDescription" &&
            entry.key() != "bitDepth")
            throw std::invalid_argument("Unknown texture field: " + entry.key());
    for (const char* field : {"name", "colorSpace"})
        if (!asset.at(field).is_string() || asset.at(field).get<std::string>().empty())
            throw std::invalid_argument(std::string("Texture requires non-empty string: ") + field);
    if (isArray)
    {
        if (asset.contains("source"))
            throw std::invalid_argument("Texture array must use sources, not source");
        if (!asset.contains("sources") || !asset.at("sources").is_array() || asset.at("sources").empty())
            throw std::invalid_argument("Texture array requires non-empty sources");
        for (const auto& source : asset.at("sources"))
            if (!source.is_string() || source.get<std::string>().empty())
                throw std::invalid_argument("Texture array sources must be non-empty strings");
        // displacementSources 是层 source-set 的编辑器侧 16-bit 位移源，运行时不上传。
        // 若存在必须与 sources 同序同长，避免预览按 slice 取到错层。
        if (asset.contains("displacementSources"))
        {
            if (!asset.at("displacementSources").is_array() ||
                asset.at("displacementSources").size() != asset.at("sources").size())
                throw std::invalid_argument(
                    "Texture array displacementSources must match sources");
            for (const auto& source : asset.at("displacementSources"))
                if (!source.is_string() || source.get<std::string>().empty())
                    throw std::invalid_argument(
                        "Texture array displacementSources must be non-empty strings");
        }
    }
    else
    {
        if (asset.contains("sources"))
            throw std::invalid_argument("Single texture must use source, not sources");
        if (asset.contains("displacementSources"))
            throw std::invalid_argument(
                "Single texture must not use displacementSources");
        if (!asset.at("source").is_string() || asset.at("source").get<std::string>().empty())
            throw std::invalid_argument("Texture requires non-empty string: source");
    }
    for (const char* field : {"colorSpace", "filter", "wrapMode"})
    {
        if (!asset.contains(field)) continue;
        std::string value = asset.at(field).get<std::string>();
        for (char& character : value)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        const std::string key(field);
        if ((key == "colorSpace" && value != "linear" && value != "srgb") ||
            (key == "filter" && value != "linear" && value != "nearest") ||
            (key == "wrapMode" && value != "repeat" && value != "clamp"))
            throw std::invalid_argument("Invalid texture " + key + ": " + value);
    }
    if (asset.contains("mipmaps") && !asset.at("mipmaps").is_boolean())
        throw std::invalid_argument("Texture mipmaps must be boolean");
    if (asset.contains("bitDepth") &&
        (!asset.at("bitDepth").is_number_integer() ||
         (asset.at("bitDepth").get<int>() != 8 &&
          asset.at("bitDepth").get<int>() != 16)))
        throw std::invalid_argument("Texture bitDepth must be 8 or 16");
    if (asset.contains("channelsDescription"))
    {
        const auto& channels = asset.at("channelsDescription");
        if (!channels.is_object()) throw std::invalid_argument("channelsDescription must be object");
        for (const auto& entry : channels.items())
            if ((entry.key() != "r" && entry.key() != "g" && entry.key() != "b" && entry.key() != "a") ||
                !entry.value().is_string())
                throw std::invalid_argument("Invalid channelsDescription entry: " + entry.key());
    }
}
