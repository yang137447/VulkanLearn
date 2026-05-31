#include "render/backend/rendererObjectResourceManager.h"

#include <memory>
#include <vector>

#include "commonFunction.h"
#include "material.h"
#include "materialInstance.h"
#include "pipeline/pipelineBase.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorWriter.h"
#include "render/resource/rendererResourceCache.h"
#include "shaderReflect.h"

namespace VL
{
namespace
{

void CreateObjectUniformBuffer(
    RendererBackendVulkan& rendererBackend,
    const std::string& objectName,
    RendererObjectGpuResources& resources)
{
    vk::BufferUsageFlags usage = vk::BufferUsageFlagBits::eUniformBuffer;
    vk::MemoryPropertyFlags memoryPropertyFlags =
        vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;
    rendererBackend.CreatePerSwapchainBufferSet(
        resources.objectUniformBuffer,
        sizeof(UBOModel),
        usage,
        memoryPropertyFlags,
        "UBO_Model: " + objectName);
    rendererBackend.SetupDescriptorBufferInfos(resources.objectUniformBuffer);
}

void CreateObjectDescriptorSets(
    RendererBackendVulkan& rendererBackend,
    const std::string& objectName,
    const MaterialInstance& materialInstance,
    RendererObjectGpuResources& resources)
{
    const uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    std::shared_ptr<Material> baseMaterial = materialInstance.GetBaseMaterial().lock();
    const std::vector<ShaderBinding>& shaderBindings = baseMaterial->GetRenderPipeline()->GetShaderBindings();
    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;

    for (const ShaderBinding& binding : shaderBindings)
    {
        if (binding.set == PassSetIndex)
        {
            continue;
        }

        vk::DescriptorPoolSize poolSize;
        poolSize
            .setType(binding.type)
            .setDescriptorCount(swapChainImageCount);
        descriptorPoolSizes.push_back(poolSize);
    }

    const auto& pipelineSetLayouts = baseMaterial->GetRenderPipeline()->GetDescriptorSetLayouts();

    // Pass descriptors are owned by RenderGraph/Renderpass. Object draw
    // resources allocate global, material, and object sets for the legacy path.
    std::vector<vk::DescriptorSetLayout> allocateLayouts;
    for (size_t i = 0; i < pipelineSetLayouts.size(); ++i)
    {
        if (i != PassSetIndex)
        {
            allocateLayouts.push_back(pipelineSetLayouts[i]);
        }
    }
    const uint32_t setLayoutCount = static_cast<uint32_t>(allocateLayouts.size());

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(swapChainImageCount * setLayoutCount)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    resources.descriptorPool = rendererBackend.CreateDescriptorPool(
        descriptorPoolCreateInfo,
        "DescriptorPool: " + objectName);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(resources.descriptorPool)
        .setSetLayouts(allocateLayouts);

    resources.descriptorSets.resize(swapChainImageCount);
    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        resources.descriptorSets[i].resize(setLayoutCount);
        rendererBackend.AllocateDescriptorSets(descriptorSetAllocateInfo, resources.descriptorSets[i]);
        for (uint32_t j = 0; j < setLayoutCount; j++)
        {
            rendererBackend.SetDescriptorSetDebugName(
                resources.descriptorSets[i][j],
                "DescriptorSet: " + objectName +
                    " (SwapchainIndex " + std::to_string(i) +
                    ", Set " + std::to_string(j) + ")");
        }
    }
}

void UpdateObjectDescriptorSets(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    MaterialInstance& materialInstance,
    RendererObjectGpuResources& resources)
{
    const uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    resources.writeDescriptorSets.resize(swapChainImageCount);
    std::shared_ptr<Material> baseMaterial = materialInstance.GetBaseMaterial().lock();
    const auto& shaderBindings = baseMaterial->GetRenderPipeline()->GetShaderBindings();

    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        resources.writeDescriptorSets[i].clear();
        RendererDescriptorWriteInputs writeInputs;
        writeInputs.descriptorContext = &descriptorContext;
        writeInputs.materialInstance = &materialInstance;
        if (i < resources.objectUniformBuffer.bufferInfos.size())
        {
            writeInputs.objectUniformBufferInfo = &resources.objectUniformBuffer.bufferInfos[i];
        }

        for (const ShaderBinding& binding : shaderBindings)
        {
            if (binding.set == PassSetIndex)
            {
                continue;
            }

            RendererDescriptorUpdate update;
            update.setIndex = binding.set;
            update.binding = binding.binding;
            update.descriptorType = binding.type;
            update.resourceName = binding.name;

            if (binding.type == vk::DescriptorType::eUniformBuffer)
            {
                if (binding.set == GlobalSetIndex)
                {
                    update.source = RendererDescriptorUpdateSource::GlobalUniform;
                }
                else if (binding.set == MaterialSetIndex)
                {
                    update.source = RendererDescriptorUpdateSource::MaterialUniform;
                }
                else if (binding.set == ObjectSetIndex)
                {
                    update.source = RendererDescriptorUpdateSource::ObjectUniform;
                }
                else
                {
                    continue;
                }
            }
            else if (binding.type == vk::DescriptorType::eStorageBuffer)
            {
                update.source = RendererDescriptorUpdateSource::LightStorage;
            }
            else if (binding.type == vk::DescriptorType::eCombinedImageSampler)
            {
                update.source = binding.set == GlobalSetIndex
                    ? RendererDescriptorUpdateSource::GlobalTexture
                    : RendererDescriptorUpdateSource::MaterialTexture;
            }
            else
            {
                continue;
            }

            vk::WriteDescriptorSet write;
            if (BuildRendererDescriptorWrite(
                    update,
                    resources.descriptorSets[i][binding.set],
                    i,
                    writeInputs,
                    write))
            {
                resources.writeDescriptorSets[i].push_back(write);
            }
        }

        if (!resources.writeDescriptorSets[i].empty())
        {
            rendererBackend.UpdateDescriptorSets(resources.writeDescriptorSets[i]);
        }
    }
}

