#include "render/backend/rendererBackendVulkan.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <mutex>
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
#include "render/rhi/vulkan/rhiDeviceVulkan.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererThumbnailImages.h"
#include "vulkanDebug.h"

#include "render/backend/rendererBackendVulkanInternal.h"

namespace VL
{
std::unique_ptr<PipelineFactory> RendererBackendVulkan::CreatePipelineFactory()
{
    return std::unique_ptr<PipelineFactory>(
        new PipelineFactory(this, rhiDevice->GetDevice()));
}

vk::ShaderModule RendererBackendVulkan::CreateShaderModule(
    const vk::ShaderModuleCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::Device& device = rhiDevice->GetDevice();
    vk::ShaderModule shaderModule = device.createShaderModule(createInfo);
    VulkanDebug::SetObjectName(
        device,
        shaderModule,
        vk::ObjectType::eShaderModule,
        debugName);
    return shaderModule;
}

void RendererBackendVulkan::DestroyShaderModule(
    vk::ShaderModule& shaderModule) noexcept
{
    if (shaderModule)
    {
        rhiDevice->GetDevice().destroyShaderModule(shaderModule);
        shaderModule = nullptr;
    }
}

vk::PipelineLayout RendererBackendVulkan::CreatePipelineLayout(
    const vk::PipelineLayoutCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::Device& device = rhiDevice->GetDevice();
    vk::PipelineLayout pipelineLayout =
        device.createPipelineLayout(createInfo);
    VulkanDebug::SetObjectName(
        device,
        pipelineLayout,
        vk::ObjectType::ePipelineLayout,
        debugName);
    return pipelineLayout;
}

void RendererBackendVulkan::DestroyPipelineLayout(
    vk::PipelineLayout& pipelineLayout) noexcept
{
    if (pipelineLayout)
    {
        rhiDevice->GetDevice().destroyPipelineLayout(pipelineLayout);
        pipelineLayout = nullptr;
    }
}

GraphicsPipelineBuildResult
RendererBackendVulkan::BuildGraphicsPipeline(
    const GraphicsPipelineBuildDesc& buildDesc)
{
    return GraphicsPipelineBuilder::Build(
        rhiDevice->GetDevice(),
        buildDesc);
}

void RendererBackendVulkan::DestroyPipeline(
    vk::Pipeline& pipeline) noexcept
{
    if (pipeline)
    {
        rhiDevice->GetDevice().destroyPipeline(pipeline);
        pipeline = nullptr;
    }
}

void RendererBackendVulkan::DestroyPipelineCache(
    vk::PipelineCache& pipelineCache) noexcept
{
    if (pipelineCache)
    {
        rhiDevice->GetDevice().destroyPipelineCache(pipelineCache);
        pipelineCache = nullptr;
    }
}

uint32_t RendererBackendVulkan::GetSwapchainImageCount() const
{
    return rhiDevice->GetSwapchainImageCount(rhiDevice->GetPresentTarget());
}

vk::Extent2D RendererBackendVulkan::GetSwapchainExtent() const
{
    return rhiDevice->GetSwapchainExtent(rhiDevice->GetPresentTarget());
}

vk::Format RendererBackendVulkan::GetSwapchainImageFormat() const
{
    return rhiDevice->GetSwapchainImageFormat(rhiDevice->GetPresentTarget());
}

const std::vector<vk::ImageView>& RendererBackendVulkan::GetSwapchainImageViews() const
{
    return rhiDevice->GetSwapchainImageViews(rhiDevice->GetPresentTarget());
}

float RendererBackendVulkan::GetTimestampPeriodNanoseconds()
{
    return rhiDevice->GetTimestampPeriodNanoseconds();
}

uint32_t RendererBackendVulkan::GetGraphicsTimestampValidBits()
{
    return rhiDevice->GetGraphicsTimestampValidBits();
}

bool RendererBackendVulkan::SupportsTimestampQueries()
{
    return rhiDevice->SupportsTimestampQueries();
}

bool RendererBackendVulkan::SupportsDualSourceBlend() const
{
    // 材质加载只读取 backend 的能力快照，避免把 Vulkan 设备细节泄漏到资源层。
    return rhiDevice->IsDualSourceBlendEnabled();
}

bool RendererBackendVulkan::SupportsDrawIndirectCount() const
{
    return rhiDevice->IsDrawIndirectCountEnabled();
}

vk::QueryPool RendererBackendVulkan::CreateTimestampQueryPool(
    uint32_t queryCount,
    const std::string& debugName)
{
    return rhiDevice->CreateTimestampQueryPool(queryCount, debugName);
}

void RendererBackendVulkan::DestroyQueryPool(vk::QueryPool& queryPool)
{
    rhiDevice->DestroyQueryPool(queryPool);
}

void RendererBackendVulkan::ReadTimestampQueryPair(
    vk::QueryPool queryPool,
    uint32_t firstQuery,
    std::array<uint64_t, 2>& timestamps)
{
    rhiDevice->ReadTimestampQueryPair(queryPool, firstQuery, timestamps);
}

bool RendererBackendVulkan::TryReadTimestampQueryPair(
    vk::QueryPool queryPool,
    uint32_t firstQuery,
    std::array<uint64_t, 2>& timestamps)
{
    return rhiDevice->TryReadTimestampQueryPair(queryPool, firstQuery, timestamps);
}

RHIRenderPassHandle RendererBackendVulkan::RequireRenderPassHandle(vk::RenderPass renderPass) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = renderPassHandlesByRenderPass.find(static_cast<VkRenderPass>(renderPass));
    if (!renderPass || handleIt == renderPassHandlesByRenderPass.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "render pass",
            static_cast<VkRenderPass>(renderPass)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindRenderPassHandle(RHIRenderPassHandle renderPassHandle)
{
    const VulkanRenderPassResource renderPassResource =
        rhiDevice->GetVulkanRenderPassResource(renderPassHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    renderPassHandlesByRenderPass[static_cast<VkRenderPass>(renderPassResource.renderPass)] =
        renderPassHandle;
}

void RendererBackendVulkan::UnbindRenderPassHandle(RHIRenderPassHandle renderPassHandle)
{
    const VulkanRenderPassResource renderPassResource =
        rhiDevice->GetVulkanRenderPassResource(renderPassHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    renderPassHandlesByRenderPass.erase(static_cast<VkRenderPass>(renderPassResource.renderPass));
}

RHIFramebufferHandle RendererBackendVulkan::RequireFramebufferHandle(vk::Framebuffer framebuffer) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = framebufferHandlesByFramebuffer.find(static_cast<VkFramebuffer>(framebuffer));
    if (!framebuffer || handleIt == framebufferHandlesByFramebuffer.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "framebuffer",
            static_cast<VkFramebuffer>(framebuffer)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindFramebufferHandle(RHIFramebufferHandle framebufferHandle)
{
    const VulkanFramebufferResource framebufferResource =
        rhiDevice->GetVulkanFramebufferResource(framebufferHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    framebufferHandlesByFramebuffer[static_cast<VkFramebuffer>(framebufferResource.framebuffer)] =
        framebufferHandle;
}

void RendererBackendVulkan::UnbindFramebufferHandle(RHIFramebufferHandle framebufferHandle)
{
    const VulkanFramebufferResource framebufferResource =
        rhiDevice->GetVulkanFramebufferResource(framebufferHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    framebufferHandlesByFramebuffer.erase(static_cast<VkFramebuffer>(framebufferResource.framebuffer));
}

vk::RenderPass RendererBackendVulkan::CreateRenderPass(
    const vk::RenderPassCreateInfo2& createInfo,
    const std::string& debugName)
{
    RHIRenderPassHandle renderPassHandle = rhiDevice->CreateRenderPass(createInfo, debugName);
    BindRenderPassHandle(renderPassHandle);
    return rhiDevice->GetVulkanRenderPassResource(renderPassHandle).renderPass;
}

RHIRenderPassHandle RendererBackendVulkan::GetRenderPassHandle(vk::RenderPass renderPass) const
{
    return RequireRenderPassHandle(renderPass);
}

void RendererBackendVulkan::DestroyRenderPass(vk::RenderPass& renderPass)
{
    if (!renderPass)
    {
        return;
    }

    RHIRenderPassHandle renderPassHandle = RequireRenderPassHandle(renderPass);
    UnbindRenderPassHandle(renderPassHandle);
    rhiDevice->DestroyRenderPass(renderPassHandle);
    renderPass = nullptr;
}

void RendererBackendVulkan::DestroyRenderPass(RHIRenderPassHandle& renderPassHandle)
{
    if (!renderPassHandle.IsValid())
    {
        return;
    }

    UnbindRenderPassHandle(renderPassHandle);
    rhiDevice->DestroyRenderPass(renderPassHandle);
    renderPassHandle = RHIRenderPassHandle();
}

vk::Framebuffer RendererBackendVulkan::CreateFramebuffer(
    const vk::FramebufferCreateInfo& createInfo,
    const std::string& debugName)
{
    RHIFramebufferHandle framebufferHandle = rhiDevice->CreateFramebuffer(
        RequireRenderPassHandle(createInfo.renderPass),
        createInfo,
        debugName);
    BindFramebufferHandle(framebufferHandle);
    return rhiDevice->GetVulkanFramebufferResource(framebufferHandle).framebuffer;
}

RHIFramebufferHandle RendererBackendVulkan::GetFramebufferHandle(vk::Framebuffer framebuffer) const
{
    return RequireFramebufferHandle(framebuffer);
}

void RendererBackendVulkan::DestroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers)
{
    for (vk::Framebuffer& framebuffer : framebuffers)
    {
        if (!framebuffer)
        {
            continue;
        }

        RHIFramebufferHandle framebufferHandle = RequireFramebufferHandle(framebuffer);
        UnbindFramebufferHandle(framebufferHandle);
        rhiDevice->DestroyFramebuffer(framebufferHandle);
        framebuffer = nullptr;
    }
    framebuffers.clear();
}

void RendererBackendVulkan::DestroyFramebuffers(
    std::vector<RHIFramebufferHandle>& framebufferHandles)
{
    for (RHIFramebufferHandle& framebufferHandle : framebufferHandles)
    {
        if (!framebufferHandle.IsValid())
        {
            continue;
        }

        UnbindFramebufferHandle(framebufferHandle);
        rhiDevice->DestroyFramebuffer(framebufferHandle);
        framebufferHandle = RHIFramebufferHandle();
    }
    framebufferHandles.clear();
}

} // namespace VL
