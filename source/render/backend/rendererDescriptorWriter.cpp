#include "render/backend/rendererDescriptorWriter.h"

#include <stdexcept>
#include <string>

#include "materialInstance.h"
#include "render/resource/rendererResourceCache.h"
#include "texture.h"

namespace VL
{

const vk::DescriptorBufferInfo& RequireRendererDescriptorBufferInfo(
    const std::vector<vk::DescriptorBufferInfo>* bufferInfos,
    uint32_t swapChainImageIndex,
    const char* ownerName,
    const char* debugName)
{
    if (bufferInfos == nullptr || swapChainImageIndex >= bufferInfos->size())
    {
        throw std::runtime_error(
            std::string(ownerName) +
            " descriptor context is missing " +
            debugName +
            " for this swapchain image.");
    }

    return (*bufferInfos)[swapChainImageIndex];
}

const RendererResourceCache& RequireRendererDescriptorResourceCache(
    const RendererDescriptorContext& descriptorContext,
    const char* ownerName)
{
    if (descriptorContext.resourceCache == nullptr)
    {
        throw std::runtime_error(
            std::string(ownerName) +
            " descriptor context is missing the renderer resource cache.");
    }

    return *descriptorContext.resourceCache;
}

bool BuildRendererDescriptorWrite(
    const RendererDescriptorUpdate& update,
    vk::DescriptorSet destinationSet,
    uint32_t swapChainImageIndex,
    const RendererDescriptorWriteInputs& inputs,
    vk::WriteDescriptorSet& outWrite)
{
    if (inputs.descriptorContext == nullptr)
    {
        throw std::runtime_error("Descriptor write inputs are missing RendererDescriptorContext.");
    }

    outWrite = vk::WriteDescriptorSet();
    outWrite
        .setDstSet(destinationSet)
        .setDstBinding(update.binding)
        .setDstArrayElement(0)
        .setDescriptorType(update.descriptorType)
        .setDescriptorCount(1);

    if (update.source == RendererDescriptorUpdateSource::GlobalUniform)
    {
        outWrite.setBufferInfo(RequireRendererDescriptorBufferInfo(
            inputs.descriptorContext->globalUniformBufferInfos,
            swapChainImageIndex,
            "RendererDescriptorWriter",
            "global uniform buffer info"));
    }
    else if (update.source == RendererDescriptorUpdateSource::GlobalSkyUniform)
    {
        outWrite.setBufferInfo(RequireRendererDescriptorBufferInfo(
            inputs.descriptorContext->skyParametersBufferInfos,
            swapChainImageIndex,
            "RendererDescriptorWriter",
            "sky parameters uniform buffer info"));
    }
    else if (update.source == RendererDescriptorUpdateSource::MaterialUniform)
    {
        if (inputs.materialInstance == nullptr)
        {
            return false;
        }
        const std::vector<vk::DescriptorBufferInfo>& bufferInfos =
            inputs.materialInstance->GetUboMaterialInstanceInfo();
        outWrite.setBufferInfo(RequireRendererDescriptorBufferInfo(
            &bufferInfos,
            swapChainImageIndex,
            "RendererDescriptorWriter",
            "material instance uniform buffer info"));
    }
    else if (update.source == RendererDescriptorUpdateSource::ObjectUniform)
    {
        if (inputs.objectUniformBufferInfo == nullptr)
        {
            return false;
        }
        outWrite.setBufferInfo(*inputs.objectUniformBufferInfo);
    }
    else if (update.source == RendererDescriptorUpdateSource::LightStorage)
    {
        outWrite.setBufferInfo(RequireRendererDescriptorBufferInfo(
            inputs.descriptorContext->lightBufferInfos,
            swapChainImageIndex,
            "RendererDescriptorWriter",
            "light storage buffer info"));
    }
    else if (update.source == RendererDescriptorUpdateSource::GlobalTexture)
    {
        const RendererResourceCache& resourceCache =
            RequireRendererDescriptorResourceCache(*inputs.descriptorContext, "RendererDescriptorWriter");
        const std::shared_ptr<Texture>* texture = resourceCache.GetGlobalTexture(update.resourceName);
        if (texture == nullptr || *texture == nullptr)
        {
            return false;
        }
        outWrite.setImageInfo((*texture)->GetDescriptorInfo());
    }
    else if (update.source == RendererDescriptorUpdateSource::MaterialTexture)
    {
        if (inputs.materialInstance == nullptr)
        {
            return false;
        }
        outWrite.setImageInfo(inputs.materialInstance->GetTextureDescriptorInfo(update.resourceName));
    }
    else if (update.source == RendererDescriptorUpdateSource::PassInputTexture)
    {
        if (inputs.passInputImageInfos == nullptr ||
            update.passInputBinding >= inputs.passInputImageInfos->size())
        {
            return false;
        }
        outWrite.setPImageInfo(&(*inputs.passInputImageInfos)[update.passInputBinding]);
    }
    else
    {
        return false;
    }

    return true;
}

} // namespace VL
