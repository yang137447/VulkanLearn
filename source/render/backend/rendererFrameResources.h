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
        const std::vector<LightSnapshot>& lights,
        float timeSeconds = 0.0f);
    // GPU per-particle light 输出：在每帧 BeginFrame 之后、粒子 simulate
    // 之前清零；host-visible 内存已由 image fence 保护。
    void ClearParticleLightBuffer(
        vk::CommandBuffer commandBuffer,
        uint32_t swapChainImageIndex);
    void UpdateMaterialInstanceUniformBuffer(
        uint32_t swapChainImageIndex,
        MaterialInstance& materialInstance);
    void UpdateObjectUniformBuffer(
        uint32_t swapChainImageIndex,
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket,
        const SpeedTreeWindStateGPU* speedTreeWindState,
        bool uploadSkinPalette = true);
    // GPU 预蒙皮帧前上传：只写 current/previous palette，不碰对象 UBO。
    // 必须在 pre-skin compute dispatch 之前、且在本帧所有 draw 之前调用。
    void UpdateObjectSkinPalette(
        uint32_t swapChainImageIndex,
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket);

    // 屏幕空间 tile 光照聚类的输出缓冲。尺寸跟随渲染目标（sceneColor），
    // resize 或自定义图改变分辨率时重建；返回 true 表示 descriptor 必须刷新。
    bool EnsureLightCullingGrid(
        uint32_t renderWidth,
        uint32_t renderHeight,
        RendererBackendVulkan& rendererBackend);

    const std::vector<vk::DescriptorBufferInfo>& GetGlobalUniformBufferInfos() const;
    const std::vector<vk::DescriptorBufferInfo>& GetLightBufferInfos() const;
    const std::vector<vk::DescriptorBufferInfo>&
        GetParticleLightBufferInfos() const;
    const std::vector<vk::DescriptorBufferInfo>&
        GetLightGridBufferInfos() const
    {
        return lightGridBuffer.bufferInfos;
    }
    const std::vector<vk::DescriptorBufferInfo>&
        GetLightIndexBufferInfos() const
    {
        return lightIndexBuffer.bufferInfos;
    }
    const std::vector<vk::DescriptorBufferInfo>&
        GetLightGridParamBufferInfos() const
    {
        return lightGridParamBuffer.bufferInfos;
    }
    const std::vector<vk::Buffer>&
        GetParticleLightBufferHandles() const noexcept
    {
        return particleLightBuffer.buffers;
    }
    size_t GetLightCapacity() const { return maxLightCount; }
    bool HasLightCullingGrid() const
    {
        return lightGridBuffer.HasResources() &&
            lightIndexBuffer.HasResources() &&
            lightGridParamBuffer.HasResources();
    }
    uint32_t GetLightCullingTilesX() const { return lightCullingTilesX; }
    uint32_t GetLightCullingTilesY() const { return lightCullingTilesY; }
    uint32_t GetLightCullingRenderWidth() const { return lightCullingRenderWidth; }
    uint32_t GetLightCullingRenderHeight() const { return lightCullingRenderHeight; }
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
    void WriteObjectSkinPalette(
        uint32_t swapChainImageIndex,
        RendererObjectGpuResources& objectResources,
        const RenderDrawPacket& drawPacket);

    Buffer globalUniformBuffer;
    Buffer lightBuffer;
    Buffer particleLightBuffer;
    // grid: header + tiles；indices: tiles × maxLightsPerTile；params: std140 块。
    Buffer lightGridBuffer;
    Buffer lightIndexBuffer;
    Buffer lightGridParamBuffer;
    uint32_t lightCullingRenderWidth = 0;
    uint32_t lightCullingRenderHeight = 0;
    uint32_t lightCullingTilesX = 0;
    uint32_t lightCullingTilesY = 0;
    size_t maxLightCount = 0;
    bool initialized = false;
};

} // namespace VL
