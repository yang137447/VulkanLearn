#include "render/rhi/vulkan/rhiDeviceVulkan.h"

#include <algorithm>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string>

#include "commonFunction.h"
#include "pipeline/pipelineFactory.h"
#include "vulkanDebug.h"
#include "vulkanManager.h"

namespace VL
{

namespace
{

template<typename Handle>
std::string BuildInvalidLifecycleHandleMessage(const char* resourceType, Handle handle)
{
    return std::string("Invalid Vulkan ") +
        resourceType +
        " lifecycle handle: id=" +
        std::to_string(handle.id);
}

void RequireFrameSyncIndex(const char* resourceName, uint32_t index, size_t size)
{
    if (index < size)
    {
        return;
    }

    throw std::runtime_error(
        std::string("RHIDeviceVulkan frame sync resource index out of range: ") +
        resourceName +
        ", index=" +
        std::to_string(index) +
        ", count=" +
        std::to_string(size));
}

} // namespace

void RHIDeviceVulkan::Initialize(
    std::vector<const char*>& instanceExtensions,
    SDL_Window* window,
    VL::PresentModePreference presentMode)
{
    VulkanManager::GetInstance().SetPresentModePreference(presentMode);
    VulkanManager::GetInstance().Init(instanceExtensions, window);
    initialized = true;
}

void RHIDeviceVulkan::WaitIdle()
{
    if (!initialized)
    {
        return;
    }

    VulkanManager::GetInstance().GetSubmissionGate().WaitIdle(GetDevice());
}

void RHIDeviceVulkan::RecreateSwapchain(VL::PresentTarget& target, int width, int height)
{
    if (!initialized)
    {
        return;
    }

    // 目标 0 之外的呈现目标尚未接入：目前只有主窗口。
    (void)target;
    VulkanManager::GetInstance().ReCreateSwapChain(width, height);
}

void RHIDeviceVulkan::WaitForFence(vk::Fence fence)
{
    const vk::Result result = GetDevice().waitForFences(fence, true, UINT64_MAX);
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to wait for fence");
    }
}

void RHIDeviceVulkan::ResetFence(vk::Fence fence)
{
    GetDevice().resetFences(fence);
}

SwapchainFrameResult RHIDeviceVulkan::AcquireNextSwapchainImage(
    VL::PresentTarget& target,
    vk::Semaphore imageAcquiredSemaphore,
    uint32_t& swapchainImageIndex)
{
    try
    {
        return ClassifySwapchainFrameResult(GetDevice().acquireNextImageKHR(
            target.swapChain,
            UINT64_MAX,
            imageAcquiredSemaphore,
            nullptr,
            &swapchainImageIndex));
    }
    catch (const vk::OutOfDateKHRError&)
    {
        return SwapchainFrameResult::OutOfDate;
    }
}

void RHIDeviceVulkan::SubmitToGraphicsQueue(
    const vk::SubmitInfo& submitInfo,
    vk::Fence signalFence)
{
    VulkanManager::GetInstance().GetSubmissionGate().Submit(
        GetGraphicsQueue(), submitInfo, signalFence);
}

SwapchainFrameResult RHIDeviceVulkan::PresentSwapchainImage(
    VL::PresentTarget& target,
    vk::Semaphore renderFinishedSemaphore,
    uint32_t swapchainImageIndex)
{
    vk::SwapchainKHR swapchain = target.swapChain;
    vk::PresentInfoKHR presentInfo;
    presentInfo
        .setSwapchains(swapchain)
        .setImageIndices(swapchainImageIndex)
        .setWaitSemaphores(renderFinishedSemaphore);

    try
    {
        return ClassifySwapchainFrameResult(
            VulkanManager::GetInstance().GetSubmissionGate().Present(
                GetGraphicsQueue(), presentInfo));
    }
    catch (const vk::OutOfDateKHRError&)
    {
        return SwapchainFrameResult::OutOfDate;
    }
}

vk::Device& RHIDeviceVulkan::GetDevice()
{
    return VulkanManager::GetInstance().GetDevice();
}

float RHIDeviceVulkan::GetTimestampPeriodNanoseconds()
{
    return GetPhysicalDevice().getProperties().limits.timestampPeriod;
}

uint32_t RHIDeviceVulkan::GetGraphicsTimestampValidBits()
{
    return VulkanManager::GetInstance().GetGraphicsQueueTimestampValidBits();
}

bool RHIDeviceVulkan::SupportsTimestampQueries()
{
    // timestampComputeAndGraphics 决定 graphics/compute 队列能否写 timestamp；
    // 具体队列的 timestampValidBits 仍由 GetGraphicsTimestampValidBits 单独校验。
    return GetPhysicalDevice().getProperties().limits.timestampComputeAndGraphics != 0;
}

bool RHIDeviceVulkan::IsDualSourceBlendEnabled() const
{
    // 由 VulkanManager 返回创建逻辑设备时实际启用的状态，避免误用仅物理支持的结果。
    return VulkanManager::GetInstance().IsDualSourceBlendEnabled();
}

bool RHIDeviceVulkan::IsDrawIndirectCountEnabled() const
{
    return VulkanManager::GetInstance().IsDrawIndirectCountEnabled();
}

vk::QueryPool RHIDeviceVulkan::CreateTimestampQueryPool(
    uint32_t queryCount,
    const std::string& debugName)
{
    if (queryCount == 0)
    {
        throw std::runtime_error("Timestamp query pool must contain at least one query.");
    }

    vk::QueryPoolCreateInfo createInfo;
    createInfo
        .setQueryType(vk::QueryType::eTimestamp)
        .setQueryCount(queryCount);
    vk::QueryPool queryPool = GetDevice().createQueryPool(createInfo);
    VulkanDebug::SetObjectName(
        GetDevice(),
        queryPool,
        vk::ObjectType::eQueryPool,
        debugName);
    return queryPool;
}

void RHIDeviceVulkan::DestroyQueryPool(vk::QueryPool& queryPool)
{
    if (!queryPool)
    {
        return;
    }

    GetDevice().destroyQueryPool(queryPool);
    queryPool = nullptr;
}

