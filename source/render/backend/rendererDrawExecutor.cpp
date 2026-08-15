#include "render/backend/rendererDrawExecutor.h"

#include "commonFunction.h"
#include "material.h"
#include "pipeline/pipelineBase.h"
#include "renderGraph.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "renderableObject.h"
#include "shaderReflect.h"
#include "vulkanDebug.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace VL
{
namespace
{

struct ResolvedDrawResources
{
    std::shared_ptr<RenderableObject> renderableObject;
    RendererObjectGpuResources* objectResources = nullptr;
    const RenderDrawPacket* drawPacket = nullptr;
};

std::string BuildResolvedDrawErrorPrefix(const char* passName, const ResolvedDrawPacket& draw)
{
    return std::string("RendererDrawExecutor.") + passName +
        " received invalid resolved draw packet at index " +
        std::to_string(draw.drawPacketIndex) + ": ";
}

ResolvedDrawResources RequireResolvedDrawResources(
    const char* passName,
    const ResolvedDrawPacket& draw,
    const RenderScene& renderScene)
{
    if (draw.drawPacketIndex >= renderScene.drawPackets.size())
    {
        throw std::runtime_error(
            BuildResolvedDrawErrorPrefix(passName, draw) +
            "draw packet count is " +
            std::to_string(renderScene.drawPackets.size()) + ".");
    }

    const RenderDrawPacket& drawPacket = renderScene.drawPackets[draw.drawPacketIndex];
    std::shared_ptr<RenderableObject> renderableObject = draw.renderableObject.lock();
    if (!renderableObject)
    {
        throw std::runtime_error(
            BuildResolvedDrawErrorPrefix(passName, draw) +
            "renderable object expired for '" +
            drawPacket.debugName +
            "' (mesh='" +
            drawPacket.mesh.key +
            "').");
    }
    if (!draw.objectResourceEntry)
    {
        throw std::runtime_error(
            BuildResolvedDrawErrorPrefix(passName, draw) +
            "object GPU resource entry is missing for '" +
            drawPacket.debugName +
            "'. RendererObjectResourceRegistry should initialize resolved draw resources before pass recording.");
    }

    ResolvedDrawResources resources;
    resources.renderableObject = std::move(renderableObject);
    resources.objectResources = &draw.objectResourceEntry->GetResources();
    resources.drawPacket = &drawPacket;
    return resources;
}

} // namespace

bool RendererDrawExecutor::PipelineUsesDescriptorSet(
    const PipelineBase& pipeline,
    uint32_t setIndex) const
{
    for (const auto& binding : pipeline.GetShaderBindings())
    {
        if (binding.set == setIndex)
        {
            return true;
        }
    }
    return false;
}

void RendererDrawExecutor::DrawShadowScene(
    const PipelineBase& commonOpaquePipeline,
    RendererDrawContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;
    vk::Pipeline boundPipeline;

    for (const ResolvedMaterialGroup& materialGroup : context.resolvedRenderScene.materialGroups)
    {
        // ResolvedScene 已经完成最终路由：None 跳过，CommonOpaque 使用 pass 公共管线，
        // MaterialPass 使用 Material 持有的专用管线。
        if (materialGroup.shadowCaster.kind == MaterialShadowCasterKind::None)
        {
            continue;
        }

        const PipelineBase* shadowPipeline = &commonOpaquePipeline;
        if (materialGroup.shadowCaster.kind == MaterialShadowCasterKind::MaterialPass)
        {
            const std::shared_ptr<PipelineBase>& materialShadowPipeline =
                materialGroup.material->GetShadowPipeline();
            if (!materialShadowPipeline)
            {
                throw std::runtime_error(
                    "Resolved material group selected MaterialPass ShadowCaster without a pipeline: " +
                    materialGroup.material->GetMaterialKey());
            }
            shadowPipeline = materialShadowPipeline.get();
        }

        const std::string pipelineRegionName =
            "Shadow Pipeline: " + materialGroup.material->GetMaterialKey();
        VulkanDebug::ScopedRegion pipelineRegion(
            commandBuffer,
            pipelineRegionName,
            VulkanDebug::DebugCategory::ePipeline);
        if (boundPipeline != shadowPipeline->GetPipeline())
        {
            commandBuffer.bindPipeline(shadowPipeline->GetBindPoint(), shadowPipeline->GetPipeline());
            boundPipeline = shadowPipeline->GetPipeline();
        }

        const bool usesMaterialShadowPass =
            materialGroup.shadowCaster.kind == MaterialShadowCasterKind::MaterialPass;
        for (const ResolvedMaterialInstanceGroup& materialInstanceGroup : materialGroup.materialInstances)
        {
            // 只有专用 Shadow shader 会读取材质参数；公共 Opaque 路径没有 Set 1，
            // 因而无需更新 MaterialInstance UBO。
            if (usesMaterialShadowPass)
            {
                context.services.UpdateMaterialInstanceUBOForPass(materialInstanceGroup.materialInstance);
            }

            for (const ResolvedDrawPacket& draw : materialInstanceGroup.draws)
            {
                ResolvedDrawResources drawResources =
                    RequireResolvedDrawResources("Shadow", draw, context.renderScene);
                RendererObjectGpuResources& objectResources = *drawResources.objectResources;
                const RenderDrawPacket& drawPacket = *drawResources.drawPacket;
                VulkanDebug::ScopedRegion region(
                    commandBuffer,
                    drawPacket.debugName,
                    VulkanDebug::DebugCategory::eObject);

                if (usesMaterialShadowPass)
                {
                    // 专用 Shadow pipeline 的 layout 与 Surface Set 0~2 完全一致，
                    // 从 Set 0 起一次绑定对象已有的 Global/Material/Object sets。
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        shadowPipeline->GetPipelineLayout(),
                        GlobalSetIndex,
                        objectResources.descriptorSets[context.swapChainImageIndex],
                        nullptr);
                }
                else
                {
                    // 公共 Opaque pipeline 只使用 Shadow pass 的 Global set 和对象的
                    // 精简 Object set，不绑定 Surface Material set。
                    const auto& descriptorSets =
                        objectResources.shadowDescriptorSets[context.swapChainImageIndex];
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        shadowPipeline->GetPipelineLayout(),
                        GlobalSetIndex,
                        renderPass.GetDescriptorSets()[context.swapChainImageIndex][GlobalSetIndex],
                        nullptr);
                    commandBuffer.bindDescriptorSets(
                        vk::PipelineBindPoint::eGraphics,
                        shadowPipeline->GetPipelineLayout(),
                        ObjectSetIndex,
                        descriptorSets[ObjectSetIndex],
                        nullptr);
                }

                context.services.UpdateObjectUBOForPass(objectResources, drawPacket);
                drawResources.renderableObject->Draw(commandBuffer, renderPass.width, renderPass.height);
            }
        }
    }
}

