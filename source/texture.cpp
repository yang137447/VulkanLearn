#include "texture.h"
#include "commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"
#include "resource/device/deviceTextureFactory.h"
#include "resource/image/textureIO.h"
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

Texture::Texture(VL::RendererBackendVulkan& rendererBackend, const std::string& texturePath)
    : Texture(rendererBackend, texturePath, HostImage::TextureSemantic::Color, TextureIO::LoadOptions::Transfer::SRGB)
{
}

Texture::Texture(
    VL::RendererBackendVulkan& rendererBackend,
    const std::string& texturePath,
    HostImage::TextureSemantic semantic,
    TextureIO::LoadOptions::Transfer transfer)
    : Texture(rendererBackend, texturePath, MakeTextureCreateDesc(semantic, transfer))
{
}

Texture::Texture(
    VL::RendererBackendVulkan& rendererBackend,
    const std::string& texturePath,
    const CreateDesc& createDesc)
{
    this->rendererBackend = &rendererBackend;
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
    std::tie(image, imageMemory, mipLevels, format) =
        DeviceTextureFactory::CreateFromHostImage(rendererBackend, *cpuImage, debugName, createOptions);

    imageView = rendererBackend.Create2DImageView(
        image,
        mipLevels,
        format,
        vk::ImageAspectFlagBits::eColor,
        "TextureView: " + debugName);
    sampler = rendererBackend.Create2DSampler(
        createDesc.filter,
        createDesc.wrapMode,
        createDesc.generateMipmaps,
        "TextureSampler: " + debugName);
    descriptorInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(imageView)
        .setSampler(sampler);
}

Texture::Texture(
    VL::RendererBackendVulkan& rendererBackend,
    vk::Image image,
    vk::DeviceMemory imageMemory,
    vk::ImageView imageView,
    vk::Sampler sampler,
    uint32_t mipLevels,
    vk::Format format)
{
    this->rendererBackend = &rendererBackend;
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
    if (rendererBackend != nullptr)
    {
        rendererBackend->DestroyImageResource(image, imageMemory, imageView, sampler);
    }
}