void RHIDeviceVulkan::ReadTimestampQueryPair(
    vk::QueryPool queryPool,
    uint32_t firstQuery,
    std::array<uint64_t, 2>& timestamps)
{
    const VkResult result = vkGetQueryPoolResults(
        static_cast<VkDevice>(GetDevice()),
        static_cast<VkQueryPool>(queryPool),
        firstQuery,
        static_cast<uint32_t>(timestamps.size()),
        sizeof(timestamps),
        timestamps.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY)
    {
        throw std::runtime_error(
            "Vulkan timestamp query was not ready after its swapchain image fence completed.");
    }
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to read Vulkan timestamp query results.");
    }
}

bool RHIDeviceVulkan::TryReadTimestampQueryPair(
    vk::QueryPool queryPool,
    uint32_t firstQuery,
    std::array<uint64_t, 2>& timestamps)
{
    // 刻意不传 VK_QUERY_RESULT_WAIT_BIT：统计路径绝不允许阻塞渲染线程。
    const VkResult result = vkGetQueryPoolResults(
        static_cast<VkDevice>(GetDevice()),
        static_cast<VkQueryPool>(queryPool),
        firstQuery,
        static_cast<uint32_t>(timestamps.size()),
        sizeof(timestamps),
        timestamps.data(),
        sizeof(uint64_t),
        VK_QUERY_RESULT_64_BIT);
    if (result == VK_NOT_READY)
    {
        return false;
    }
    if (result != VK_SUCCESS)
    {
        throw std::runtime_error("Failed to read Vulkan timestamp query results.");
    }
    return true;
}

vk::PhysicalDevice& RHIDeviceVulkan::GetPhysicalDevice()
{
    return VulkanManager::GetInstance().GetPhysicalDevice();
}

vk::PhysicalDeviceMemoryProperties& RHIDeviceVulkan::GetGpuMemoryProperties()
{
    return VulkanManager::GetInstance().GetGpuMemoryProperties();
}

vk::Queue& RHIDeviceVulkan::GetGraphicsQueue()
{
    return VulkanManager::GetInstance().GetGraphicsQueue();
}

vk::CommandPool& RHIDeviceVulkan::GetCommandPool()
{
    return VulkanManager::GetInstance().GetCommandPool();
}

vk::CommandPool& RHIDeviceVulkan::GetSingleTimeCommandPool()
{
    return VulkanManager::GetInstance().GetSingleTimeCommandPool();
}

// 目标 0 = 主窗口。多窗口接线时这里改为按后端所属窗口解析，而调用点无需再变 ——
// 这正是把"无参的 GetSwapchain"换成"带身份的目标访问"的目的。
VL::PresentTarget& RHIDeviceVulkan::GetPresentTarget()
{
    return VulkanManager::GetInstance().GetPresentTarget(0);
}

uint32_t RHIDeviceVulkan::GetFrameFenceCount() const
{
    return static_cast<uint32_t>(VulkanManager::GetInstance().GetTaskFinishedFences().size());
}

VulkanFrameSyncResources RHIDeviceVulkan::GetFrameSyncResources(uint32_t frameIndex) const
{
    auto& taskFinishedFences = VulkanManager::GetInstance().GetTaskFinishedFences();
    auto& imageAcquiredSemaphores = VulkanManager::GetInstance().GetImageAcquiredSemaphores();

    RequireFrameSyncIndex("taskFinishedFences", frameIndex, taskFinishedFences.size());
    RequireFrameSyncIndex("imageAcquiredSemaphores", frameIndex, imageAcquiredSemaphores.size());

    VulkanFrameSyncResources resources;
    resources.taskFinishedFence = taskFinishedFences[frameIndex];
    resources.imageAcquiredSemaphore = imageAcquiredSemaphores[frameIndex];
    return resources;
}

VulkanSwapchainImageSyncResources RHIDeviceVulkan::GetSwapchainImageSyncResources(
    const VL::PresentTarget& target, uint32_t swapchainImageIndex) const
{
    // 两个索引都按**该目标**的图像数校验：命令缓冲与 renderFinished 信号量都随目标走。
    RequireFrameSyncIndex("commandBuffers", swapchainImageIndex, target.commandBuffers.size());
    RequireFrameSyncIndex("renderFinishedSemaphores", swapchainImageIndex,
        target.renderFinishedSemaphores.size());

    VulkanSwapchainImageSyncResources resources;
    resources.commandBuffer = target.commandBuffers[swapchainImageIndex];
    resources.renderFinishedSemaphore = target.renderFinishedSemaphores[swapchainImageIndex];
    return resources;
}

vk::Fence RHIDeviceVulkan::GetImageInFlightFence(
    const VL::PresentTarget& target, uint32_t swapchainImageIndex) const
{
    const auto& imagesInFlightFences = target.imagesInFlightFences;
    RequireFrameSyncIndex("imagesInFlightFences", swapchainImageIndex, imagesInFlightFences.size());
    return imagesInFlightFences[swapchainImageIndex];
}

void RHIDeviceVulkan::SetImageInFlightFence(
    VL::PresentTarget& target, uint32_t swapchainImageIndex, vk::Fence fence)
{
    auto& imagesInFlightFences = target.imagesInFlightFences;
    RequireFrameSyncIndex("imagesInFlightFences", swapchainImageIndex, imagesInFlightFences.size());
    imagesInFlightFences[swapchainImageIndex] = fence;
}

uint32_t RHIDeviceVulkan::GetSwapchainImageCount(const VL::PresentTarget& target) const
{
    return target.imageCount;
}

vk::Extent2D RHIDeviceVulkan::GetSwapchainExtent(const VL::PresentTarget& target) const
{
    return target.extent;
}

vk::Format RHIDeviceVulkan::GetSwapchainImageFormat(const VL::PresentTarget& target) const
{
    return target.surfaceFormat.format;
}

const std::vector<vk::ImageView>& RHIDeviceVulkan::GetSwapchainImageViews(
    const VL::PresentTarget& target) const
{
    return target.imageViews;
}

const std::vector<vk::Image>& RHIDeviceVulkan::GetSwapchainImages(
    const VL::PresentTarget& target) const
{
    return target.images;
}

