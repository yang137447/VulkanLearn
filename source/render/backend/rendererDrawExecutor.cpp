#include "render/backend/rendererDrawExecutor.h"

#include "commonFunction.h"
#include "material.h"
#include "Profiler.h"
#include "pipeline/pipelineBase.h"
#include "renderGraph.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "renderableObject.h"
#include "shaderReflect.h"
#include "vulkanDebug.h"

#include <algorithm>
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

struct SortedTransparentDraw
{
    // 这些指针只引用本帧冻结的 ResolvedRenderScene，排序和提交期间其所有者不会变化。
    const ResolvedMaterialGroup* materialGroup = nullptr;
    const ResolvedMaterialInstanceGroup* materialInstanceGroup = nullptr;
    const ResolvedDrawPacket* draw = nullptr;
    float cameraDistanceSquared = 0.0f;
};

std::string BuildResolvedDrawErrorPrefix(
    const char* passName,
    const ResolvedDrawPacket& draw);

struct TransparentDrawBackToFront
{
    bool operator()(
        const SortedTransparentDraw& left,
        const SortedTransparentDraw& right) const
    {
        if (left.cameraDistanceSquared != right.cameraDistanceSquared)
        {
            return left.cameraDistanceSquared > right.cameraDistanceSquared;
        }
        // 距离相同时使用稳定的 DrawPacket 顺序，避免相机轻微移动造成无意义闪烁。
        return left.draw->drawPacketIndex < right.draw->drawPacketIndex;
    }
};

std::vector<SortedTransparentDraw> BuildSortedTransparentDraws(
    const RendererDrawContext& context)
{
    std::vector<SortedTransparentDraw> draws;
    for (const ResolvedMaterialGroup& materialGroup :
         context.resolvedRenderScene.materialGroups)
    {
        if (!IsTransparentRenderMode(
                materialGroup.material->GetShaderVariantKey().renderMode))
        {
            continue;
        }

        for (const ResolvedMaterialInstanceGroup& materialInstanceGroup :
             materialGroup.materialInstances)
        {
            for (const ResolvedDrawPacket& draw : materialInstanceGroup.draws)
            {
                if (draw.drawPacketIndex >= context.renderScene.drawPackets.size())
                {
                    throw std::runtime_error(
                        BuildResolvedDrawErrorPrefix(
                            "ForwardTransparent",
                            draw) +
                        "draw packet count is " +
                        std::to_string(context.renderScene.drawPackets.size()) +
                        ".");
                }

                const RenderDrawPacket& drawPacket =
                    context.renderScene.drawPackets[draw.drawPacketIndex];
                // 当前 RenderScene 没有逐三角形透明深度，使用世界包围盒中心作为对象级排序键。
                // 这是传统透明排序的有意近似，相交几何仍不保证完全正确。
                const Eigen::Vector3f boundsCenter =
                    (drawPacket.worldBoundsMin + drawPacket.worldBoundsMax) *
                    0.5f;
                SortedTransparentDraw sortedDraw;
                sortedDraw.materialGroup = &materialGroup;
                sortedDraw.materialInstanceGroup = &materialInstanceGroup;
                sortedDraw.draw = &draw;
                sortedDraw.cameraDistanceSquared =
                    (boundsCenter - context.renderScene.camera.position)
                        .squaredNorm();
                draws.push_back(sortedDraw);
            }
        }
    }

    // 保留原始顺序作为最终兜底，使相同排序键在不同帧保持确定性。
    std::stable_sort(
        draws.begin(),
        draws.end(),
        TransparentDrawBackToFront{});
    return draws;
}

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
    DrawSurfaceScene("Geometry", SurfaceDrawDomain::Geometry, context);
}

void RendererDrawExecutor::DrawForwardOpaqueScene(
    RendererDrawContext& context) const
{
    context.services.UploadLightsForPass(
        context.swapChainImageIndex,
        context.renderScene.lights);
    DrawSurfaceScene("ForwardOpaque", SurfaceDrawDomain::ForwardOpaque, context);
}

void RendererDrawExecutor::DrawForwardEyeInnerScene(
    RendererDrawContext& context) const
{
    PROFILE_SCOPE("Eye/ForwardEyeInner");
    context.services.UploadLightsForPass(
        context.swapChainImageIndex,
        context.renderScene.lights);
    DrawSurfaceScene(
        "ForwardEyeInner",
        SurfaceDrawDomain::ForwardEyeInner,
        context);
}

