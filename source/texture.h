#pragma once
#include <vulkan/vulkan.hpp>
#include <string>
#include "resource/image/hostImage.h"
#include "resource/image/textureIO.h"

namespace VL
{
class RendererBackendVulkan;
}

class Texture
{
public:
    struct CreateDesc
    {
        HostImage::TextureSemantic semantic = HostImage::TextureSemantic::Color;
        TextureIO::LoadOptions::Transfer transfer = TextureIO::LoadOptions::Transfer::SRGB;
        bool generateMipmaps = true;
        vk::Filter filter = vk::Filter::eLinear;
        vk::SamplerAddressMode wrapMode = vk::SamplerAddressMode::eRepeat;
        std::string debugName;
    };

    Texture(VL::RendererBackendVulkan& rendererBackend, const std::string& texturePath);
    Texture(
        VL::RendererBackendVulkan& rendererBackend,
        const std::string& texturePath,
        HostImage::TextureSemantic semantic,
        TextureIO::LoadOptions::Transfer transfer);
    Texture(
        VL::RendererBackendVulkan& rendererBackend,
        const std::string& texturePath,
        const CreateDesc& createDesc);
    Texture(
        VL::RendererBackendVulkan& rendererBackend,
        vk::Image image,
        vk::DeviceMemory imageMemory,
        vk::ImageView imageView,
        vk::Sampler sampler,
        uint32_t mipLevels,
        vk::Format format);
    ~Texture();
    inline vk::Image getImage() const { return image; }
    inline vk::DeviceMemory getImageMemory() const { return imageMemory; }
    inline vk::ImageView getImageView() const { return imageView; }
    inline vk::Sampler getSampler() const { return sampler; }
    inline uint32_t getMipLevels() const { return mipLevels; }
    inline vk::Format GetFormat() const { return format; }
    inline const vk::DescriptorImageInfo& GetDescriptorInfo() const { return descriptorInfo; }
private:
    Texture();
    
    vk::Image image;
    vk::DeviceMemory imageMemory;
    vk::ImageView imageView;
    vk::Sampler sampler;
    uint32_t mipLevels;
    vk::Format format;
    vk::DescriptorImageInfo descriptorInfo;
    VL::RendererBackendVulkan* rendererBackend = nullptr;
};
