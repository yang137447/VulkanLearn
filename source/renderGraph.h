#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <nlohmann/json.hpp>

#include "render/backend/rendererDescriptorWriter.h"
#include "render/backend/rendererDescriptorPlan.h"
#include "render/framegraph/frameGraphCompiler.h"

// RenderGraph-owned image resource. MSAA resources are currently modeled as an
// explicit source image plus a resolved image when a pass needs to read the
// result in a later pass; subpass attachment-only handoff can be optimized once
// the frame graph compiler owns those dependencies end to end.
struct RenderResource
{
    std::string name;
    vk::Image image;
    vk::DeviceMemory memory;
    vk::ImageView imageView;
    vk::Format format;
    vk::Sampler sampler;
    uint32_t width;
    uint32_t height;
};

class MaterialInstance;
namespace VL
{
class RendererBackendVulkan;

enum class RenderGraphReleaseMode
{
    // Used when the caller already waited for the GPU, such as shutdown or the
    // conservative swapchain resize path.
    Immediate,
    // Used by graph reload: old GPU objects stay alive until the submitted
    // frame epoch that may reference them has completed.
    Retire
};
}

struct Renderpass
{
    void Draw(vk::CommandBuffer& commandBuffer) const;

    void CreateUniformBuffers();
    void SetupDescriptors(class RenderGraph& renderGraph, VL::RendererBackendVulkan& rendererBackend);
    void CreatePassDescriptorSetLayout(VL::RendererBackendVulkan& rendererBackend);
    void CreateDescriptorSets(VL::RendererBackendVulkan& rendererBackend);
    void UpdateDescriptorSets(
        VL::RendererBackendVulkan& rendererBackend,
        const VL::RendererDescriptorContext& descriptorContext,
        const VL::RendererPassDescriptorPlan& descriptorPlan);

    const std::vector<std::vector<vk::DescriptorSet>>& GetDescriptorSets() const { return descriptorSets; }

    std::string name;
    vk::RenderPass renderPass;
    std::vector<vk::ClearValue> clearValues;
    std::vector<vk::Framebuffer> framebuffers;
    uint32_t width;
    uint32_t height;
    uint32_t colorAttachmentCount = 0;
    vk::SampleCountFlagBits sampleCount = vk::SampleCountFlagBits::e1;
    // pass 输入输出资源
    std::vector<std::string> inputResources;
    std::vector<VL::CompiledFrameGraphPassInputDescriptor> inputDescriptorPlan;
    std::vector<std::string> outputResources;
    std::unordered_map<std::string, vk::AttachmentLoadOp> outputLoadOps;
    std::unordered_map<std::string, vk::AttachmentStoreOp> outputStoreOps;
    // pass 输入描述符集, 统一使用Set3。inputDescriptorImageInfos 按 binding 下标保存，
    // binding 关系来自 CompiledFrameGraph，而不是运行时的输入数组顺序推断。
    vk::DescriptorPool descriptorPool;
    std::vector<std::vector<vk::DescriptorImageInfo>> inputDescriptorImageInfos;
    vk::DescriptorSetLayout emptyDescriptorSetLayout;
    vk::DescriptorSetLayout descriptorSetLayout;
    std::vector<std::vector<vk::DescriptorSet>> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;

    // materialInstance 相关
    std::weak_ptr<MaterialInstance> materialInstance;
};

/**
 * @class RenderGraph
 * @brief 管理渲染管线的相关资源的创建、销毁和管理
 */
