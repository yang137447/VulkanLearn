#pragma once

#include <cstddef>
#include <memory>
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
    struct PreparedLightCapacity
    {
        Buffer replacement;
        size_t capacity = 0;
        RendererBackendVulkan* rendererBackend = nullptr;
        std::shared_ptr<void> retiredResource;
        bool adopted = false;

        PreparedLightCapacity() = default;
        ~PreparedLightCapacity();
        PreparedLightCapacity(const PreparedLightCapacity&) = delete;
        PreparedLightCapacity& operator=(const PreparedLightCapacity&) = delete;
        PreparedLightCapacity(PreparedLightCapacity&& other) noexcept;
        PreparedLightCapacity& operator=(PreparedLightCapacity&& other) noexcept;

        bool HasReplacement() const noexcept
        {
            return replacement.HasResources();
        }
        const std::vector<vk::DescriptorBufferInfo>& GetBufferInfos(
            const RendererFrameResources& active) const;
    };

    void Initialize(RendererBackendVulkan& rendererBackend);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    bool EnsureLightCapacity(size_t requestedLightCount, RendererBackendVulkan& rendererBackend);
    PreparedLightCapacity PrepareLightCapacity(
        size_t requestedLightCount,
        RendererBackendVulkan& rendererBackend) const;
    std::shared_ptr<void> CommitPreparedLightCapacity(
        PreparedLightCapacity&& prepared) noexcept;
    void UpdateGlobalUniformBuffer(
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
        const RenderDrawPacket& drawPacket,
        const SpeedTreeWindStateGPU* speedTreeWindState);

    const std::vector<vk::DescriptorBufferInfo>& GetGlobalUniformBufferInfos() const;
    const std::vector<vk::DescriptorBufferInfo>& GetLightBufferInfos() const;
    size_t GetLightCapacity() const { return maxLightCount; }
    const std::vector<RHIBufferHandle>&
        GetLightBufferHandlesForTest() const noexcept
    {
        return lightBuffer.bufferHandles;
    }
    bool IsInitialized() const { return initialized; }

private:
    static Buffer CreateLightBufferSet(
        size_t requestedLightCount,
        RendererBackendVulkan& rendererBackend);
    void CreateLightBuffer(size_t requestedLightCount, RendererBackendVulkan& rendererBackend);
    void DestroyLightBuffer(RendererBackendVulkan& rendererBackend);

    Buffer globalUniformBuffer;
    Buffer lightBuffer;
    size_t maxLightCount = 0;
    bool initialized = false;
};

} // namespace VL
