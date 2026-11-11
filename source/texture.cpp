#include "texture.h"
#include "commonFunction.h"
#include "render/backend/rendererBackendVulkan.h"
#include "resource/device/deviceTextureFactory.h"
#include "resource/image/compressedTextureDecoder.h"
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

Texture::Texture() = default;

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
    const std::string debugName = createDesc.debugName.empty()
        ? CommonFunction::Path(texturePath)
        : createDesc.debugName;

    // 文件型材质纹理统一恢复历史 V 方向合同；资产 JSON 不允许逐纹理覆盖该行为。
    const auto loadOptions = MaterialLoadOptionsForPath(createDesc, texturePath);
    auto cpuImage = TextureIO::Load(CommonFunction::Path(texturePath), loadOptions);
    if (!cpuImage.has_value())
    {
        throw std::runtime_error("Failed to load texture: " + texturePath);
    }

    CreateFromHostImage(rendererBackend, *cpuImage, createDesc, debugName);
}

TextureIO::LoadOptions Texture::MaterialLoadOptionsForPath(
    const CreateDesc& createDesc,
    const std::filesystem::path& sourcePath)
{
    TextureIO::LoadOptions options = MaterialLoadOptions(createDesc);
    if (CompressedTextureDecoder::IsCompressedContainerPath(sourcePath))
        options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
    return options;
}

Texture::Texture(
    VL::RendererBackendVulkan& rendererBackend,
    const HostImage& preparedImage,
    const CreateDesc& createDesc)
{
    CreateFromHostImage(rendererBackend, preparedImage, createDesc, createDesc.debugName);
}

std::shared_ptr<Texture> Texture::CreateEmptyArray(
    VL::RendererBackendVulkan& rendererBackend,
    const EmptyArrayDesc& desc)
{
    if (desc.width == 0 || desc.height == 0 || desc.arrayLayers == 0)
    {
        throw std::invalid_argument(
            "Texture::CreateEmptyArray requires non-zero extent and layer count");
    }
    if (desc.format == vk::Format::eUndefined)
    {
        throw std::invalid_argument(
            "Texture::CreateEmptyArray requires a concrete format");
    }

    const std::string debugName = desc.debugName.empty()
        ? "TextureEmptyArray"
        : desc.debugName;
    vk::ImageCreateInfo createInfo;
    createInfo
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ desc.width, desc.height, 1 })
        .setMipLevels(1)
        .setArrayLayers(desc.arrayLayers)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setFormat(desc.format)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferDst |
            vk::ImageUsageFlagBits::eTransferSrc)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);
    const std::pair<vk::Image, vk::DeviceMemory> created =
        rendererBackend.CreateImage(
            createInfo,
            vk::MemoryPropertyFlagBits::eDeviceLocal,
            "Texture: " + debugName);

    // 未定义内容先进入只读 layout；调用方随后用 clear/copy 填充时再按合法
    // 往返转换。这样空纹理创建本身不需要 CPU 像素，也不做 waitIdle。
    rendererBackend.TransitionImageLayout(
        created.first,
        1,
        desc.format,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eShaderReadOnlyOptimal,
        0,
        desc.arrayLayers);

    // 先由 unique_ptr 持有半成品：view/sampler 创建失败时析构函数会统一
    // 销毁已创建的 image/view/sampler，不会泄漏显存。
    std::unique_ptr<Texture> texture(new Texture());
    texture->rendererBackend = &rendererBackend;
    texture->image = created.first;
    texture->imageMemory = created.second;
    texture->mipLevels = 1;
    texture->arrayLayers = desc.arrayLayers;
    texture->format = desc.format;
    texture->imageHandle = rendererBackend.GetImageHandle(created.first);
    texture->imageView = rendererBackend.CreateImageView(
        created.first,
        vk::ImageViewType::e2DArray,
        desc.format,
        vk::ImageAspectFlagBits::eColor,
        0,
        1,
        0,
        desc.arrayLayers,
        "TextureView: " + debugName);
    texture->imageViewHandle =
        rendererBackend.GetImageViewHandle(texture->imageView);
    texture->sampler = rendererBackend.Create2DSampler(
        desc.filter,
        desc.wrapMode,
        false,
        "TextureSampler: " + debugName);
    texture->samplerHandle =
        rendererBackend.GetSamplerHandle(texture->sampler);
    texture->descriptorInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(texture->imageView)
        .setSampler(texture->sampler);
    return std::shared_ptr<Texture>(texture.release());
}