class RenderGraph
{
public:
    static RenderGraph& GetInstance()
    {
        static RenderGraph instance;
        return instance;
    }
    ~RenderGraph();
    void LoadRenderGraph(
        const nlohmann::json& renderGraphJson,
        VL::RendererBackendVulkan& rendererBackend);
    void Shutdown(
        VL::RendererBackendVulkan& rendererBackend,
        VL::RenderGraphReleaseMode releaseMode = VL::RenderGraphReleaseMode::Immediate);
    std::vector<std::string>& GetRenderpassesOrdered() { return renderpassesOrdered; }
    std::unordered_map<std::string, Renderpass>& GetRenderpasses() { return renderpasses; }
    const VL::CompiledFrameGraph& GetCompiledFrameGraph() const { return compiledFrameGraph; }
    std::vector<RenderResource>& GetDepthResourceMsaa() { return resourcesMsaa["sceneDepth"]; }
    std::vector<RenderResource>& GetDepthResourceResolve() { return resourcesResolve["sceneDepth"]; }
    std::vector<RenderResource>& GetShadowMap() { return resourcesResolve["shadowMap"]; }
    std::unordered_map<std::string, std::vector<RenderResource>>& GetResourcesMsaa() { return resourcesMsaa; }
    std::unordered_map<std::string, std::vector<RenderResource>>& GetResourcesResolve() { return resourcesResolve; }
    // Initial render-graph setup creates pass-owned descriptors. Runtime
    // refreshes reuse the compiled graph but update descriptors after a world
    // reload changes scene resources, lights, or pass material instances.
    void RenderInitialize(
        VL::RendererBackendVulkan& rendererBackend,
        const VL::RendererDescriptorContext& descriptorContext);
    void RefreshRuntimeDescriptors(
        VL::RendererBackendVulkan& rendererBackend,
        const VL::RendererDescriptorContext& descriptorContext);
    // Pass materials are world-local in the current bridge. Capture/restore is
    // used to roll back a failed world load without leaving passes bound to the
    // partially loaded world's material instances.
    std::unordered_map<std::string, std::weak_ptr<MaterialInstance>> CapturePassMaterialInstances() const;
    void RestorePassMaterialInstances(const std::unordered_map<std::string, std::weak_ptr<MaterialInstance>>& passMaterials);
    
private:
    RenderResource CreateRenderResource(
        const VL::CompiledFrameGraphResource& resourceDesc,
        VL::RendererBackendVulkan& rendererBackend,
        bool bIsMsaaSource = false);
    void DestroyRenderResource(
        RenderResource& resource,
        VL::RendererBackendVulkan& rendererBackend,
        VL::RenderGraphReleaseMode releaseMode);
    
    Renderpass CreateRenderpass(
        const VL::CompiledFrameGraphPass& passDesc,
        VL::RendererBackendVulkan& rendererBackend);
    void DestroyRenderpass(
        Renderpass& renderpass,
        VL::RendererBackendVulkan& rendererBackend,
        VL::RenderGraphReleaseMode releaseMode);

    vk::RenderPass CreateVkRenderPass(
        Renderpass& renderpass,
        VL::RendererBackendVulkan& rendererBackend,
        bool bUseMsaa);
    void DestroyVkRenderPass(vk::RenderPass& renderPass, VL::RendererBackendVulkan& rendererBackend);

    std::vector<vk::Framebuffer> CreateVkFrameBuffers(
        Renderpass renderPass,
        std::vector<std::string>& inputResources,
        std::vector<std::string>& outputResources,
        VL::RendererBackendVulkan& rendererBackend,
        bool bUseMsaa);
    void DestroyVkFrameBuffers(
        std::vector<vk::Framebuffer>& framebuffers,
        VL::RendererBackendVulkan& rendererBackend);

    vk::Format GetFormat(const std::string& formatStr);
    vk::ImageUsageFlags GetImageUsage(const std::vector<std::string>& usageStr);

    vk::AttachmentLoadOp GetAttachmentLoadOp(const std::string& loadOpStr);
    vk::AttachmentStoreOp GetAttachmentStoreOp(const std::string& storeOpStr);
    
    std::vector<vk::ClearValue> GetClearValues(const std::vector<std::string>& outputResources, bool bUseMsaa);
private:
    std::unordered_map<std::string, std::vector<RenderResource>> resourcesMsaa;
    std::unordered_map<std::string, std::vector<RenderResource>> resourcesResolve;

    std::vector<std::string> renderpassesOrdered;
    std::unordered_map<std::string, Renderpass> renderpasses;
    VL::CompiledFrameGraph compiledFrameGraph;
    VL::RendererDescriptorPlanCache descriptorPlanCache;
};