std::vector<vk::Fence>& RHIDeviceVulkan::GetTaskFinishedFences()
{
    return VulkanManager::GetInstance().GetTaskFinishedFences();
}

std::vector<vk::Semaphore>& RHIDeviceVulkan::GetImageAcquiredSemaphores()
{
    return VulkanManager::GetInstance().GetImageAcquiredSemaphores();
}

RHIBufferHandle RHIDeviceVulkan::CreateBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    auto [buffer, memory] = CommonFunction::CreateBuffer(
        GetDevice(),
        size,
        usage,
        GetGpuMemoryProperties(),
        memoryPropertyFlags,
        debugName);

    RHIBufferHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextBufferId++;
        buffers.emplace(handle.id, VulkanBufferResource{ buffer, memory });
    }
    return handle;
}

void* RHIDeviceVulkan::MapBufferMemory(RHIBufferHandle bufferHandle, vk::DeviceSize size)
{
    const VulkanBufferResource resource = RequireBufferResource(bufferHandle);
    return GetDevice().mapMemory(resource.memory, 0, size);
}

void RHIDeviceVulkan::UnmapBufferMemory(RHIBufferHandle bufferHandle)
{
    const VulkanBufferResource resource = RequireBufferResource(bufferHandle);
    GetDevice().unmapMemory(resource.memory);
}

void RHIDeviceVulkan::DestroyBuffer(RHIBufferHandle bufferHandle)
{
    if (!bufferHandle.IsValid())
    {
        return;
    }

    VulkanBufferResource resource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = buffers.find(bufferHandle.id);
        if (resourceIt == buffers.end())
        {
            return;
        }
        resource = resourceIt->second;
        buffers.erase(resourceIt);
    }

    vk::Device& device = GetDevice();
    if (resource.buffer)
    {
        device.destroyBuffer(resource.buffer);
    }
    if (resource.memory)
    {
        device.freeMemory(resource.memory);
    }
}

void RHIDeviceVulkan::CopyBufferToBuffer(
    RHIBufferHandle source,
    RHIBufferHandle destination,
    vk::DeviceSize size)
{
    const VulkanBufferResource sourceResource = RequireBufferResource(source);
    const VulkanBufferResource destinationResource = RequireBufferResource(destination);
    vk::Buffer sourceBuffer = sourceResource.buffer;
    vk::Buffer destinationBuffer = destinationResource.buffer;
    vk::DeviceSize copySize = size;
    CommonFunction::CopyBufferToBuffer(
        GetDevice(),
        GetGraphicsQueue(),
        GetSingleTimeCommandPool(),
        sourceBuffer,
        destinationBuffer,
        copySize);
}

VulkanBufferResource RHIDeviceVulkan::GetVulkanBufferResource(
    RHIBufferHandle bufferHandle) const
{
    return RequireBufferResource(bufferHandle);
}

VulkanBufferResource RHIDeviceVulkan::RequireBufferResource(
    RHIBufferHandle bufferHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = buffers.find(bufferHandle.id);
    if (!bufferHandle.IsValid() || resourceIt == buffers.end())
    {
        throw std::runtime_error(BuildInvalidLifecycleHandleMessage("buffer", bufferHandle));
    }

    return resourceIt->second;
}

RHIImageHandle RHIDeviceVulkan::CreateImage(
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
    auto [image, memory] = CommonFunction::CreateImage(
        GetDevice(),
        width,
        height,
        mipLevels,
        samples,
        format,
        tiling,
        usage,
        GetGpuMemoryProperties(),
        memoryPropertyFlags,
        debugName);

    RHIImageHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextImageId++;
        images.emplace(handle.id, VulkanImageResource{ image, memory });
    }
    return handle;
}

RHIImageHandle RHIDeviceVulkan::CreateImage(
    const vk::ImageCreateInfo& createInfo,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    vk::Device& device = GetDevice();
    vk::Image image = device.createImage(createInfo);
    vk::MemoryRequirements memoryRequirements = device.getImageMemoryRequirements(image);

    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo
        .setAllocationSize(memoryRequirements.size)
        .setMemoryTypeIndex(CommonFunction::FindMemoryType(
            GetGpuMemoryProperties(),
            memoryRequirements.memoryTypeBits,
            memoryPropertyFlags));

    vk::DeviceMemory imageMemory = device.allocateMemory(allocateInfo);
    device.bindImageMemory(image, imageMemory, 0);

    if (!debugName.empty())
    {
        VulkanDebug::SetObjectName(device, image, vk::ObjectType::eImage, debugName);
        VulkanDebug::SetObjectName(
            device,
            imageMemory,
            vk::ObjectType::eDeviceMemory,
            "DeviceMemory: " + debugName);
    }

    RHIImageHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextImageId++;
        images.emplace(handle.id, VulkanImageResource{ image, imageMemory });
    }
    return handle;
}

void RHIDeviceVulkan::TransitionImageLayout(
    RHIImageHandle imageHandle,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    const VulkanImageResource resource = RequireImageResource(imageHandle);
    vk::Image image = resource.image;
    vk::Format transitionFormat = format;
    CommonFunction::TransitionImageLayout(
        image,
        mipLevels,
        transitionFormat,
        GetDevice(),
        GetSingleTimeCommandPool(),
        GetGraphicsQueue(),
        oldLayout,
        newLayout,
        baseArrayLayer,
        layerCount);
}

RHIImageViewHandle RHIDeviceVulkan::Create2DImageView(
    RHIImageHandle imageHandle,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageAspectFlags aspectMask,
    const std::string& debugName)
{
    const VulkanImageResource resource = RequireImageResource(imageHandle);
    vk::Image image = resource.image;
    vk::Format imageFormat = format;
    vk::ImageView imageView = CommonFunction::Create2DImageView(
        GetDevice(),
        image,
        mipLevels,
        imageFormat,
        aspectMask,
        debugName);
    return RegisterImageView(imageView);
}

