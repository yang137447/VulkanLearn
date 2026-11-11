#include "renderGraph.h"
#include "commonFunction.h"
#include "shaderReflect.h"
#include "material.h"
#include "material/materialAssetUtils.h"
#include "materialInstance.h"
#include "pipeline/graphicsPipeline.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorWriter.h"
#include "render/rendergraph/renderGraphCompiler.h"
#include "render/rendergraph/sceneViewportOutputValidation.h"
#include "render/resource/resourceRetireQueue.h"
#include "render/resource/rendererResourceCache.h"
#include "render/camera/autoExposureCompute.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/volumetricFog/volumetricFogRenderer.h"
#include "render/lightCulling/lightCullingRenderer.h"
#include "hair/hairSimulationPass.h"
#include "render/skinning/skinDeformPass.h"
#include "texture.h"
#include <algorithm>
#include <memory>
#include <stdint.h>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <unordered_set>
#include "profiler.h"

void RenderGraph::MaybeFailResourceCreation()
{
    ++testResourceCreationCount;
    if (testFaultInjection.failResourceCreationAt != 0 &&
        testResourceCreationCount ==
            testFaultInjection.failResourceCreationAt)
    {
        throw std::runtime_error(
            "Injected render graph resource creation failure");
    }
}

void RenderGraph::MaybeFailRenderPassCreation()
{
    ++testRenderPassCreationCount;
    if (testFaultInjection.failRenderPassCreationAt != 0 &&
        testRenderPassCreationCount ==
            testFaultInjection.failRenderPassCreationAt)
    {
        throw std::runtime_error(
            "Injected render pass creation failure");
    }
}

void RenderGraph::MaybeFailFramebufferCreation()
{
    ++testFramebufferCreationCount;
    if (testFaultInjection.failFramebufferCreationAt != 0 &&
        testFramebufferCreationCount ==
            testFaultInjection.failFramebufferCreationAt)
    {
        throw std::runtime_error(
            "Injected framebuffer creation failure");
    }
}

void RenderGraph::MaybeFailDescriptorCreation()
{
    ++testDescriptorCreationCount;
    if (testFaultInjection.failDescriptorCreationAt != 0 &&
        testDescriptorCreationCount ==
            testFaultInjection.failDescriptorCreationAt)
    {
        throw std::runtime_error(
            "Injected render graph descriptor creation failure");
    }
}

vk::Format RenderGraph::GetFormat(const std::string& formatStr)
{
    if (formatStr == "A2B10G10R10_UNORM_PACK32")
    {
        return vk::Format::eA2B10G10R10UnormPack32;
    }
    else if (formatStr == "R8G8B8A8_SRGB")
    {
        return vk::Format::eR8G8B8A8Srgb;
    }
    else if (formatStr == "D32_SFLOAT")
    {
        return vk::Format::eD32Sfloat;
    }
    else if (formatStr == "D16_UNORM")
    {
        return vk::Format::eD16Unorm;
    }
    else if (formatStr == "D24_UNORM_S8_UINT")
    {
        return vk::Format::eD24UnormS8Uint;
    }
    else if (formatStr == "R8G8B8A8_UNORM")
    {
        return vk::Format::eR8G8B8A8Unorm;
    }
    else if (formatStr == "R16G16B16A16_SFLOAT")
    {
        return vk::Format::eR16G16B16A16Sfloat;
    }
    else if (formatStr == "R32_SFLOAT")
    {
        return vk::Format::eR32Sfloat;
    }
    else
    {
        throw std::runtime_error("Unsupported format string");
    }
}

vk::ImageUsageFlags RenderGraph::GetImageUsage(const std::vector<std::string>& usageStr)
{
    vk::ImageUsageFlags imageUsage;
    for(const auto& usage : usageStr)
    {
        if (usage == "colorAttachment")
        {
            imageUsage |= vk::ImageUsageFlagBits::eColorAttachment;
        }
        else if (usage == "depthStencilAttachment")
        {
            imageUsage |= vk::ImageUsageFlagBits::eDepthStencilAttachment;
        }
        else if (usage == "sampled")
        {
            imageUsage |= vk::ImageUsageFlagBits::eSampled;
        }
        else if (usage == "transferSrc")
        {
            imageUsage |= vk::ImageUsageFlagBits::eTransferSrc;
        }
        else if (usage == "transferDst")
        {
            imageUsage |= vk::ImageUsageFlagBits::eTransferDst;
        }
        else if (usage == "storage")
        {
            imageUsage |= vk::ImageUsageFlagBits::eStorage;
        }
    }
    return imageUsage;
}

