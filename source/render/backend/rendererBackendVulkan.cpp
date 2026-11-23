#include "render/backend/rendererBackendVulkan.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <tuple>

#include "pipeline/pipelineFactory.h"
#include "pipeline/graphicsPipelineBuilder.h"
#include "profiler.h"
#include "render/backend/frameEpochRing.h"
#include "render/backend/frameAbandonVulkan.h"
#include "render/backend/screenshotReadback.h"
#include "render/rhi/vulkan/rhiDeviceVulkan.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererThumbnailImages.h"
#include "vulkanDebug.h"

namespace VL
{
namespace
{

bool WriteScreenshotBmp(
    const std::filesystem::path& path,
    const uint8_t* pixels,
    uint32_t width,
    uint32_t height,
    vk::Format format,
    std::string& outError)
{
    const std::optional<ScreenshotReadback::BmpLayout> layout =
        ScreenshotReadback::MakeBmpLayout(width, height);
    if (pixels == nullptr || !layout.has_value())
    {
        outError = "Screenshot readback returned an empty image.";
        return false;
    }

    if (!ScreenshotReadback::IsSupportedScreenshotFormat(format))
    {
        outError = "Unsupported swapchain screenshot format: " + vk::to_string(format);
        return false;
    }

    try
    {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty())
        {
            std::filesystem::create_directories(parent);
        }

        const std::array<uint8_t, 54> header =
            ScreenshotReadback::MakeBmpHeader(*layout);

        std::ofstream output(path, std::ios::binary);
        if (!output)
        {
            outError = "Cannot open screenshot path: " + path.string();
            return false;
        }
        output.write(reinterpret_cast<const char*>(header.data()), header.size());

        std::vector<uint8_t> row(layout->paddedRowBytes, 0);
        const bool isBgra = ScreenshotReadback::IsBgraFormat(format);
        for (uint32_t pixelY = 0; pixelY < height; ++pixelY)
        {
            const uint8_t* sourceRow =
                pixels + static_cast<size_t>(pixelY) * width * 4u;
            for (uint32_t pixelX = 0; pixelX < width; ++pixelX)
            {
                ScreenshotReadback::ConvertPixelToBgr24(
                    sourceRow + pixelX * 4u,
                    isBgra,
                    row.data() + pixelX * 3u);
            }
            output.write(reinterpret_cast<const char*>(row.data()), row.size());
        }
        if (!output)
        {
            outError = "Failed while writing screenshot: " + path.string();
            return false;
        }
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        return false;
    }

    return true;
}

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

} // namespace

bool RendererBackendVulkan::wireframeCapabilityEnabled = false;

bool RendererBackendVulkan::SupportsWireframe() const
{
    return rhiDevice->IsFillModeNonSolidEnabled();
}

bool RendererBackendVulkan::SupportsWireframeCapability()
{
    return wireframeCapabilityEnabled;
}

RendererBackendVulkan::RendererBackendVulkan()
    : rhiDevice(std::make_unique<RHIDeviceVulkan>()),
      thumbnailImages(std::make_unique<RendererThumbnailImages>(*this))
{
}

RendererBackendVulkan::~RendererBackendVulkan()
{
    // EngineLoop 已停止 RT 并等待 idle；必须在 backend 的 handle 映射和 device 之前释放。
    thumbnailImages.reset();
}

std::shared_ptr<const RendererGpuImageToken> RendererBackendVulkan::RecordThumbnailImage(
    vk::CommandBuffer command, vk::Image source, uint32_t width, uint32_t height)
{
    return thumbnailImages->RecordCopy(command, source, width, height);
}

vk::DescriptorImageInfo RendererBackendVulkan::GetThumbnailImageDescriptor(
    uint64_t id, bool linearSampling) const
{
    return thumbnailImages->GetDescriptor(id, linearSampling);
}

void RendererBackendVulkan::CollectThumbnailImages()
{
    thumbnailImages->CollectUnused();
}

void RendererBackendVulkan::Initialize(
    std::vector<const char*>& instanceExtensions,
    SDL_Window* window,
    VL::PresentModePreference presentMode)
{
    rhiDevice->Initialize(instanceExtensions, window, presentMode);
    wireframeCapabilityEnabled =
        rhiDevice->IsFillModeNonSolidEnabled();
    frameFenceEpochs.assign(rhiDevice->GetFrameFenceCount(), 0);
    initialized = true;
}

void RendererBackendVulkan::WaitIdle()
{
    rhiDevice->WaitIdle();
    ReleasePendingScreenshotAfterIdle();
}

