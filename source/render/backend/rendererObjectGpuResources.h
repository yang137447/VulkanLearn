#pragma once

#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"
#include "render/rhi/rhiResourceHandles.h"
#include "animation/skinPose.h"

namespace VL
{

// GPU resources needed to draw one object with the current descriptor layout.
// RendererObjectResourceEntry owns the lifetime; draw execution receives the
// package through ResolvedDrawPacket without touching gameplay wrappers.
struct RendererObjectGpuResources
{
    Buffer objectUniformBuffer;
    // 只有蒙皮对象分配逐 swapchain palette。权重由同包 Renderable 共享并随 GPU epoch 退休。
    Buffer skinPalette;
    vk::DescriptorBufferInfo skinInfluences;
    size_t skinJointCount = 0;
    Animation::SubmittedSkinPose submittedSkin;

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
