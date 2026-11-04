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
namespace
{

struct PassViewportState
{
    vk::Viewport viewport;
    vk::Rect2D scissor;
};

PassViewportState BuildPassViewportState(const PassRuntimeContext& context)
{
    const Renderpass& renderPass = context.renderPass;
    const uint32_t passWidth = renderPass.width;
    const uint32_t passHeight = renderPass.height;
    PassViewportState state;
    // 游戏和独立 Viewport 都渲染完整 backing framebuffer；Dock 矩形只用于
    // 最终 UI 采样，不能改变 fullscreen pass 的 UV 覆盖范围。
    state.viewport
        .setX(0.0f)
        .setY(0.0f)
        .setWidth(static_cast<float>(passWidth))
        .setHeight(static_cast<float>(passHeight))
        .setMinDepth(0.0f)
        .setMaxDepth(1.0f);
    state.scissor
        .setOffset({ 0, 0 })
        .setExtent({ passWidth, passHeight });
    return state;
}

} // namespace

void PassRuntime::RecordPass(const std::string& passName, PassRuntimeContext& context) const
{
    PreparePassResources(context);

    switch (context.renderPass.type)
    {
    case RenderGraphPassType::Shadow:
        RecordShadowPass(context);
        return;
    case RenderGraphPassType::LocalShadow:
        RecordLocalShadowPass(context);
        return;
    case RenderGraphPassType::Geometry:
        RecordGeometryPass(context);
        return;
    case RenderGraphPassType::ForwardOpaque:
        RecordForwardOpaquePass(context);
        return;
    case RenderGraphPassType::ForwardTransparent:
        RecordForwardTransparentPass(context);
        return;
    case RenderGraphPassType::PostProcess:
        RecordPostProcessPass(context);
        return;
    case RenderGraphPassType::EditorGrid:
        RecordEditorGridPass(context);
        return;
    case RenderGraphPassType::Particle:
        RecordParticlePass(context);
        return;
    case RenderGraphPassType::Distortion:
        RecordDistortionPass(context);
        return;
    case RenderGraphPassType::DecalStencil:
        RecordDecalStencilPass(context);
        return;
    case RenderGraphPassType::Decal:
        RecordDecalPass(context);
        return;
    case RenderGraphPassType::Unknown:
    case RenderGraphPassType::AutoExposure:
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
    const PassViewportState viewportState = BuildPassViewportState(context);

    vk::RenderPassBeginInfo renderPassBeginInfo;
    const bool useViewport = context.renderToViewport &&
        renderPass.outputLoadOps.find("swapChain") != renderPass.outputLoadOps.end();
    if (useViewport)
    {
        if (!renderPass.sceneViewportRenderPass)
        {
            throw std::runtime_error(
                "Scene viewport output is unavailable for pass: " + renderPass.name);
        }
        renderPassBeginInfo.setRenderPass(renderPass.sceneViewportRenderPass);
        renderPassBeginInfo.setFramebuffer(
            renderPass.sceneViewportFramebuffers.at(context.swapChainImageIndex));
    }
    else
    {
        renderPassBeginInfo.setRenderPass(renderPass.renderPass);
        renderPassBeginInfo.setFramebuffer(renderPass.framebuffers.at(context.swapChainImageIndex));
    }
    renderPassBeginInfo.setRenderArea(vk::Rect2D(
        vk::Offset2D(0, 0),
        vk::Extent2D(renderPass.width, renderPass.height)));
    renderPassBeginInfo.setClearValues(renderPass.clearValues);
    context.commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
    context.commandBuffer.setViewport(0, viewportState.viewport);
    context.commandBuffer.setScissor(0, viewportState.scissor);
}

void PassRuntime::UpdateSceneGlobalUBO(PassRuntimeContext& context) const
{
    context.services.UpdateGlobalUBOForPass(context.commandBuffer);
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

void PassRuntime::RecordLocalShadowPass(
    PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;
    const uint32_t lightIndex = renderPass.localShadowIndex;

    // 未激活的层仍要 begin/end：render graph 依赖这个 pass 把
    // localShadowMap 从上一状态转换到 depth attachment，后续采样才合法。
    if (!context.services.IsLocalShadowActive(lightIndex))
    {
        BeginConfiguredRenderPass(context);
        commandBuffer.endRenderPass();
        return;
    }

    std::shared_ptr<MaterialInstance> passMaterialInstance =
        renderPass.materialInstance.lock();
    if (!passMaterialInstance)
    {
        throw std::runtime_error(
            "LocalShadow pass '" + renderPass.name +
            "' has no common opaque pass material instance");
    }
    std::shared_ptr<Material> baseMaterial =
        passMaterialInstance->GetBaseMaterial().lock();
    if (!baseMaterial || !baseMaterial->GetLocalShadowPipeline())
    {
        throw std::runtime_error(
            "LocalShadow pass '" + renderPass.name +
            "' common opaque material has no LocalShadow pipeline");
    }

    const PipelineBase& commonLocalShadowPipeline =
        *baseMaterial->GetLocalShadowPipeline();
    const uint32_t mode =
        context.services.GetLocalShadowMode(lightIndex);
    const uint32_t drawCount = mode == 1 ? 8u : 1u;
    // caster 剔除只依赖灯的冻结参数；点光 8 象限共用同一体积。
    // 没有服务的缩略图/离线会话返回 nullptr，此时退回全量绘制。
    const LightSnapshot* localShadowLight =
        context.services.GetLocalShadowLight(lightIndex);

    RendererDrawContext drawContext{
        commandBuffer,
        renderPass,
        context.swapChainImageIndex,
        context.renderScene,
        context.resolvedRenderScene,
        context.services
    };

    for (uint32_t octant = 0; octant < drawCount; ++octant)
    {
        context.services.UpdateLocalShadowUBOForPass(
            commandBuffer,
            renderPass.width,
            renderPass.height,
            lightIndex,
            octant,
            mode);
        BeginConfiguredRenderPass(context);
        if (octant == 0)
        {
            // loadOp=load 是 pass 合同要求的 layout/load 语义；每个灯开始时
            // 用 clearAttachments 清空当前 slice，后续象限必须保留已写深度。
            vk::ClearAttachment attachment;
            attachment
                .setAspectMask(vk::ImageAspectFlagBits::eDepth)
                .setColorAttachment(0)
                .setClearValue(vk::ClearValue().setDepthStencil(
                    vk::ClearDepthStencilValue{ 1.0f, 0 }));
            vk::ClearRect clearRect;
            clearRect
                .setRect(vk::Rect2D(
                    vk::Offset2D(0, 0),
                    vk::Extent2D(renderPass.width, renderPass.height)))
                .setBaseArrayLayer(0)
                .setLayerCount(1);
            commandBuffer.clearAttachments(attachment, clearRect);
        }

        drawExecutor.DrawLocalShadowScene(
            commonLocalShadowPipeline,
            drawContext,
            localShadowLight,
            // 只在第一个象限/聚光唯一一次上报，避免 8 象限重复计数。
            octant == 0);
        commandBuffer.endRenderPass();
    }
}

void PassRuntime::RecordGeometryPass(PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    UpdateSceneGlobalUBO(context);
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
    UpdateSceneGlobalUBO(context);
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

void PassRuntime::RecordForwardTransparentPass(
    PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    // 透明表面复用场景的 Global/Light 数据，在已有 sceneColor 上混合；
    // 深度测试与写入策略由具体材质管线合同决定。
    UpdateSceneGlobalUBO(context);
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
    if (!BindPassMaterialPipeline(context))
    {
        return;
    }

    context.renderPass.Draw(context.commandBuffer);
    context.commandBuffer.endRenderPass();
}

void PassRuntime::RecordEditorGridPass(PassRuntimeContext& context) const
{
    // 参考网格与 sky 同构：只向 sceneColor 合成。区别在深度状态由 pass 决定 ——
    // 网格按世界空间投影，depthTestEnable 让它被场景几何正确遮挡，depthWriteEnable
    // 关闭使它不改变后续 pass 看到的深度。
    if (!BindPassMaterialPipeline(context))
    {
        return;
    }

    context.services.DrawEditorGridGeometry(context.commandBuffer);
    context.commandBuffer.endRenderPass();
}

void PassRuntime::RecordParticlePass(
    PassRuntimeContext& context) const
{
    // 粒子与透明表面同属合成阶段：load 已有 sceneColor，sceneDepth 通过
    // set 3 作为只读输入；pass 本身不挂深度附件，避免采样/写入反馈环。
    BeginConfiguredRenderPass(context);
    context.services.RecordParticlePass(
        context.commandBuffer,
        context.renderPass,
        context.swapChainImageIndex);
    context.commandBuffer.endRenderPass();
}

void PassRuntime::RecordDistortionPass(
    PassRuntimeContext& context) const
{
    // 扰动 pass 只写 distortion 附件；sceneDepth 通过 set 3 只读采样。
    // 不绑定 sceneColor，避免同一帧内同时把它作为附件与采样源。
    BeginConfiguredRenderPass(context);
    context.services.RecordDistortionPass(
        context.commandBuffer,
        context.renderPass,
        context.swapChainImageIndex);
    context.commandBuffer.endRenderPass();
}

void PassRuntime::RecordDecalPass(
    PassRuntimeContext& context) const
{
    BeginConfiguredRenderPass(context);
    context.services.RecordDecalPass(
        context.commandBuffer,
        context.renderPass,
        context.swapChainImageIndex,
        context.renderScene);
    context.commandBuffer.endRenderPass();
}

void PassRuntime::RecordDecalStencilPass(
    PassRuntimeContext& context) const
{
    // mask pass 只有一个 D24S8 深度/stencil 附件，不采样 sceneDepth。
    // 它与后续 decals pass 分离，避免 decal shader 采样 sceneDepth 时
    // 同一 pass 又把 depth 当 attachment。
    BeginConfiguredRenderPass(context);
    context.services.RecordDecalStencilPass(
        context.commandBuffer,
        context.renderPass,
        context.swapChainImageIndex,
        context.renderScene);
    context.commandBuffer.endRenderPass();
}

bool PassRuntime::BindPassMaterialPipeline(PassRuntimeContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    std::shared_ptr<MaterialInstance> passMaterialInstance = renderPass.materialInstance.lock();
    if (!passMaterialInstance)
    {
        return false;
    }

    context.services.UpdateMaterialInstanceUBOForPass(passMaterialInstance);
    std::shared_ptr<Material> baseMaterial = passMaterialInstance->GetBaseMaterial().lock();
    if (!baseMaterial)
    {
        return false;
    }

    const PipelineBase& renderPipeline = *baseMaterial->GetRenderPipeline();
    if (drawExecutor.PipelineUsesDescriptorSet(renderPipeline, GlobalSetIndex))
    {
        UpdateSceneGlobalUBO(context);
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

    return true;
}

} // namespace VL