void RendererBackendVulkan::ReleasePendingScreenshotAfterIdle()
{
    // 只允许在 device idle 之后调用。失败/被丢弃的截图请求可能留下 staging buffer；
    // 正常情况下 CompleteSwapchainScreenshot 已经释放，这里只是安全兜底。
    if (!pendingScreenshot.readbackBuffer && !pendingScreenshot.readbackMemory)
        return;
    try
    {
        DestroyBuffer(
            pendingScreenshot.readbackBuffer,
            pendingScreenshot.readbackMemory);
    }
    catch (...)
    {
    }
    pendingScreenshot = {};
}

bool RendererBackendVulkan::RecordSwapchainScreenshot(
    const RendererFrameContext& frameContext,
    const std::string& path,
    std::string& outError)
{
    if (!initialized)
    {
        outError = "Cannot record a screenshot before the renderer is initialized.";
        return false;
    }
    if (pendingScreenshot.readbackBuffer || pendingScreenshot.readbackMemory)
    {
        outError = "A screenshot readback is already pending.";
        return false;
    }

    VL::PresentTarget& presentTarget = rhiDevice->GetPresentTarget();
    const vk::Extent2D extent = presentTarget.extent;
    const vk::Format format = presentTarget.surfaceFormat.format;
    const std::vector<vk::Image>& images = presentTarget.images;
    if (frameContext.swapchainImageIndex >= images.size())
    {
        outError = "Swapchain image index is out of range: " +
            std::to_string(frameContext.swapchainImageIndex);
        return false;
    }

    const vk::DeviceSize readbackSize =
        static_cast<vk::DeviceSize>(extent.width) *
        static_cast<vk::DeviceSize>(extent.height) * 4u;
    try
    {
        vk::Buffer readbackBuffer;
        vk::DeviceMemory readbackMemory;
        std::tie(readbackBuffer, readbackMemory) = CreateBuffer(
            readbackSize,
            vk::BufferUsageFlagBits::eTransferDst,
            ScreenshotReadback::StagingMemoryFlags,
            "DebugScreenshotReadback");

        vk::CommandBuffer commandBuffer = frameContext.commandBuffer;
        vk::ImageMemoryBarrier toTransfer;
        toTransfer
            .setOldLayout(vk::ImageLayout::ePresentSrcKHR)
            .setNewLayout(vk::ImageLayout::eTransferSrcOptimal)
            .setSrcAccessMask(vk::AccessFlagBits::eNone)
            .setDstAccessMask(vk::AccessFlagBits::eTransferRead)
            .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
            .setImage(images[frameContext.swapchainImageIndex])
            .setSubresourceRange(vk::ImageSubresourceRange()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setBaseMipLevel(0)
                .setLevelCount(1)
                .setBaseArrayLayer(0)
                .setLayerCount(1));
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eBottomOfPipe,
            vk::PipelineStageFlagBits::eTransfer,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            toTransfer);

        vk::BufferImageCopy copyRegion;
        copyRegion
            .setBufferOffset(0)
            .setBufferRowLength(0)
            .setBufferImageHeight(0)
            .setImageSubresource(vk::ImageSubresourceLayers()
                .setAspectMask(vk::ImageAspectFlagBits::eColor)
                .setMipLevel(0)
                .setBaseArrayLayer(0)
                .setLayerCount(1))
            .setImageOffset(vk::Offset3D{0, 0, 0})
            .setImageExtent(vk::Extent3D{extent.width, extent.height, 1});
        commandBuffer.copyImageToBuffer(
            images[frameContext.swapchainImageIndex],
            vk::ImageLayout::eTransferSrcOptimal,
            readbackBuffer,
            copyRegion);

        vk::ImageMemoryBarrier backToPresent = toTransfer;
        backToPresent
            .setOldLayout(vk::ImageLayout::eTransferSrcOptimal)
            .setNewLayout(vk::ImageLayout::ePresentSrcKHR)
            .setSrcAccessMask(vk::AccessFlagBits::eTransferRead)
            .setDstAccessMask(vk::AccessFlagBits::eNone);
        commandBuffer.pipelineBarrier(
            vk::PipelineStageFlagBits::eTransfer,
            vk::PipelineStageFlagBits::eBottomOfPipe,
            vk::DependencyFlags(),
            nullptr,
            nullptr,
            backToPresent);

        pendingScreenshot.readbackBuffer = readbackBuffer;
        pendingScreenshot.readbackMemory = readbackMemory;
        pendingScreenshot.extent = extent;
        pendingScreenshot.format = format;
        pendingScreenshot.path = path;
        // copy 已经录进这一帧的命令缓冲；记下槽位与 fence，Complete 只等这次提交。
        pendingScreenshot.frameIndex = frameContext.frameIndex;
        pendingScreenshot.submitFence =
            rhiDevice->GetFrameSyncResources(frameContext.frameIndex).taskFinishedFence;
        pendingScreenshot.submittedEpoch = 0;
        return true;
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        return false;
    }
}