void Texture::CreateFromHostImage(
    VL::RendererBackendVulkan& rendererBackend,
    const HostImage& cpuImage,
    const CreateDesc& createDesc,
    const std::string& debugName)
{
    // 在分配 GPU 资源前确认 view 类型与层数匹配，避免失败路径留下半成品。
    if (createDesc.textureArray && cpuImage.arrayLayers == 0)
    {
        throw std::runtime_error("Texture array requires at least one layer: " + debugName);
    }
    if (!createDesc.textureArray && cpuImage.arrayLayers != 1)
    {
        throw std::runtime_error(
            "Texture with multiple array layers must be created as a texture array: " + debugName);
    }
    this->rendererBackend = &rendererBackend;
    DeviceTextureCreateOptions createOptions;
    createOptions.semantic = cpuImage.semantic;
    // 块压缩纹理的 mip 链必须由容器离线提供；资产 JSON 的 mipmaps 标志对
    // BC 源不生效，避免在设备端对压缩块生成错误的 mip。
    createOptions.generateMipmapsOnDevice =
        createDesc.generateMipmaps && !HostImage::IsBlockCompressed(cpuImage.format);
    DeviceTextureResource textureResource =
        DeviceTextureFactory::CreateResourceFromHostImage(rendererBackend, cpuImage, debugName, createOptions);
    imageHandle = textureResource.imageHandle;
    image = textureResource.image;
    imageMemory = textureResource.imageMemory;
    mipLevels = textureResource.mipLevels;
    arrayLayers = textureResource.arrayLayers;
    format = textureResource.format;

    if (createDesc.textureArray)
    {
        imageView = rendererBackend.CreateImageView(
            image,
            vk::ImageViewType::e2DArray,
            format,
            vk::ImageAspectFlagBits::eColor,
            0,
            mipLevels,
            0,
            arrayLayers,
            "TextureView: " + debugName);
    }
    else
    {
        imageView = rendererBackend.Create2DImageView(
            image,
            mipLevels,
            format,
            vk::ImageAspectFlagBits::eColor,
            "TextureView: " + debugName);
    }
    imageViewHandle = rendererBackend.GetImageViewHandle(imageView);
    sampler = rendererBackend.Create2DSampler(
        createDesc.filter,
        createDesc.wrapMode,
        mipLevels > 1,
        "TextureSampler: " + debugName);
    samplerHandle = rendererBackend.GetSamplerHandle(sampler);
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
    vk::Format format,
    uint32_t arrayLayers)
{
    this->rendererBackend = &rendererBackend;
    this->image = image;
    this->imageMemory = imageMemory;
    this->imageView = imageView;
    this->sampler = sampler;
    this->mipLevels = mipLevels;
    this->arrayLayers = arrayLayers;
    this->format = format;
    if (image)
    {
        imageHandle = rendererBackend.GetImageHandle(image);
    }
    if (imageView)
    {
        imageViewHandle = rendererBackend.GetImageViewHandle(imageView);
    }
    if (sampler)
    {
        samplerHandle = rendererBackend.GetSamplerHandle(sampler);
    }
    descriptorInfo
        .setImageLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setImageView(imageView)
        .setSampler(sampler);
}

Texture::~Texture()
{
    if (rendererBackend != nullptr)
    {
        if (imageHandle.IsValid() || imageViewHandle.IsValid() || samplerHandle.IsValid())
        {
            rendererBackend->DestroyImageResource(imageHandle, imageViewHandle, samplerHandle);
            image = nullptr;
            imageMemory = nullptr;
            imageView = nullptr;
            sampler = nullptr;
        }
        else
        {
            rendererBackend->DestroyImageResource(image, imageMemory, imageView, sampler);
        }
    }
}
