#include "render/backend/rendererBackendVulkan.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>

#include "pipeline/pipelineFactory.h"
#include "profiler.h"
#include "render/rhi/vulkan/rhiDeviceVulkan.h"
#include "render/resource/resourceRetireQueue.h"

namespace VL
{
namespace
{

struct RendererSubmitPlan
{
    uint32_t frameIndex = 0;
    uint32_t swapchainImageIndex = 0;
    vk::CommandBuffer commandBuffer;
    vk::Semaphore imageAcquiredSemaphore;
    vk::Semaphore renderFinishedSemaphore;
    vk::Fence taskFinishedFence;
    vk::PipelineStageFlags waitDstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    uint64_t submittedEpoch = 0;
};

template <typename HandleType>
uint64_t ToRawHandleValue(HandleType handle)
{
    if constexpr (std::is_pointer_v<HandleType>)
    {
        return reinterpret_cast<uint64_t>(handle);
    }
    else
    {
        return static_cast<uint64_t>(handle);
    }
}

template <typename HandleType>
std::string FormatRawHandle(HandleType handle)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << ToRawHandleValue(handle);
    return stream.str();
}

template <typename HandleType>
std::string BuildUnknownRawHandleMessage(const char* resourceType, HandleType handle)
{
    return std::string("RendererBackendVulkan received an unknown ") +
        resourceType +
        " raw handle " +
        FormatRawHandle(handle) +
        ". The resource should be created or registered through RendererBackendVulkan before use.";
}

} // namespace

RendererBackendVulkan::RendererBackendVulkan()
    : rhiDevice(std::make_unique<RHIDeviceVulkan>())
{
}

RendererBackendVulkan::~RendererBackendVulkan() = default;

void RendererBackendVulkan::Initialize(
    std::vector<const char*>& instanceExtensions,
    SDL_Window* window)
{
    rhiDevice->Initialize(instanceExtensions, window);
    frameFenceEpochs.assign(rhiDevice->GetFrameFenceCount(), 0);
    initialized = true;
}

void RendererBackendVulkan::WaitIdle()
{
    rhiDevice->WaitIdle();
}

RendererBackendResourceIdentityCounts
RendererBackendVulkan::CaptureResourceIdentityCounts() const noexcept
{
    RendererBackendResourceIdentityCounts counts;
    counts.buffers = bufferHandlesByBuffer.size();
    counts.images = imageHandlesByImage.size();
    counts.imageViews = imageViewHandlesByImageView.size();
    counts.samplers = samplerHandlesBySampler.size();
    counts.descriptorSetLayouts =
        descriptorSetLayoutHandlesByLayout.size();
    counts.descriptorPools = descriptorPoolHandlesByPool.size();
    counts.descriptorSets = descriptorSetHandlesBySet.size();
    counts.renderPasses = renderPassHandlesByRenderPass.size();
    counts.framebuffers = framebufferHandlesByFramebuffer.size();
    return counts;
}

RendererBackendImageResourceDebugNames
RendererBackendVulkan::CaptureImageResourceDebugNames() const
{
    RendererBackendImageResourceDebugNames names;
    names.images.reserve(imageDebugNamesByImage.size());
    names.imageViews.reserve(imageViewDebugNamesByImageView.size());
    names.samplers.reserve(samplerDebugNamesBySampler.size());
    for (const auto& [image, debugName] : imageDebugNamesByImage)
    {
        (void)image;
        names.images.push_back(debugName);
    }
    for (const auto& [imageView, debugName] : imageViewDebugNamesByImageView)
    {
        (void)imageView;
        names.imageViews.push_back(debugName);
    }
    for (const auto& [sampler, debugName] : samplerDebugNamesBySampler)
    {
        (void)sampler;
        names.samplers.push_back(debugName);
    }
    std::sort(names.images.begin(), names.images.end());
    std::sort(names.imageViews.begin(), names.imageViews.end());
    std::sort(names.samplers.begin(), names.samplers.end());
    return names;
}

void RendererBackendVulkan::RecreateSwapchain(int width, int height)
{
    rhiDevice->RecreateSwapchain(width, height);
    frameFenceEpochs.assign(rhiDevice->GetFrameFenceCount(), 0);
}

std::unique_ptr<PipelineFactory> RendererBackendVulkan::CreatePipelineFactory()
{
    return std::make_unique<PipelineFactory>(this);
}

uint32_t RendererBackendVulkan::GetSwapchainImageCount() const
{
    return rhiDevice->GetSwapchainImageCount();
}

vk::Extent2D RendererBackendVulkan::GetSwapchainExtent() const
{
    return rhiDevice->GetSwapchainExtent();
}

vk::Format RendererBackendVulkan::GetSwapchainImageFormat() const
{
    return rhiDevice->GetSwapchainImageFormat();
}

const std::vector<vk::ImageView>& RendererBackendVulkan::GetSwapchainImageViews() const
{
    return rhiDevice->GetSwapchainImageViews();
}

vk::Device& RendererBackendVulkan::GetDevice()
{
    return rhiDevice->GetDevice();
}

const vk::Device& RendererBackendVulkan::GetDevice() const
{
    return rhiDevice->GetDevice();
}

float RendererBackendVulkan::GetTimestampPeriodNanoseconds()
{
    return rhiDevice->GetTimestampPeriodNanoseconds();
}

uint32_t RendererBackendVulkan::GetGraphicsTimestampValidBits()
{
    return rhiDevice->GetGraphicsTimestampValidBits();
}

vk::QueryPool RendererBackendVulkan::CreateTimestampQueryPool(
    uint32_t queryCount,
    const std::string& debugName)
{
    return rhiDevice->CreateTimestampQueryPool(queryCount, debugName);
}

void RendererBackendVulkan::DestroyQueryPool(vk::QueryPool& queryPool)
{
    rhiDevice->DestroyQueryPool(queryPool);
}

void RendererBackendVulkan::ReadTimestampQueryPair(
    vk::QueryPool queryPool,
    uint32_t firstQuery,
    std::array<uint64_t, 2>& timestamps)
{
    rhiDevice->ReadTimestampQueryPair(queryPool, firstQuery, timestamps);
}

RHIBufferHandle RendererBackendVulkan::RequireBufferHandle(vk::Buffer buffer) const
{
    auto handleIt = bufferHandlesByBuffer.find(static_cast<VkBuffer>(buffer));
    if (!buffer || handleIt == bufferHandlesByBuffer.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage("buffer", static_cast<VkBuffer>(buffer)));
    }

    return handleIt->second;
}