RHIImageViewHandle RHIDeviceVulkan::CreateImageView(
    RHIImageHandle imageHandle,
    vk::ImageViewType viewType,
    vk::Format format,
    vk::ImageAspectFlags aspectMask,
    uint32_t baseMipLevel,
    uint32_t mipLevelCount,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    const std::string& debugName)
{
    const VulkanImageResource resource = RequireImageResource(imageHandle);
    vk::Image image = resource.image;
    vk::Format imageFormat = format;
    vk::ImageView imageView = CommonFunction::CreateImageViewBase(
        GetDevice(),
        image,
        viewType,
        imageFormat,
        aspectMask,
        baseMipLevel,
        mipLevelCount,
        baseArrayLayer,
        layerCount,
        debugName);
    return RegisterImageView(imageView);
}

RHIImageViewHandle RHIDeviceVulkan::CreateCubeImageView(
    RHIImageHandle imageHandle,
    uint32_t mipLevels,
    vk::Format format,
    const std::string& debugName)
{
    const VulkanImageResource resource = RequireImageResource(imageHandle);
    vk::Image image = resource.image;
    vk::Format imageFormat = format;
    vk::ImageView imageView = CommonFunction::CreateCubeImageView(
        GetDevice(),
        image,
        mipLevels,
        imageFormat,
        debugName);
    return RegisterImageView(imageView);
}

RHIImageViewHandle RHIDeviceVulkan::CreateCubeStorageImageView(
    RHIImageHandle imageHandle,
    vk::Format format,
    const std::string& debugName)
{
    const VulkanImageResource resource = RequireImageResource(imageHandle);
    vk::Image image = resource.image;
    vk::Format imageFormat = format;
    vk::ImageView imageView = CommonFunction::CreateCubeStorageImageView(
        GetDevice(),
        image,
        imageFormat,
        debugName);
    return RegisterImageView(imageView);
}

void RHIDeviceVulkan::DestroyImageView(RHIImageViewHandle imageViewHandle)
{
    if (!imageViewHandle.IsValid())
    {
        return;
    }

    VulkanImageViewResource resource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = imageViews.find(imageViewHandle.id);
        if (resourceIt == imageViews.end())
        {
            return;
        }
        resource = resourceIt->second;
        imageViews.erase(resourceIt);
    }

    if (resource.imageView)
    {
        GetDevice().destroyImageView(resource.imageView);
    }
}

VulkanImageViewResource RHIDeviceVulkan::GetVulkanImageViewResource(
    RHIImageViewHandle imageViewHandle) const
{
    return RequireImageViewResource(imageViewHandle);
}

RHIImageViewHandle RHIDeviceVulkan::RegisterImageView(vk::ImageView imageView)
{
    RHIImageViewHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextImageViewId++;
        imageViews.emplace(handle.id, VulkanImageViewResource{ imageView });
    }
    return handle;
}

RHISamplerHandle RHIDeviceVulkan::Create2DSampler(const std::string& debugName)
{
    vk::Sampler sampler = CommonFunction::Create2DSampler(
        GetDevice(),
        GetPhysicalDevice(),
        debugName);
    return RegisterSampler(sampler);
}

RHISamplerHandle RHIDeviceVulkan::Create2DSampler(
    vk::Filter filter,
    vk::SamplerAddressMode addressMode,
    bool enableMipmaps,
    const std::string& debugName)
{
    vk::Sampler sampler = CommonFunction::Create2DSampler(
        GetDevice(),
        GetPhysicalDevice(),
        filter,
        addressMode,
        enableMipmaps,
        debugName);
    return RegisterSampler(sampler);
}

RHISamplerHandle RHIDeviceVulkan::CreateSampler(
    const vk::SamplerCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::Sampler sampler = CommonFunction::CreateSamplerBase(GetDevice(), createInfo, debugName);
    return RegisterSampler(sampler);
}

RHISamplerHandle RHIDeviceVulkan::CreateCubeSampler(
    float maxLod,
    const std::string& debugName)
{
    vk::Sampler sampler = CommonFunction::CreateCubeSampler(GetDevice(), maxLod, debugName);
    return RegisterSampler(sampler);
}

void RHIDeviceVulkan::DestroySampler(RHISamplerHandle samplerHandle)
{
    if (!samplerHandle.IsValid())
    {
        return;
    }

    VulkanSamplerResource resource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = samplers.find(samplerHandle.id);
        if (resourceIt == samplers.end())
        {
            return;
        }
        resource = resourceIt->second;
        samplers.erase(resourceIt);
    }

    if (resource.sampler)
    {
        GetDevice().destroySampler(resource.sampler);
    }
}

VulkanSamplerResource RHIDeviceVulkan::GetVulkanSamplerResource(
    RHISamplerHandle samplerHandle) const
{
    return RequireSamplerResource(samplerHandle);
}

RHISamplerHandle RHIDeviceVulkan::RegisterSampler(vk::Sampler sampler)
{
    RHISamplerHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextSamplerId++;
        samplers.emplace(handle.id, VulkanSamplerResource{ sampler });
    }
    return handle;
}

RHISamplerHandle RHIDeviceVulkan::CreateDepthSampler(const std::string& debugName)
{
    vk::Sampler sampler = CommonFunction::CreateDepthSampler(
        GetDevice(),
        GetPhysicalDevice(),
        debugName);
    return RegisterSampler(sampler);
}

RHISamplerHandle RHIDeviceVulkan::CreateDepthCompareSampler(const std::string& debugName)
{
    vk::Sampler sampler = CommonFunction::CreateDepthCompareSampler(
        GetDevice(),
        GetPhysicalDevice(),
        debugName);
    return RegisterSampler(sampler);
}

vk::CommandBuffer RHIDeviceVulkan::BeginSingleTimeCommands()
{
    return CommonFunction::BeginSingleTimeCommands(
        GetDevice(),
        GetSingleTimeCommandPool());
}

void RHIDeviceVulkan::EndSingleTimeCommands(vk::CommandBuffer& commandBuffer)
{
    CommonFunction::EndSingleTimeCommands(
        GetDevice(),
        commandBuffer,
        GetGraphicsQueue(),
        GetSingleTimeCommandPool());
}

void RHIDeviceVulkan::AbortSingleTimeCommands(vk::CommandBuffer& commandBuffer)
{
    CommonFunction::AbortSingleTimeCommands(
        GetDevice(),
        commandBuffer,
        GetSingleTimeCommandPool());
}

