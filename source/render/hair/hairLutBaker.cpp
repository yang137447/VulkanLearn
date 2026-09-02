#include "render/hair/hairLutBaker.h"

#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "commonFunction.h"
#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/rendererResourceLoadContext.h"
#include "render/hair/hairAssets.h"
#include "render/hair/hairResourceSet.h"
#include "shader/build/contentHash.h"
#include "texture.h"

namespace VL
{
namespace
{

struct GpuVec4
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

// 与 hairAzimuthalLut.comp 的 std430 GenerationParameters 保持同序。
struct GenerationParameters
{
    GpuVec4 contract;
};

static_assert(sizeof(GpuVec4) == sizeof(float) * 4);

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

class GpuBufferOwner
{
public:
    explicit GpuBufferOwner(RendererBackendVulkan& rendererBackend)
        : rendererBackend(&rendererBackend)
    {
    }

    ~GpuBufferOwner()
    {
        if (rendererBackend != nullptr)
        {
            rendererBackend->DestroyBuffer(buffer, memory);
        }
    }

    GpuBufferOwner(const GpuBufferOwner&) = delete;
    GpuBufferOwner& operator=(const GpuBufferOwner&) = delete;

    void Adopt(vk::Buffer value, vk::DeviceMemory valueMemory) noexcept
    {
        buffer = value;
        memory = valueMemory;
    }

    vk::Buffer GetBuffer() const noexcept
    {
        return buffer;
    }

    vk::DeviceMemory GetMemory() const noexcept
    {
        return memory;
    }

private:
    RendererBackendVulkan* rendererBackend = nullptr;
    vk::Buffer buffer;
    vk::DeviceMemory memory;
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
        if (rendererBackend != nullptr && pool)
        {
            rendererBackend->DestroyDescriptorPool(pool);
        }
    }

    DescriptorPoolOwner(const DescriptorPoolOwner&) = delete;
    DescriptorPoolOwner& operator=(const DescriptorPoolOwner&) = delete;

    void Adopt(vk::DescriptorPool value) noexcept
    {
        pool = value;
    }

    vk::DescriptorPool Get() const noexcept
    {
        return pool;
    }

private:
    RendererBackendVulkan* rendererBackend = nullptr;
    vk::DescriptorPool pool;
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
        if (rendererBackend != nullptr && sampler)
        {
            rendererBackend->DestroySampler(sampler);
        }
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
    RendererBackendVulkan* rendererBackend = nullptr;
    vk::Sampler sampler;
};

GeneratedImage CreateHairLutImage(
    RendererBackendVulkan& rendererBackend,
    std::string_view sourceIdentity)
{
    constexpr vk::Format format = vk::Format::eR16G16B16A16Sfloat;
    vk::ImageCreateInfo createInfo;
    createInfo
        .setImageType(vk::ImageType::e2D)
        .setFormat(format)
        .setExtent(vk::Extent3D{
            HairAzimuthalLutWidth,
            HairAzimuthalLutHeight,
            1})
        .setMipLevels(1)
        .setArrayLayers(HairAzimuthalLutLayerCount)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(vk::ImageTiling::eOptimal)
        .setUsage(
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eSampled)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined);

    const std::string debugName =
        "HairAzimuthalLut:" + std::string(sourceIdentity);
    GeneratedImageOwner owner(rendererBackend);
    auto [image, memory] = rendererBackend.CreateImage(
        createInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        debugName);
    owner.Adopt({image, memory, nullptr});
    owner.Get().view = rendererBackend.CreateImageView(
        image,
        vk::ImageViewType::e2DArray,
        format,
        vk::ImageAspectFlagBits::eColor,
        0,
        1,
        0,
        HairAzimuthalLutLayerCount,
        debugName + ":View");
    GeneratedImage result = owner.Get();
    owner.Release();
    return result;
}

vk::ImageMemoryBarrier BuildImageBarrier(
    vk::Image image,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    vk::AccessFlags sourceAccess,
    vk::AccessFlags destinationAccess)
{
    vk::ImageMemoryBarrier barrier;
    barrier
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
                .setLayerCount(HairAzimuthalLutLayerCount))
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess);
    return barrier;
}

void AddHairFloatBits(
    CanonicalFieldHasher& hasher,
    std::string_view name,
    float value)
{
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value));
    std::memcpy(&bits, &value, sizeof(bits));
    hasher.AddUInt32(name, bits);
}