vk::AttachmentLoadOp RenderGraph::GetAttachmentLoadOp(const std::string& loadOpStr)
{
    if (loadOpStr == "load")
    {
        return vk::AttachmentLoadOp::eLoad;
    }
    if (loadOpStr == "dontCare")
    {
        return vk::AttachmentLoadOp::eDontCare;
    }
    return vk::AttachmentLoadOp::eClear;
}

vk::AttachmentStoreOp RenderGraph::GetAttachmentStoreOp(const std::string& storeOpStr)
{
    if (storeOpStr == "dontCare")
    {
        return vk::AttachmentStoreOp::eDontCare;
    }
    return vk::AttachmentStoreOp::eStore;
}

bool RenderGraph::IsDepthResource(const std::string& resourceName) const
{
    auto msaaIt = resourcesMsaa.find(resourceName);
    if (msaaIt != resourcesMsaa.end() && !msaaIt->second.empty())
    {
        return CommonFunction::IsDepthFormat(msaaIt->second[0].format);
    }

    auto resolveIt = resourcesResolve.find(resourceName);
    if (resolveIt != resourcesResolve.end() && !resolveIt->second.empty())
    {
        return CommonFunction::IsDepthFormat(resolveIt->second[0].format);
    }

    return false;
}

std::vector<vk::ClearValue> RenderGraph::GetClearValues(
    const std::vector<std::string>& outputResources,
    bool bUseMsaa)
{
    std::vector<vk::ClearValue> clearValues;
    const std::array<float, 4> defaultClearColor{
        0.2f, 0.2f, 0.2f, 1.0f};
    vk::ClearValue clearDepth;
    clearDepth.setDepthStencil(vk::ClearDepthStencilValue(1.0f, 0));
    // 总览：清除值顺序需与 RenderPass attachments 顺序一致
    // 1) 颜色（MSAA/非MSAA）
    // 2) 仅在启用 MSAA 时追加 resolve 颜色
    // 3) 若包含深度资源，追加深度清除
    // 1. Color Attachments (MSAA & Resolve)
    for (const auto& resourceName : outputResources)
    {
        if (IsDepthResource(resourceName)) continue;

        // 每个颜色附件独立取 clearColor，不能复用上一个附件的值：
        // DBuffer 等资源用零清除，普通场景颜色仍用默认灰色。
        std::array<float, 4> color = defaultClearColor;
        const auto msaaIt = resourcesMsaa.find(resourceName);
        if (msaaIt != resourcesMsaa.end() && !msaaIt->second.empty())
        {
            color = msaaIt->second[0].clearColor;
        }
        else
        {
            // 单采样输出只存在于 resolve 表；缺失时回退默认清除色。
            const auto resolveIt = resourcesResolve.find(resourceName);
            if (resolveIt != resourcesResolve.end() &&
                !resolveIt->second.empty())
            {
                color = resolveIt->second[0].clearColor;
            }
        }
        vk::ClearValue clearColor;
        clearColor.setColor(vk::ClearColorValue(color));
        // MSAA / Main Attachment
        clearValues.push_back(clearColor);
    }
    
    // 2. Resolve Attachments
    if (bUseMsaa)
    {
        for (const auto& resourceName : outputResources)
        {
            if (IsDepthResource(resourceName)) continue;

            std::array<float, 4> color = defaultClearColor;
            const auto resolveIt = resourcesResolve.find(resourceName);
            if (resolveIt != resourcesResolve.end() &&
                !resolveIt->second.empty())
            {
                color = resolveIt->second[0].clearColor;
            }
            vk::ClearValue clearColor;
            clearColor.setColor(vk::ClearColorValue(color));
            clearValues.push_back(clearColor);
        }
    }

    // 3. Depth Attachment
    bool bHasDepth = false;
    for(const auto& r : outputResources)
    {
        if (IsDepthResource(r))
        {
            bHasDepth = true;
            break;
        }
    }
    if (bHasDepth)
    {
        clearValues.push_back(clearDepth);
        if (bUseMsaa)
        {
            clearValues.push_back(clearDepth);
        }
    }

    return clearValues;
}
