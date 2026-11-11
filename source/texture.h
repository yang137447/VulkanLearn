#pragma once
#include <filesystem>
#include <memory>
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
        // 数组纹理必须建 2D_ARRAY view；单图路径保持 false，避免把 2D 材质绑到数组 view。
        bool textureArray = false;
        std::string debugName;
    };

    // 无 CPU 像素的数组纹理：用于层 atlas 这类先分配、后按页 GPU 写入的资源。
    // 创建后内容是未定义的，调用方必须自行 clear 或填满。
    struct EmptyArrayDesc
    {
        uint32_t width = 1;
        uint32_t height = 1;
        uint32_t arrayLayers = 1;
        vk::Format format = vk::Format::eR8G8B8A8Unorm;
        vk::Filter filter = vk::Filter::eLinear;
        vk::SamplerAddressMode wrapMode =
            vk::SamplerAddressMode::eClampToEdge;
        std::string debugName;
    };

    static std::shared_ptr<Texture> CreateEmptyArray(
        VL::RendererBackendVulkan& rendererBackend,
        const EmptyArrayDesc& desc);

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
    // 压缩容器（DDS/KTX2 的 BC 块）不能在运行时按行翻转，朝向必须离线烘焙；
    // 因此文件型入口对这类源显式退回 ForceOff，其余源保持统一的 V 翻转合同。
    static TextureIO::LoadOptions MaterialLoadOptionsForPath(
        const CreateDesc& createDesc,
        const std::filesystem::path& sourcePath);
    Texture(
        VL::RendererBackendVulkan& rendererBackend,
        vk::Image image,
        vk::DeviceMemory imageMemory,
        vk::ImageView imageView,
        vk::Sampler sampler,
        uint32_t mipLevels,
        vk::Format format,
        uint32_t arrayLayers = 1);
    ~Texture();
    inline vk::Image getImage() const { return image; }
    inline vk::DeviceMemory getImageMemory() const { return imageMemory; }
    inline vk::ImageView getImageView() const { return imageView; }
    inline vk::Sampler getSampler() const { return sampler; }
    inline VL::RHIImageHandle GetImageHandle() const { return imageHandle; }
    inline VL::RHIImageViewHandle GetImageViewHandle() const { return imageViewHandle; }
    inline VL::RHISamplerHandle GetSamplerHandle() const { return samplerHandle; }
    inline uint32_t getMipLevels() const { return mipLevels; }
    inline uint32_t GetArrayLayers() const { return arrayLayers; }
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
    uint32_t arrayLayers = 1;
    vk::Format format = vk::Format::eUndefined;
    vk::DescriptorImageInfo descriptorInfo;
    VL::RendererBackendVulkan* rendererBackend = nullptr;
};
