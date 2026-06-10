#include "render/pass/passRuntime.h"

#include <stdexcept>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "passBarrier.h"
#include "pipeline/pipelineBase.h"
#include "profiler.h"
#include "renderGraph.h"

namespace VL
{

void PassRuntime::RecordPass(const std::string& passName, PassRuntimeContext& context) const
{
    PreparePassResources(context);

    if (context.renderPass.type == "shadow")
    {
        RecordShadowPass(context);
        return;
    }
    if (context.renderPass.type == "geometry")
    {
        RecordGeometryPass(context);
        return;
    }
    if (context.renderPass.type == "postProcess")
    {
        RecordPostProcessPass(context);
        return;
    }

    throw std::runtime_error(
        "Unsupported render pass type for pass '" +
        passName +
        "': " +
        context.renderPass.type);
}

void PassRuntime::PreparePassResources(PassRuntimeContext& context) const
{
    PROFILE_SCOPE("PassRuntime:PrepareForPass");
    PassBarrier::PrepareForPass(
        context.commandBuffer,
        context.renderGraph,
        context.passIndex,
        context.swapChainImageIndex);
}

void PassRuntime::BeginConfiguredRenderPass(PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;

    vk::RenderPassBeginInfo renderPassBeginInfo;
    renderPassBeginInfo.setRenderPass(renderPass.renderPass);
    renderPassBeginInfo.setFramebuffer(renderPass.framebuffers[context.swapChainImageIndex]);
    renderPassBeginInfo.setRenderArea(vk::Rect2D(
        vk::Offset2D(0, 0),
        vk::Extent2D(renderPass.width, renderPass.height)));
    renderPassBeginInfo.setClearValues(renderPass.clearValues);
    context.commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
}

void PassRuntime::RecordShadowPass(PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    // Shadow still uses a dedicated pass material while the full PassRuntime
    // model is landing. Keep render pass begin/end ownership local here.
    context.services.UpdateShadowGlobalUBOForPass(
        commandBuffer,
        renderPass.width,
        renderPass.height,
        renderPass.shadowCascadeIndex);
    BeginConfiguredRenderPass(context);

    std::shared_ptr<MaterialInstance> passMaterialInstance = renderPass.materialInstance.lock();
    if (!passMaterialInstance)
    {
        // 这里是个保底，确保pass执行，不会导致crash，以下类似操作同理
        commandBuffer.endRenderPass();
        return;
    }

    context.services.UpdateMaterialInstanceUBOForPass(passMaterialInstance);
    std::shared_ptr<Material> baseMaterial = passMaterialInstance->GetBaseMaterial().lock();
    if (!baseMaterial)
    {
        commandBuffer.endRenderPass();
        return;
    }

    const PipelineBase& renderPipeline = *baseMaterial->GetRenderPipeline();
    commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());

    RendererDrawContext drawContext{
        commandBuffer,
        renderPass,
        context.swapChainImageIndex,
        context.renderScene,
        context.resolvedRenderScene,
        context.services
    };
    drawExecutor.DrawShadowScene(renderPipeline, drawContext);

    commandBuffer.endRenderPass();
}

void PassRuntime::RecordGeometryPass(PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    context.services.UpdateGlobalUBOForPass(commandBuffer);
    BeginConfiguredRenderPass(context);

    RendererDrawContext drawContext{
        commandBuffer,
        renderPass,
        context.swapChainImageIndex,
        context.renderScene,
        context.resolvedRenderScene,
        context.services
    };
    drawExecutor.DrawGeometryScene(drawContext);

    commandBuffer.endRenderPass();
}

void PassRuntime::RecordPostProcessPass(PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    std::shared_ptr<MaterialInstance> passMaterialInstance = renderPass.materialInstance.lock();
    if (!passMaterialInstance)
    {
        return;
    }

    context.services.UpdateMaterialInstanceUBOForPass(passMaterialInstance);
    std::shared_ptr<Material> baseMaterial = passMaterialInstance->GetBaseMaterial().lock();
    if (!baseMaterial)
    {
        return;
    }

    const PipelineBase& renderPipeline = *baseMaterial->GetRenderPipeline();
    if (drawExecutor.PipelineUsesDescriptorSet(renderPipeline, GlobalSetIndex))
    {
        context.services.UpdateGlobalUBOForPass(commandBuffer);
    }

    BeginConfiguredRenderPass(context);
    commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());

    for (uint32_t setIndex = 0; setIndex < renderPass.GetDescriptorSets()[context.swapChainImageIndex].size(); ++setIndex)
    {
        if (!drawExecutor.PipelineUsesDescriptorSet(renderPipeline, setIndex))
        {
            continue;
        }

        commandBuffer.bindDescriptorSets(
            vk::PipelineBindPoint::eGraphics,
            renderPipeline.GetPipelineLayout(),
            setIndex,
            renderPass.GetDescriptorSets()[context.swapChainImageIndex][setIndex],
            nullptr);
    }

    renderPass.Draw(commandBuffer);
    commandBuffer.endRenderPass();
}

} // namespace VL
