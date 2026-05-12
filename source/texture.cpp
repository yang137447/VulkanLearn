#include "texture.h"
#include "commonFunction.h"
#include "resource/device/deviceTextureFactory.h"
#include "resource/image/textureIO.h"
#include "vulkanManager.h"
#include <stdexcept>

Texture::Texture(const std::string& texturePath)
{
    TextureIO::LoadOptions loadOptions;
    loadOptions.semantic = HostImage::TextureSemantic::Color;
    loadOptions.flipY = TextureIO::LoadOptions::FlipYMode::ForceOn;
    loadOptions.transfer = TextureIO::LoadOptions::Transfer::SRGB;
    loadOptions.forceChannels = 4;
    auto cpuImage = TextureIO::Load(CommonFunction::Path(texturePath), loadOptions);
    if (!cpuImage.has_value())
    {
        throw std::runtime_error("Failed to load texture: " + texturePath);
    }

    DeviceTextureCreateOptions createOptions;
    createOptions.semantic = cpuImage->semantic;
    createOptions.generateMipmapsOnDevice = true;
    std::tie(image, imageMemory, mipLevels, format) = DeviceTextureFactory::CreateFromHostImage(*cpuImage, CommonFunction::Path(texturePath), createOptions);

    auto& vulkanManager = VulkanManager::GetInstance();
    imageView = CommonFunction::Create2DImageView(vulkanManager.GetDevice(), image, mipLevels, format, vk::ImageAspectFlagBits::eColor, "TextureView: " + CommonFunction::Path(texturePath));
    sampler = CommonFunction::Create2DSampler(vulkanManager.GetDevice(), vulkanManager.GetPhysicalDevice(), "TextureSampler: " + CommonFunction::Path(texturePath));
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