void RHIDeviceVulkan::CopyBufferToImage(
    RHIBufferHandle bufferHandle,
    RHIImageHandle imageHandle,
    uint32_t width,
    uint32_t height,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    vk::DeviceSize layerBytes,
    uint32_t bufferRowLength)
{
    const VulkanBufferResource bufferResource = RequireBufferResource(bufferHandle);
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    vk::Buffer buffer = bufferResource.buffer;
    vk::Image image = imageResource.image;
    CommonFunction::CopyBufferToImage(
        GetDevice(),
        GetGraphicsQueue(),
        GetSingleTimeCommandPool(),
        buffer,
        image,
        width,
        height,
        false,
        0,
        baseArrayLayer,
        layerCount,
        layerBytes,
        bufferRowLength);
}

void RHIDeviceVulkan::CopyBufferToImageRegion(
    RHIBufferHandle bufferHandle,
    RHIImageHandle imageHandle,
    vk::DeviceSize bufferOffset,
    uint32_t bufferRowLength,
    uint32_t imageOffsetX,
    uint32_t imageOffsetY,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevel,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    vk::DeviceSize layerBytes)
{
    const VulkanBufferResource bufferResource = RequireBufferResource(bufferHandle);
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    vk::Buffer buffer = bufferResource.buffer;
    vk::Image image = imageResource.image;
    CommonFunction::CopyBufferToImageRegion(
        GetDevice(),
        GetGraphicsQueue(),
        GetSingleTimeCommandPool(),
        buffer,
        image,
        bufferOffset,
        bufferRowLength,
        imageOffsetX,
        imageOffsetY,
        width,
        height,
        mipLevel,
        baseArrayLayer,
        layerCount,
        layerBytes);
}

void RHIDeviceVulkan::CopyBufferToImageRegions(
    RHIBufferHandle bufferHandle,
    RHIImageHandle imageHandle,
    const std::vector<vk::BufferImageCopy>& regions)
{
    const VulkanBufferResource bufferResource = RequireBufferResource(bufferHandle);
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    vk::Buffer buffer = bufferResource.buffer;
    vk::Image image = imageResource.image;
    CommonFunction::CopyBufferToImageRegions(
        GetDevice(),
        GetGraphicsQueue(),
        GetSingleTimeCommandPool(),
        buffer,
        image,
        regions);
}

void RHIDeviceVulkan::RecordTransitionImageLayout(
    vk::CommandBuffer commandBuffer,
    RHIImageHandle imageHandle,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    CommonFunction::RecordTransitionImageLayout(
        commandBuffer,
        imageResource.image,
        mipLevels,
        format,
        oldLayout,
        newLayout,
        baseArrayLayer,
        layerCount);
}

void RHIDeviceVulkan::RecordCopyBufferToImageRegions(
    vk::CommandBuffer commandBuffer,
    RHIBufferHandle bufferHandle,
    RHIImageHandle imageHandle,
    const std::vector<vk::BufferImageCopy>& regions)
{
    const VulkanBufferResource bufferResource = RequireBufferResource(bufferHandle);
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    CommonFunction::RecordCopyBufferToImageRegions(
        commandBuffer,
        bufferResource.buffer,
        imageResource.image,
        regions);
}

void RHIDeviceVulkan::RecordClearColorImage(
    vk::CommandBuffer commandBuffer,
    RHIImageHandle imageHandle,
    vk::ImageLayout layout,
    vk::ClearColorValue clearValue,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    CommonFunction::RecordClearColorImage(
        commandBuffer,
        imageResource.image,
        layout,
        clearValue,
        baseArrayLayer,
        layerCount);
}

void RHIDeviceVulkan::RecordCopyImageRegions(
    vk::CommandBuffer commandBuffer,
    RHIImageHandle sourceHandle,
    RHIImageHandle destinationHandle,
    const std::vector<vk::ImageCopy>& regions)
{
    const VulkanImageResource source = RequireImageResource(sourceHandle);
    const VulkanImageResource destination =
        RequireImageResource(destinationHandle);
    CommonFunction::RecordCopyImageRegions(
        commandBuffer,
        source.image,
        destination.image,
        regions);
}

void RHIDeviceVulkan::CopyImageRegions(
    RHIImageHandle sourceHandle,
    RHIImageHandle destinationHandle,
    const std::vector<vk::ImageCopy>& regions)
{
    const VulkanImageResource source = RequireImageResource(sourceHandle);
    const VulkanImageResource destination =
        RequireImageResource(destinationHandle);
    CommonFunction::CopyImageRegions(
        GetDevice(),
        GetGraphicsQueue(),
        GetSingleTimeCommandPool(),
        source.image,
        destination.image,
        regions);
}

void RHIDeviceVulkan::RecordCopyImageToBuffer(
    vk::CommandBuffer commandBuffer,
    RHIImageHandle imageHandle,
    RHIBufferHandle bufferHandle,
    uint32_t width,
    uint32_t height,
    bool flipY,
    vk::DeviceSize rowBytes)
{
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    const VulkanBufferResource bufferResource = RequireBufferResource(bufferHandle);
    CommonFunction::RecordCopyImageToBuffer(
        commandBuffer,
        imageResource.image,
        bufferResource.buffer,
        width,
        height,
        flipY,
        rowBytes);
}

void RHIDeviceVulkan::CopyImageToBuffer(
    RHIImageHandle imageHandle,
    RHIBufferHandle bufferHandle,
    uint32_t width,
    uint32_t height,
    bool flipY,
    vk::DeviceSize rowBytes)
{
    const VulkanImageResource imageResource = RequireImageResource(imageHandle);
    const VulkanBufferResource bufferResource = RequireBufferResource(bufferHandle);
    vk::Image image = imageResource.image;
    vk::Buffer buffer = bufferResource.buffer;
    CommonFunction::CopyImageToBuffer(
        GetDevice(),
        GetGraphicsQueue(),
        GetSingleTimeCommandPool(),
        image,
        buffer,
        width,
        height,
        flipY,
        rowBytes);
}

