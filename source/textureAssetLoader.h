#pragma once

#include <string>
#include <string_view>
#include <vulkan/vulkan.hpp>
#include "texture.h"

// 纹理资产 JSON 解析后的运行时加载描述。
struct TextureBindingLoadDesc
{
    std::string assetPath;
    std::string source;
    HostImage::TextureSemantic semantic = HostImage::TextureSemantic::Color;
    TextureIO::LoadOptions::Transfer transfer = TextureIO::LoadOptions::Transfer::SRGB;
    bool generateMipmaps = true;
    vk::Filter filter = vk::Filter::eLinear;
    vk::SamplerAddressMode wrapMode = vk::SamplerAddressMode::eRepeat;
};

// 材质实例里的纹理绑定目前要求显式引用 T_*.json 纹理资产。
void ValidateTextureAssetReference(
    std::string_view textureName,
    std::string_view textureAssetPath,
    std::string_view materialInstancePath);

// 从纹理资产 JSON 读取加载参数。
TextureBindingLoadDesc LoadTextureAssetDesc(std::string_view textureAssetPath);
// 转成 Texture 构造所需的创建描述。
Texture::CreateDesc ToTextureCreateDesc(const TextureBindingLoadDesc& loadDesc);
// 生成稳定 cache key，避免相同加载参数重复创建纹理。
std::string BuildTextureCacheKey(const TextureBindingLoadDesc& loadDesc);