RHIBufferHandle RendererBackendVulkan::RequireBufferMemoryHandle(vk::DeviceMemory memory) const
{
    auto handleIt = bufferHandlesByMemory.find(static_cast<VkDeviceMemory>(memory));
    if (!memory || handleIt == bufferHandlesByMemory.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "buffer memory",
            static_cast<VkDeviceMemory>(memory)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindBufferHandle(RHIBufferHandle bufferHandle)
{
    const VulkanBufferResource& bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
    bufferHandlesByBuffer[static_cast<VkBuffer>(bufferResource.buffer)] = bufferHandle;
    bufferHandlesByMemory[static_cast<VkDeviceMemory>(bufferResource.memory)] = bufferHandle;
}

void RendererBackendVulkan::UnbindBufferHandle(RHIBufferHandle bufferHandle)
{
    const VulkanBufferResource& bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
    bufferHandlesByBuffer.erase(static_cast<VkBuffer>(bufferResource.buffer));
    bufferHandlesByMemory.erase(static_cast<VkDeviceMemory>(bufferResource.memory));
}

std::pair<vk::Buffer, vk::DeviceMemory> RendererBackendVulkan::CreateBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    RHIBufferHandle bufferHandle = rhiDevice->CreateBuffer(size, usage, memoryPropertyFlags, debugName);
    BindBufferHandle(bufferHandle);
    const VulkanBufferResource& bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
    return { bufferResource.buffer, bufferResource.memory };
}

void* RendererBackendVulkan::MapMemory(vk::DeviceMemory memory, vk::DeviceSize size)
{
    return rhiDevice->MapBufferMemory(RequireBufferMemoryHandle(memory), size);
}

void RendererBackendVulkan::UnmapMemory(vk::DeviceMemory memory)
{
    rhiDevice->UnmapBufferMemory(RequireBufferMemoryHandle(memory));
}

void RendererBackendVulkan::DestroyBuffer(vk::Buffer buffer, vk::DeviceMemory memory)
{
    if (!buffer && !memory)
    {
        return;
    }

    RHIBufferHandle bufferHandle = RequireBufferHandle(buffer);
    UnbindBufferHandle(bufferHandle);
    rhiDevice->DestroyBuffer(bufferHandle);
}

void RendererBackendVulkan::CopyBufferToBuffer(
    vk::Buffer source,
    vk::Buffer destination,
    vk::DeviceSize size)
{
    rhiDevice->CopyBufferToBuffer(
        RequireBufferHandle(source),
        RequireBufferHandle(destination),
        size);
}

void RendererBackendVulkan::CreatePerSwapchainBufferSet(
    Buffer& bufferSet,
    vk::DeviceSize bufferSize,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugNamePrefix)
{
    if (bufferSize == 0)
    {
        throw std::invalid_argument(
            "RendererBackendVulkan cannot create a zero-sized buffer set: " +
            debugNamePrefix);
    }

    if (!bufferSet.buffers.empty())
    {
        DestroyBufferSet(bufferSet);
    }

    const uint32_t swapchainImageCount = GetSwapchainImageCount();
    bufferSet.bufferHandles.resize(swapchainImageCount);
    bufferSet.buffers.resize(swapchainImageCount);
    bufferSet.bufferMemories.resize(swapchainImageCount);
    bufferSet.buffersMapped.resize(swapchainImageCount);
    bufferSet.bufferSize = static_cast<uint32_t>(bufferSize);

    for (uint32_t i = 0; i < swapchainImageCount; i++)
    {
        RHIBufferHandle bufferHandle = rhiDevice->CreateBuffer(
            bufferSize,
            usage,
            memoryPropertyFlags,
            debugNamePrefix + " (SwapchainIndex " + std::to_string(i) + ")");
        BindBufferHandle(bufferHandle);

        const VulkanBufferResource& bufferResource = rhiDevice->GetVulkanBufferResource(bufferHandle);
        bufferSet.bufferHandles[i] = bufferHandle;
        bufferSet.buffers[i] = bufferResource.buffer;
        bufferSet.bufferMemories[i] = bufferResource.memory;
        bufferSet.buffersMapped[i] = rhiDevice->MapBufferMemory(bufferHandle, bufferSize);
    }

    SetupDescriptorBufferInfos(bufferSet);
}

void RendererBackendVulkan::DestroyBufferSet(Buffer& bufferSet)
{
    const size_t bufferCount = bufferSet.bufferHandles.size() > bufferSet.buffers.size()
        ? bufferSet.bufferHandles.size()
        : bufferSet.buffers.size();
    for (size_t i = 0; i < bufferCount; i++)
    {
        RHIBufferHandle bufferHandle;
        if (i < bufferSet.bufferHandles.size())
        {
            bufferHandle = bufferSet.bufferHandles[i];
        }
        if (!bufferHandle.IsValid() && i < bufferSet.buffers.size() && bufferSet.buffers[i])
        {
            bufferHandle = RequireBufferHandle(bufferSet.buffers[i]);
        }
        if (!bufferHandle.IsValid())
        {
            continue;
        }

        if (i < bufferSet.buffersMapped.size() && bufferSet.buffersMapped[i] != nullptr)
        {
            rhiDevice->UnmapBufferMemory(bufferHandle);
            bufferSet.buffersMapped[i] = nullptr;
        }
        UnbindBufferHandle(bufferHandle);
        rhiDevice->DestroyBuffer(bufferHandle);
    }

    bufferSet.bufferHandles.clear();
    bufferSet.buffers.clear();
    bufferSet.bufferMemories.clear();
    bufferSet.buffersMapped.clear();
    bufferSet.bufferInfos.clear();
    bufferSet.bufferSize = 0;
}

void RendererBackendVulkan::SetupDescriptorBufferInfos(Buffer& bufferSet)
{
    bufferSet.bufferInfos.resize(bufferSet.buffers.size());
    for (size_t i = 0; i < bufferSet.buffers.size(); i++)
    {
        bufferSet.bufferInfos[i]
            .setBuffer(bufferSet.buffers[i])
            .setOffset(0)
            .setRange(bufferSet.bufferSize);
    }
}

RHIImageHandle RendererBackendVulkan::RequireImageHandle(vk::Image image) const
{
    auto handleIt = imageHandlesByImage.find(static_cast<VkImage>(image));
    if (!image || handleIt == imageHandlesByImage.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage("image", static_cast<VkImage>(image)));
    }

    return handleIt->second;
}

