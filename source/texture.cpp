#include "texture.h"
#include "commonFunction.h"
#include "resource/device/deviceTextureFactory.h"
#include "resource/image/textureIO.h"
#include "vulkanManager.h"
#include <stdexcept>

namespace
{
    Texture::CreateDesc MakeTextureCreateDesc(
        HostImage::TextureSemantic semantic,
        TextureIO::LoadOptions::Transfer transfer)
    {
        Texture::CreateDesc createDesc;
        createDesc.semantic = semantic;
        createDesc.transfer = transfer;
        return createDesc;
    }
}

Texture::Texture(const std::string& texturePath)
    : Texture(texturePath, HostImage::TextureSemantic::Color, TextureIO::LoadOptions::Transfer::SRGB)
{
}

Texture::Texture(const std::string& texturePath, HostImage::TextureSemantic semantic, TextureIO::LoadOptions::Transfer transfer)
    : Texture(texturePath, MakeTextureCreateDesc(semantic, transfer))
{
}

Texture::Texture(const std::string& texturePath, const CreateDesc& createDesc)
{
    const std::string debugName = createDesc.debugName.empty()
        ? CommonFunction::Path(texturePath)
        : createDesc.debugName;

    TextureIO::LoadOptions loadOptions;
    loadOptions.semantic = createDesc.semantic;
    loadOptions.flipY = TextureIO::LoadOptions::FlipYMode::ForceOn;
    loadOptions.transfer = createDesc.transfer;
    loadOptions.forceChannels = 4;
    auto cpuImage = TextureIO::Load(CommonFunction::Path(texturePath), loadOptions);
    if (!cpuImage.has_value())
    {
        throw std::runtime_error("Failed to load texture: " + texturePath);
    }

    DeviceTextureCreateOptions createOptions;
    createOptions.semantic = cpuImage->semantic;
    createOptions.generateMipmapsOnDevice = createDesc.generateMipmaps;
    std::tie(image, imageMemory, mipLevels, format) = DeviceTextureFactory::CreateFromHostImage(*cpuImage, debugName, createOptions);

    auto& vulkanManager = VulkanManager::GetInstance();
    imageView = CommonFunction::Create2DImageView(vulkanManager.GetDevice(), image, mipLevels, format, vk::ImageAspectFlagBits::eColor, "TextureView: " + debugName);
    sampler = CommonFunction::Create2DSampler(
        vulkanManager.GetDevice(),
        vulkanManager.GetPhysicalDevice(),
        createDesc.filter,
        createDesc.wrapMode,
        createDesc.generateMipmaps,
        "TextureSampler: " + debugName);
    descriptorInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(imageView)
        .setSampler(sampler);
}

Texture::Texture(vk::Image image, vk::DeviceMemory imageMemory, vk::ImageView imageView, vk::Sampler sampler, uint32_t mipLevels, vk::Format format)
{
    this->image = image;
    this->imageMemory = imageMemory;
    this->imageView = imageView;
    this->sampler = sampler;
    this->mipLevels = mipLevels;
    this->format = format;
    descriptorInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(imageView)
        .setSampler(sampler);
}

Texture::~Texture()
{
    auto& device = VulkanManager::GetInstance().GetDevice();
    device.destroyImageView(imageView);
    device.destroyImage(image);
    device.freeMemory(imageMemory);
    device.destroySampler(sampler);
}
