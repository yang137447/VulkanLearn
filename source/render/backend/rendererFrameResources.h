#pragma once

#include <cstddef>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"
#include "world/worldSnapshot.h"

class MaterialInstance;

namespace VL
{

struct RenderDrawPacket;
struct RendererObjectGpuResources;
class RendererBackendVulkan;

// Backend-owned per-swapchain frame data. Global/light buffers live here;
// material and object updates are centralized so pass recording does not own
// frame-local GPU buffer lifetime.
class RendererFrameResources
{
public:
    void Initialize(RendererBackendVulkan& rendererBackend);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    bool EnsureLightCapacity(size_t requestedLightCount, RendererBackendVulkan& rendererBackend);
    void UpdateGlobalUniformBuffer(
        vk::CommandBuffer& commandBuffer,
        uint32_t swapChainImageIndex,
        const UBOGlobal& uboGlobal);
    void UpdateGlobalUniformBufferExceptGpuOwnedRanges(
        vk::CommandBuffer& commandBuffer,
        uint32_t swapChainImageIndex,
        const UBOGlobal& uboGlobal);
    void UpdateLightBuffer(
        uint32_t swapChainImageIndex,
        const std::vector<LightSnapshot>& lights);
    void UpdateMaterialInstanceUniformBuffer(
        uint32_t swapChainImageIndex,
        MaterialInstance& materialInstance);
    void UpdateObjectUniformBuffer(
        uint32_t swapChainImageIndex,
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket);

    const std::vector<vk::DescriptorBufferInfo>& GetGlobalUniformBufferInfos() const;
    const std::vector<vk::DescriptorBufferInfo>& GetLightBufferInfos() const;
    size_t GetLightCapacity() const { return maxLightCount; }
    bool IsInitialized() const { return initialized; }

private:
    void CreateLightBuffer(size_t requestedLightCount, RendererBackendVulkan& rendererBackend);
    void DestroyLightBuffer(RendererBackendVulkan& rendererBackend);

    Buffer globalUniformBuffer;
    Buffer lightBuffer;
    size_t maxLightCount = 0;
    bool initialized = false;
};

} // namespace VL
