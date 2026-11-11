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
RHIDescriptorSetLayoutHandle RendererBackendVulkan::RequireDescriptorSetLayoutHandle(
    vk::DescriptorSetLayout descriptorSetLayout) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = descriptorSetLayoutHandlesByLayout.find(
        static_cast<VkDescriptorSetLayout>(descriptorSetLayout));
    if (!descriptorSetLayout || handleIt == descriptorSetLayoutHandlesByLayout.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "descriptor set layout",
            static_cast<VkDescriptorSetLayout>(descriptorSetLayout)));
    }

    return handleIt->second;
}

RHIDescriptorSetLayoutHandle RendererBackendVulkan::ResolveDescriptorSetLayoutHandle(
    vk::DescriptorSetLayout descriptorSetLayout)
{
    {
        std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
        auto handleIt = descriptorSetLayoutHandlesByLayout.find(
            static_cast<VkDescriptorSetLayout>(descriptorSetLayout));
        if (handleIt != descriptorSetLayoutHandlesByLayout.end())
        {
            return handleIt->second;
        }
    }

    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle =
        rhiDevice->RegisterDescriptorSetLayout(descriptorSetLayout, false);
    BindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    return descriptorSetLayoutHandle;
}

void RendererBackendVulkan::BindDescriptorSetLayoutHandle(
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle)
{
    const VulkanDescriptorSetLayoutResource descriptorSetLayoutResource =
        rhiDevice->GetVulkanDescriptorSetLayoutResource(descriptorSetLayoutHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    descriptorSetLayoutHandlesByLayout[
        static_cast<VkDescriptorSetLayout>(descriptorSetLayoutResource.descriptorSetLayout)] =
            descriptorSetLayoutHandle;
}

void RendererBackendVulkan::UnbindDescriptorSetLayoutHandle(
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle)
{
    const VulkanDescriptorSetLayoutResource descriptorSetLayoutResource =
        rhiDevice->GetVulkanDescriptorSetLayoutResource(descriptorSetLayoutHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    descriptorSetLayoutHandlesByLayout.erase(
        static_cast<VkDescriptorSetLayout>(descriptorSetLayoutResource.descriptorSetLayout));
}

RHIDescriptorPoolHandle RendererBackendVulkan::RequireDescriptorPoolHandle(
    vk::DescriptorPool descriptorPool) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = descriptorPoolHandlesByPool.find(static_cast<VkDescriptorPool>(descriptorPool));
    if (!descriptorPool || handleIt == descriptorPoolHandlesByPool.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "descriptor pool",
            static_cast<VkDescriptorPool>(descriptorPool)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindDescriptorPoolHandle(RHIDescriptorPoolHandle descriptorPoolHandle)
{
    const VulkanDescriptorPoolResource descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    descriptorPoolHandlesByPool[static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool)] =
        descriptorPoolHandle;
}

void RendererBackendVulkan::UnbindDescriptorPoolHandle(RHIDescriptorPoolHandle descriptorPoolHandle)
{
    const VulkanDescriptorPoolResource descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle);
    VkDescriptorPool rawDescriptorPool =
        static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    auto setHandlesIt = descriptorSetHandlesByPool.find(rawDescriptorPool);
    if (setHandlesIt != descriptorSetHandlesByPool.end())
    {
        for (auto setIt = descriptorSetHandlesBySet.begin();
             setIt != descriptorSetHandlesBySet.end();)
        {
            const auto matchesPoolSet = std::find_if(
                setHandlesIt->second.begin(),
                setHandlesIt->second.end(),
                [&setIt](RHIDescriptorSetHandle handle)
                {
                    return handle.id == setIt->second.id;
                });
            if (matchesPoolSet != setHandlesIt->second.end())
            {
                setIt = descriptorSetHandlesBySet.erase(setIt);
            }
            else
            {
                ++setIt;
            }
        }
        descriptorSetHandlesByPool.erase(setHandlesIt);
    }
    descriptorPoolHandlesByPool.erase(rawDescriptorPool);
}

RHIDescriptorSetHandle RendererBackendVulkan::RequireDescriptorSetHandle(
    vk::DescriptorSet descriptorSet) const
{
    std::shared_lock<std::shared_mutex> lock(resourceMapMutex);
    auto handleIt = descriptorSetHandlesBySet.find(static_cast<VkDescriptorSet>(descriptorSet));
    if (!descriptorSet || handleIt == descriptorSetHandlesBySet.end())
    {
        throw std::runtime_error(BuildUnknownRawHandleMessage(
            "descriptor set",
            static_cast<VkDescriptorSet>(descriptorSet)));
    }

    return handleIt->second;
}

void RendererBackendVulkan::BindDescriptorSetHandle(
    RHIDescriptorSetHandle descriptorSetHandle,
    RHIDescriptorPoolHandle descriptorPoolHandle)
{
    const VulkanDescriptorSetResource descriptorSetResource =
        rhiDevice->GetVulkanDescriptorSetResource(descriptorSetHandle);
    const VulkanDescriptorPoolResource descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle);
    VkDescriptorPool rawDescriptorPool =
        static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    descriptorSetHandlesBySet[static_cast<VkDescriptorSet>(descriptorSetResource.descriptorSet)] =
        descriptorSetHandle;
    descriptorSetHandlesByPool[rawDescriptorPool].push_back(descriptorSetHandle);
}

