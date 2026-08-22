#include "pipeline/subsurfaceLookupTableGenerator.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
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

// 必须与 subsurfaceLookupTables.comp 的 std430 GenerationParameters 完全同序。
// 数组直接按稳定 ID 索引，避免再维护一份 CPU 到 GPU 的紧凑重映射表。
struct GenerationParameters
{
    std::array<GpuVec4, SubsurfaceProfileMaximumId + 1>
        profileColorDistance;
    std::array<GpuVec4, SubsurfaceProfileMaximumId + 1>
        profileScaleActive;
    std::array<GpuVec4, PreintegratedSkinMaximumId + 1>
        skinScatterMode;
    std::array<GpuVec4, PreintegratedSkinMaximumId + 1>
        skinTransmissionThickness;
    std::array<GpuVec4, PreintegratedSkinMaximumId + 1>
        skinScaleActive;
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

    void Adopt(GeneratedImage generatedImage) noexcept
    {
        image = generatedImage;
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
        Reset();
    }

    GpuBufferOwner(const GpuBufferOwner&) = delete;
    GpuBufferOwner& operator=(const GpuBufferOwner&) = delete;

    void Adopt(vk::Buffer adoptedBuffer, vk::DeviceMemory adoptedMemory) noexcept
    {
        buffer = adoptedBuffer;
        memory = adoptedMemory;
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
    void Reset() noexcept
    {
        if (rendererBackend != nullptr)
        {
            rendererBackend->DestroyBuffer(buffer, memory);
            rendererBackend = nullptr;
        }
    }

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
        Reset();
    }

    DescriptorPoolOwner(const DescriptorPoolOwner&) = delete;
    DescriptorPoolOwner& operator=(const DescriptorPoolOwner&) = delete;

    void Adopt(vk::DescriptorPool adoptedPool) noexcept
    {
        descriptorPool = adoptedPool;
    }

