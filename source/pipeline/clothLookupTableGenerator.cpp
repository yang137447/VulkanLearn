#include "pipeline/clothLookupTableGenerator.h"

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/cloth/clothAssets.h"
#include "texture.h"

namespace VL
{
namespace
{

struct GeneratedImage
{
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView view;
    uint32_t layerCount = 1;
};

class GeneratedImageOwner
{
public:
    explicit GeneratedImageOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~GeneratedImageOwner()
    {
        Reset();
    }

    GeneratedImageOwner(const GeneratedImageOwner&) = delete;
    GeneratedImageOwner& operator=(const GeneratedImageOwner&) = delete;

    void Adopt(GeneratedImage value) noexcept
    {
        image = value;
    }

    GeneratedImage& Get() noexcept
    {
        return image;
    }

    void Release() noexcept
    {
        rendererBackend = nullptr;
        image = {};
    }

private:
    void Reset() noexcept
    {
        if (rendererBackend == nullptr)
        {
            return;
        }
        vk::Sampler sampler;
        rendererBackend->DestroyImageResource(
            image.image,
            image.memory,
            image.view,
            sampler);
        rendererBackend = nullptr;
        image = {};
    }

    RendererBackendVulkan* rendererBackend = nullptr;
    GeneratedImage image;
};

class DescriptorPoolOwner
{
public:
    explicit DescriptorPoolOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~DescriptorPoolOwner()
    {
        Reset();
    }

    DescriptorPoolOwner(const DescriptorPoolOwner&) = delete;
    DescriptorPoolOwner& operator=(const DescriptorPoolOwner&) = delete;

    void Adopt(vk::DescriptorPool value) noexcept
    {
        descriptorPool = value;
    }

    vk::DescriptorPool Get() const noexcept
    {
        return descriptorPool;
    }

    void Release() noexcept
    {
        rendererBackend = nullptr;
        descriptorPool = nullptr;
    }

private:
    void Reset() noexcept
    {
        if (rendererBackend != nullptr)
        {
            rendererBackend->DestroyDescriptorPool(descriptorPool);
            rendererBackend = nullptr;
        }
    }

    RendererBackendVulkan* rendererBackend = nullptr;
    vk::DescriptorPool descriptorPool;
};

class SamplerOwner
{
public:
    explicit SamplerOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~SamplerOwner()
    {
        Reset();
    }

    SamplerOwner(const SamplerOwner&) = delete;
    SamplerOwner& operator=(const SamplerOwner&) = delete;

    void Adopt(vk::Sampler value) noexcept
    {
        sampler = value;
    }

    vk::Sampler Get() const noexcept
    {
        return sampler;
    }

    void Release() noexcept
    {
        rendererBackend = nullptr;
        sampler = nullptr;
    }

private:
    void Reset() noexcept
    {
        if (rendererBackend != nullptr)
        {
            rendererBackend->DestroySampler(sampler);
            rendererBackend = nullptr;
        }
    }

    RendererBackendVulkan* rendererBackend = nullptr;
    vk::Sampler sampler;
};

vk::ImageMemoryBarrier BuildImageBarrier(
    vk::Image image,
    uint32_t layerCount,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    vk::AccessFlags sourceAccess,
    vk::AccessFlags destinationAccess)
{
    return vk::ImageMemoryBarrier()
        .setOldLayout(oldLayout)
        .setNewLayout(newLayout)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(image)
        .setSubresourceRange(
            vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(layerCount))
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess);
}

GeneratedImage CreateLutImage(
    RendererBackendVulkan& rendererBackend,
    uint32_t width,
    uint32_t height,
    uint32_t layerCount,
    vk::ImageViewType viewType,
    const std::string& imageName,
    const std::string& viewName)
{
    constexpr vk::Format format = vk::Format::eR16G16B16A16Sfloat;
    const vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eSampled;
    const vk::ImageCreateInfo createInfo = vk::ImageCreateInfo()
        .setImageType(vk::ImageType::e2D)
        .setFormat(format)
        .setExtent(vk::Extent3D(width, height, 1))
        .setMipLevels(1)
        .setArrayLayers(layerCount)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(usage)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);
    auto [image, memory] = rendererBackend.CreateImage(
        createInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        imageName);
    vk::ImageView view = viewType == vk::ImageViewType::e2D
        ? rendererBackend.Create2DImageView(
            image,
            1,
            format,
            vk::ImageAspectFlagBits::eColor,
            viewName)
        : rendererBackend.CreateImageView(
            image,
            viewType,
            format,
            vk::ImageAspectFlagBits::eColor,
            0,
            1,
            0,
            layerCount,
            viewName);
    return {
        image,
        memory,
        view,
        layerCount};
}

} // namespace