std::shared_ptr<Material> GetShadowMaterial(
    const RendererDescriptorContext& descriptorContext)
{
    const RendererResourceCache& resourceCache =
        RequireRendererDescriptorResourceCache(descriptorContext, "RendererObjectResourceManager");
    const std::shared_ptr<Material>* shadowMaterialPtr = resourceCache.GetMaterial("shadow");
    if (shadowMaterialPtr == nullptr)
    {
        return nullptr;
    }
    return *shadowMaterialPtr;
}

void CreateShadowDescriptorSets(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    const std::string& objectName,
    RendererObjectGpuResources& resources)
{
    const uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    std::shared_ptr<Material> shadowMaterial = GetShadowMaterial(descriptorContext);
    if (!shadowMaterial)
    {
        return;
    }

    const std::vector<ShaderBinding>& shaderBindings = shadowMaterial->GetRenderPipeline()->GetShaderBindings();
    std::vector<vk::DescriptorPoolSize> descriptorPoolSizes;
    for (const ShaderBinding& binding : shaderBindings)
    {
        if (binding.set != ObjectSetIndex)
        {
            continue;
        }

        vk::DescriptorPoolSize poolSize;
        poolSize
            .setType(binding.type)
            .setDescriptorCount(swapChainImageCount);
        descriptorPoolSizes.push_back(poolSize);
    }

    if (descriptorPoolSizes.empty())
    {
        return;
    }

    const auto& pipelineSetLayouts = shadowMaterial->GetRenderPipeline()->GetDescriptorSetLayouts();
    if (pipelineSetLayouts.size() <= ObjectSetIndex)
    {
        return;
    }

    vk::DescriptorPoolCreateInfo descriptorPoolCreateInfo;
    descriptorPoolCreateInfo
        .setMaxSets(swapChainImageCount)
        .setFlags(vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet)
        .setPoolSizes(descriptorPoolSizes);

    resources.shadowDescriptorPool = rendererBackend.CreateDescriptorPool(
        descriptorPoolCreateInfo,
        "DescriptorPool: Shadow: " + objectName);

    vk::DescriptorSetLayout objectSetLayout = pipelineSetLayouts[ObjectSetIndex];
    std::vector<vk::DescriptorSetLayout> allocateLayouts(swapChainImageCount, objectSetLayout);

    vk::DescriptorSetAllocateInfo descriptorSetAllocateInfo;
    descriptorSetAllocateInfo
        .setDescriptorPool(resources.shadowDescriptorPool)
        .setSetLayouts(allocateLayouts);

    std::vector<vk::DescriptorSet> allocatedSets(swapChainImageCount);
    rendererBackend.AllocateDescriptorSets(descriptorSetAllocateInfo, allocatedSets);

    resources.shadowDescriptorSets.resize(swapChainImageCount);
    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        resources.shadowDescriptorSets[i].resize(ObjectSetIndex + 1);
        resources.shadowDescriptorSets[i][ObjectSetIndex] = allocatedSets[i];
        rendererBackend.SetDescriptorSetDebugName(
            allocatedSets[i],
            "DescriptorSet: Shadow: " + objectName +
                " (SwapchainIndex " + std::to_string(i) + ")");
    }
}