    vk::DescriptorPool Get() const noexcept
    {
        return descriptorPool;
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

    void Adopt(vk::Sampler adoptedSampler) noexcept
    {
        sampler = adoptedSampler;
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

GenerationParameters BuildGenerationParameters(
    const std::vector<SubsurfaceProfileAsset>& profiles,
    const std::vector<PreintegratedSkinLutAsset>& skinLuts)
{
    GenerationParameters parameters;
    // 未启用的行保持 neutral 数据；ID 0 因此可以作为稳定 fallback，
    // Compute Shader 仍会完整写入目标纹理而不依赖未初始化显存。
    for (GpuVec4& value : parameters.profileColorDistance)
    {
        value = {1.0f, 1.0f, 1.0f, 1.0f};
    }
    for (GpuVec4& value : parameters.profileScaleActive)
    {
        value = {1.0f, 0.0f, 0.0f, 0.0f};
    }
    for (GpuVec4& value : parameters.skinScatterMode)
    {
        value = {1.0f, 1.0f, 1.0f, 1.0f};
    }
    for (GpuVec4& value : parameters.skinTransmissionThickness)
    {
        value = {1.0f, 1.0f, 1.0f, 1.0f};
    }
    for (GpuVec4& value : parameters.skinScaleActive)
    {
        value = {1.0f, 0.0f, 0.0f, 0.0f};
    }

    // CPU 只按 SSBO ABI 打包作者参数，不在这里求 profile 权重或 LUT response。
    for (const SubsurfaceProfileAsset& asset : profiles)
    {
        parameters.profileColorDistance[asset.profileId] = {
            asset.meanFreePathColor[0],
            asset.meanFreePathColor[1],
            asset.meanFreePathColor[2],
            asset.meanFreePathDistance};
        parameters.profileScaleActive[asset.profileId] = {
            asset.worldUnitScale,
            1.0f,
            0.0f,
            0.0f};
    }

    for (const PreintegratedSkinLutAsset& asset : skinLuts)
    {
        parameters.skinScatterMode[asset.skinLutId] = {
            asset.scatterColor[0],
            asset.scatterColor[1],
            asset.scatterColor[2],
            asset.outputMode ==
                    PreintegratedSkinOutputMode::FinalDiffuseResponse
                ? 1.0f
                : 0.0f};
        parameters.skinTransmissionThickness[asset.skinLutId] = {
            asset.transmissionColor[0],
            asset.transmissionColor[1],
            asset.transmissionColor[2],
            asset.thicknessMax};
        parameters.skinScaleActive[asset.skinLutId] = {
            asset.worldUnitScale,
            1.0f,
            0.0f,
            0.0f};
    }
    return parameters;
}

GeneratedImage CreateLookupImage(
    RendererBackendVulkan& rendererBackend,
    uint32_t width,
    uint32_t height,
    const std::string& debugName)
{
    // 同一张图先作为 storage image 写入，再作为 sampled image 被 lighting/filter 使用。
    constexpr vk::Format Format =
        vk::Format::eR16G16B16A16Sfloat;
    GeneratedImageOwner imageOwner(rendererBackend);
    auto [image, memory] = rendererBackend.CreateImage(
        width,
        height,
        1,
        vk::SampleCountFlagBits::e1,
        Format,
        vk::ImageTiling::eOptimal,
        vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eSampled,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        debugName);
    imageOwner.Adopt({image, memory, nullptr});
    imageOwner.Get().view = rendererBackend.Create2DImageView(
        image,
        1,
        Format,
        vk::ImageAspectFlagBits::eColor,
        debugName + "View");
    const GeneratedImage result = imageOwner.Get();
    imageOwner.Release();
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
                .setLayerCount(1))
        .setSrcAccessMask(sourceAccess)
        .setDstAccessMask(destinationAccess);
    return barrier;
}

vk::Sampler CreateLookupSampler(
    RendererBackendVulkan& rendererBackend,
    vk::Filter filter,
    const std::string& debugName)
{
    return rendererBackend.Create2DSampler(
        filter,
        vk::SamplerAddressMode::eClampToEdge,
        false,
        debugName);
}

} // namespace

SubsurfaceLookupTableGenerationResult
SubsurfaceLookupTableGenerator::Generate(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend,
    const std::vector<SubsurfaceProfileAsset>& profiles,
    const std::vector<PreintegratedSkinLutAsset>& skinLuts,
    const std::string& sourceDigest)
{
    std::shared_ptr<ComputePipeline> computePipeline =
        pipelineFactory.CreateComputePipelineCandidate(
            "generator/subsurfaceLookupTables");
    GeneratedImageOwner profileImageOwner(rendererBackend);
    profileImageOwner.Adopt(CreateLookupImage(
        rendererBackend,
        SubsurfaceProfileTableWidth,
        SubsurfaceProfileTableHeight,
        "SubsurfaceProfileTable:" + sourceDigest));
    GeneratedImageOwner skinImageOwner(rendererBackend);
    skinImageOwner.Adopt(CreateLookupImage(
        rendererBackend,
        PreintegratedSkinLutWidth,
        PreintegratedSkinLutTableHeight,
        "PreintegratedSkinLutTable:" + sourceDigest));

    const GenerationParameters parameters =
        BuildGenerationParameters(profiles, skinLuts);
    // 参数 buffer 只服务本次一次性 Compute dispatch，完成后立即释放。
    GpuBufferOwner parameterBufferOwner(rendererBackend);
    auto [parameterBuffer, parameterMemory] = rendererBackend.CreateBuffer(
            sizeof(parameters),
            vk::BufferUsageFlagBits::eStorageBuffer,
            vk::MemoryPropertyFlagBits::eHostVisible |
                vk::MemoryPropertyFlagBits::eHostCoherent,
            "SubsurfaceLookupGenerationParameters");
    parameterBufferOwner.Adopt(parameterBuffer, parameterMemory);
    void* mappedParameters = rendererBackend.MapMemory(
        parameterBufferOwner.GetMemory(),
        sizeof(parameters));
    std::memcpy(mappedParameters, &parameters, sizeof(parameters));
    rendererBackend.UnmapMemory(parameterBufferOwner.GetMemory());

    const std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize{
            vk::DescriptorType::eStorageImage,
            2},
        vk::DescriptorPoolSize{
            vk::DescriptorType::eStorageBuffer,
            1},
    };
    vk::DescriptorPoolCreateInfo poolCreateInfo;
    poolCreateInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(1);
    DescriptorPoolOwner descriptorPoolOwner(rendererBackend);
    descriptorPoolOwner.Adopt(rendererBackend.CreateDescriptorPool(
        poolCreateInfo,
        "DescriptorPool: SubsurfaceLookupGeneration"));

    const vk::DescriptorSetLayout setLayout =
        computePipeline->GetDescriptorSetLayouts()[0];
    vk::DescriptorSetAllocateInfo allocateInfo;
    allocateInfo
        .setDescriptorPool(descriptorPoolOwner.Get())
        .setSetLayouts(setLayout);
    std::vector<vk::DescriptorSet> descriptorSets(1);
    rendererBackend.AllocateDescriptorSets(
        allocateInfo,
        descriptorSets);
    const vk::DescriptorSet descriptorSet = descriptorSets[0];

