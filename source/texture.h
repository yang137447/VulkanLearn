#pragma once
#include <vulkan/vulkan.hpp>
#include <string>
#include "render/rhi/rhiResourceHandles.h"
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
    // worker 已按材质纹理合同解码并翻转；这里仅创建 GPU 资源，不再次翻转。
    Texture(
        VL::RendererBackendVulkan& rendererBackend,
        const HostImage& preparedImage,
        const CreateDesc& createDesc);
    static TextureIO::LoadOptions MaterialLoadOptions(const CreateDesc& createDesc)
    {
        TextureIO::LoadOptions options;
        options.semantic = createDesc.semantic;
        options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOn;
        options.transfer = createDesc.transfer;
        options.forceChannels = 4;
        return options;
    }
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
    inline VL::RHIImageHandle GetImageHandle() const { return imageHandle; }
    inline VL::RHIImageViewHandle GetImageViewHandle() const { return imageViewHandle; }
    inline VL::RHISamplerHandle GetSamplerHandle() const { return samplerHandle; }
    inline uint32_t getMipLevels() const { return mipLevels; }
    inline vk::Format GetFormat() const { return format; }
    inline const vk::DescriptorImageInfo& GetDescriptorInfo() const { return descriptorInfo; }
private:
    Texture();
    void CreateFromHostImage(
        VL::RendererBackendVulkan& rendererBackend,
        const HostImage& cpuImage,
        const CreateDesc& createDesc,
        const std::string& debugName);
    
    VL::RHIImageHandle imageHandle;
    VL::RHIImageViewHandle imageViewHandle;
    VL::RHISamplerHandle samplerHandle;
    vk::Image image = nullptr;
    vk::DeviceMemory imageMemory = nullptr;
    vk::ImageView imageView = nullptr;
    vk::Sampler sampler = nullptr;
    uint32_t mipLevels = 1;
    vk::Format format = vk::Format::eUndefined;
    vk::DescriptorImageInfo descriptorInfo;
    VL::RendererBackendVulkan* rendererBackend = nullptr;
};
