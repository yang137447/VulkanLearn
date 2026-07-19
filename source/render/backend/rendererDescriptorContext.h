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
    const RendererResourceCache* resourceCache = nullptr;
    std::shared_ptr<Material> commonOpaqueShadowMaterial;
};

} // namespace VL