std::string BuildHairSourceDigest(
    const HairAzimuthalLutMetadata& metadata)
{
    CanonicalFieldHasher hasher("vulkanlearn.hair-azimuthal-lut.v2");
    hasher.AddUInt32("schemaVersion", metadata.schemaVersion);
    hasher.AddUInt32("lutVersion", metadata.lutVersion);
    hasher.AddUInt32("kernelVersion", metadata.kernelVersion);
    hasher.AddUInt32("width", metadata.width);
    hasher.AddUInt32("height", metadata.height);
    hasher.AddUInt32("layers", metadata.layers);
    hasher.AddUInt32("channels", metadata.channels);
    hasher.AddUInt32("roughnessSlices", metadata.roughnessSlices);
    hasher.AddUInt32("thetaDSamples", metadata.thetaDSamples);
    AddHairFloatBits(hasher, "ior", metadata.ior);
    AddHairFloatBits(hasher, "fiberRadius", metadata.fiberRadius);
    AddHairFloatBits(hasher, "generatorLongitudinalRoughness", 0.22f);
    AddHairFloatBits(hasher, "generatorAzimuthalRoughness", 0.25f);
    hasher.AddString("unit", metadata.unit);
    hasher.AddString("thetaDCoordinate", metadata.thetaDCoordinate);
    hasher.AddString("deltaPhiCoordinate", metadata.deltaPhiCoordinate);
    hasher.AddString("roughnessMapping", metadata.roughnessMapping);
    hasher.AddString("wrap", metadata.wrap);
    hasher.AddString("pathConvention", metadata.pathConvention);
    hasher.AddDigest(
        "generatorShader",
        ContentHasher::HashFile(CommonFunction::Path(
            "shader/glsl/generator/hairAzimuthalLut.comp")));
    if (!metadata.sourceIdentity.empty())
    {
        hasher.AddString("sourceIdentity", metadata.sourceIdentity);
    }
    return hasher.Finalize().ToHex();
}

} // namespace

std::shared_ptr<Texture> GenerateHairAzimuthalLutTexture(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    const HairLutBakeInput& input,
    std::string_view sourceIdentity)
{
    if (input.width != HairAzimuthalLutWidth ||
        input.height != HairAzimuthalLutHeight ||
        input.layers != HairAzimuthalLutLayerCount ||
        input.ior <= 1.0f ||
        input.fiberRadius <= 0.0f ||
        !std::isfinite(input.ior) ||
        !std::isfinite(input.fiberRadius) ||
        !std::isfinite(input.longitudinalRoughness) ||
        !std::isfinite(input.azimuthalRoughness) ||
        input.longitudinalRoughness <= 0.0f ||
        input.azimuthalRoughness <= 0.0f ||
        sourceIdentity.empty())
    {
        throw std::runtime_error(
            "Hair LUT baker received an input outside the frozen asset contract");
    }

    std::shared_ptr<ComputePipeline> computePipeline =
        pipelineFactory.CreateComputePipelineCandidate(
            "generator/hairAzimuthalLut");
    GeneratedImageOwner imageOwner(rendererBackend);
    imageOwner.Adopt(CreateHairLutImage(rendererBackend, sourceIdentity));

    GenerationParameters parameters;
    parameters.contract = {
        input.ior,
        input.fiberRadius,
        input.longitudinalRoughness,
        input.azimuthalRoughness};

    GpuBufferOwner parameterBufferOwner(rendererBackend);
    auto [parameterBuffer, parameterMemory] = rendererBackend.CreateBuffer(
        sizeof(parameters),
        vk::BufferUsageFlagBits::eStorageBuffer,
        vk::MemoryPropertyFlagBits::eHostVisible |
            vk::MemoryPropertyFlagBits::eHostCoherent,
        "HairAzimuthalLutGenerationParameters");
    parameterBufferOwner.Adopt(parameterBuffer, parameterMemory);
    void* mapped = rendererBackend.MapMemory(
        parameterBufferOwner.GetMemory(),
        sizeof(parameters));
    std::memcpy(mapped, &parameters, sizeof(parameters));
    rendererBackend.UnmapMemory(parameterBufferOwner.GetMemory());

    const std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize{
            vk::DescriptorType::eStorageImage,
            1},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eStorageBuffer,
            1}};
    vk::DescriptorPoolCreateInfo poolCreateInfo;
    poolCreateInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(1);
    DescriptorPoolOwner descriptorPoolOwner(rendererBackend);
    descriptorPoolOwner.Adopt(rendererBackend.CreateDescriptorPool(
        poolCreateInfo,
        "DescriptorPool: HairAzimuthalLutGeneration"));

    const vk::DescriptorSetLayout setLayout =
        computePipeline->GetDescriptorSetLayouts().at(0);
    vk::DescriptorSetAllocateInfo allocateInfo;
    allocateInfo
        .setDescriptorPool(descriptorPoolOwner.Get())
        .setSetLayouts(setLayout);
    std::vector<vk::DescriptorSet> descriptorSets(1);
    rendererBackend.AllocateDescriptorSets(allocateInfo, descriptorSets);
    const vk::DescriptorSet descriptorSet = descriptorSets[0];

    const vk::DescriptorImageInfo imageInfo{
        vk::Sampler(),
        imageOwner.Get().view,
        vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo parameterInfo{
        parameterBufferOwner.GetBuffer(),
        0,
        sizeof(parameters)};

    std::array<vk::WriteDescriptorSet, 2> writes;
    writes[0]
        .setDstSet(descriptorSet)
        .setDstBinding(0)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageImage)
        .setImageInfo(imageInfo);
    writes[1]
        .setDstSet(descriptorSet)
        .setDstBinding(1)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageBuffer)
        .setBufferInfo(parameterInfo);
    rendererBackend.UpdateDescriptorSets(
        std::vector<vk::WriteDescriptorSet>(writes.begin(), writes.end()));

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
            descriptorSet,
            nullptr);
        computePipeline->Dispatch(
            commandBuffer,
            (input.width + 7) / 8,
            (input.height + 7) / 8,
            input.layers);

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
    vk::SamplerCreateInfo samplerInfo;
    samplerInfo
        .setMagFilter(vk::Filter::eLinear)
        .setMinFilter(vk::Filter::eLinear)
        .setMipmapMode(vk::SamplerMipmapMode::eNearest)
        .setAddressModeU(vk::SamplerAddressMode::eRepeat)
        .setAddressModeV(vk::SamplerAddressMode::eClampToEdge)
        .setAddressModeW(vk::SamplerAddressMode::eClampToEdge)
        .setAnisotropyEnable(VK_FALSE)
        .setMaxAnisotropy(1.0f)
        .setBorderColor(vk::BorderColor::eIntOpaqueBlack)
        .setUnnormalizedCoordinates(VK_FALSE)
        .setCompareEnable(VK_FALSE)
        .setCompareOp(vk::CompareOp::eAlways)
        .setMinLod(0.0f)
        .setMaxLod(0.0f);
    samplerOwner.Adopt(rendererBackend.CreateSampler(
        samplerInfo,
        "HairAzimuthalLutSampler"));
    std::shared_ptr<Texture> texture = std::make_shared<Texture>(
        rendererBackend,
        imageOwner.Get().image,
        imageOwner.Get().memory,
        imageOwner.Get().view,
        samplerOwner.Get(),
        1,
        vk::Format::eR16G16B16A16Sfloat);
    imageOwner.Release();
    samplerOwner.Release();
    return texture;
}

