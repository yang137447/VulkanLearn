#include "render/backend/rendererBackendVulkan.h"

#include <stdexcept>

#include "pipeline/pipelineFactory.h"
#include "profiler.h"
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
    vk::Queue graphicsQueue;
    vk::SwapchainKHR swapchain;
    vk::PipelineStageFlags waitDstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    uint64_t submittedEpoch = 0;
};

} // namespace

void RendererBackendVulkan::Initialize(
    std::vector<const char*>& instanceExtensions,
    SDL_Window* window)
{
    rhiDevice.Initialize(instanceExtensions, window);
    frameFenceEpochs.assign(rhiDevice.GetTaskFinishedFences().size(), 0);
    initialized = true;
}

void RendererBackendVulkan::WaitIdle()
{
    rhiDevice.WaitIdle();
}

void RendererBackendVulkan::RecreateSwapchain(int width, int height)
{
    rhiDevice.RecreateSwapchain(width, height);
    frameFenceEpochs.assign(rhiDevice.GetTaskFinishedFences().size(), 0);
}

std::unique_ptr<PipelineFactory> RendererBackendVulkan::CreatePipelineFactory()
{
    // Pipeline objects still need the Vulkan device while the pipeline layer is
    // Vulkan-backed. Keeping that handle pull inside the backend stops higher
    // runtime code from depending on raw device access.
    return std::make_unique<PipelineFactory>(&rhiDevice.GetDevice());
}

uint32_t RendererBackendVulkan::GetSwapchainImageCount() const
{
    return rhiDevice.GetSwapchainImageCount();
}

vk::Extent2D RendererBackendVulkan::GetSwapchainExtent() const
{
    return rhiDevice.GetSwapchainExtent();
}

vk::Format RendererBackendVulkan::GetSwapchainImageFormat() const
{
    return rhiDevice.GetSwapchainImageFormat();
}

const std::vector<vk::ImageView>& RendererBackendVulkan::GetSwapchainImageViews() const
{
    return rhiDevice.GetSwapchainImageViews();
}

std::pair<vk::Buffer, vk::DeviceMemory> RendererBackendVulkan::CreateBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    return rhiDevice.CreateBuffer(size, usage, memoryPropertyFlags, debugName);
}

void* RendererBackendVulkan::MapMemory(vk::DeviceMemory memory, vk::DeviceSize size)
{
    return rhiDevice.MapMemory(memory, size);
}

void RendererBackendVulkan::UnmapMemory(vk::DeviceMemory memory)
{
    rhiDevice.UnmapMemory(memory);
}

void RendererBackendVulkan::DestroyBuffer(vk::Buffer buffer, vk::DeviceMemory memory)
{
    rhiDevice.DestroyBuffer(buffer, memory);
}

void RendererBackendVulkan::CopyBufferToBuffer(
    vk::Buffer source,
    vk::Buffer destination,
    vk::DeviceSize size)
{
    rhiDevice.CopyBufferToBuffer(source, destination, size);
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
    bufferSet.buffers.resize(swapchainImageCount);
    bufferSet.bufferMemories.resize(swapchainImageCount);
    bufferSet.buffersMapped.resize(swapchainImageCount);
    bufferSet.bufferSize = static_cast<uint32_t>(bufferSize);

    for (uint32_t i = 0; i < swapchainImageCount; i++)
    {
        std::tie(bufferSet.buffers[i], bufferSet.bufferMemories[i]) = CreateBuffer(
            bufferSize,
            usage,
            memoryPropertyFlags,
            debugNamePrefix + " (SwapchainIndex " + std::to_string(i) + ")");
        bufferSet.buffersMapped[i] = MapMemory(bufferSet.bufferMemories[i], bufferSize);
    }

    SetupDescriptorBufferInfos(bufferSet);
}

