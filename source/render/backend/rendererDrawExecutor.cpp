#include "render/backend/rendererDrawExecutor.h"

#include <iostream>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineBase.h"
#include "renderGraph.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "renderableObject.h"
#include "shaderReflect.h"
#include "vulkanDebug.h"

namespace VL
{

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
                if (draw.renderableObject.expired() || !draw.objectResourceEntry)
                {
                    std::cout << "object draw resources are expired" << std::endl;
                    continue;
                }
                if (draw.drawPacketIndex >= context.renderScene.drawPackets.size())
                {
                    std::cout << "draw packet index is out of range" << std::endl;
                    continue;
                }

                std::shared_ptr<RenderableObject> renderableObject = draw.renderableObject.lock();
                RendererObjectGpuResources& objectResources = draw.objectResourceEntry->GetResources();
                const RenderDrawPacket& drawPacket = context.renderScene.drawPackets[draw.drawPacketIndex];
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
                renderableObject->Draw(commandBuffer, renderPass.width, renderPass.height);
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
                if (draw.renderableObject.expired() || !draw.objectResourceEntry)
                {
                    std::cout << "object draw resources are expired" << std::endl;
                    continue;
                }
                if (draw.drawPacketIndex >= context.renderScene.drawPackets.size())
                {
                    std::cout << "draw packet index is out of range" << std::endl;
                    continue;
                }

                std::shared_ptr<RenderableObject> renderableObject = draw.renderableObject.lock();
                RendererObjectGpuResources& objectResources = draw.objectResourceEntry->GetResources();
                const RenderDrawPacket& drawPacket = context.renderScene.drawPackets[draw.drawPacketIndex];
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
                renderableObject->Draw(commandBuffer, renderPass.width, renderPass.height);
            }
        }
    }
}

} // namespace VL