void RendererDrawExecutor::DrawGeometryScene(RendererDrawContext& context) const
{
    context.services.UploadLightsForPass(
        context.swapChainImageIndex,
        context.renderScene.lights);
    DrawSurfaceScene("Geometry", false, context);
}

void RendererDrawExecutor::DrawForwardTransparentScene(
    RendererDrawContext& context) const
{
    context.services.UploadLightsForPass(
        context.swapChainImageIndex,
        context.renderScene.lights);
    DrawSurfaceScene("ForwardTransparent", true, context);
}

void RendererDrawExecutor::DrawSurfaceScene(
    const char* passName,
    bool drawTransparent,
    RendererDrawContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    for (const ResolvedMaterialGroup& materialGroup : context.resolvedRenderScene.materialGroups)
    {
        // Surface pipeline 在资源加载时已经按 RenderMode 绑定到对应的 pass
        // contract；这里仍必须做互斥过滤，保证透明材质不进入 GBuffer，
        // 不透明材质也不会在 sceneColor 上被重复绘制。
        const bool isTransparent = IsTransparentRenderMode(
            materialGroup.material->GetShaderVariantKey().renderMode);
        if (isTransparent != drawTransparent)
        {
            continue;
        }

        const std::string& materialKey = materialGroup.material->GetMaterialKey();
        const PipelineBase& renderPipeline = *materialGroup.material->GetRenderPipeline();

        VulkanDebug::ScopedRegion pipelineRegion(
            commandBuffer,
            "Pipeline: " + materialKey,
            VulkanDebug::DebugCategory::ePipeline);
        commandBuffer.bindPipeline(renderPipeline.GetBindPoint(), renderPipeline.GetPipeline());
        if (!renderPass.GetDescriptorSets()[context.swapChainImageIndex].empty() &&
            PipelineUsesDescriptorSet(renderPipeline, PassSetIndex))
        {
            commandBuffer.bindDescriptorSets(
                vk::PipelineBindPoint::eGraphics,
                renderPipeline.GetPipelineLayout(),
                PassSetIndex,
                renderPass.GetDescriptorSets()[context.swapChainImageIndex][PassSetIndex],
                nullptr);
        }

        for (const ResolvedMaterialInstanceGroup& materialInstanceGroup : materialGroup.materialInstances)
        {
            context.services.UpdateMaterialInstanceUBOForPass(materialInstanceGroup.materialInstance);

            for (const ResolvedDrawPacket& draw : materialInstanceGroup.draws)
            {
                ResolvedDrawResources drawResources =
                    RequireResolvedDrawResources(passName, draw, context.renderScene);
                RendererObjectGpuResources& objectResources = *drawResources.objectResources;
                const RenderDrawPacket& drawPacket = *drawResources.drawPacket;
                VulkanDebug::ScopedRegion region(
                    commandBuffer,
                    drawPacket.debugName,
                    VulkanDebug::DebugCategory::eObject);
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    renderPipeline.GetPipelineLayout(),
                    0,
                    objectResources.descriptorSets[context.swapChainImageIndex],
                    nullptr);

                context.services.UpdateObjectUBOForPass(objectResources, drawPacket);
                drawResources.renderableObject->Draw(commandBuffer, renderPass.width, renderPass.height);
            }
        }
    }
}

} // namespace VL
