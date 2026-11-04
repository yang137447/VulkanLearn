#pragma once

#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

class Material;

namespace VL
{

class RendererResourceCache;

// Read-only descriptor inputs that are owned outside pass/object descriptor
// writers. Passing this explicitly keeps descriptor updates from reaching back
// into RenderSystem or other high-level singletons.
struct RendererDescriptorContext
{
    const std::vector<vk::DescriptorBufferInfo>* globalUniformBufferInfos = nullptr;
    const std::vector<vk::DescriptorBufferInfo>* lightBufferInfos = nullptr;
    const std::vector<vk::DescriptorBufferInfo>* particleLightBufferInfos = nullptr;
    // 屏幕空间 tile 光照列表（材质 set 0 binding 5/6/7）。
    const std::vector<vk::DescriptorBufferInfo>* lightGridBufferInfos = nullptr;
    const std::vector<vk::DescriptorBufferInfo>* lightIndexBufferInfos = nullptr;
    const std::vector<vk::DescriptorBufferInfo>* lightGridParamBufferInfos = nullptr;
    const RendererResourceCache* resourceCache = nullptr;
    std::shared_ptr<Material> commonOpaqueShadowMaterial;
};

} // namespace VL