RHIImageHandle RendererBackendVulkan::RequireImageMemoryHandle(vk::DeviceMemory memory) const
{
    auto handleIt = imageHandlesByMemory.find(static_cast<VkDeviceMemory>(memory));
    if (!memory || handleIt == imageHandlesByMemory.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "image memory",
            static_cast<VkDeviceMemory>(memory)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindImageHandle(RHIImageHandle imageHandle)
{
    const VulkanImageResource& imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    imageHandlesByImage[static_cast<VkImage>(imageResource.image)] = imageHandle;
    imageHandlesByMemory[static_cast<VkDeviceMemory>(imageResource.memory)] = imageHandle;
}

void RendererBackendVulkan::UnbindImageHandle(RHIImageHandle imageHandle)
{
    const VulkanImageResource& imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    imageHandlesByImage.erase(static_cast<VkImage>(imageResource.image));
    imageHandlesByMemory.erase(static_cast<VkDeviceMemory>(imageResource.memory));
}

RHIImageViewHandle RendererBackendVulkan::RequireImageViewHandle(vk::ImageView imageView) const
{
    auto handleIt = imageViewHandlesByImageView.find(static_cast<VkImageView>(imageView));
    if (!imageView || handleIt == imageViewHandlesByImageView.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "image view",
            static_cast<VkImageView>(imageView)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindImageViewHandle(RHIImageViewHandle imageViewHandle)
{
    const VulkanImageViewResource& imageViewResource =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle);
    imageViewHandlesByImageView[static_cast<VkImageView>(imageViewResource.imageView)] = imageViewHandle;
}

void RendererBackendVulkan::UnbindImageViewHandle(RHIImageViewHandle imageViewHandle)
{
    const VulkanImageViewResource& imageViewResource =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle);
    imageViewHandlesByImageView.erase(static_cast<VkImageView>(imageViewResource.imageView));
}

RHISamplerHandle RendererBackendVulkan::RequireSamplerHandle(vk::Sampler sampler) const
{
    auto handleIt = samplerHandlesBySampler.find(static_cast<VkSampler>(sampler));
    if (!sampler || handleIt == samplerHandlesBySampler.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage("sampler", static_cast<VkSampler>(sampler)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindSamplerHandle(RHISamplerHandle samplerHandle)
{
    const VulkanSamplerResource& samplerResource = rhiDevice->GetVulkanSamplerResource(samplerHandle);
    samplerHandlesBySampler[static_cast<VkSampler>(samplerResource.sampler)] = samplerHandle;
}

void RendererBackendVulkan::UnbindSamplerHandle(RHISamplerHandle samplerHandle)
{
    const VulkanSamplerResource& samplerResource = rhiDevice->GetVulkanSamplerResource(samplerHandle);
    samplerHandlesBySampler.erase(static_cast<VkSampler>(samplerResource.sampler));
}

RHIDescriptorSetLayoutHandle RendererBackendVulkan::RequireDescriptorSetLayoutHandle(
    vk::DescriptorSetLayout descriptorSetLayout) const
{
    auto handleIt = descriptorSetLayoutHandlesByLayout.find(
        static_cast<VkDescriptorSetLayout>(descriptorSetLayout));
    if (!descriptorSetLayout || handleIt == descriptorSetLayoutHandlesByLayout.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "descriptor set layout",
            static_cast<VkDescriptorSetLayout>(descriptorSetLayout)));
    }

    return handleIt->second;
}

RHIDescriptorSetLayoutHandle RendererBackendVulkan::ResolveDescriptorSetLayoutHandle(
    vk::DescriptorSetLayout descriptorSetLayout)
{
    auto handleIt = descriptorSetLayoutHandlesByLayout.find(
        static_cast<VkDescriptorSetLayout>(descriptorSetLayout));
    if (handleIt != descriptorSetLayoutHandlesByLayout.end())
    {
        return handleIt->second;
    }

    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle =
        rhiDevice->RegisterDescriptorSetLayout(descriptorSetLayout, false);
    BindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    return descriptorSetLayoutHandle;
}

void RendererBackendVulkan::BindDescriptorSetLayoutHandle(
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle)
{
    const VulkanDescriptorSetLayoutResource& descriptorSetLayoutResource =
        rhiDevice->GetVulkanDescriptorSetLayoutResource(descriptorSetLayoutHandle);
    descriptorSetLayoutHandlesByLayout[
        static_cast<VkDescriptorSetLayout>(descriptorSetLayoutResource.descriptorSetLayout)] =
            descriptorSetLayoutHandle;
}

void RendererBackendVulkan::UnbindDescriptorSetLayoutHandle(
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle)
{
    const VulkanDescriptorSetLayoutResource& descriptorSetLayoutResource =
        rhiDevice->GetVulkanDescriptorSetLayoutResource(descriptorSetLayoutHandle);
    descriptorSetLayoutHandlesByLayout.erase(
        static_cast<VkDescriptorSetLayout>(descriptorSetLayoutResource.descriptorSetLayout));
}

RHIDescriptorPoolHandle RendererBackendVulkan::RequireDescriptorPoolHandle(
    vk::DescriptorPool descriptorPool) const
{
    auto handleIt = descriptorPoolHandlesByPool.find(static_cast<VkDescriptorPool>(descriptorPool));
    if (!descriptorPool || handleIt == descriptorPoolHandlesByPool.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "descriptor pool",
            static_cast<VkDescriptorPool>(descriptorPool)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindDescriptorPoolHandle(RHIDescriptorPoolHandle descriptorPoolHandle)
{
    const VulkanDescriptorPoolResource& descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle);
    descriptorPoolHandlesByPool[static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool)] =
        descriptorPoolHandle;
}

void RendererBackendVulkan::UnbindDescriptorPoolHandle(RHIDescriptorPoolHandle descriptorPoolHandle)
{
    const VulkanDescriptorPoolResource& descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle);
    VkDescriptorPool rawDescriptorPool =
        static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool);
    auto setHandlesIt = descriptorSetHandlesByPool.find(rawDescriptorPool);
    if (setHandlesIt != descriptorSetHandlesByPool.end())
    {
        for (RHIDescriptorSetHandle descriptorSetHandle : setHandlesIt->second)
        {
            const VulkanDescriptorSetResource& descriptorSetResource =
                rhiDevice->GetVulkanDescriptorSetResource(descriptorSetHandle);
            descriptorSetHandlesBySet.erase(
                static_cast<VkDescriptorSet>(descriptorSetResource.descriptorSet));
        }
        descriptorSetHandlesByPool.erase(setHandlesIt);
    }
    descriptorPoolHandlesByPool.erase(rawDescriptorPool);
}

RHIDescriptorSetHandle RendererBackendVulkan::RequireDescriptorSetHandle(
    vk::DescriptorSet descriptorSet) const
{
    auto handleIt = descriptorSetHandlesBySet.find(static_cast<VkDescriptorSet>(descriptorSet));
    if (!descriptorSet || handleIt == descriptorSetHandlesBySet.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "descriptor set",
            static_cast<VkDescriptorSet>(descriptorSet)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindDescriptorSetHandle(
    RHIDescriptorSetHandle descriptorSetHandle,
    RHIDescriptorPoolHandle descriptorPoolHandle)
{
    const VulkanDescriptorSetResource& descriptorSetResource =
        rhiDevice->GetVulkanDescriptorSetResource(descriptorSetHandle);
    const VulkanDescriptorPoolResource& descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle);
    VkDescriptorPool rawDescriptorPool =
        static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool);
    descriptorSetHandlesBySet[static_cast<VkDescriptorSet>(descriptorSetResource.descriptorSet)] =
        descriptorSetHandle;
    descriptorSetHandlesByPool[rawDescriptorPool].push_back(descriptorSetHandle);
}

void RendererBackendVulkan::UnbindDescriptorSetHandle(RHIDescriptorSetHandle descriptorSetHandle)
{
    const VulkanDescriptorSetResource& descriptorSetResource =
        rhiDevice->GetVulkanDescriptorSetResource(descriptorSetHandle);
    const VulkanDescriptorPoolResource& descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorSetResource.descriptorPoolHandle);
    VkDescriptorPool rawDescriptorPool =
        static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool);
    descriptorSetHandlesBySet.erase(static_cast<VkDescriptorSet>(descriptorSetResource.descriptorSet));

    auto setHandlesIt = descriptorSetHandlesByPool.find(rawDescriptorPool);
    if (setHandlesIt != descriptorSetHandlesByPool.end())
    {
        auto& descriptorSetHandles = setHandlesIt->second;
        for (auto handleIt = descriptorSetHandles.begin(); handleIt != descriptorSetHandles.end();)
        {
            if (handleIt->id == descriptorSetHandle.id)
            {
                handleIt = descriptorSetHandles.erase(handleIt);
            }
            else
            {
                ++handleIt;
            }
        }
        if (descriptorSetHandles.empty())
        {
            descriptorSetHandlesByPool.erase(setHandlesIt);
        }
    }
}

