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
    const PipelineBase& shadowPipeline,
    RendererDrawContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    for (const ResolvedMaterialGroup& materialGroup : context.resolvedRenderScene.materialGroups)
    {
        for (const ResolvedMaterialInstanceGroup& materialInstanceGroup : materialGroup.materialInstances)
        {
            context.services.UpdateMaterialInstanceUBOForPass(materialInstanceGroup.materialInstance);

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

                const auto& descriptorSets =
                    objectResources.shadowDescriptorSets[context.swapChainImageIndex];
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    shadowPipeline.GetPipelineLayout(),
                    GlobalSetIndex,
                    renderPass.GetDescriptorSets()[context.swapChainImageIndex][GlobalSetIndex],
                    nullptr);
                commandBuffer.bindDescriptorSets(
                    vk::PipelineBindPoint::eGraphics,
                    shadowPipeline.GetPipelineLayout(),
                    ObjectSetIndex,
                    descriptorSets[ObjectSetIndex],
                    nullptr);

                context.services.UpdateObjectUBOForPass(objectResources, drawPacket);
                drawResources.renderableObject->Draw(commandBuffer, renderPass.width, renderPass.height);
            }
        }
    }
}

void RendererDrawExecutor::DrawGeometryScene(RendererDrawContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    context.services.UploadLightsForPass(context.swapChainImageIndex, context.renderScene.lights);

    for (const ResolvedMaterialGroup& materialGroup : context.resolvedRenderScene.materialGroups)
    {
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
                    RequireResolvedDrawResources("Geometry", draw, context.renderScene);
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
