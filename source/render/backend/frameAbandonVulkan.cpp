#include "render/backend/frameAbandonVulkan.h"

namespace VL
{
SwapchainFrameResult RecoverAcquiredFrame(
    const FrameAbandonVulkanSync& sync, AcquiredFrameDisposition& disposition,
    const FrameAbandonSubmit& submit, const FrameAbandonPresent& present)
{
    if (disposition.Phase() != AcquiredFramePhase::Discarded ||
        !sync.device || !sync.command || !sync.image || !sync.acquired ||
        !sync.finished || !sync.fence || !sync.imageView || !sync.extent.width ||
        !sync.extent.height || sync.format == vk::Format::eUndefined || !submit || !present)
        throw std::logic_error("Recovery requires a discarded acquired frame and live sync handles");
    vk::AttachmentDescription attachment;
    attachment.setFormat(sync.format).setSamples(vk::SampleCountFlagBits::e1)
        .setLoadOp(vk::AttachmentLoadOp::eClear).setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare).setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined).setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
    const vk::AttachmentReference color(0, vk::ImageLayout::eColorAttachmentOptimal);
    vk::SubpassDescription subpass;
    subpass.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics).setColorAttachments(color);
    vk::RenderPassCreateInfo passInfo;
    passInfo.setAttachments(attachment).setSubpasses(subpass);
    auto pass = sync.device.createRenderPassUnique(passInfo);
    vk::FramebufferCreateInfo framebufferInfo;
    framebufferInfo.setRenderPass(*pass).setAttachments(sync.imageView)
        .setWidth(sync.extent.width).setHeight(sync.extent.height).setLayers(1);
    auto framebuffer = sync.device.createFramebufferUnique(framebufferInfo);
    const auto record = [&] {
        sync.command.begin(vk::CommandBufferBeginInfo(vk::CommandBufferUsageFlagBits::eOneTimeSubmit));
        const vk::ClearValue clear(vk::ClearColorValue(std::array<float, 4>{0, 0, 0, 1}));
        vk::RenderPassBeginInfo begin;
        begin.setRenderPass(*pass).setFramebuffer(*framebuffer)
            .setRenderArea(vk::Rect2D({}, sync.extent)).setClearValues(clear);
        sync.command.beginRenderPass(begin, vk::SubpassContents::eInline);
        sync.command.endRenderPass();
        sync.command.end();
    };
    const vk::PipelineStageFlags waitStage = vk::PipelineStageFlagBits::eAllCommands;
    vk::SubmitInfo info;
    info.setWaitSemaphores(sync.acquired).setWaitDstStageMask(waitStage)
        .setCommandBuffers(sync.command).setSignalSemaphores(sync.finished);
    try
    {
        const auto result = CompleteDiscardedFrame(disposition, record,
            [&] { sync.device.resetFences(sync.fence); },
            [&] { submit(info, sync.fence); },
            [&] { return present(sync.finished); });
        if (sync.device.waitForFences(sync.fence, true, 5000000000ull) != vk::Result::eSuccess)
            throw std::runtime_error("Explicit recovery fence did not complete within five seconds");
        return result;
    }
    catch (...)
    {
        // 仅恢复本身已失败的终止路径等待 idle，避免销毁仍在飞的临时 clear 资源。
        try { sync.device.waitIdle(); } catch (...) {}
        throw;
    }
}
}