bool RendererBackendVulkan::CompleteSwapchainScreenshot(std::string& outError)
{
    if (!pendingScreenshot.readbackBuffer || !pendingScreenshot.readbackMemory)
    {
        return false;
    }

    // 录制阶段已经在 acquired image 上完成 copy，且 SubmitFrame 把这次提交的 epoch
    // 盖进了 pendingScreenshot。epoch 为 0 说明 copy 从未随某一帧提交（例如录制后
    // 命令被丢弃），此时既不能等 fence，也不能读 staging。
    if (pendingScreenshot.submittedEpoch == 0 ||
        lastSubmittedFrameIndex != pendingScreenshot.frameIndex)
    {
        outError = "Screenshot readback was recorded but never submitted.";
        return false;
    }

    try
    {
        // R15 的截图停顿来自 device-wide WaitIdle（含 present 队列排空）+ 非 cached
        // 3.7 MiB 主机读。现在只等承载 copy 的那次提交自己的 frame fence：GPU 执行完
        // 本帧命令即返回，与其它在途/已 present 的帧无关。等待仍有上限，超时说明 GPU
        // 已挂起，由调用方按失败上报；pending 保留到下一个 device idle 安全点释放。
        if (!rhiDevice->WaitForFence(
                pendingScreenshot.submitFence,
                ScreenshotReadback::FenceTimeoutNanoseconds))
        {
            outError = "Timed out waiting for the screenshot readback fence.";
            return false;
        }

        const vk::DeviceSize readbackSize =
            static_cast<vk::DeviceSize>(pendingScreenshot.extent.width) *
            static_cast<vk::DeviceSize>(pendingScreenshot.extent.height) * 4u;
        void* mapped = MapMemory(pendingScreenshot.readbackMemory, readbackSize);
        const bool writeSucceeded = WriteScreenshotBmp(
            std::filesystem::path(pendingScreenshot.path),
            static_cast<const uint8_t*>(mapped),
            pendingScreenshot.extent.width,
            pendingScreenshot.extent.height,
            pendingScreenshot.format,
            outError);
        UnmapMemory(pendingScreenshot.readbackMemory);
        DestroyBuffer(
            pendingScreenshot.readbackBuffer,
            pendingScreenshot.readbackMemory);
        pendingScreenshot = {};
        return writeSucceeded;
    }
    catch (const std::exception& exception)
    {
        outError = exception.what();
        try
        {
            DestroyBuffer(
                pendingScreenshot.readbackBuffer,
                pendingScreenshot.readbackMemory);
        }
        catch (...)
        {
        }
        pendingScreenshot = {};
        return false;
    }
}

RendererBackendResourceIdentityCounts
RendererBackendVulkan::CaptureResourceIdentityCounts() const noexcept
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
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
    {
        std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
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
    }
    std::sort(names.images.begin(), names.images.end());
    std::sort(names.imageViews.begin(), names.imageViews.end());
    std::sort(names.samplers.begin(), names.samplers.end());
    return names;
}

void RendererBackendVulkan::RecreateSwapchain(int width, int height)
{
    // Swapchain 重建会替换图像、view、命令缓冲和每图像同步对象。调用方必须先
    // 让渲染线程停在安全点；这里额外等待 device idle，确保在途 submit/present
    // 不会与销毁并发。共享 command pool 不在重建路径里销毁。
    rhiDevice->WaitIdle();
    ReleasePendingScreenshotAfterIdle();
    rhiDevice->RecreateSwapchain(rhiDevice->GetPresentTarget(), width, height);
    // RHI 重建前已等待 device idle；旧 fence 对应的提交和退休 epoch 均已完成。
    ResourceRetireQueue::GetInstance().CollectCompletedEpoch(
        ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch());
    frameFenceEpochs.assign(rhiDevice->GetFrameFenceCount(), 0);
    swapchainRecreateRequired.store(false, std::memory_order_release);
}