void RendererBackendVulkan::UnbindDescriptorSetHandle(RHIDescriptorSetHandle descriptorSetHandle)
{
    const VulkanDescriptorSetResource descriptorSetResource =
        rhiDevice->GetVulkanDescriptorSetResource(descriptorSetHandle);
    const VulkanDescriptorPoolResource descriptorPoolResource =
        rhiDevice->GetVulkanDescriptorPoolResource(descriptorSetResource.descriptorPoolHandle);
    VkDescriptorPool rawDescriptorPool =
        static_cast<VkDescriptorPool>(descriptorPoolResource.descriptorPool);
    std::unique_lock<std::shared_mutex> lock(resourceMapMutex);
    descriptorSetHandlesBySet.erase(static_cast<VkDescriptorSet>(descriptorSetResource.descriptorSet));

    auto setHandlesIt = descriptorSetHandlesByPool.find(rawDescriptorPool);
    if (setHandlesIt != descriptorSetHandlesByPool.end())
    {
        auto& descriptorSetHandles = setHandlesIt->second;
        for (auto handleIt = descriptorSetHandles.begin(); handleIt != descriptorSetHandles.end();)
        {
            if (handleIt->id == descriptorSetHandle.id)
            {
                handleIt = descriptorSetHandles.erase(handleIt);
            }
            else
            {
                ++handleIt;
            }
        }
        if (descriptorSetHandles.empty())
        {
            descriptorSetHandlesByPool.erase(setHandlesIt);
        }
    }
}

std::vector<RHIDescriptorWrite> RendererBackendVulkan::BuildDescriptorWrites(
    const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets) const
{
    std::vector<RHIDescriptorWrite> rhiWrites;
    rhiWrites.reserve(writeDescriptorSets.size());
    for (const vk::WriteDescriptorSet& writeDescriptorSet : writeDescriptorSets)
    {
        RHIDescriptorWrite rhiWrite;
        rhiWrite.destinationSet = RequireDescriptorSetHandle(writeDescriptorSet.dstSet);
        rhiWrite.destinationBinding = writeDescriptorSet.dstBinding;
        rhiWrite.destinationArrayElement = writeDescriptorSet.dstArrayElement;
        rhiWrite.descriptorCount = writeDescriptorSet.descriptorCount;
        rhiWrite.descriptorType = writeDescriptorSet.descriptorType;
        if (writeDescriptorSet.pImageInfo != nullptr)
        {
            rhiWrite.imageInfos.assign(
                writeDescriptorSet.pImageInfo,
                writeDescriptorSet.pImageInfo + writeDescriptorSet.descriptorCount);
        }
        if (writeDescriptorSet.pBufferInfo != nullptr)
        {
            rhiWrite.bufferInfos.assign(
                writeDescriptorSet.pBufferInfo,
                writeDescriptorSet.pBufferInfo + writeDescriptorSet.descriptorCount);
        }
        if (writeDescriptorSet.pTexelBufferView != nullptr)
        {
            rhiWrite.texelBufferViews.assign(
                writeDescriptorSet.pTexelBufferView,
                writeDescriptorSet.pTexelBufferView + writeDescriptorSet.descriptorCount);
        }
        rhiWrites.push_back(std::move(rhiWrite));
    }
    return rhiWrites;
}

vk::DescriptorSetLayout RendererBackendVulkan::CreateDescriptorSetLayout(
    const vk::DescriptorSetLayoutCreateInfo& createInfo,
    const std::string& debugName)
{
    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle =
        rhiDevice->CreateDescriptorSetLayout(createInfo, debugName);
    BindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    return rhiDevice->GetVulkanDescriptorSetLayoutResource(
        descriptorSetLayoutHandle).descriptorSetLayout;
}

RHIDescriptorSetLayoutHandle RendererBackendVulkan::GetDescriptorSetLayoutHandle(
    vk::DescriptorSetLayout descriptorSetLayout) const
{
    return RequireDescriptorSetLayoutHandle(descriptorSetLayout);
}

void RendererBackendVulkan::DestroyDescriptorSetLayout(
    vk::DescriptorSetLayout& descriptorSetLayout)
{
    if (!descriptorSetLayout)
    {
        return;
    }

    RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle =
        RequireDescriptorSetLayoutHandle(descriptorSetLayout);
    UnbindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    rhiDevice->DestroyDescriptorSetLayout(descriptorSetLayoutHandle);
    descriptorSetLayout = nullptr;
}

