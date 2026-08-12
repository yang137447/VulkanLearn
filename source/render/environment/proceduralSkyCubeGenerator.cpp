#include "render/environment/proceduralSkyCubeGenerator.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <tuple>

#include "pipeline/computePipeline.h"
#include "pipeline/pipelineFactory.h"
#include "render/backend/rendererBackendVulkan.h"
#include "texture.h"
#include "vulkanDebug.h"

namespace
{

constexpr uint32_t kCubemapFaceCount = 6;

struct alignas(16) SkyCubeDispatchParams
{
    SkyParametersGPU skyParameters;
    std::array<int32_t, 4> updateControl{};
};

static_assert(
    offsetof(SkyCubeDispatchParams, updateControl) == sizeof(SkyParametersGPU),
    "SkyCubeDispatchParams must match the std140 shader layout.");
static_assert(
    sizeof(SkyCubeDispatchParams) == 128,
    "SkyCubeDispatchParams must match the GLSL uniform block size.");

vk::ImageSubresourceRange BuildCubeRange()
{
    vk::ImageSubresourceRange range;
    range
        .setAspectMask(vk::ImageAspectFlagBits::eColor)
        .setBaseMipLevel(0)
        .setLevelCount(1)
        .setBaseArrayLayer(0)
        .setLayerCount(kCubemapFaceCount);
    return range;
}

vk::PipelineStageFlags GetPendingSourceStage(vk::ImageLayout layout)
{
    switch (layout)
    {
    case vk::ImageLayout::eUndefined:
        return vk::PipelineStageFlagBits::eTopOfPipe;
    case vk::ImageLayout::eGeneral:
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return vk::PipelineStageFlagBits::eComputeShader;
    case vk::ImageLayout::eTransferSrcOptimal:
    case vk::ImageLayout::eTransferDstOptimal:
        return vk::PipelineStageFlagBits::eTransfer;
    default:
        throw std::runtime_error("Unsupported procedural sky pending cube layout.");
    }
}

vk::AccessFlags GetPendingSourceAccess(vk::ImageLayout layout)
{
    switch (layout)
    {
    case vk::ImageLayout::eUndefined:
        return vk::AccessFlagBits::eNone;
    case vk::ImageLayout::eGeneral:
        return vk::AccessFlagBits::eShaderWrite;
    case vk::ImageLayout::eShaderReadOnlyOptimal:
        return vk::AccessFlagBits::eShaderRead;
    case vk::ImageLayout::eTransferSrcOptimal:
        return vk::AccessFlagBits::eTransferRead;
    case vk::ImageLayout::eTransferDstOptimal:
        return vk::AccessFlagBits::eTransferWrite;
    default:
        throw std::runtime_error("Unsupported procedural sky pending cube access state.");
    }
}

} // namespace