RendererFrameContext RendererBackendVulkan::BeginFrame(uint32_t currentFrame)
{
    if (!initialized)
    {
        throw std::runtime_error("RendererBackendVulkan is not initialized");
    }

    // 本帧作用于哪个呈现目标。目前只有主窗口，因此与 GetPresentTarget() 一致；
    // 声明成局部引用是为了让本函数其余部分只依赖"目标"，不依赖"唯一那个目标"。
    VL::PresentTarget& presentTarget = rhiDevice->GetPresentTarget();

    RendererFrameContext frameContext;
    if (NeedsSwapchainRecreate())
    {
        return frameContext;
    }
    frameContext.swapchainImageCount = rhiDevice->GetSwapchainImageCount(presentTarget);
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

    frameContext.completedEpoch =
        ConsumeCompletedFrameEpoch(frameFenceEpochs, frameContext.frameIndex);
    if (frameContext.completedEpoch != 0)
    {
        ResourceRetireQueue::GetInstance().CollectCompletedEpoch(
            frameContext.completedEpoch);
    }
    CollectThumbnailImages();

    {
        PROFILE_SCOPE("Render:AcquireImage");
        const SwapchainFrameResult acquireResult = rhiDevice->AcquireNextSwapchainImage(
            presentTarget,
            frameSync.imageAcquiredSemaphore,
            frameContext.swapchainImageIndex);
        if (RequiresSwapchainRecreate(acquireResult))
        {
            swapchainRecreateRequired.store(true, std::memory_order_release);
        }
        if (!HasAcquiredSwapchainImage(acquireResult))
        {
            // 未取得 image 时不重置 fence，也不消费 acquire semaphore。
            return frameContext;
        }
    }

    const vk::Fence imageInFlightFence =
        rhiDevice->GetImageInFlightFence(presentTarget, frameContext.swapchainImageIndex);
    if (imageInFlightFence)
    {
        rhiDevice->WaitForFence(imageInFlightFence);
    }
    rhiDevice->SetImageInFlightFence(
        presentTarget,
        frameContext.swapchainImageIndex,
        frameSync.taskFinishedFence);

    const VulkanSwapchainImageSyncResources imageSync =
        rhiDevice->GetSwapchainImageSyncResources(presentTarget, frameContext.swapchainImageIndex);
    frameContext.commandBuffer = imageSync.commandBuffer;
    frameContext.commandBuffer.reset();

    vk::CommandBufferBeginInfo beginInfo;
    beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    frameContext.commandBuffer.begin(beginInfo);
    frameContext.ready = true;
    return frameContext;
}

uint32_t RendererBackendVulkan::GetFrameFenceCount() const
{
    if (!initialized)
        return 0;
    return rhiDevice->GetFrameFenceCount();
}

void RendererBackendVulkan::SubmitFrame(
    const RendererFrameContext& frameContext,
    uint32_t currentFrame)
{
    if (!initialized)
    {
        throw std::runtime_error("RendererBackendVulkan is not initialized");
    }

    // 与 BeginFrame 取同一个目标：帧的 acquire 与 present 必须落在同一个呈现目标上，
    // 否则同步对象与图像索引会跨目标错配。
    VL::PresentTarget& presentTarget = rhiDevice->GetPresentTarget();

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
        rhiDevice->GetSwapchainImageSyncResources(presentTarget, frameContext.swapchainImageIndex);
    submitPlan.imageAcquiredSemaphore = frameSync.imageAcquiredSemaphore;
    submitPlan.renderFinishedSemaphore = imageSync.renderFinishedSemaphore;
    submitPlan.taskFinishedFence = frameSync.taskFinishedFence;
    submitPlan.waitDstStageMask = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    submitPlan.submittedEpoch = static_cast<uint64_t>(currentFrame) + 1;

    vk::CommandBuffer commandBuffer = submitPlan.commandBuffer;
    if (!frameContext.ready ||
        frameContext.disposition.Phase() != AcquiredFramePhase::Recording)
        throw std::logic_error("Submit requires a live recording frame");
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

        rhiDevice->ResetFence(submitPlan.taskFinishedFence);
        rhiDevice->SubmitToGraphicsQueue(submitInfo, submitPlan.taskFinishedFence);
        frameContext.disposition.Submitted();
    }

    RecordSubmittedFrameEpoch(
        frameFenceEpochs,
        submitPlan.frameIndex,
        submitPlan.submittedEpoch);
    ResourceRetireQueue::GetInstance().MarkFrameSubmitted(submitPlan.submittedEpoch);
    hasSubmittedFrame = true;
    lastSubmittedFrameIndex = submitPlan.frameIndex;
    // 截图 copy 就在刚提交的命令缓冲里：把提交 epoch 绑定到 pending 请求，
    // Complete 用它确认“允许读 staging”，并只等这一次提交的 fence。
    if (pendingScreenshot.readbackBuffer &&
        pendingScreenshot.submittedEpoch == 0 &&
        pendingScreenshot.frameIndex == submitPlan.frameIndex)
    {
        pendingScreenshot.submittedEpoch = submitPlan.submittedEpoch;
    }

    {
        PROFILE_SCOPE("Render:Present");
        const SwapchainFrameResult presentResult = rhiDevice->PresentSwapchainImage(
            presentTarget,
            submitPlan.renderFinishedSemaphore,
            submitPlan.swapchainImageIndex);
        if (RequiresSwapchainRecreate(presentResult))
        {
            // 提交和 epoch 已登记；即使 present 失效也必须等 GT 的安全边界重建。
            swapchainRecreateRequired.store(true, std::memory_order_release);
        }
    }
}

