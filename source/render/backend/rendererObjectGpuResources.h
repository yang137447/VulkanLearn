#pragma once

#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"

namespace VL
{

// GPU resources needed to draw one object with the current descriptor layout.
// RendererObjectResourceEntry owns the lifetime; draw execution receives the
// package through ResolvedDrawPacket without touching SceneObject.
struct RendererObjectGpuResources
{
    Buffer objectUniformBuffer;

    vk::DescriptorPool descriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;

    vk::DescriptorPool shadowDescriptorPool;
    std::vector<std::vector<vk::DescriptorSet>> shadowDescriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> shadowWriteDescriptorSets;

    bool initialized = false;
};

} // namespace VL