namespace VL
{

void ProceduralSkyCubeGenerator::Initialize(
    PipelineFactory& pipelineFactory,
    RendererBackendVulkan& rendererBackend)
{
    if (initialized)
    {
        return;
    }

    skyToCubemapPipeline = pipelineFactory.CreateComputePipeline("generator/skyToCubemap");
    CreateSkyCubeResources(rendererBackend);
    CreateDescriptorResources(rendererBackend);

    this->rendererBackend = &rendererBackend;
    initialized = true;
}

void ProceduralSkyCubeGenerator::Shutdown(RendererBackendVulkan& rendererBackend)
{
    if (!initialized)
    {
        return;
    }

    DestroyDescriptorResources(rendererBackend);
    DestroySkyCubeResources(rendererBackend);
    skyToCubemapPipeline.reset();

    this->rendererBackend = nullptr;
    initialized = false;
}

void ProceduralSkyCubeGenerator::RecordFace(
    vk::CommandBuffer commandBuffer,
    uint32_t swapchainImageIndex,
    const SkyParametersGPU& skyParameters,
    uint32_t faceIndex)
{
    if (!initialized)
    {
        return;
    }
    if (faceIndex >= kCubemapFaceCount)
    {
        throw std::runtime_error("Procedural sky cubemap face record parameters are invalid.");
    }

    // 同一帧可能消耗多个 face 预算。每个 image/face 必须使用独立参数 UBO，
    // 否则提交前的多次 memcpy 会让所有 dispatch 最终都读取最后一个 faceIndex。
    const size_t descriptorIndex =
        static_cast<size_t>(swapchainImageIndex) * kCubemapFaceCount + faceIndex;
    if (descriptorIndex >= skyToCubemapDescriptorSets.size() ||
        descriptorIndex >= dispatchParamBuffers.size())
    {
        throw std::runtime_error("Procedural sky cubemap descriptor index is invalid.");
    }

    VulkanDebug::ScopedRegion debugRegion(
        commandBuffer,
        "Environment:CubemapFace" + std::to_string(faceIndex),
        VulkanDebug::DebugCategory::eResource);

    if (faceIndex == 0)
    {
        // 新代际总是从 face 0 开始。若上一代际中途被替换，这个 barrier
        // 会把旧的 compute read/write 或 commit copy 排在本次覆盖写之前。
        PreparePendingCubeForCompute(commandBuffer);
    }

    SkyCubeDispatchParams params;
    params.skyParameters = skyParameters;
    params.updateControl[0] = static_cast<int32_t>(faceIndex);
    std::memcpy(
        dispatchParamBuffers[descriptorIndex].mapped,
        &params,
        sizeof(params));

    skyToCubemapPipeline->Bind(commandBuffer);
    commandBuffer.bindDescriptorSets(
        vk::PipelineBindPoint::eCompute,
        skyToCubemapPipeline->GetPipelineLayout(),
        0,
        skyToCubemapDescriptorSets[descriptorIndex],
        nullptr);
    skyToCubemapPipeline->Dispatch(
        commandBuffer,
        (pendingCube.size + 7) / 8,
        (pendingCube.size + 7) / 8,
        1);

    if (faceIndex + 1 == kCubemapFaceCount)
    {
        FinalizePendingCubeForSampling(commandBuffer);
    }
}

void ProceduralSkyCubeGenerator::RecordCommit(vk::CommandBuffer commandBuffer)
{
    if (!initialized)
    {
        return;
    }
    if (pendingCube.layout != vk::ImageLayout::eShaderReadOnlyOptimal ||
        activeCube.layout != vk::ImageLayout::eShaderReadOnlyOptimal)
    {
        throw std::runtime_error("Procedural sky cubemap commit requires readable active and pending images.");
    }

    VulkanDebug::ScopedRegion debugRegion(
        commandBuffer,
        "Environment:CubemapCommit",
        VulkanDebug::DebugCategory::eResource);

    const vk::ImageSubresourceRange cubeRange = BuildCubeRange();
    std::array<vk::ImageMemoryBarrier, 2> prepareCopyBarriers;
    prepareCopyBarriers[0]
        .setOldLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(pendingCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eTransferRead);
    prepareCopyBarriers[1]
        .setOldLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activeCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eShaderRead)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);

    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        prepareCopyBarriers);

    vk::ImageCopy copyRegion;
    copyRegion
        .setSrcSubresource(vk::ImageSubresourceLayers(
            vk::ImageAspectFlagBits::eColor,
            0,
            0,
            kCubemapFaceCount))
        .setDstSubresource(vk::ImageSubresourceLayers(
            vk::ImageAspectFlagBits::eColor,
            0,
            0,
            kCubemapFaceCount))
        .setExtent(vk::Extent3D{ activeCube.size, activeCube.size, 1 });
    commandBuffer.copyImage(
        pendingCube.texture->getImage(),
        vk::ImageLayout::eTransferSrcOptimal,
        activeCube.texture->getImage(),
        vk::ImageLayout::eTransferDstOptimal,
        copyRegion);

    vk::ImageMemoryBarrier activeToSampleBarrier;
    activeToSampleBarrier
        .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activeCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        activeToSampleBarrier);

    pendingCube.layout = vk::ImageLayout::eTransferSrcOptimal;
    activeCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

std::shared_ptr<Texture> ProceduralSkyCubeGenerator::GetActiveEnvironmentCube() const
{
    if (!activeCube.texture)
    {
        throw std::runtime_error("Procedural sky active cubemap is not initialized.");
    }
    return activeCube.texture;
}

std::shared_ptr<Texture> ProceduralSkyCubeGenerator::GetPendingEnvironmentCube() const
{
    if (!pendingCube.texture)
    {
        throw std::runtime_error("Procedural sky pending cubemap is not initialized.");
    }
    return pendingCube.texture;
}

void ProceduralSkyCubeGenerator::CreateSkyCubeResources(
    RendererBackendVulkan& rendererBackend)
{
    CreateSkyCubeResource(
        rendererBackend,
        activeCube,
        false,
        "ProceduralSkyActiveCube");
    CreateSkyCubeResource(
        rendererBackend,
        pendingCube,
        true,
        "ProceduralSkyPendingCube");
    InitializeActiveCube(rendererBackend);
}