    const vk::DescriptorImageInfo profileImageInfo{
        vk::Sampler(),
        profileImageOwner.Get().view,
        vk::ImageLayout::eGeneral};
    const vk::DescriptorImageInfo skinImageInfo{
        vk::Sampler(),
        skinImageOwner.Get().view,
        vk::ImageLayout::eGeneral};
    const vk::DescriptorBufferInfo parameterBufferInfo{
        parameterBufferOwner.GetBuffer(),
        0,
        sizeof(parameters)};

    std::array<vk::WriteDescriptorSet, 3> writes;
    writes[0]
        .setDstSet(descriptorSet)
        .setDstBinding(0)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageImage)
        .setImageInfo(profileImageInfo);
    writes[1]
        .setDstSet(descriptorSet)
        .setDstBinding(1)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageImage)
        .setImageInfo(skinImageInfo);
    writes[2]
        .setDstSet(descriptorSet)
        .setDstBinding(2)
        .setDescriptorCount(1)
        .setDescriptorType(vk::DescriptorType::eStorageBuffer)
        .setBufferInfo(parameterBufferInfo);
    rendererBackend.UpdateDescriptorSets(
        std::vector<vk::WriteDescriptorSet>(
            writes.begin(),
            writes.end()));

    vk::CommandBuffer commandBuffer =
        rendererBackend.BeginSingleTimeCommands();
    try
    {
        // Compute 写入前显式把两张新图从 undefined 转成 general。
        const std::array<vk::ImageMemoryBarrier, 2> toGeneral = {
            BuildImageBarrier(
                profileImageOwner.Get().image,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eGeneral,
                vk::AccessFlagBits::eNone,
                vk::AccessFlagBits::eShaderWrite),
            BuildImageBarrier(
                skinImageOwner.Get().image,
                vk::ImageLayout::eUndefined,
                vk::ImageLayout::eGeneral,
                vk::AccessFlagBits::eNone,
                vk::AccessFlagBits::eShaderWrite),
        };
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTopOfPipe,
            vk::PipelineStageFlagBits::eComputeShader,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            toGeneral);

        // 一个覆盖 128x1024 atlas 的 dispatch 同时生成较小的 14x256 profile table。
        computePipeline->Bind(commandBuffer);
        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eCompute,
            computePipeline->GetPipelineLayout(),
            0,
            descriptorSet,
            nullptr);
        computePipeline->Dispatch(
            commandBuffer,
            (PreintegratedSkinLutWidth + 7) / 8,
            (PreintegratedSkinLutTableHeight + 7) / 8,
            1);

        // 写入完成后统一转成 fragment shader 可采样布局，禁止材质看到半生成状态。
        const std::array<vk::ImageMemoryBarrier, 2> toSample = {
            BuildImageBarrier(
                profileImageOwner.Get().image,
                vk::ImageLayout::eGeneral,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::AccessFlagBits::eShaderWrite,
                vk::AccessFlagBits::eShaderRead),
            BuildImageBarrier(
                skinImageOwner.Get().image,
                vk::ImageLayout::eGeneral,
                vk::ImageLayout::eShaderReadOnlyOptimal,
                vk::AccessFlagBits::eShaderWrite,
                vk::AccessFlagBits::eShaderRead),
        };
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

    constexpr vk::Format Format =
        vk::Format::eR16G16B16A16Sfloat;
    // image/memory/view 的所有权转交 Texture，随 World-local 资源包和 GPU epoch 退休。
    SubsurfaceLookupTableGenerationResult result;
    SamplerOwner profileSamplerOwner(rendererBackend);
    profileSamplerOwner.Adopt(CreateLookupSampler(
        rendererBackend,
        vk::Filter::eNearest,
        "SubsurfaceProfileTableSampler"));
    result.profileTableTexture = std::make_shared<Texture>(
        rendererBackend,
        profileImageOwner.Get().image,
        profileImageOwner.Get().memory,
        profileImageOwner.Get().view,
        profileSamplerOwner.Get(),
        1,
        Format);
    profileImageOwner.Release();
    profileSamplerOwner.Release();

    SamplerOwner skinSamplerOwner(rendererBackend);
    skinSamplerOwner.Adopt(CreateLookupSampler(
        rendererBackend,
        vk::Filter::eLinear,
        "PreintegratedSkinLutTableSampler"));
    result.skinLutTableTexture = std::make_shared<Texture>(
        rendererBackend,
        skinImageOwner.Get().image,
        skinImageOwner.Get().memory,
        skinImageOwner.Get().view,
        skinSamplerOwner.Get(),
        1,
        Format);
    skinImageOwner.Release();
    skinSamplerOwner.Release();
    return result;
}

} // namespace VL