HairResourceLoader::HairResourceLoader(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    RendererResourceLoadContext& loadContext)
    : pipelineFactory(pipelineFactory)
    , rendererBackend(rendererBackend)
    , loadContext(loadContext)
{
}

void HairResourceLoader::Load() const
{
    const std::filesystem::path resourceRoot = CommonFunction::GetResourcePath();
    const HairAzimuthalLutAsset authoredAsset =
        LoadHairAzimuthalLutAsset(resourceRoot);
    const HairAzimuthalLutMetadata& metadata = authoredAsset.metadata;

    const std::string sourceDigest = BuildHairSourceDigest(metadata);
    const std::string generatedMetadataBytes =
        SerializeHairAzimuthalLutMetadata(metadata).dump(4) + "\n";
    loadContext.QueueGeneratedFile(
        resourceRoot / "Generated" / "Runtime" / "hairAzimuthalLut.json",
        std::vector<uint8_t>(
            generatedMetadataBytes.begin(),
            generatedMetadataBytes.end()));

    if (loadContext.previousWorldResources &&
        loadContext.previousWorldResources->hairResources &&
        loadContext.previousWorldResources->hairResources->sourceDigest == sourceDigest)
    {
        loadContext.resourceCache.BindHairResources(
            loadContext.previousWorldResources->hairResources);
        loadContext.resourceCache.BindWorldTexture(
            "hairAzimuthalLut",
            loadContext.previousWorldResources->hairResources->azimuthalLutTexture);
        return;
    }

    HairLutBakeInput bakeInput;
    bakeInput.width = metadata.width;
    bakeInput.height = metadata.height;
    bakeInput.layers = metadata.layers;
    bakeInput.ior = metadata.ior;
    bakeInput.fiberRadius = metadata.fiberRadius;
    bakeInput.longitudinalRoughness = 0.22f;
    bakeInput.azimuthalRoughness = 0.25f;

    std::shared_ptr<HairResourceSet> resourceSet =
        std::make_shared<HairResourceSet>();
    resourceSet->sourceDigest = sourceDigest;
    resourceSet->sourceIdentity = metadata.sourceIdentity;
    resourceSet->lutMetadata = metadata;
    resourceSet->azimuthalLutTexture = GenerateHairAzimuthalLutTexture(
        pipelineFactory,
        rendererBackend,
        bakeInput,
        sourceDigest);

    loadContext.resourceCache.BindWorldTexture(
        "hairAzimuthalLut",
        resourceSet->azimuthalLutTexture);
    loadContext.resourceCache.BindHairResources(std::move(resourceSet));
}

} // namespace VL