void UpdateShadowDescriptorSets(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    RendererObjectGpuResources& resources)
{
    if (resources.shadowDescriptorSets.empty())
    {
        return;
    }

    const uint32_t swapChainImageCount = rendererBackend.GetSwapchainImageCount();
    std::shared_ptr<Material> shadowMaterial = GetShadowMaterial(descriptorContext);
    if (!shadowMaterial)
    {
        return;
    }

    const auto& shaderBindings = shadowMaterial->GetRenderPipeline()->GetShaderBindings();
    resources.shadowWriteDescriptorSets.resize(swapChainImageCount);

    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        resources.shadowWriteDescriptorSets[i].clear();
        RendererDescriptorWriteInputs writeInputs;
        writeInputs.descriptorContext = &descriptorContext;
        if (i < resources.objectUniformBuffer.bufferInfos.size())
        {
            writeInputs.objectUniformBufferInfo = &resources.objectUniformBuffer.bufferInfos[i];
        }

        for (const ShaderBinding& binding : shaderBindings)
        {
            if (binding.set != ObjectSetIndex ||
                binding.type != vk::DescriptorType::eUniformBuffer)
            {
                continue;
            }

            RendererDescriptorUpdate update;
            update.setIndex = binding.set;
            update.binding = binding.binding;
            update.descriptorType = binding.type;
            update.source = RendererDescriptorUpdateSource::ObjectUniform;

            vk::WriteDescriptorSet write;
            if (BuildRendererDescriptorWrite(
                    update,
                    resources.shadowDescriptorSets[i][binding.set],
                    i,
                    writeInputs,
                    write))
            {
                resources.shadowWriteDescriptorSets[i].push_back(write);
            }
        }

        if (!resources.shadowWriteDescriptorSets[i].empty())
        {
            rendererBackend.UpdateDescriptorSets(resources.shadowWriteDescriptorSets[i]);
        }
    }
}

void DestroyObjectDescriptorSets(
    RendererBackendVulkan& rendererBackend,
    RendererObjectGpuResources& resources)
{
    if (resources.descriptorSets.empty())
    {
        return;
    }

    for (std::vector<vk::DescriptorSet>& set : resources.descriptorSets)
    {
        for (vk::DescriptorSet& descriptorSet : set)
        {
            rendererBackend.FreeDescriptorSet(resources.descriptorPool, descriptorSet);
        }
    }
    rendererBackend.DestroyDescriptorPool(resources.descriptorPool);
    resources.descriptorSets.clear();
    resources.writeDescriptorSets.clear();
}

void DestroyShadowDescriptorSets(
    RendererBackendVulkan& rendererBackend,
    RendererObjectGpuResources& resources)
{
    if (resources.shadowDescriptorPool)
    {
        rendererBackend.DestroyDescriptorPool(resources.shadowDescriptorPool);
    }
    resources.shadowDescriptorSets.clear();
    resources.shadowWriteDescriptorSets.clear();
}

} // namespace

void RendererObjectResourceManager::InitializeObjectResources(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    const std::string& objectName,
    MaterialInstance& materialInstance,
    RendererObjectGpuResources& resources) const
{
    if (resources.initialized)
    {
        return;
    }

    CreateObjectUniformBuffer(rendererBackend, objectName, resources);
    CreateObjectDescriptorSets(rendererBackend, objectName, materialInstance, resources);
    UpdateObjectDescriptorSets(rendererBackend, descriptorContext, materialInstance, resources);
    CreateShadowDescriptorSets(rendererBackend, descriptorContext, objectName, resources);
    UpdateShadowDescriptorSets(rendererBackend, descriptorContext, resources);
    resources.initialized = true;
}

void RendererObjectResourceManager::ShutdownObjectResources(
    RendererBackendVulkan* rendererBackend,
    RendererObjectGpuResources& resources) const
{
    if (rendererBackend == nullptr)
    {
        resources = RendererObjectGpuResources();
        return;
    }

    DestroyShadowDescriptorSets(*rendererBackend, resources);
    DestroyObjectDescriptorSets(*rendererBackend, resources);
    if (!resources.objectUniformBuffer.buffers.empty())
    {
        rendererBackend->DestroyBufferSet(resources.objectUniformBuffer);
    }
    resources = RendererObjectGpuResources();
}

} // namespace VL
