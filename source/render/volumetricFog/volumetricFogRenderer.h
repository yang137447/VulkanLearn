#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "render/volumetricFog/volumetricFogSettings.h"

class PipelineBase;
class PipelineFactory;
class RenderGraph;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;
struct RendererDescriptorContext;
struct RenderScene;

// 体积雾 GPU 侧拥有者：一个注入 compute（按深度层写 2D array）和一个
// 全屏合成 pass。两个 pass 都不经过 Material/MI 资产，避免为了雾新增
// 资源库里的 pass 材质；shader 由 ShaderCompiler 直接编译成独立 pair/compute。
class VolumetricFogRenderer
{
public:
    ~VolumetricFogRenderer();

    // graph 的候选实例与活动实例各自拥有自己的 fog renderer；SwapState 时
    // 整个 owner 跟着候选图一起切换，因此这里持有的是图资源的非拥有视图。
    void Initialize(
        PipelineFactory& factory,
        RendererBackendVulkan& backend,
        RenderGraph& graph,
        const RendererDescriptorContext& descriptorContext);
    void Shutdown(RendererBackendVulkan& backend);
    // World 换代可能因灯光容量变化重建 light SSBO；图资源本身不变，
    // 但 set 0 的 buffer 必须重新写入，否则雾会读到已退休的灯缓冲。
    void RefreshDescriptors(
        RendererBackendVulkan& backend,
        const RendererDescriptorContext& descriptorContext);

    void Record(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass);
    // 由 PassRuntime 在已经 beginRenderPass 之后调用。
    void RecordComposite(
        vk::CommandBuffer commandBuffer,
        uint32_t imageIndex,
        const Renderpass& renderPass);

    bool IsInitialized() const noexcept { return initialized; }
    bool HasComposite() const noexcept { return compositePipeline != nullptr; }

    uint32_t GetWidth() const noexcept { return width; }
    uint32_t GetHeight() const noexcept { return height; }
    uint32_t GetLayerCount() const noexcept { return layerCount; }

private:
    struct ImageResources
    {
        vk::DescriptorImageInfo sceneDepth;
        vk::DescriptorImageInfo shadowMap;
        vk::DescriptorImageInfo localShadowMap;
        vk::DescriptorImageInfo fogStorage;
        vk::DescriptorImageInfo fogSample;
    };

    void CreateComputeDescriptors(
        RendererBackendVulkan& backend,
        RenderGraph& graph,
        const RendererDescriptorContext& descriptorContext);
    void CreateCompositeDescriptors(
        RendererBackendVulkan& backend,
        RenderGraph& graph);
    void DestroyDescriptorState(RendererBackendVulkan& backend);
    void DestroyPipelines();

    RendererBackendVulkan* backend = nullptr;
    bool initialized = false;

    std::shared_ptr<PipelineBase> computePipeline;
    std::shared_ptr<PipelineBase> compositePipeline;

    vk::DescriptorPool computePool{};
    vk::DescriptorPool compositePool{};
    // 每 image 一组按 set index 排列的 descriptor set（0=全局/灯，1=图输入）。
    std::vector<std::vector<vk::DescriptorSet>> computeSets;
    std::vector<vk::DescriptorSet> compositeSets;

    std::vector<ImageResources> images;
    std::vector<vk::DescriptorBufferInfo> globalUniformBufferInfos;
    std::string fogResourceName;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t layerCount = 0;
};

} // namespace VL