void ProceduralSkyCubeGenerator::CreateSkyCubeResource(
    RendererBackendVulkan& rendererBackend,
    SkyCubeResources& resources,
    bool createStorageView,
    const std::string& debugName)
{
    vk::ImageCreateInfo imageInfo;
    imageInfo
        .setFlags(vk::ImageCreateFlagBits::eCubeCompatible)
        .setImageType(vk::ImageType::e2D)
        .setExtent(vk::Extent3D{ resources.size, resources.size, 1 })
        .setMipLevels(1)
        .setArrayLayers(kCubemapFaceCount)
        .setFormat(resources.format)
        .setTiling(vk::ImageTiling::eOptimal)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setUsage(
            vk::ImageUsageFlagBits::eStorage |
            vk::ImageUsageFlagBits::eSampled |
            vk::ImageUsageFlagBits::eTransferSrc |
            vk::ImageUsageFlagBits::eTransferDst)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setSamples(vk::SampleCountFlagBits::e1);

    vk::Image image;
    vk::DeviceMemory memory;
    std::tie(image, memory) = rendererBackend.CreateImage(
        imageInfo,
        vk::MemoryPropertyFlagBits::eDeviceLocal,
        debugName + "Image");

    if (createStorageView)
    {
        resources.storageView = rendererBackend.CreateCubeStorageImageView(
            image,
            resources.format,
            debugName + "StorageView");
    }

    vk::ImageView sampleView = rendererBackend.CreateCubeImageView(
        image,
        1,
        resources.format,
        debugName + "SampleView");
    vk::Sampler sampler = rendererBackend.CreateCubeSampler(
        0.0f,
        debugName + "Sampler");
    resources.texture = std::make_shared<Texture>(
        rendererBackend,
        image,
        memory,
        sampleView,
        sampler,
        1,
        resources.format);
    resources.layout = vk::ImageLayout::eUndefined;
}

void ProceduralSkyCubeGenerator::InitializeActiveCube(
    RendererBackendVulkan& rendererBackend)
{
    vk::CommandBuffer commandBuffer = rendererBackend.BeginSingleTimeCommands();
    const vk::ImageSubresourceRange cubeRange = BuildCubeRange();

    vk::ImageMemoryBarrier toClearBarrier;
    toClearBarrier
        .setOldLayout(vk::ImageLayout::eUndefined)
        .setNewLayout(vk::ImageLayout::eTransferDstOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activeCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eNone)
        .setDstAccessMask(vk::AccessFlagBits::eTransferWrite);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTopOfPipe,
        vk::PipelineStageFlagBits::eTransfer,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        toClearBarrier);

    commandBuffer.clearColorImage(
        activeCube.texture->getImage(),
        vk::ImageLayout::eTransferDstOptimal,
        vk::ClearColorValue(std::array<float, 4>{ 0.0f, 0.0f, 0.0f, 1.0f }),
        cubeRange);

    vk::ImageMemoryBarrier toSampleBarrier;
    toSampleBarrier
        .setOldLayout(vk::ImageLayout::eTransferDstOptimal)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(activeCube.texture->getImage())
        .setSubresourceRange(cubeRange)
        .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eTransfer,
        vk::PipelineStageFlagBits::eComputeShader |
            vk::PipelineStageFlagBits::eFragmentShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        toSampleBarrier);

    rendererBackend.EndSingleTimeCommands(commandBuffer);
    activeCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

void ProceduralSkyCubeGenerator::DestroySkyCubeResources(
    RendererBackendVulkan& rendererBackend)
{
    DestroySkyCubeResource(rendererBackend, pendingCube);
    DestroySkyCubeResource(rendererBackend, activeCube);
}

void ProceduralSkyCubeGenerator::DestroySkyCubeResource(
    RendererBackendVulkan& rendererBackend,
    SkyCubeResources& resources)
{
    rendererBackend.DestroyImageView(resources.storageView);
    resources.texture.reset();
    resources.layout = vk::ImageLayout::eUndefined;
}