void RendererDrawExecutor::DrawForwardEyeCorneaScene(
    RendererDrawContext& context) const
{
    PROFILE_SCOPE("Eye/ForwardEyeCornea");
    context.services.UploadLightsForPass(
        context.swapChainImageIndex,
        context.renderScene.lights);
    DrawSurfaceScene(
        "ForwardEyeCornea",
        SurfaceDrawDomain::ForwardEyeCornea,
        context);
}

void RendererDrawExecutor::DrawForwardTransparentScene(
    RendererDrawContext& context) const
{
    context.services.UploadLightsForPass(
        context.swapChainImageIndex,
        context.renderScene.lights);
    // Alpha、Additive 和 Thin Translucent 统一参与对象级后向前排序。
    DrawSortedTransparentScene(context);
}

void RendererDrawExecutor::DrawSortedTransparentScene(
    RendererDrawContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;
    const std::vector<SortedTransparentDraw> sortedDraws =
        BuildSortedTransparentDraws(context);

    vk::Pipeline boundPipeline;
    const MaterialInstance* boundMaterialInstance = nullptr;
    // 全局排序会打散原有 Material 分组，因此在排序结果上显式缓存当前 pipeline/MI，
    // 既保持正确绑定，又避免相邻对象恰好同材质时重复更新状态。
    for (const SortedTransparentDraw& sortedDraw : sortedDraws)
    {
        const ResolvedMaterialGroup& materialGroup =
            *sortedDraw.materialGroup;
        const ResolvedMaterialInstanceGroup& materialInstanceGroup =
            *sortedDraw.materialInstanceGroup;
        const PipelineBase& renderPipeline =
            *materialGroup.material->GetRenderPipeline();

        VulkanDebug::ScopedRegion pipelineRegion(
            commandBuffer,
            "Pipeline: " + materialGroup.material->GetMaterialKey(),
            VulkanDebug::DebugCategory::ePipeline);
        if (boundPipeline != renderPipeline.GetPipeline())
        {
            commandBuffer.bindPipeline(
                renderPipeline.GetBindPoint(),
                renderPipeline.GetPipeline());
            boundPipeline = renderPipeline.GetPipeline();
            boundMaterialInstance = nullptr;

            // Pass Set 属于 pipeline layout；跨材质切换 pipeline 后必须按新 layout 重新绑定。
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
        }

        if (boundMaterialInstance != materialInstanceGroup.materialInstance.get())
        {
            context.services.UpdateMaterialInstanceUBOForPass(
                materialInstanceGroup.materialInstance);
            boundMaterialInstance = materialInstanceGroup.materialInstance.get();
        }

        ResolvedDrawResources drawResources =
            RequireResolvedDrawResources(
                "ForwardTransparent",
                *sortedDraw.draw,
                context.renderScene);
        RendererObjectGpuResources& objectResources =
            *drawResources.objectResources;
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

        context.services.UpdateObjectUBOForPass(
            objectResources,
            drawPacket);
        drawResources.renderableObject->Draw(
            commandBuffer,
            renderPass.width,
            renderPass.height);
    }
}

void RendererDrawExecutor::DrawSurfaceScene(
    const char* passName,
    SurfaceDrawDomain domain,
    RendererDrawContext& context) const
{
    const Renderpass& renderPass = context.renderPass;
    vk::CommandBuffer& commandBuffer = context.commandBuffer;

    for (const ResolvedMaterialGroup& materialGroup : context.resolvedRenderScene.materialGroups)
    {
        const RenderMode renderMode =
            materialGroup.material->GetShaderVariantKey().renderMode;
        const bool matchesDomain =
            domain == SurfaceDrawDomain::Geometry
                ? IsGeometryRenderMode(renderMode)
                : domain == SurfaceDrawDomain::ForwardOpaque
                    ? renderMode == RenderMode::ForwardOpaque
                    : domain == SurfaceDrawDomain::ForwardEyeInner
                        ? renderMode == RenderMode::ForwardEyeInner
                        : renderMode == RenderMode::ForwardEyeCornea;
        if (!matchesDomain)
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
        if (materialGroup.material->GetShaderVariantKey().shadingModelMacro ==
            "SHADING_MODEL_EYE")
        {
            context.services.RecordEyeDescriptorBind();
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

                if (materialGroup.material->GetShaderVariantKey().shadingModelMacro ==
                    "SHADING_MODEL_EYE")
                {
                    // Geometry/Forward 的 Eye evaluator 至少消费一次 caustic LUT；
                    // 该值是 CPU-side draw-domain estimate，不把 debug readback 当 steady-state。
                    context.services.RecordEyeDraw(1);
                }
                context.services.UpdateObjectUBOForPass(objectResources, drawPacket);
                drawResources.renderableObject->Draw(commandBuffer, renderPass.width, renderPass.height);
            }
        }
    }
}

} // namespace VL
