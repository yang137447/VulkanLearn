#pragma once

#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"
#include "render/rhi/rhiResourceHandles.h"

namespace VL
{

// GPU resources needed to draw one object with the current descriptor layout.
// RendererObjectResourceEntry owns the lifetime; draw execution receives the
// package through ResolvedDrawPacket without touching gameplay wrappers.
struct RendererObjectGpuResources
{
    Buffer objectUniformBuffer;

    RHIDescriptorPoolHandle descriptorPoolHandle;
    vk::DescriptorPool descriptorPool;
    std::vector<std::vector<RHIDescriptorSetHandle>> descriptorSetHandles;
    std::vector<std::vector<vk::DescriptorSet>> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;

    RHIDescriptorPoolHandle shadowDescriptorPoolHandle;
    vk::DescriptorPool shadowDescriptorPool;
    std::vector<std::vector<RHIDescriptorSetHandle>> shadowDescriptorSetHandles;
    std::vector<std::vector<vk::DescriptorSet>> shadowDescriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> shadowWriteDescriptorSets;

    bool initialized = false;
};

} // namespace VL
