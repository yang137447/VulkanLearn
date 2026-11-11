#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/lightCulling/lightCulling.h"

class ComputePipeline;
class PipelineFactory;
class RenderGraph;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;
struct RendererDescriptorContext;

// 屏幕空间 tile 光照聚类的 GPU owner。
//
// 输出缓冲不属于 RenderGraph（它们是每 swapchain image 的帧资源，
// 由 RendererFrameResources 持有）；本类只负责创建 compute pipeline、
// 建立 set 0 描述符，并在 pass 内写入 tile 参数、清 header、dispatch。
class LightCullingRenderer
{
public:
    ~LightCullingRenderer();

    void Initialize(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph,
        const RendererDescriptorContext& descriptorContext);
    void Shutdown(RendererBackendVulkan& backend);
    // World 换代可能重建 light SSBO；图资源不变，只需重写 set 0。
    void RefreshDescriptors(
        RendererBackendVulkan& backend,
        const RendererDescriptorContext& descriptorContext);

    void Record(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass);

    bool IsInitialized() const noexcept { return initialized; }
    uint32_t GetWidth() const noexcept { return width; }
    uint32_t GetHeight() const noexcept { return height; }
    uint32_t GetTilesX() const noexcept { return tilesX; }
    uint32_t GetTilesY() const noexcept { return tilesY; }

private:
    struct BufferView
    {
        vk::Buffer grid{};
        vk::Buffer indices{};
        vk::Buffer params{};
        vk::DescriptorBufferInfo gridInfo{};
        vk::DescriptorBufferInfo indexInfo{};
        vk::DescriptorBufferInfo paramInfo{};
        vk::DescriptorBufferInfo globalUniformInfo{};
        vk::DescriptorBufferInfo lightInfo{};
    };

    void CreateDescriptors(
        RendererBackendVulkan& backend,
        const RendererDescriptorContext& descriptorContext);
    void DestroyDescriptorState(RendererBackendVulkan& backend);
    bool ResolveExtent(RenderGraph& graph);

    RendererBackendVulkan* backend = nullptr;
    bool initialized = false;
    std::shared_ptr<ComputePipeline> pipeline;
    vk::DescriptorPool descriptorPool{};
    std::vector<vk::DescriptorSet> descriptorSets;
    std::vector<BufferView> buffers;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t tilesX = 0;
    uint32_t tilesY = 0;
};

} // namespace VL