std::vector<RHIDescriptorWrite> RendererBackendVulkan::BuildDescriptorWrites(
    const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets) const
{
    std::vector<RHIDescriptorWrite> rhiWrites;
    rhiWrites.reserve(writeDescriptorSets.size());
    for (const vk::WriteDescriptorSet& writeDescriptorSet : writeDescriptorSets)
    {
        RHIDescriptorWrite rhiWrite;
        rhiWrite.destinationSet = RequireDescriptorSetHandle(writeDescriptorSet.dstSet);
        rhiWrite.destinationBinding = writeDescriptorSet.dstBinding;
        rhiWrite.destinationArrayElement = writeDescriptorSet.dstArrayElement;
        rhiWrite.descriptorCount = writeDescriptorSet.descriptorCount;
        rhiWrite.descriptorType = writeDescriptorSet.descriptorType;
        if (writeDescriptorSet.pImageInfo != nullptr)
        {
            rhiWrite.imageInfos.assign(
                writeDescriptorSet.pImageInfo,
                writeDescriptorSet.pImageInfo + writeDescriptorSet.descriptorCount);
        }
        if (writeDescriptorSet.pBufferInfo != nullptr)
        {
            rhiWrite.bufferInfos.assign(
                writeDescriptorSet.pBufferInfo,
                writeDescriptorSet.pBufferInfo + writeDescriptorSet.descriptorCount);
        }
        if (writeDescriptorSet.pTexelBufferView != nullptr)
        {
            rhiWrite.texelBufferViews.assign(
                writeDescriptorSet.pTexelBufferView,
                writeDescriptorSet.pTexelBufferView + writeDescriptorSet.descriptorCount);
        }
        rhiWrites.push_back(std::move(rhiWrite));
    }
    return rhiWrites;
}