void ProceduralSkyCubeGenerator::CreateDescriptorResources(
    RendererBackendVulkan& rendererBackend)
{
    const uint32_t swapchainImageCount = rendererBackend.GetSwapchainImageCount();
    const uint32_t descriptorSetCount = swapchainImageCount * kCubemapFaceCount;
    std::array<vk::DescriptorPoolSize, 2> poolSizes = {
        vk::DescriptorPoolSize{ vk::DescriptorType::eUniformBuffer, descriptorSetCount },
        vk::DescriptorPoolSize{ vk::DescriptorType::eStorageImage, descriptorSetCount },
    };

    vk::DescriptorPoolCreateInfo poolCreateInfo;
    poolCreateInfo
        .setPoolSizes(poolSizes)
        .setMaxSets(descriptorSetCount);
    descriptorPool = rendererBackend.CreateDescriptorPool(
        poolCreateInfo,
        "DescriptorPool: ProceduralSkyCube");

    std::vector<vk::DescriptorSetLayout> layouts(
        descriptorSetCount,
        skyToCubemapPipeline->GetDescriptorSetLayouts()[0]);
    skyToCubemapDescriptorSets.resize(descriptorSetCount);

    vk::DescriptorSetAllocateInfo allocInfo;
    allocInfo
        .setDescriptorPool(descriptorPool)
        .setSetLayouts(layouts);
    rendererBackend.AllocateDescriptorSets(allocInfo, skyToCubemapDescriptorSets);

    dispatchParamBuffers.resize(descriptorSetCount);
    for (uint32_t imageIndex = 0; imageIndex < swapchainImageCount; ++imageIndex)
    {
        for (uint32_t faceIndex = 0; faceIndex < kCubemapFaceCount; ++faceIndex)
        {
            const size_t descriptorIndex =
                static_cast<size_t>(imageIndex) * kCubemapFaceCount + faceIndex;
            auto [buffer, memory] = rendererBackend.CreateBuffer(
                sizeof(SkyCubeDispatchParams),
                vk::BufferUsageFlagBits::eUniformBuffer,
                vk::MemoryPropertyFlagBits::eHostVisible |
                    vk::MemoryPropertyFlagBits::eHostCoherent,
                "ProceduralSkyCubeParams_Image" + std::to_string(imageIndex) +
                    "_Face" + std::to_string(faceIndex));
            dispatchParamBuffers[descriptorIndex] = BufferResource{
                buffer,
                memory,
                rendererBackend.MapMemory(memory, sizeof(SkyCubeDispatchParams))
            };

            vk::DescriptorBufferInfo paramInfo;
            paramInfo
                .setBuffer(buffer)
                .setOffset(0)
                .setRange(sizeof(SkyCubeDispatchParams));
            vk::DescriptorImageInfo storageInfo;
            storageInfo
                .setImageView(pendingCube.storageView)
                .setImageLayout(vk::ImageLayout::eGeneral);

            std::array<vk::WriteDescriptorSet, 2> writes;
            writes[0]
                .setDstSet(skyToCubemapDescriptorSets[descriptorIndex])
                .setDstBinding(0)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eUniformBuffer)
                .setBufferInfo(paramInfo);
            writes[1]
                .setDstSet(skyToCubemapDescriptorSets[descriptorIndex])
                .setDstBinding(1)
                .setDescriptorCount(1)
                .setDescriptorType(vk::DescriptorType::eStorageImage)
                .setImageInfo(storageInfo);
            rendererBackend.UpdateDescriptorSets(
                std::vector<vk::WriteDescriptorSet>(writes.begin(), writes.end()));
        }
    }
}

void ProceduralSkyCubeGenerator::DestroyDescriptorResources(
    RendererBackendVulkan& rendererBackend)
{
    for (BufferResource& params : dispatchParamBuffers)
    {
        rendererBackend.UnmapMemory(params.memory);
        rendererBackend.DestroyBuffer(params.buffer, params.memory);
    }
    dispatchParamBuffers.clear();
    skyToCubemapDescriptorSets.clear();
    rendererBackend.DestroyDescriptorPool(descriptorPool);
}

void ProceduralSkyCubeGenerator::PreparePendingCubeForCompute(
    vk::CommandBuffer commandBuffer)
{
    const vk::ImageLayout oldLayout = pendingCube.layout;
    vk::ImageMemoryBarrier barrier;
    barrier
        .setOldLayout(oldLayout)
        .setNewLayout(vk::ImageLayout::eGeneral)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(pendingCube.texture->getImage())
        .setSubresourceRange(BuildCubeRange())
        .setSrcAccessMask(GetPendingSourceAccess(oldLayout))
        .setDstAccessMask(vk::AccessFlagBits::eShaderWrite);
    commandBuffer.pipelineBarrier(
        GetPendingSourceStage(oldLayout),
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        barrier);
    pendingCube.layout = vk::ImageLayout::eGeneral;
}

void ProceduralSkyCubeGenerator::FinalizePendingCubeForSampling(
    vk::CommandBuffer commandBuffer)
{
    vk::ImageMemoryBarrier barrier;
    barrier
        .setOldLayout(vk::ImageLayout::eGeneral)
        .setNewLayout(vk::ImageLayout::eShaderReadOnlyOptimal)
        .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
        .setImage(pendingCube.texture->getImage())
        .setSubresourceRange(BuildCubeRange())
        .setSrcAccessMask(vk::AccessFlagBits::eShaderWrite)
        .setDstAccessMask(vk::AccessFlagBits::eShaderRead);
    commandBuffer.pipelineBarrier(
        vk::PipelineStageFlagBits::eComputeShader,
        vk::PipelineStageFlagBits::eComputeShader,
        vk::DependencyFlags(),
        nullptr,
        nullptr,
        barrier);
    pendingCube.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
}

} // namespace VL