ClothLookupTableGenerationResult ClothLookupTableGenerator::Generate(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    const std::string& sourceDigest)
{
    std::shared_ptr<ComputePipeline> pipeline =
        pipelineFactory.CreateComputePipeline("generator/clothLookupTables");
    return GenerateWithPipeline(
        pipeline,
        rendererBackend,
        sourceDigest);
}

ClothLookupTableGenerationResult
ClothLookupTableGenerator::GenerateWithPipeline(
    const std::shared_ptr<ComputePipeline>& computePipeline,
    RendererBackendVulkan& rendererBackend,
    const std::string& sourceDigest)
{
    if (!computePipeline)
    {
        throw std::runtime_error(
            "Cloth lookup generation requires a Compute pipeline");
    }
    // digest 由 loader/participant 管理；这里不参与数值生成，避免 CPU 旁路。
    (void)sourceDigest;

    GeneratedImageOwner isotropicImageOwner(rendererBackend);
    isotropicImageOwner.Adopt(CreateLutImage(
        rendererBackend,
        ClothDirectionalAlbedoLutWidth,
        ClothDirectionalAlbedoLutHeight,
        1,
        vk::ImageViewType::e2D,
        "ClothDirectionalAlbedoLutImage",
        "ClothDirectionalAlbedoLutView"));
    GeneratedImageOwner anisotropicImageOwner(rendererBackend);
    anisotropicImageOwner.Adopt(CreateLutImage(
        rendererBackend,
        ClothAnisotropicDirectionalAlbedoLutWidth,
        ClothAnisotropicDirectionalAlbedoLutHeight,
        ClothAnisotropicDirectionalAlbedoLutLayers,
        vk::ImageViewType::e2DArray,
        "ClothAnisotropicDirectionalAlbedoLutImage",
        "ClothAnisotropicDirectionalAlbedoLutView"));

    DescriptorPoolOwner descriptorPoolOwner(rendererBackend);
    const std::array<vk::DescriptorPoolSize, 1> poolSizes{{
        vk::DescriptorPoolSize(vk::DescriptorType::eStorageImage, 2)}};
    descriptorPoolOwner.Adopt(rendererBackend.CreateDescriptorPool(
        vk::DescriptorPoolCreateInfo()
            .setPoolSizes(poolSizes)
            .setMaxSets(1),
        "DescriptorPool: ClothDirectionalAlbedoLut"));

    const vk::DescriptorSetLayout setLayout =
        computePipeline->GetDescriptorSetLayouts().at(0);
    std::vector<vk::DescriptorSet> descriptorSets;
    rendererBackend.AllocateDescriptorSets(
        vk::DescriptorSetAllocateInfo()
            .setDescriptorPool(descriptorPoolOwner.Get())
            .setSetLayouts(setLayout),
        descriptorSets);
    const vk::DescriptorImageInfo isotropicImageInfo{
        vk::Sampler(),
        isotropicImageOwner.Get().view,
        vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo anisotropicImageInfo{
        vk::Sampler(),
        anisotropicImageOwner.Get().view,
        vk::ImageLayout::eGeneral};
    const std::vector<vk::WriteDescriptorSet> writes{
        vk::WriteDescriptorSet()
            .setDstSet(descriptorSets.at(0))
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageImage)
            .setImageInfo(isotropicImageInfo),
        vk::WriteDescriptorSet()
            .setDstSet(descriptorSets.at(0))
            .setDstBinding(1)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageImage)
            .setImageInfo(anisotropicImageInfo)};
    rendererBackend.UpdateDescriptorSets(writes);

    vk::CommandBuffer commandBuffer =
        rendererBackend.BeginSingleTimeCommands();
    try
    {
        const vk::ImageMemoryBarrier isotropicToGeneral = BuildImageBarrier(
            isotropicImageOwner.Get().image,
            isotropicImageOwner.Get().layerCount,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eGeneral,
            vk::AccessFlagBits::eNone,
            vk::AccessFlagBits::eShaderWrite);
        const vk::ImageMemoryBarrier anisotropicToGeneral = BuildImageBarrier(
            anisotropicImageOwner.Get().image,
            anisotropicImageOwner.Get().layerCount,
            vk::ImageLayout::eUndefined,
            vk::ImageLayout::eGeneral,
            vk::AccessFlagBits::eNone,
            vk::AccessFlagBits::eShaderWrite);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            std::array<vk::ImageMemoryBarrier, 2>{
                isotropicToGeneral,
                anisotropicToGeneral});
        computePipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            computePipeline->GetPipelineLayout(),
            0,
            descriptorSets.at(0),
            nullptr);
        // x/y 以 v1 LUT 的尺寸 dispatch，shader 内对 v2 的较小 2D 域做边界裁剪；
        // z 覆盖每一层 signed anisotropy，保证两张 LUT 在一次 Compute 事务中同时完成。
        computePipeline->Dispatch(
            commandBuffer,
            (ClothDirectionalAlbedoLutWidth + 7) / 8,
            (ClothDirectionalAlbedoLutHeight + 7) / 8,
            ClothAnisotropicDirectionalAlbedoLutLayers);
        const vk::ImageMemoryBarrier isotropicToSample = BuildImageBarrier(
            isotropicImageOwner.Get().image,
            isotropicImageOwner.Get().layerCount,
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead);
        const vk::ImageMemoryBarrier anisotropicToSample = BuildImageBarrier(
            anisotropicImageOwner.Get().image,
            anisotropicImageOwner.Get().layerCount,
            vk::ImageLayout::eGeneral,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eShaderRead);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eComputeShader,
            vk::PipelineStageFlagBits::eFragmentShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            std::array<vk::ImageMemoryBarrier, 2>{
                isotropicToSample,
                anisotropicToSample});
        rendererBackend.EndSingleTimeCommands(commandBuffer);
    }
    catch (...)
    {
        rendererBackend.AbortSingleTimeCommands(commandBuffer);
        throw;
    }

    SamplerOwner isotropicSamplerOwner(rendererBackend);
    isotropicSamplerOwner.Adopt(rendererBackend.Create2DSampler(
        vk::Filter::eLinear,
        vk::SamplerAddressMode::eClampToEdge,
        false,
        "ClothDirectionalAlbedoLutSampler"));
    SamplerOwner anisotropicSamplerOwner(rendererBackend);
    anisotropicSamplerOwner.Adopt(rendererBackend.Create2DSampler(
        vk::Filter::eLinear,
        vk::SamplerAddressMode::eClampToEdge,
        false,
        "ClothAnisotropicDirectionalAlbedoLutSampler"));
    ClothLookupTableGenerationResult result;
    result.directionalAlbedoLutTexture = std::make_shared<Texture>(
        rendererBackend,
        isotropicImageOwner.Get().image,
        isotropicImageOwner.Get().memory,
        isotropicImageOwner.Get().view,
        isotropicSamplerOwner.Get(),
        1,
        vk::Format::eR16G16B16A16Sfloat);
    result.anisotropicDirectionalAlbedoLutTexture = std::make_shared<Texture>(
        rendererBackend,
        anisotropicImageOwner.Get().image,
        anisotropicImageOwner.Get().memory,
        anisotropicImageOwner.Get().view,
        anisotropicSamplerOwner.Get(),
        1,
        vk::Format::eR16G16B16A16Sfloat);
    isotropicImageOwner.Release();
    anisotropicImageOwner.Release();
    isotropicSamplerOwner.Release();
    anisotropicSamplerOwner.Release();

    return result;
}

} // namespace VL