void RendererBackendVulkan::DestroyBufferSet(Buffer& bufferSet)
{
    for (size_t i = 0; i < bufferSet.buffers.size(); i++)
    {
        if (i < bufferSet.buffersMapped.size() && bufferSet.buffersMapped[i] != nullptr)
        {
            UnmapMemory(bufferSet.bufferMemories[i]);
            bufferSet.buffersMapped[i] = nullptr;
        }
        DestroyBuffer(bufferSet.buffers[i], bufferSet.bufferMemories[i]);
    }

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
    return rhiDevice.CreateImage(
        width,
        height,
        mipLevels,
        samples,
        format,
        tiling,
        usage,
        memoryPropertyFlags,
        debugName);
}

std::pair<vk::Image, vk::DeviceMemory> RendererBackendVulkan::CreateImage(
    const vk::ImageCreateInfo& createInfo,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    return rhiDevice.CreateImage(createInfo, memoryPropertyFlags, debugName);
}

void RendererBackendVulkan::TransitionImageLayout(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout)
{
    rhiDevice.TransitionImageLayout(image, mipLevels, format, oldLayout, newLayout);
}

vk::ImageView RendererBackendVulkan::Create2DImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageAspectFlagBits aspectMask,
    const std::string& debugName)
{
    return rhiDevice.Create2DImageView(image, mipLevels, format, aspectMask, debugName);
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
    return rhiDevice.CreateImageView(
        image,
        viewType,
        format,
        aspectMask,
        baseMipLevel,
        mipLevelCount,
        baseArrayLayer,
        layerCount,
        debugName);
}

vk::ImageView RendererBackendVulkan::CreateCubeImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    const std::string& debugName)
{
    return rhiDevice.CreateCubeImageView(image, mipLevels, format, debugName);
}

vk::ImageView RendererBackendVulkan::CreateCubeStorageImageView(
    vk::Image image,
    vk::Format format,
    const std::string& debugName)
{
    return rhiDevice.CreateCubeStorageImageView(image, format, debugName);
}

void RendererBackendVulkan::DestroyImageView(vk::ImageView& imageView)
{
    rhiDevice.DestroyImageView(imageView);
}

vk::Sampler RendererBackendVulkan::Create2DSampler(const std::string& debugName)
{
    return rhiDevice.Create2DSampler(debugName);
}

vk::Sampler RendererBackendVulkan::Create2DSampler(
    vk::Filter filter,
    vk::SamplerAddressMode addressMode,
    bool enableMipmaps,
    const std::string& debugName)
{
    return rhiDevice.Create2DSampler(filter, addressMode, enableMipmaps, debugName);
}

vk::Sampler RendererBackendVulkan::CreateSampler(
    const vk::SamplerCreateInfo& createInfo,
    const std::string& debugName)
{
    return rhiDevice.CreateSampler(createInfo, debugName);
}

vk::Sampler RendererBackendVulkan::CreateCubeSampler(
    float maxLod,
    const std::string& debugName)
{
    return rhiDevice.CreateCubeSampler(maxLod, debugName);
}

void RendererBackendVulkan::DestroySampler(vk::Sampler& sampler)
{
    rhiDevice.DestroySampler(sampler);
}

vk::Sampler RendererBackendVulkan::CreateDepthSampler(const std::string& debugName)
{
    return rhiDevice.CreateDepthSampler(debugName);
}

vk::Sampler RendererBackendVulkan::CreateDepthCompareSampler(const std::string& debugName)
{
    return rhiDevice.CreateDepthCompareSampler(debugName);
}

vk::CommandBuffer RendererBackendVulkan::BeginSingleTimeCommands()
{
    return rhiDevice.BeginSingleTimeCommands();
}

void RendererBackendVulkan::EndSingleTimeCommands(vk::CommandBuffer& commandBuffer)
{
    rhiDevice.EndSingleTimeCommands(commandBuffer);
}

