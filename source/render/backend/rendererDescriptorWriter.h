#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/backend/rendererDescriptorContext.h"

class MaterialInstance;

namespace VL
{

enum class RendererDescriptorUpdateSource
{
    GlobalUniform,
    GlobalSkyUniform,
    MaterialUniform,
    ObjectUniform,
    LightStorage,
    GlobalTexture,
    MaterialTexture,
    PassInputTexture
};

struct RendererDescriptorUpdate
{
    uint32_t setIndex = 0;
    uint32_t binding = 0;
    uint32_t passInputBinding = 0;
    vk::DescriptorType descriptorType = vk::DescriptorType::eUniformBuffer;
    RendererDescriptorUpdateSource source = RendererDescriptorUpdateSource::GlobalUniform;
    std::string resourceName;
};

struct RendererDescriptorWriteInputs
{
    const RendererDescriptorContext* descriptorContext = nullptr;
    const MaterialInstance* materialInstance = nullptr;
    const vk::DescriptorBufferInfo* objectUniformBufferInfo = nullptr;
    const std::vector<vk::DescriptorImageInfo>* passInputImageInfos = nullptr;
};

const vk::DescriptorBufferInfo& RequireRendererDescriptorBufferInfo(
    const std::vector<vk::DescriptorBufferInfo>* bufferInfos,
    uint32_t swapChainImageIndex,
    const char* ownerName,
    const char* debugName);

const RendererResourceCache& RequireRendererDescriptorResourceCache(
    const RendererDescriptorContext& descriptorContext,
    const char* ownerName);

// Converts a precomputed descriptor update into a concrete Vulkan write. It
// returns false when an optional binding has no live resource for this frame.
bool BuildRendererDescriptorWrite(
    const RendererDescriptorUpdate& update,
    vk::DescriptorSet destinationSet,
    uint32_t swapChainImageIndex,
    const RendererDescriptorWriteInputs& inputs,
    vk::WriteDescriptorSet& outWrite);

} // namespace VL
