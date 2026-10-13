#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <nlohmann/json.hpp>

// 文件 loader 与隔离资产候选共用同一字段/枚举合同，不读取或解码 source。
inline void ValidateTextureAssetJson(const nlohmann::json& asset)
{
    if (!asset.is_object()) throw std::invalid_argument("Texture asset must be an object");
    for (const auto& entry : asset.items())
        if (entry.key() != "name" && entry.key() != "type" && entry.key() != "source" &&
            entry.key() != "colorSpace" && entry.key() != "mipmaps" && entry.key() != "filter" &&
            entry.key() != "wrapMode" && entry.key() != "channelsDescription")
            throw std::invalid_argument("Unknown texture field: " + entry.key());
    if (asset.at("type") != "texture") throw std::invalid_argument("Texture type must be texture");
    for (const char* field : {"name", "source", "colorSpace"})
        if (!asset.at(field).is_string() || asset.at(field).get<std::string>().empty())
            throw std::invalid_argument(std::string("Texture requires non-empty string: ") + field);
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