void RendererBackendVulkan::DiscardFrameCommands(const RendererFrameContext& frameContext)
{
    if (!frameContext.ready ||
        frameContext.disposition.Phase() != AcquiredFramePhase::Recording)
        throw std::logic_error("Only unsubmitted acquired frame commands can be discarded");
    // 本帧如果登记了截图 copy，命令被丢弃后 staging 再也不会被写；这里立即释放，
    // 避免 pending 卡住后续截图请求（buffer 里没有已提交的 GPU 访问）。
    if (pendingScreenshot.readbackBuffer && pendingScreenshot.submittedEpoch == 0 &&
        pendingScreenshot.frameIndex == frameContext.frameIndex)
    {
        try
        {
            DestroyBuffer(
                pendingScreenshot.readbackBuffer,
                pendingScreenshot.readbackMemory);
        }
        catch (...)
        {
        }
        pendingScreenshot = {};
    }
    frameContext.commandBuffer.reset();
    frameContext.disposition.Discarded();
}

FrameAbortResult RendererBackendVulkan::FrameAbandonAfterAcquire(
    const RendererFrameContext& frameContext, uint32_t currentFrame)
{
    if (!initialized || !frameContext.ready)
        throw std::logic_error("Frame abandon requires a live backend acquired frame");
    auto& target = rhiDevice->GetPresentTarget();
    const auto frameSync = rhiDevice->GetFrameSyncResources(frameContext.frameIndex);
    const auto imageSync = rhiDevice->GetSwapchainImageSyncResources(target, frameContext.swapchainImageIndex);
    const FrameAbandonVulkanSync sync{rhiDevice->GetDevice(), frameContext.commandBuffer,
        rhiDevice->GetSwapchainImages(target).at(frameContext.swapchainImageIndex),
        frameSync.imageAcquiredSemaphore, imageSync.renderFinishedSemaphore, frameSync.taskFinishedFence,
        rhiDevice->GetSwapchainImageViews(target).at(frameContext.swapchainImageIndex),
        rhiDevice->GetSwapchainImageFormat(target), target.extent};
    const auto presentResult = RecoverAcquiredFrame(sync, frameContext.disposition,
        [this](const vk::SubmitInfo& info, vk::Fence fence) {
            rhiDevice->SubmitToGraphicsQueue(info, fence);
        },
        [this, &target, &frameContext](vk::Semaphore finished) {
            return rhiDevice->PresentSwapchainImage(target, finished, frameContext.swapchainImageIndex);
        });
    const uint64_t epoch = static_cast<uint64_t>(currentFrame) + 1;
    RecordSubmittedFrameEpoch(frameFenceEpochs, frameContext.frameIndex, epoch);
    ResourceRetireQueue::GetInstance().MarkFrameSubmitted(epoch);
    hasSubmittedFrame = true;
    lastSubmittedFrameIndex = frameContext.frameIndex;
    const bool recreate = RequiresSwapchainRecreate(presentResult);
    if (recreate)
        swapchainRecreateRequired.store(true, std::memory_order_release);
    return {epoch, frameContext.frameIndex, frameContext.swapchainImageIndex, recreate};
}

void RendererBackendVulkan::WaitForPreviousFrameFence(
    uint32_t currentFrameIndex)
{
    if (!hasSubmittedFrame ||
        lastSubmittedFrameIndex == currentFrameIndex)
    {
        return;
    }
    const uint32_t frameFenceCount =
        rhiDevice->GetFrameFenceCount();
    if (frameFenceCount == 0 ||
        lastSubmittedFrameIndex >= frameFenceCount)
    {
        return;
    }
    const VulkanFrameSyncResources previous =
        rhiDevice->GetFrameSyncResources(lastSubmittedFrameIndex);
    rhiDevice->WaitForFence(previous.taskFinishedFence);
}

} // namespace VL