void RendererBackendVulkan::CopyBufferToImage(
    vk::Buffer buffer,
    vk::Image image,
    uint32_t width,
    uint32_t height)
{
    rhiDevice.CopyBufferToImage(buffer, image, width, height);
}

void RendererBackendVulkan::CopyImageToBuffer(
    vk::Image image,
    vk::Buffer buffer,
    uint32_t width,
    uint32_t height,
    bool flipY,
    vk::DeviceSize rowBytes)
{
    rhiDevice.CopyImageToBuffer(image, buffer, width, height, flipY, rowBytes);
}

void RendererBackendVulkan::GenerateMipmaps(
    vk::Image image,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels)
{
    rhiDevice.GenerateMipmaps(image, width, height, mipLevels);
}

void RendererBackendVulkan::DestroyImageResource(
    vk::Image& image,
    vk::DeviceMemory& memory,
    vk::ImageView& imageView,
    vk::Sampler& sampler)
{
    rhiDevice.DestroyImageResource(image, memory, imageView, sampler);
}

vk::DescriptorSetLayout RendererBackendVulkan::CreateDescriptorSetLayout(
    const vk::DescriptorSetLayoutCreateInfo& createInfo,
    const std::string& debugName)
{
    return rhiDevice.CreateDescriptorSetLayout(createInfo, debugName);
}

void RendererBackendVulkan::DestroyDescriptorSetLayout(
    vk::DescriptorSetLayout& descriptorSetLayout)
{
    rhiDevice.DestroyDescriptorSetLayout(descriptorSetLayout);
}

vk::DescriptorPool RendererBackendVulkan::CreateDescriptorPool(
    const vk::DescriptorPoolCreateInfo& createInfo,
    const std::string& debugName)
{
    return rhiDevice.CreateDescriptorPool(createInfo, debugName);
}

void RendererBackendVulkan::DestroyDescriptorPool(vk::DescriptorPool& descriptorPool)
{
    rhiDevice.DestroyDescriptorPool(descriptorPool);
}

void RendererBackendVulkan::AllocateDescriptorSets(
    const vk::DescriptorSetAllocateInfo& allocateInfo,
    std::vector<vk::DescriptorSet>& descriptorSets)
{
    rhiDevice.AllocateDescriptorSets(allocateInfo, descriptorSets);
}

void RendererBackendVulkan::FreeDescriptorSet(
    vk::DescriptorPool descriptorPool,
    vk::DescriptorSet& descriptorSet)
{
    rhiDevice.FreeDescriptorSet(descriptorPool, descriptorSet);
}

void RendererBackendVulkan::UpdateDescriptorSets(
    const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets)
{
    rhiDevice.UpdateDescriptorSets(writeDescriptorSets);
}

void RendererBackendVulkan::SetDescriptorSetDebugName(
    vk::DescriptorSet descriptorSet,
    const std::string& debugName)
{
    rhiDevice.SetDescriptorSetDebugName(descriptorSet, debugName);
}

vk::RenderPass RendererBackendVulkan::CreateRenderPass(
    const vk::RenderPassCreateInfo2& createInfo,
    const std::string& debugName)
{
    return rhiDevice.CreateRenderPass(createInfo, debugName);
}

void RendererBackendVulkan::DestroyRenderPass(vk::RenderPass& renderPass)
{
    rhiDevice.DestroyRenderPass(renderPass);
}

vk::Framebuffer RendererBackendVulkan::CreateFramebuffer(
    const vk::FramebufferCreateInfo& createInfo,
    const std::string& debugName)
{
    return rhiDevice.CreateFramebuffer(createInfo, debugName);
}

void RendererBackendVulkan::DestroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers)
{
    rhiDevice.DestroyFramebuffers(framebuffers);
}