void RHIDeviceVulkan::GenerateMipmaps(
    RHIImageHandle imageHandle,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    uint32_t baseArrayLayer,
    uint32_t layerCount)
{
    const VulkanImageResource resource = RequireImageResource(imageHandle);
    vk::Image image = resource.image;
    CommonFunction::GenerateMipmaps(
        GetDevice(),
        GetGraphicsQueue(),
        GetSingleTimeCommandPool(),
        image,
        width,
        height,
        mipLevels,
        baseArrayLayer,
        layerCount);
}

void RHIDeviceVulkan::DestroyImageResource(
    RHIImageHandle imageHandle,
    RHIImageViewHandle imageViewHandle,
    RHISamplerHandle samplerHandle)
{
    if (!imageHandle.IsValid())
    {
        return;
    }

    VulkanImageResource imageResource{};
    VulkanImageViewResource imageViewResource{};
    VulkanSamplerResource samplerResource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = images.find(imageHandle.id);
        if (resourceIt == images.end())
        {
            return;
        }
        imageResource = resourceIt->second;
        images.erase(resourceIt);

        auto imageViewIt = imageViews.find(imageViewHandle.id);
        if (imageViewHandle.IsValid() && imageViewIt != imageViews.end())
        {
            imageViewResource = imageViewIt->second;
            imageViews.erase(imageViewIt);
        }
        auto samplerIt = samplers.find(samplerHandle.id);
        if (samplerHandle.IsValid() && samplerIt != samplers.end())
        {
            samplerResource = samplerIt->second;
            samplers.erase(samplerIt);
        }
    }

    vk::Device& device = GetDevice();
    if (imageViewResource.imageView)
    {
        device.destroyImageView(imageViewResource.imageView);
    }
    if (samplerResource.sampler)
    {
        device.destroySampler(samplerResource.sampler);
    }
    if (imageResource.image)
    {
        device.destroyImage(imageResource.image);
    }
    if (imageResource.memory)
    {
        device.freeMemory(imageResource.memory);
    }
}

VulkanImageResource RHIDeviceVulkan::GetVulkanImageResource(
    RHIImageHandle imageHandle) const
{
    return RequireImageResource(imageHandle);
}

VulkanImageResource RHIDeviceVulkan::RequireImageResource(
    RHIImageHandle imageHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = images.find(imageHandle.id);
    if (!imageHandle.IsValid() || resourceIt == images.end())
    {
        throw std::runtime_error(BuildInvalidLifecycleHandleMessage("image", imageHandle));
    }

    return resourceIt->second;
}

VulkanImageViewResource RHIDeviceVulkan::RequireImageViewResource(
    RHIImageViewHandle imageViewHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = imageViews.find(imageViewHandle.id);
    if (!imageViewHandle.IsValid() || resourceIt == imageViews.end())
    {
        throw std::runtime_error(BuildInvalidLifecycleHandleMessage("image view", imageViewHandle));
    }

    return resourceIt->second;
}

VulkanSamplerResource RHIDeviceVulkan::RequireSamplerResource(
    RHISamplerHandle samplerHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = samplers.find(samplerHandle.id);
    if (!samplerHandle.IsValid() || resourceIt == samplers.end())
    {
        throw std::runtime_error(BuildInvalidLifecycleHandleMessage("sampler", samplerHandle));
    }

    return resourceIt->second;
}

VulkanDescriptorSetLayoutResource RHIDeviceVulkan::RequireDescriptorSetLayoutResource(
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = descriptorSetLayouts.find(descriptorSetLayoutHandle.id);
    if (!descriptorSetLayoutHandle.IsValid() || resourceIt == descriptorSetLayouts.end())
    {
        throw std::runtime_error(
            BuildInvalidLifecycleHandleMessage("descriptor set layout", descriptorSetLayoutHandle));
    }

    return resourceIt->second;
}

VulkanDescriptorPoolResource RHIDeviceVulkan::RequireDescriptorPoolResource(
    RHIDescriptorPoolHandle descriptorPoolHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = descriptorPools.find(descriptorPoolHandle.id);
    if (!descriptorPoolHandle.IsValid() || resourceIt == descriptorPools.end())
    {
        throw std::runtime_error(
            BuildInvalidLifecycleHandleMessage("descriptor pool", descriptorPoolHandle));
    }

    return resourceIt->second;
}

VulkanDescriptorSetResource RHIDeviceVulkan::RequireDescriptorSetResource(
    RHIDescriptorSetHandle descriptorSetHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = descriptorSets.find(descriptorSetHandle.id);
    if (!descriptorSetHandle.IsValid() || resourceIt == descriptorSets.end())
    {
        throw std::runtime_error(
            BuildInvalidLifecycleHandleMessage("descriptor set", descriptorSetHandle));
    }

    return resourceIt->second;
}

RHIDescriptorSetLayoutHandle RHIDeviceVulkan::CreateDescriptorSetLayout(
    const vk::DescriptorSetLayoutCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::DescriptorSetLayout descriptorSetLayout;
    vk::Result result = GetDevice().createDescriptorSetLayout(
        &createInfo,
        nullptr,
        &descriptorSetLayout);
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to create descriptor set layout: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        descriptorSetLayout,
        vk::ObjectType::eDescriptorSetLayout,
        debugName);
    return RegisterDescriptorSetLayout(descriptorSetLayout, true);
}

void RHIDeviceVulkan::DestroyDescriptorSetLayout(
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle)
{
    if (!descriptorSetLayoutHandle.IsValid())
    {
        return;
    }

    VulkanDescriptorSetLayoutResource resource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = descriptorSetLayouts.find(descriptorSetLayoutHandle.id);
        if (resourceIt == descriptorSetLayouts.end())
        {
            return;
        }
        resource = resourceIt->second;
        descriptorSetLayouts.erase(resourceIt);
    }

    if (resource.ownsResource && resource.descriptorSetLayout)
    {
        GetDevice().destroyDescriptorSetLayout(resource.descriptorSetLayout, nullptr);
    }
}

VulkanDescriptorSetLayoutResource RHIDeviceVulkan::GetVulkanDescriptorSetLayoutResource(
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle) const
{
    return RequireDescriptorSetLayoutResource(descriptorSetLayoutHandle);
}

