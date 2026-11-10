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
    // GPU 预蒙皮（--prerender-skin）：单份 device-local 输出（64 B/顶点，跨帧
    // 由显式屏障复用）+ 16 B vertexCount 参数块。仅蒙皮对象在 flag 打开时分配。
    Buffer skinDeform;
    Buffer skinDeformParams;
    vk::DescriptorBufferInfo skinSourceVertices;
    uint32_t skinDeformVertexCount = 0;
    bool prerenderSkin = false;
    // TressFX current guide centers are updated at the frame boundary and
    // consumed by the strand vertex factory through Object Set 2 binding 3.
    vk::DescriptorBufferInfo tressfxPositions;
    // TressFX 分组预览：binding 5 是按 guideIndex 索引的 group id，
    // binding 6 是 vec4 组颜色表（index 0 = 默认灰）。两者都由 RenderableObject
    // 拥有且静态，这里只冻结 descriptor info。
    vk::DescriptorBufferInfo tressfxGuideGroupIds;
    vk::DescriptorBufferInfo tressfxGuideGroupColors;
    // binding 7：per-guide ghost weight；target 复用 UBO selectionData.z，
    // active/remove 状态由 selectionData.w 的 bit2/bit3 表达。
    // per-swapchain ghost 权重；上一帧可能仍读取旧槽。
    std::vector<vk::DescriptorBufferInfo> tressfxBrushWeights;
    bool tressfxBrushActive = false;
    bool tressfxBrushRemoveMode = false;
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
