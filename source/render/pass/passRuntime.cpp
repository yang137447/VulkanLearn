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

    switch (context.renderPass.type)
    {
    case RenderGraphPassType::Shadow:
        RecordShadowPass(context);
        return;
    case RenderGraphPassType::Geometry:
        RecordGeometryPass(context);
        return;
    case RenderGraphPassType::ForwardOpaque:
        RecordForwardOpaquePass(context);
        return;
    case RenderGraphPassType::ForwardEyeInner:
        RecordForwardEyeInnerPass(context);
        return;
    case RenderGraphPassType::ForwardEyeCornea:
        RecordForwardEyeCorneaPass(context);
        return;
    case RenderGraphPassType::ForwardTransparent:
        RecordForwardTransparentPass(context);
        return;
    case RenderGraphPassType::PostProcess:
        RecordPostProcessPass(context);
        return;
    case RenderGraphPassType::Unknown:
        break;
    }

    throw std::runtime_error(
        "Unsupported render pass type for pass '" +
        passName +
        "': " +
        std::string(
            RenderGraphPassTypeToString(
                context.renderPass.type)));
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

    // Keep the compiled shadow pass and its layout transition even when CSM is
    // disabled: the geometry pass still expects shadowMap to leave this pass
    // in a valid shader-readable layout.
    if (!context.services.IsCsmEnabled() ||
        !context.services.IsShadowCascadeActive(
            renderPass.shadowCascadeIndex))
    {
        BeginConfiguredRenderPass(context);
        commandBuffer.endRenderPass();
        return;
    }

    std::shared_ptr<MaterialInstance> passMaterialInstance = renderPass.materialInstance.lock();
    if (!passMaterialInstance)
    {
        throw std::runtime_error(
            "Shadow pass '" + renderPass.name + "' has no common opaque pass material instance");
    }

    std::shared_ptr<Material> baseMaterial = passMaterialInstance->GetBaseMaterial().lock();
    if (!baseMaterial)
    {
        throw std::runtime_error(
            "Shadow pass '" + renderPass.name + "' common opaque material has expired");
    }

    const PipelineBase& commonOpaquePipeline = *baseMaterial->GetRenderPipeline();

    context.services.UpdateShadowGlobalUBOForPass(
        commandBuffer,
        renderPass.width,
        renderPass.height,
        renderPass.shadowCascadeIndex);
    BeginConfiguredRenderPass(context);

    RendererDrawContext drawContext{
        commandBuffer,
        renderPass,
        context.swapChainImageIndex,
        context.renderScene,
        context.resolvedRenderScene,
        context.services
    };
    drawExecutor.DrawShadowScene(commonOpaquePipeline, drawContext);

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

void PassRuntime::RecordForwardOpaquePass(
    PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    // Eye 先写入 sceneColor/depth，再由 sky 和透明 pass 继续消费；
    // 该 pass 不参与 GBuffer，避免普通 deferred packing 被模型专用字段挤占。
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
    drawExecutor.DrawForwardOpaqueScene(drawContext);

    commandBuffer.endRenderPass();
}

void PassRuntime::RecordForwardEyeInnerPass(
    PassRuntimeContext& context) const
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
    drawExecutor.DrawForwardEyeInnerScene(drawContext);
    commandBuffer.endRenderPass();
}

void PassRuntime::RecordForwardEyeCorneaPass(
    PassRuntimeContext& context) const
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
    drawExecutor.DrawForwardEyeCorneaScene(drawContext);
    commandBuffer.endRenderPass();
}

void PassRuntime::RecordForwardTransparentPass(
    PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    // 透明表面复用场景的 Global/Light 数据，在已有 sceneColor 上混合；
    // 深度只测试不写入，具体状态由 forwardTransparent 的 pass contract 决定。
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
    drawExecutor.DrawForwardTransparentScene(drawContext);

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
