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
                .setLayerCount(1))
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess);
}

GeneratedImage CreateLutImage(RendererBackendVulkan& rendererBackend)
{
    constexpr vk::Format format = vk::Format::eR16G16B16A16Sfloat;
    const vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eStorage |
        vk::ImageUsageFlagBits::eSampled;
    auto [image, memory] = rendererBackend.CreateImage(
        ClothDirectionalAlbedoLutWidth,
        ClothDirectionalAlbedoLutHeight,
        1,
        vk::SampleCountFlagBits::e1,
        format,
        vk::ImageTiling::eOptimal,
        usage,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        "ClothDirectionalAlbedoLutImage");
    return {
        image,
        memory,
        rendererBackend.Create2DImageView(
            image,
            1,
            format,
            vk::ImageAspectFlagBits::eColor,
            "ClothDirectionalAlbedoLutView")};
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

    GeneratedImageOwner imageOwner(rendererBackend);
    imageOwner.Adopt(CreateLutImage(rendererBackend));

    DescriptorPoolOwner descriptorPoolOwner(rendererBackend);
    const vk::DescriptorPoolSize poolSize{
        vk::DescriptorType::eStorageImage,
        1};
    descriptorPoolOwner.Adopt(rendererBackend.CreateDescriptorPool(
        vk::DescriptorPoolCreateInfo()
            .setPoolSizes(poolSize)
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
    const vk::DescriptorImageInfo imageInfo{
        vk::Sampler(),
        imageOwner.Get().view,
        vk::ImageLayout::eGeneral};
    rendererBackend.UpdateDescriptorSets({
        vk::WriteDescriptorSet()
            .setDstSet(descriptorSets.at(0))
            .setDstBinding(0)
            .setDescriptorCount(1)
            .setDescriptorType(vk::DescriptorType::eStorageImage)
            .setImageInfo(imageInfo)});

    vk::CommandBuffer commandBuffer =
        rendererBackend.BeginSingleTimeCommands();
    try
    {
        const vk::ImageMemoryBarrier toGeneral = BuildImageBarrier(
            imageOwner.Get().image,
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
            toGeneral);
        computePipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            computePipeline->GetPipelineLayout(),
            0,
            descriptorSets.at(0),
            nullptr);
        computePipeline->Dispatch(
            commandBuffer,
            (ClothDirectionalAlbedoLutWidth + 7) / 8,
            (ClothDirectionalAlbedoLutHeight + 7) / 8,
            1);
        const vk::ImageMemoryBarrier toSample = BuildImageBarrier(
            imageOwner.Get().image,
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
            toSample);
        rendererBackend.EndSingleTimeCommands(commandBuffer);
    }
    catch (...)
    {
        rendererBackend.AbortSingleTimeCommands(commandBuffer);
        throw;
    }

    SamplerOwner samplerOwner(rendererBackend);
    samplerOwner.Adopt(rendererBackend.Create2DSampler(
        vk::Filter::eLinear,
        vk::SamplerAddressMode::eClampToEdge,
        false,
        "ClothDirectionalAlbedoLutSampler"));
    ClothLookupTableGenerationResult result;
    result.directionalAlbedoLutTexture = std::make_shared<Texture>(
        rendererBackend,
        imageOwner.Get().image,
        imageOwner.Get().memory,
        imageOwner.Get().view,
        samplerOwner.Get(),
        1,
        vk::Format::eR16G16B16A16Sfloat);
    imageOwner.Release();
    samplerOwner.Release();

    return result;
}

} // namespace VL