void RendererBackendVulkan::DestroyDescriptorSetLayout(
    RHIDescriptorSetLayoutHandle& descriptorSetLayoutHandle)
{
    if (!descriptorSetLayoutHandle.IsValid())
    {
        return;
    }

    UnbindDescriptorSetLayoutHandle(descriptorSetLayoutHandle);
    rhiDevice->DestroyDescriptorSetLayout(descriptorSetLayoutHandle);
    descriptorSetLayoutHandle = RHIDescriptorSetLayoutHandle();
}

vk::DescriptorPool RendererBackendVulkan::CreateDescriptorPool(
    const vk::DescriptorPoolCreateInfo& createInfo,
    const std::string& debugName)
{
    RHIDescriptorPoolHandle descriptorPoolHandle =
        rhiDevice->CreateDescriptorPool(createInfo, debugName);
    BindDescriptorPoolHandle(descriptorPoolHandle);
    return rhiDevice->GetVulkanDescriptorPoolResource(descriptorPoolHandle).descriptorPool;
}

RHIDescriptorPoolHandle RendererBackendVulkan::GetDescriptorPoolHandle(
    vk::DescriptorPool descriptorPool) const
{
    return RequireDescriptorPoolHandle(descriptorPool);
}

void RendererBackendVulkan::DestroyDescriptorPool(vk::DescriptorPool& descriptorPool)
{
    if (!descriptorPool)
    {
        return;
    }

    RHIDescriptorPoolHandle descriptorPoolHandle = RequireDescriptorPoolHandle(descriptorPool);
    UnbindDescriptorPoolHandle(descriptorPoolHandle);
    rhiDevice->DestroyDescriptorPool(descriptorPoolHandle);
    descriptorPool = nullptr;
}

void RendererBackendVulkan::DestroyDescriptorPool(
    RHIDescriptorPoolHandle& descriptorPoolHandle)
{
    if (!descriptorPoolHandle.IsValid())
    {
        return;
    }

    UnbindDescriptorPoolHandle(descriptorPoolHandle);
    rhiDevice->DestroyDescriptorPool(descriptorPoolHandle);
    descriptorPoolHandle = RHIDescriptorPoolHandle();
}

RHIDescriptorSetHandle RendererBackendVulkan::GetDescriptorSetHandle(
    vk::DescriptorSet descriptorSet) const
{
    return RequireDescriptorSetHandle(descriptorSet);
}

void RendererBackendVulkan::AllocateDescriptorSets(
    const vk::DescriptorSetAllocateInfo& allocateInfo,
    std::vector<vk::DescriptorSet>& descriptorSets)
{
    RHIDescriptorPoolHandle descriptorPoolHandle =
        RequireDescriptorPoolHandle(allocateInfo.descriptorPool);
    std::vector<RHIDescriptorSetLayoutHandle> descriptorSetLayoutHandles;
    descriptorSetLayoutHandles.reserve(allocateInfo.descriptorSetCount);
    for (uint32_t i = 0; i < allocateInfo.descriptorSetCount; ++i)
    {
        descriptorSetLayoutHandles.push_back(
            ResolveDescriptorSetLayoutHandle(allocateInfo.pSetLayouts[i]));
    }

    std::vector<RHIDescriptorSetHandle> descriptorSetHandles =
        rhiDevice->AllocateDescriptorSets(descriptorPoolHandle, descriptorSetLayoutHandles);
    descriptorSets.resize(descriptorSetHandles.size());
    for (size_t i = 0; i < descriptorSetHandles.size(); ++i)
    {
        BindDescriptorSetHandle(descriptorSetHandles[i], descriptorPoolHandle);
        descriptorSets[i] =
            rhiDevice->GetVulkanDescriptorSetResource(descriptorSetHandles[i]).descriptorSet;
    }
}

void RendererBackendVulkan::FreeDescriptorSet(
    vk::DescriptorPool descriptorPool,
    vk::DescriptorSet& descriptorSet)
{
    if (!descriptorPool || !descriptorSet)
    {
        return;
    }

    RHIDescriptorPoolHandle descriptorPoolHandle = RequireDescriptorPoolHandle(descriptorPool);
    RHIDescriptorSetHandle descriptorSetHandle = RequireDescriptorSetHandle(descriptorSet);
    UnbindDescriptorSetHandle(descriptorSetHandle);
    rhiDevice->FreeDescriptorSet(descriptorPoolHandle, descriptorSetHandle);
    descriptorSet = nullptr;
}

void RendererBackendVulkan::UpdateDescriptorSets(
    const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets)
{
    rhiDevice->UpdateDescriptorSets(BuildDescriptorWrites(writeDescriptorSets));
}

void RendererBackendVulkan::SetDescriptorSetDebugName(
    vk::DescriptorSet descriptorSet,
    const std::string& debugName)
{
    if (!descriptorSet)
    {
        return;
    }

    rhiDevice->SetDescriptorSetDebugName(RequireDescriptorSetHandle(descriptorSet), debugName);
}

} // namespace VL