RHIDescriptorSetLayoutHandle RHIDeviceVulkan::RegisterDescriptorSetLayout(
    vk::DescriptorSetLayout descriptorSetLayout,
    bool ownsResource)
{
    RHIDescriptorSetLayoutHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextDescriptorSetLayoutId++;
        descriptorSetLayouts.emplace(
            handle.id,
            VulkanDescriptorSetLayoutResource{ descriptorSetLayout, ownsResource });
    }
    return handle;
}

RHIDescriptorPoolHandle RHIDeviceVulkan::CreateDescriptorPool(
    const vk::DescriptorPoolCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::DescriptorPool descriptorPool;
    vk::Result result = GetDevice().createDescriptorPool(&createInfo, nullptr, &descriptorPool);
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to create descriptor pool: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        descriptorPool,
        vk::ObjectType::eDescriptorPool,
        debugName);
    RHIDescriptorPoolHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextDescriptorPoolId++;
        descriptorPools.emplace(handle.id, VulkanDescriptorPoolResource{ descriptorPool });
    }
    return handle;
}

void RHIDeviceVulkan::DestroyDescriptorPool(RHIDescriptorPoolHandle descriptorPoolHandle)
{
    if (!descriptorPoolHandle.IsValid())
    {
        return;
    }

    VulkanDescriptorPoolResource resource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = descriptorPools.find(descriptorPoolHandle.id);
        if (resourceIt == descriptorPools.end())
        {
            return;
        }
        resource = resourceIt->second;
        descriptorPools.erase(resourceIt);

        for (auto setIt = descriptorSets.begin(); setIt != descriptorSets.end();)
        {
            if (setIt->second.descriptorPoolHandle.id == descriptorPoolHandle.id)
            {
                setIt = descriptorSets.erase(setIt);
            }
            else
            {
                ++setIt;
            }
        }
    }

    if (resource.descriptorPool)
    {
        GetDevice().destroyDescriptorPool(resource.descriptorPool, nullptr);
    }
}

VulkanDescriptorPoolResource RHIDeviceVulkan::GetVulkanDescriptorPoolResource(
    RHIDescriptorPoolHandle descriptorPoolHandle) const
{
    return RequireDescriptorPoolResource(descriptorPoolHandle);
}

std::vector<RHIDescriptorSetHandle> RHIDeviceVulkan::AllocateDescriptorSets(
    RHIDescriptorPoolHandle descriptorPoolHandle,
    const std::vector<RHIDescriptorSetLayoutHandle>& descriptorSetLayoutHandles)
{
    std::vector<RHIDescriptorSetHandle> descriptorSetHandles;
    if (descriptorSetLayoutHandles.empty())
    {
        return descriptorSetHandles;
    }

    const VulkanDescriptorPoolResource descriptorPoolResource =
        RequireDescriptorPoolResource(descriptorPoolHandle);
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    descriptorSetLayouts.reserve(descriptorSetLayoutHandles.size());
    for (RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle : descriptorSetLayoutHandles)
    {
        descriptorSetLayouts.push_back(
            RequireDescriptorSetLayoutResource(descriptorSetLayoutHandle).descriptorSetLayout);
    }

    vk::DescriptorSetAllocateInfo allocateInfo;
    allocateInfo
        .setDescriptorPool(descriptorPoolResource.descriptorPool)
        .setSetLayouts(descriptorSetLayouts);

    std::vector<vk::DescriptorSet> allocatedDescriptorSets(descriptorSetLayouts.size());
    vk::Result result = GetDevice().allocateDescriptorSets(
        &allocateInfo,
        allocatedDescriptorSets.data());
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to allocate descriptor sets");
    }

    descriptorSetHandles.reserve(allocatedDescriptorSets.size());
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        for (vk::DescriptorSet descriptorSet : allocatedDescriptorSets)
        {
            RHIDescriptorSetHandle descriptorSetHandle;
            descriptorSetHandle.id = nextDescriptorSetId++;
            descriptorSets.emplace(
                descriptorSetHandle.id,
                VulkanDescriptorSetResource{ descriptorSet, descriptorPoolHandle });
            descriptorSetHandles.push_back(descriptorSetHandle);
        }
    }
    return descriptorSetHandles;
}

void RHIDeviceVulkan::FreeDescriptorSet(
    RHIDescriptorPoolHandle descriptorPoolHandle,
    RHIDescriptorSetHandle descriptorSetHandle)
{
    if (!descriptorPoolHandle.IsValid() || !descriptorSetHandle.IsValid())
    {
        return;
    }

    VulkanDescriptorSetResource descriptorSetResource{};
    {
        std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
        auto setIt = descriptorSets.find(descriptorSetHandle.id);
        if (setIt == descriptorSets.end())
        {
            return;
        }
        descriptorSetResource = setIt->second;
    }

    const VulkanDescriptorPoolResource descriptorPoolResource =
        RequireDescriptorPoolResource(descriptorPoolHandle);
    if (descriptorPoolResource.descriptorPool && descriptorSetResource.descriptorSet)
    {
        GetDevice().freeDescriptorSets(
            descriptorPoolResource.descriptorPool,
            1,
            &descriptorSetResource.descriptorSet);
    }
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        descriptorSets.erase(descriptorSetHandle.id);
    }
}

VulkanDescriptorSetResource RHIDeviceVulkan::GetVulkanDescriptorSetResource(
    RHIDescriptorSetHandle descriptorSetHandle) const
{
    return RequireDescriptorSetResource(descriptorSetHandle);
}