RendererFrameContext RendererBackendVulkan::BeginFrame(uint32_t currentFrame)
{
    if (!initialized)
    {
        throw std::runtime_error("RendererBackendVulkan is not initialized");
    }

    vk::Device& device = rhiDevice.GetDevice();

    RendererFrameContext frameContext;
    frameContext.swapchainImageCount = rhiDevice.GetSwapchainImageCount();
    const uint32_t frameFenceCount = static_cast<uint32_t>(rhiDevice.GetTaskFinishedFences().size());
    if (frameFenceCount == 0)
    {
        throw std::runtime_error("RendererBackendVulkan has no frame fences");
    }
    frameContext.frameIndex = currentFrame % frameFenceCount;

    vk::Fence& taskFinishedFence = rhiDevice.GetTaskFinishedFences()[frameContext.frameIndex];
    vk::Semaphore& imageAcquiredSemaphore = rhiDevice.GetImageAcquiredSemaphores()[frameContext.frameIndex];
    auto& imagesInFlightFences = rhiDevice.GetImagesInFlightFences();

    vk::Result result;
    {
        PROFILE_SCOPE("Render:WaitFence");
        result = device.waitForFences(taskFinishedFence, true, UINT64_MAX);
    }
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to wait for fence");
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
        result = device.acquireNextImageKHR(
            rhiDevice.GetSwapchain(),
            UINT64_MAX,
            imageAcquiredSemaphore,
            nullptr,
            &frameContext.swapchainImageIndex);
    }
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to acquire next image");
    }

    if (imagesInFlightFences[frameContext.swapchainImageIndex])
    {
        result = device.waitForFences(
            imagesInFlightFences[frameContext.swapchainImageIndex],
            true,
            UINT64_MAX);
        if (result != vk::Result::eSuccess)
        {
            throw std::runtime_error("Failed to wait for image fence");
        }
    }
    imagesInFlightFences[frameContext.swapchainImageIndex] = taskFinishedFence;

    device.resetFences(taskFinishedFence);

    frameContext.commandBuffer =
        rhiDevice.GetCommandBuffers()[frameContext.swapchainImageIndex];
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
    RendererSubmitPlan submitPlan;
    submitPlan.frameIndex = frameContext.frameIndex;
    submitPlan.swapchainImageIndex = frameContext.swapchainImageIndex;
    submitPlan.commandBuffer = frameContext.commandBuffer;
    submitPlan.imageAcquiredSemaphore = rhiDevice.GetImageAcquiredSemaphores()[frameContext.frameIndex];
    submitPlan.renderFinishedSemaphore =
        rhiDevice.GetRenderFinishedSemaphores()[frameContext.swapchainImageIndex];
    submitPlan.taskFinishedFence = rhiDevice.GetTaskFinishedFences()[frameContext.frameIndex];
    submitPlan.graphicsQueue = rhiDevice.GetGraphicsQueue();
    submitPlan.swapchain = rhiDevice.GetSwapchain();
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

        submitPlan.graphicsQueue.submit(submitInfo, submitPlan.taskFinishedFence);
    }

    if (submitPlan.frameIndex < frameFenceEpochs.size())
    {
        frameFenceEpochs[submitPlan.frameIndex] = submitPlan.submittedEpoch;
    }
    ResourceRetireQueue::GetInstance().MarkFrameSubmitted(submitPlan.submittedEpoch);

    vk::PresentInfoKHR presentInfo;
    vk::SwapchainKHR swapchain = submitPlan.swapchain;
    vk::Semaphore renderFinishedSemaphore = submitPlan.renderFinishedSemaphore;
    presentInfo
        .setSwapchains(swapchain)
        .setImageIndices(submitPlan.swapchainImageIndex)
        .setWaitSemaphores(renderFinishedSemaphore);

    {
        PROFILE_SCOPE("Render:Present");
        vk::Result result = submitPlan.graphicsQueue.presentKHR(presentInfo);
        if (result != vk::Result::eSuccess)
        {
            throw std::runtime_error("Failed to present image");
        }
    }
}

} // namespace VL