RHIRenderPassHandle RendererBackendVulkan::RequireRenderPassHandle(vk::RenderPass renderPass) const
{
    auto handleIt = renderPassHandlesByRenderPass.find(static_cast<VkRenderPass>(renderPass));
    if (!renderPass || handleIt == renderPassHandlesByRenderPass.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "render pass",
            static_cast<VkRenderPass>(renderPass)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindRenderPassHandle(RHIRenderPassHandle renderPassHandle)
{
    const VulkanRenderPassResource& renderPassResource =
        rhiDevice->GetVulkanRenderPassResource(renderPassHandle);
    renderPassHandlesByRenderPass[static_cast<VkRenderPass>(renderPassResource.renderPass)] =
        renderPassHandle;
}

void RendererBackendVulkan::UnbindRenderPassHandle(RHIRenderPassHandle renderPassHandle)
{
    const VulkanRenderPassResource& renderPassResource =
        rhiDevice->GetVulkanRenderPassResource(renderPassHandle);
    renderPassHandlesByRenderPass.erase(static_cast<VkRenderPass>(renderPassResource.renderPass));
}

RHIFramebufferHandle RendererBackendVulkan::RequireFramebufferHandle(vk::Framebuffer framebuffer) const
{
    auto handleIt = framebufferHandlesByFramebuffer.find(static_cast<VkFramebuffer>(framebuffer));
    if (!framebuffer || handleIt == framebufferHandlesByFramebuffer.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "framebuffer",
            static_cast<VkFramebuffer>(framebuffer)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindFramebufferHandle(RHIFramebufferHandle framebufferHandle)
{
    const VulkanFramebufferResource& framebufferResource =
        rhiDevice->GetVulkanFramebufferResource(framebufferHandle);
    framebufferHandlesByFramebuffer[static_cast<VkFramebuffer>(framebufferResource.framebuffer)] =
        framebufferHandle;
}

void RendererBackendVulkan::UnbindFramebufferHandle(RHIFramebufferHandle framebufferHandle)
{
    const VulkanFramebufferResource& framebufferResource =
        rhiDevice->GetVulkanFramebufferResource(framebufferHandle);
    framebufferHandlesByFramebuffer.erase(static_cast<VkFramebuffer>(framebufferResource.framebuffer));
}

std::pair<vk::Image, vk::DeviceMemory> RendererBackendVulkan::CreateImage(
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    vk::SampleCountFlagBits samples,
    vk::Format format,
    vk::ImageTiling tiling,
    vk::ImageUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    RHIImageHandle imageHandle = rhiDevice->CreateImage(
        width,
        height,
        mipLevels,
        samples,
        format,
        tiling,
        usage,
        memoryPropertyFlags,
        debugName);
    BindImageHandle(imageHandle);
    const VulkanImageResource& imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    imageDebugNamesByImage[static_cast<VkImage>(imageResource.image)] =
        debugName;
    return { imageResource.image, imageResource.memory };
}

std::pair<vk::Image, vk::DeviceMemory> RendererBackendVulkan::CreateImage(
    const vk::ImageCreateInfo& createInfo,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    RHIImageHandle imageHandle = rhiDevice->CreateImage(createInfo, memoryPropertyFlags, debugName);
    BindImageHandle(imageHandle);
    const VulkanImageResource& imageResource = rhiDevice->GetVulkanImageResource(imageHandle);
    imageDebugNamesByImage[static_cast<VkImage>(imageResource.image)] =
        debugName;
    return { imageResource.image, imageResource.memory };
}

void RendererBackendVulkan::TransitionImageLayout(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout)
{
    rhiDevice->TransitionImageLayout(RequireImageHandle(image), mipLevels, format, oldLayout, newLayout);
}

vk::ImageView RendererBackendVulkan::Create2DImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageAspectFlagBits aspectMask,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle =
        rhiDevice->Create2DImageView(RequireImageHandle(image), mipLevels, format, aspectMask, debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
        debugName;
    return imageView;
}

vk::ImageView RendererBackendVulkan::CreateImageView(
    vk::Image image,
    vk::ImageViewType viewType,
    vk::Format format,
    vk::ImageAspectFlagBits aspectMask,
    uint32_t baseMipLevel,
    uint32_t mipLevelCount,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle = rhiDevice->CreateImageView(
        RequireImageHandle(image),
        viewType,
        format,
        aspectMask,
        baseMipLevel,
        mipLevelCount,
        baseArrayLayer,
        layerCount,
        debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
        debugName;
    return imageView;
}

vk::ImageView RendererBackendVulkan::CreateCubeImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle =
        rhiDevice->CreateCubeImageView(RequireImageHandle(image), mipLevels, format, debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
        debugName;
    return imageView;
}

vk::ImageView RendererBackendVulkan::CreateCubeStorageImageView(
    vk::Image image,
    vk::Format format,
    const std::string& debugName)
{
    RHIImageViewHandle imageViewHandle =
        rhiDevice->CreateCubeStorageImageView(RequireImageHandle(image), format, debugName);
    BindImageViewHandle(imageViewHandle);
    const vk::ImageView imageView =
        rhiDevice->GetVulkanImageViewResource(imageViewHandle).imageView;
    imageViewDebugNamesByImageView[static_cast<VkImageView>(imageView)] =
        debugName;
    return imageView;
}

void RendererBackendVulkan::DestroyImageView(vk::ImageView& imageView)
{
    if (!imageView)
    {
        return;
    }

    RHIImageViewHandle imageViewHandle = RequireImageViewHandle(imageView);
    imageViewDebugNamesByImageView.erase(
        static_cast<VkImageView>(imageView));
    UnbindImageViewHandle(imageViewHandle);
    rhiDevice->DestroyImageView(imageViewHandle);
    imageView = nullptr;
}

vk::Sampler RendererBackendVulkan::Create2DSampler(const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->Create2DSampler(debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
        debugName;
    return sampler;
}

vk::Sampler RendererBackendVulkan::Create2DSampler(
    vk::Filter filter,
    vk::SamplerAddressMode addressMode,
    bool enableMipmaps,
    const std::string& debugName)
{
    RHISamplerHandle samplerHandle =
        rhiDevice->Create2DSampler(filter, addressMode, enableMipmaps, debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
        debugName;
    return sampler;
}

vk::Sampler RendererBackendVulkan::CreateSampler(
    const vk::SamplerCreateInfo& createInfo,
    const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateSampler(createInfo, debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
        debugName;
    return sampler;
}

vk::Sampler RendererBackendVulkan::CreateCubeSampler(
    float maxLod,
    const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateCubeSampler(maxLod, debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
        debugName;
    return sampler;
}

void RendererBackendVulkan::DestroySampler(vk::Sampler& sampler)
{
    if (!sampler)
    {
        return;
    }

    RHISamplerHandle samplerHandle = RequireSamplerHandle(sampler);
    samplerDebugNamesBySampler.erase(
        static_cast<VkSampler>(sampler));
    UnbindSamplerHandle(samplerHandle);
    rhiDevice->DestroySampler(samplerHandle);
    sampler = nullptr;
}

vk::Sampler RendererBackendVulkan::CreateDepthSampler(const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateDepthSampler(debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
        debugName;
    return sampler;
}

vk::Sampler RendererBackendVulkan::CreateDepthCompareSampler(const std::string& debugName)
{
    RHISamplerHandle samplerHandle = rhiDevice->CreateDepthCompareSampler(debugName);
    BindSamplerHandle(samplerHandle);
    const vk::Sampler sampler =
        rhiDevice->GetVulkanSamplerResource(samplerHandle).sampler;
    samplerDebugNamesBySampler[static_cast<VkSampler>(sampler)] =
        debugName;
    return sampler;
}

vk::CommandBuffer RendererBackendVulkan::BeginSingleTimeCommands()
{
    return rhiDevice->BeginSingleTimeCommands();
}

void RendererBackendVulkan::EndSingleTimeCommands(vk::CommandBuffer& commandBuffer)
{
    rhiDevice->EndSingleTimeCommands(commandBuffer);
}

void RendererBackendVulkan::CopyBufferToImage(
    vk::Buffer buffer,
    vk::Image image,
    uint32_t width,
    uint32_t height)
{
    rhiDevice->CopyBufferToImage(RequireBufferHandle(buffer), RequireImageHandle(image), width, height);
}

void RendererBackendVulkan::CopyImageToBuffer(
    vk::Image image,
    vk::Buffer buffer,
    uint32_t width,
    uint32_t height,
    bool flipY,
    vk::DeviceSize rowBytes)
{
    rhiDevice->CopyImageToBuffer(
        RequireImageHandle(image),
        RequireBufferHandle(buffer),
        width,
        height,
        flipY,
        rowBytes);
}

void RendererBackendVulkan::GenerateMipmaps(
    vk::Image image,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels)
{
    rhiDevice->GenerateMipmaps(RequireImageHandle(image), width, height, mipLevels);
}

RHIImageHandle RendererBackendVulkan::GetImageHandle(vk::Image image) const
{
    return RequireImageHandle(image);
}

RHIImageViewHandle RendererBackendVulkan::GetImageViewHandle(vk::ImageView imageView) const
{
    return RequireImageViewHandle(imageView);
}

RHISamplerHandle RendererBackendVulkan::GetSamplerHandle(vk::Sampler sampler) const
{
    return RequireSamplerHandle(sampler);
}

void RendererBackendVulkan::DestroyImageResource(
    vk::Image& image,
    vk::DeviceMemory& memory,
    vk::ImageView& imageView,
    vk::Sampler& sampler)
{
    if (!image && !memory)
    {
        DestroyImageView(imageView);
        DestroySampler(sampler);
        return;
    }

    RHIImageHandle imageHandle = RequireImageHandle(image);
    RHIImageViewHandle imageViewHandle;
    RHISamplerHandle samplerHandle;
    if (imageView)
    {
        imageViewHandle = RequireImageViewHandle(imageView);
        imageViewDebugNamesByImageView.erase(
            static_cast<VkImageView>(imageView));
        UnbindImageViewHandle(imageViewHandle);
    }
    if (sampler)
    {
        samplerHandle = RequireSamplerHandle(sampler);
        samplerDebugNamesBySampler.erase(
            static_cast<VkSampler>(sampler));
        UnbindSamplerHandle(samplerHandle);
    }
    imageDebugNamesByImage.erase(
        static_cast<VkImage>(image));
    UnbindImageHandle(imageHandle);
    rhiDevice->DestroyImageResource(imageHandle, imageViewHandle, samplerHandle);
    image = nullptr;
    memory = nullptr;
    imageView = nullptr;
    sampler = nullptr;
}

void RendererBackendVulkan::DestroyImageResource(
    RHIImageHandle& imageHandle,
    RHIImageViewHandle& imageViewHandle,
    RHISamplerHandle& samplerHandle)
{
    if (!imageHandle.IsValid())
    {
        if (imageViewHandle.IsValid())
        {
            const vk::ImageView imageView =
                rhiDevice->GetVulkanImageViewResource(
                    imageViewHandle).imageView;
            imageViewDebugNamesByImageView.erase(
                static_cast<VkImageView>(imageView));
            UnbindImageViewHandle(imageViewHandle);
            rhiDevice->DestroyImageView(imageViewHandle);
            imageViewHandle = RHIImageViewHandle();
        }
        if (samplerHandle.IsValid())
        {
            const vk::Sampler sampler =
                rhiDevice->GetVulkanSamplerResource(
                    samplerHandle).sampler;
            samplerDebugNamesBySampler.erase(
                static_cast<VkSampler>(sampler));
            UnbindSamplerHandle(samplerHandle);
            rhiDevice->DestroySampler(samplerHandle);
            samplerHandle = RHISamplerHandle();
        }
        return;
    }

    if (imageViewHandle.IsValid())
    {
        const vk::ImageView imageView =
            rhiDevice->GetVulkanImageViewResource(
                imageViewHandle).imageView;
        imageViewDebugNamesByImageView.erase(
            static_cast<VkImageView>(imageView));
        UnbindImageViewHandle(imageViewHandle);
    }
    if (samplerHandle.IsValid())
    {
        const vk::Sampler sampler =
            rhiDevice->GetVulkanSamplerResource(
                samplerHandle).sampler;
        samplerDebugNamesBySampler.erase(
            static_cast<VkSampler>(sampler));
        UnbindSamplerHandle(samplerHandle);
    }
    const vk::Image image =
        rhiDevice->GetVulkanImageResource(
            imageHandle).image;
    imageDebugNamesByImage.erase(
        static_cast<VkImage>(image));
    UnbindImageHandle(imageHandle);
    rhiDevice->DestroyImageResource(imageHandle, imageViewHandle, samplerHandle);

    imageHandle = RHIImageHandle();
    imageViewHandle = RHIImageViewHandle();
    samplerHandle = RHISamplerHandle();
}

vk::DescriptorSetLayout RendererBackendVulkan::CreateDescriptorSetLayout(
    const vk::DescriptorSetLayoutCreateInfo& createInfo,
    const std::string& debugName)
{
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle =
        rhiDevice->CreateDescriptorSetLayout(createInfo, debugName);
    BindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    return rhiDevice->GetVulkanDescriptorSetLayoutResource(
        descriptorSetLayoutHandle).descriptorSetLayout;
}

RHIDescriptorSetLayoutHandle RendererBackendVulkan::GetDescriptorSetLayoutHandle(
    vk::DescriptorSetLayout descriptorSetLayout) const
{
    return RequireDescriptorSetLayoutHandle(descriptorSetLayout);
}

void RendererBackendVulkan::DestroyDescriptorSetLayout(
    vk::DescriptorSetLayout& descriptorSetLayout)
{
    if (!descriptorSetLayout)
    {
        return;
    }

    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle =
        RequireDescriptorSetLayoutHandle(descriptorSetLayout);
    UnbindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    rhiDevice->DestroyDescriptorSetLayout(descriptorSetLayoutHandle);
    descriptorSetLayout = nullptr;
}

void RendererBackendVulkan::DestroyDescriptorSetLayout(
    RHIDescriptorSetLayoutHandle& descriptorSetLayoutHandle)
{
    if (!descriptorSetLayoutHandle.IsValid())
    {
        return;
    }

    UnbindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    rhiDevice->DestroyDescriptorSetLayout(descriptorSetLayoutHandle);
    descriptorSetLayoutHandle = RHIDescriptorSetLayoutHandle();
}

vk::DescriptorPool RendererBackendVulkan::CreateDescriptorPool(
    const vk::DescriptorPoolCreateInfo& createInfo,
    const std::string& debugName)
{
    RHIDescriptorPoolHandle descriptorPoolHandle =
        rhiDevice->CreateDescriptorPool(createInfo, debugName);
    BindDescriptorPoolHandle(descriptorPoolHandle);
    return rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle).descriptorPool;
}

RHIDescriptorPoolHandle RendererBackendVulkan::GetDescriptorPoolHandle(
    vk::DescriptorPool descriptorPool) const
{
    return RequireDescriptorPoolHandle(descriptorPool);
}

void RendererBackendVulkan::DestroyDescriptorPool(vk::DescriptorPool& descriptorPool)
{
    if (!descriptorPool)
    {
        return;
    }

    RHIDescriptorPoolHandle descriptorPoolHandle = RequireDescriptorPoolHandle(descriptorPool);
    UnbindDescriptorPoolHandle(descriptorPoolHandle);
    rhiDevice->DestroyDescriptorPool(descriptorPoolHandle);
    descriptorPool = nullptr;
}

void RendererBackendVulkan::DestroyDescriptorPool(
    RHIDescriptorPoolHandle& descriptorPoolHandle)
{
    if (!descriptorPoolHandle.IsValid())
    {
        return;
    }

    UnbindDescriptorPoolHandle(descriptorPoolHandle);
    rhiDevice->DestroyDescriptorPool(descriptorPoolHandle);
    descriptorPoolHandle = RHIDescriptorPoolHandle();
}

RHIDescriptorSetHandle RendererBackendVulkan::GetDescriptorSetHandle(
    vk::DescriptorSet descriptorSet) const
{
    return RequireDescriptorSetHandle(descriptorSet);
}

void RendererBackendVulkan::AllocateDescriptorSets(
    const vk::DescriptorSetAllocateInfo& allocateInfo,
    std::vector<vk::DescriptorSet>& descriptorSets)
{
    RHIDescriptorPoolHandle descriptorPoolHandle =
        RequireDescriptorPoolHandle(allocateInfo.descriptorPool);
    std::vector<RHIDescriptorSetLayoutHandle> descriptorSetLayoutHandles;
    descriptorSetLayoutHandles.reserve(allocateInfo.descriptorSetCount);
    for (uint32_t i = 0; i < allocateInfo.descriptorSetCount; ++i)
    {
        descriptorSetLayoutHandles.push_back(
            ResolveDescriptorSetLayoutHandle(allocateInfo.pSetLayouts[i]));
    }

    std::vector<RHIDescriptorSetHandle> descriptorSetHandles =
        rhiDevice->AllocateDescriptorSets(descriptorPoolHandle, descriptorSetLayoutHandles);
    descriptorSets.resize(descriptorSetHandles.size());
    for (size_t i = 0; i < descriptorSetHandles.size(); ++i)
    {
        BindDescriptorSetHandle(descriptorSetHandles[i], descriptorPoolHandle);
        descriptorSets[i] =
            rhiDevice->GetVulkanDescriptorSetResource(descriptorSetHandles[i]).descriptorSet;
    }
}

void RendererBackendVulkan::FreeDescriptorSet(
    vk::DescriptorPool descriptorPool,
    vk::DescriptorSet& descriptorSet)
{
    if (!descriptorPool || !descriptorSet)
    {
        return;
    }

    RHIDescriptorPoolHandle descriptorPoolHandle = RequireDescriptorPoolHandle(descriptorPool);
    RHIDescriptorSetHandle descriptorSetHandle = RequireDescriptorSetHandle(descriptorSet);
    UnbindDescriptorSetHandle(descriptorSetHandle);
    rhiDevice->FreeDescriptorSet(descriptorPoolHandle, descriptorSetHandle);
    descriptorSet = nullptr;
}

void RendererBackendVulkan::UpdateDescriptorSets(
    const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets)
{
    rhiDevice->UpdateDescriptorSets(BuildDescriptorWrites(writeDescriptorSets));
}

void RendererBackendVulkan::SetDescriptorSetDebugName(
    vk::DescriptorSet descriptorSet,
    const std::string& debugName)
{
    if (!descriptorSet)
    {
        return;
    }

    rhiDevice->SetDescriptorSetDebugName(RequireDescriptorSetHandle(descriptorSet), debugName);
}

vk::RenderPass RendererBackendVulkan::CreateRenderPass(
    const vk::RenderPassCreateInfo2& createInfo,
    const std::string& debugName)
{
    RHIRenderPassHandle renderPassHandle = rhiDevice->CreateRenderPass(createInfo, debugName);
    BindRenderPassHandle(renderPassHandle);
    return rhiDevice->GetVulkanRenderPassResource(renderPassHandle).renderPass;
}

RHIRenderPassHandle RendererBackendVulkan::GetRenderPassHandle(vk::RenderPass renderPass) const
{
    return RequireRenderPassHandle(renderPass);
}

void RendererBackendVulkan::DestroyRenderPass(vk::RenderPass& renderPass)
{
    if (!renderPass)
    {
        return;
    }

    RHIRenderPassHandle renderPassHandle = RequireRenderPassHandle(renderPass);
    UnbindRenderPassHandle(renderPassHandle);
    rhiDevice->DestroyRenderPass(renderPassHandle);
    renderPass = nullptr;
}

void RendererBackendVulkan::DestroyRenderPass(RHIRenderPassHandle& renderPassHandle)
{
    if (!renderPassHandle.IsValid())
    {
        return;
    }

    UnbindRenderPassHandle(renderPassHandle);
    rhiDevice->DestroyRenderPass(renderPassHandle);
    renderPassHandle = RHIRenderPassHandle();
}

vk::Framebuffer RendererBackendVulkan::CreateFramebuffer(
    const vk::FramebufferCreateInfo& createInfo,
    const std::string& debugName)
{
    RHIFramebufferHandle framebufferHandle = rhiDevice->CreateFramebuffer(
        RequireRenderPassHandle(createInfo.renderPass),
        createInfo,
        debugName);
    BindFramebufferHandle(framebufferHandle);
    return rhiDevice->GetVulkanFramebufferResource(framebufferHandle).framebuffer;
}

RHIFramebufferHandle RendererBackendVulkan::GetFramebufferHandle(vk::Framebuffer framebuffer) const
{
    return RequireFramebufferHandle(framebuffer);
}

void RendererBackendVulkan::DestroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers)
{
    for (vk::Framebuffer& framebuffer : framebuffers)
    {
        if (!framebuffer)
        {
            continue;
        }

        RHIFramebufferHandle framebufferHandle = RequireFramebufferHandle(framebuffer);
        UnbindFramebufferHandle(framebufferHandle);
        rhiDevice->DestroyFramebuffer(framebufferHandle);
        framebuffer = nullptr;
    }
    framebuffers.clear();
}

void RendererBackendVulkan::DestroyFramebuffers(
    std::vector<RHIFramebufferHandle>& framebufferHandles)
{
    for (RHIFramebufferHandle& framebufferHandle : framebufferHandles)
    {
        if (!framebufferHandle.IsValid())
        {
            continue;
        }

        UnbindFramebufferHandle(framebufferHandle);
        rhiDevice->DestroyFramebuffer(framebufferHandle);
        framebufferHandle = RHIFramebufferHandle();
    }
    framebufferHandles.clear();
}

RendererFrameContext RendererBackendVulkan::BeginFrame(uint32_t currentFrame)
{
    if (!initialized)
    {
        throw std::runtime_error("RendererBackendVulkan is not initialized");
    }

    RendererFrameContext frameContext;
    frameContext.swapchainImageCount = rhiDevice->GetSwapchainImageCount();
    const uint32_t frameFenceCount = rhiDevice->GetFrameFenceCount();
    if (frameFenceCount == 0)
    {
        throw std::runtime_error("RendererBackendVulkan has no frame fences");
    }
    frameContext.frameIndex = currentFrame % frameFenceCount;

    const VulkanFrameSyncResources frameSync =
        rhiDevice->GetFrameSyncResources(frameContext.frameIndex);

    {
        PROFILE_SCOPE("Render:WaitFence");
        rhiDevice->WaitForFence(frameSync.taskFinishedFence);
    }

    if (frameContext.frameIndex < frameFenceEpochs.size() &&
        frameFenceEpochs[frameContext.frameIndex] != 0)
    {
        ResourceRetireQueue::GetInstance().CollectCompletedEpoch(
            frameFenceEpochs[frameContext.frameIndex]);
        frameFenceEpochs[frameContext.frameIndex] = 0;
    }

    {
        PROFILE_SCOPE("Render:AcquireImage");
        rhiDevice->AcquireNextSwapchainImage(
            frameSync.imageAcquiredSemaphore,
            frameContext.swapchainImageIndex);
    }

    const vk::Fence imageInFlightFence =
        rhiDevice->GetImageInFlightFence(frameContext.swapchainImageIndex);
    if (imageInFlightFence)
    {
        rhiDevice->WaitForFence(imageInFlightFence);
    }
    rhiDevice->SetImageInFlightFence(
        frameContext.swapchainImageIndex,
        frameSync.taskFinishedFence);

    rhiDevice->ResetFence(frameSync.taskFinishedFence);

    const VulkanSwapchainImageSyncResources imageSync =
        rhiDevice->GetSwapchainImageSyncResources(frameContext.swapchainImageIndex);
    frameContext.commandBuffer = imageSync.commandBuffer;
    frameContext.commandBuffer.reset();

    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    frameContext.commandBuffer.begin(beginInfo);
    return frameContext;
}

void RendererBackendVulkan::SubmitFrame(
    const RendererFrameContext& frameContext,
    uint32_t currentFrame)
{
    if (!initialized)
    {
        throw std::runtime_error("RendererBackendVulkan is not initialized");
    }

    // Submit synchronization handles are backend-local details. RenderSystem
    // records commands into the frame context, then hands control back here so
    // queue, semaphore, fence, swapchain, and retire epoch ownership does not
    // leak into the pass recording layer.
    //
    // The two indices intentionally point at different resource rings:
    //
    //   frameIndex          -> frame-in-flight sync ring
    //       imageAcquiredSemaphore[frameIndex]  : acquire-complete signal waited by submit
    //       taskFinishedFence[frameIndex]       : GPU-complete fence before reusing this frame slot
    //       frameFenceEpochs[frameIndex]        : retire epoch released after that fence completes
    //
    //   swapchainImageIndex -> swapchain-image resource ring
    //       commandBuffers[swapchainImageIndex] : commands recorded for the acquired image
    //       renderFinishedSemaphore[image]      : render-complete signal waited by present
    //       framebuffer / descriptor / UBO      : resources addressed by the acquired image
    //
    // Submit waits for acquire(frameIndex), signals render-finished(image),
    // and signals the frame fence(frameIndex). Present then waits for the
    // render-finished semaphore that belongs to the acquired swapchain image.
    //
    //   acquire image
    //        |
    //        v
    //   wait imageAcquiredSemaphore[frameIndex]
    //        |
    //        v
    //   submit commandBuffer[swapchainImageIndex]
    //        | \
    //        |  `-> signal taskFinishedFence[frameIndex]
    //        v
    //   signal renderFinishedSemaphore[swapchainImageIndex]
    //        |
    //        v
    //   present swapchainImageIndex
    RendererSubmitPlan submitPlan;
    submitPlan.frameIndex = frameContext.frameIndex;
    submitPlan.swapchainImageIndex = frameContext.swapchainImageIndex;
    submitPlan.commandBuffer = frameContext.commandBuffer;
    const VulkanFrameSyncResources frameSync =
        rhiDevice->GetFrameSyncResources(frameContext.frameIndex);
    const VulkanSwapchainImageSyncResources imageSync =
        rhiDevice->GetSwapchainImageSyncResources(frameContext.swapchainImageIndex);
    submitPlan.imageAcquiredSemaphore = frameSync.imageAcquiredSemaphore;
    submitPlan.renderFinishedSemaphore = imageSync.renderFinishedSemaphore;
    submitPlan.taskFinishedFence = frameSync.taskFinishedFence;
    submitPlan.waitDstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    submitPlan.submittedEpoch = static_cast<uint64_t>(currentFrame) + 1;

    vk::CommandBuffer commandBuffer = submitPlan.commandBuffer;
    commandBuffer.end();
    {
        PROFILE_SCOPE("Render:Submit");
        vk::Semaphore imageAcquiredSemaphore = submitPlan.imageAcquiredSemaphore;
        vk::Semaphore renderFinishedSemaphore = submitPlan.renderFinishedSemaphore;
        vk::PipelineStageFlags waitDstStageMask = submitPlan.waitDstStageMask;
        vk::SubmitInfo submitInfo;
        submitInfo
            .setWaitSemaphores(imageAcquiredSemaphore)
            .setSignalSemaphores(renderFinishedSemaphore)
            .setWaitDstStageMask(waitDstStageMask)
            .setCommandBuffers(commandBuffer);

        rhiDevice->SubmitToGraphicsQueue(submitInfo, submitPlan.taskFinishedFence);
    }

    if (submitPlan.frameIndex < frameFenceEpochs.size())
    {
        frameFenceEpochs[submitPlan.frameIndex] = submitPlan.submittedEpoch;
    }
    ResourceRetireQueue::GetInstance().MarkFrameSubmitted(submitPlan.submittedEpoch);

    {
        PROFILE_SCOPE("Render:Present");
        rhiDevice->PresentSwapchainImage(
            submitPlan.renderFinishedSemaphore,
            submitPlan.swapchainImageIndex);
    }
}

} // namespace VL