void RHIDeviceVulkan::UpdateDescriptorSets(
    const std::vector<RHIDescriptorWrite>& writeDescriptorSets)
{
    if (writeDescriptorSets.empty())
    {
        return;
    }

    std::vector<std::vector<vk::DescriptorImageInfo>> imageInfoStorage(writeDescriptorSets.size());
    std::vector<std::vector<vk::DescriptorBufferInfo>> bufferInfoStorage(writeDescriptorSets.size());
    std::vector<std::vector<vk::BufferView>> texelBufferViewStorage(writeDescriptorSets.size());
    std::vector<vk::WriteDescriptorSet> vulkanWrites;
    vulkanWrites.reserve(writeDescriptorSets.size());

    for (size_t i = 0; i < writeDescriptorSets.size(); ++i)
    {
        const RHIDescriptorWrite& rhiWrite = writeDescriptorSets[i];
        imageInfoStorage[i] = rhiWrite.imageInfos;
        bufferInfoStorage[i] = rhiWrite.bufferInfos;
        texelBufferViewStorage[i] = rhiWrite.texelBufferViews;

        uint32_t descriptorCount = rhiWrite.descriptorCount;
        if (descriptorCount == 0)
        {
            descriptorCount = static_cast<uint32_t>(
                std::max(
                    imageInfoStorage[i].size(),
                    std::max(bufferInfoStorage[i].size(), texelBufferViewStorage[i].size())));
        }

        vk::WriteDescriptorSet vulkanWrite;
        vulkanWrite
            .setDstSet(RequireDescriptorSetResource(rhiWrite.destinationSet).descriptorSet)
            .setDstBinding(rhiWrite.destinationBinding)
            .setDstArrayElement(rhiWrite.destinationArrayElement)
            .setDescriptorCount(descriptorCount)
            .setDescriptorType(rhiWrite.descriptorType);
        if (!imageInfoStorage[i].empty())
        {
            vulkanWrite.setPImageInfo(imageInfoStorage[i].data());
        }
        if (!bufferInfoStorage[i].empty())
        {
            vulkanWrite.setPBufferInfo(bufferInfoStorage[i].data());
        }
        if (!texelBufferViewStorage[i].empty())
        {
            vulkanWrite.setPTexelBufferView(texelBufferViewStorage[i].data());
        }
        vulkanWrites.push_back(vulkanWrite);
    }

    GetDevice().updateDescriptorSets(vulkanWrites, nullptr);
}

void RHIDeviceVulkan::SetDescriptorSetDebugName(
    RHIDescriptorSetHandle descriptorSetHandle,
    const std::string& debugName)
{
    if (!descriptorSetHandle.IsValid())
    {
        return;
    }

    const VulkanDescriptorSetResource descriptorSetResource =
        RequireDescriptorSetResource(descriptorSetHandle);
    if (!descriptorSetResource.descriptorSet)
    {
        return;
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        descriptorSetResource.descriptorSet,
        vk::ObjectType::eDescriptorSet,
        debugName);
}

VulkanRenderPassResource RHIDeviceVulkan::RequireRenderPassResource(
    RHIRenderPassHandle renderPassHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = renderPasses.find(renderPassHandle.id);
    if (!renderPassHandle.IsValid() || resourceIt == renderPasses.end())
    {
        throw std::runtime_error(BuildInvalidLifecycleHandleMessage("render pass", renderPassHandle));
    }

    return resourceIt->second;
}

VulkanFramebufferResource RHIDeviceVulkan::RequireFramebufferResource(
    RHIFramebufferHandle framebufferHandle) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto resourceIt = framebuffers.find(framebufferHandle.id);
    if (!framebufferHandle.IsValid() || resourceIt == framebuffers.end())
    {
        throw std::runtime_error(BuildInvalidLifecycleHandleMessage("framebuffer", framebufferHandle));
    }

    return resourceIt->second;
}

RHIRenderPassHandle RHIDeviceVulkan::CreateRenderPass(
    const vk::RenderPassCreateInfo2& createInfo,
    const std::string& debugName)
{
    vk::RenderPass renderPass = GetDevice().createRenderPass2(createInfo);
    if (!renderPass)
    {
        throw std::runtime_error("Failed to create render pass: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        renderPass,
        vk::ObjectType::eRenderPass,
        debugName);
    RHIRenderPassHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextRenderPassId++;
        renderPasses.emplace(handle.id, VulkanRenderPassResource{ renderPass });
    }
    return handle;
}

void RHIDeviceVulkan::DestroyRenderPass(RHIRenderPassHandle renderPassHandle)
{
    if (!renderPassHandle.IsValid())
    {
        return;
    }

    VulkanRenderPassResource resource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = renderPasses.find(renderPassHandle.id);
        if (resourceIt == renderPasses.end())
        {
            return;
        }
        resource = resourceIt->second;
        renderPasses.erase(resourceIt);
    }

    if (resource.renderPass)
    {
        GetDevice().destroyRenderPass(resource.renderPass);
    }
}

VulkanRenderPassResource RHIDeviceVulkan::GetVulkanRenderPassResource(
    RHIRenderPassHandle renderPassHandle) const
{
    return RequireRenderPassResource(renderPassHandle);
}

RHIFramebufferHandle RHIDeviceVulkan::CreateFramebuffer(
    RHIRenderPassHandle renderPassHandle,
    const vk::FramebufferCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::FramebufferCreateInfo framebufferCreateInfo = createInfo;
    framebufferCreateInfo.setRenderPass(RequireRenderPassResource(renderPassHandle).renderPass);
    vk::Framebuffer framebuffer = GetDevice().createFramebuffer(framebufferCreateInfo);
    if (!framebuffer)
    {
        throw std::runtime_error("Failed to create framebuffer: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        framebuffer,
        vk::ObjectType::eFramebuffer,
        debugName);
    RHIFramebufferHandle handle;
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        handle.id = nextFramebufferId++;
        framebuffers.emplace(handle.id, VulkanFramebufferResource{ framebuffer });
    }
    return handle;
}

void RHIDeviceVulkan::DestroyFramebuffer(RHIFramebufferHandle framebufferHandle)
{
    if (!framebufferHandle.IsValid())
    {
        return;
    }

    VulkanFramebufferResource resource{};
    {
        std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
        auto resourceIt = framebuffers.find(framebufferHandle.id);
        if (resourceIt == framebuffers.end())
        {
            return;
        }
        resource = resourceIt->second;
        framebuffers.erase(resourceIt);
    }

    if (resource.framebuffer)
    {
        GetDevice().destroyFramebuffer(resource.framebuffer);
    }
}

VulkanFramebufferResource RHIDeviceVulkan::GetVulkanFramebufferResource(
    RHIFramebufferHandle framebufferHandle) const
{
    return RequireFramebufferResource(framebufferHandle);
}

} // namespace VL
