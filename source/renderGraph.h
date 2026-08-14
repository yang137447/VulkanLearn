#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <nlohmann/json.hpp>

#include "render/backend/rendererDescriptorWriter.h"
#include "render/backend/rendererDescriptorPlan.h"
#include "render/rendergraph/renderGraphCompiler.h"
#include "render/rhi/rhiResourceHandles.h"
#include "pipeline/passPipelineContractKey.h"

// RenderGraph-owned image resource. MSAA resources are currently modeled as an
// explicit source image plus a resolved image when a pass needs to read the
// result in a later pass; subpass attachment-only handoff can be optimized once
// the frame graph compiler owns those dependencies end to end.
struct RenderResource
{
    vk::ImageView GetShaderDescriptorView() const;
    vk::ImageView GetFramebufferAttachmentView(uint32_t layer = 0) const;

    std::string name;
    std::string type = "texture2D";
    VL::RHIImageHandle imageHandle;
    vk::Image image;
    vk::DeviceMemory memory;
    // Full-resource view used by shader descriptors. texture2DArray resources
    // expose all layers through this view.
    vk::ImageView imageView;
    VL::RHIImageViewHandle imageViewHandle;
    // Per-layer views used by framebuffer attachments. Single-layer resources
    // still keep layer 0 here so attachment binding is uniform.
    std::vector<vk::ImageView> imageViews;
    std::vector<VL::RHIImageViewHandle> imageViewHandles;
    vk::Format format;
    VL::RHISamplerHandle samplerHandle;
    vk::Sampler sampler;
    uint32_t width;
    uint32_t height;
    uint32_t arrayLayers = 1;
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
    void SetupDescriptors(const class RenderGraph& renderGraph, VL::RendererBackendVulkan& rendererBackend);
    void CreatePassDescriptorSetLayout(VL::RendererBackendVulkan& rendererBackend);
    void CreateDescriptorSets(VL::RendererBackendVulkan& rendererBackend);
    void UpdateDescriptorSets(
        VL::RendererBackendVulkan& rendererBackend,
        const VL::RendererDescriptorContext& descriptorContext,
        const VL::RendererPassDescriptorPlan& descriptorPlan);

    const std::vector<std::vector<vk::DescriptorSet>>& GetDescriptorSets() const { return descriptorSets; }

    std::string name;
    std::string type;
    uint32_t shadowCascadeIndex = 0;
    VL::RHIRenderPassHandle renderPassHandle;
    vk::RenderPass renderPass;
    std::vector<vk::ClearValue> clearValues;
    std::vector<VL::RHIFramebufferHandle> framebufferHandles;
    std::vector<vk::Framebuffer> framebuffers;
    uint32_t width;
    uint32_t height;
    PassPipelineContractKey pipelineContractKey;
    // pass 输入输出资源
    std::vector<std::string> inputResources;
    std::vector<VL::CompiledRenderGraphPassInputDescriptor> inputDescriptorPlan;
    std::vector<std::string> outputResources;
    std::unordered_map<std::string, uint32_t> outputLayers;
    std::unordered_map<std::string, vk::AttachmentLoadOp> outputLoadOps;
    std::unordered_map<std::string, vk::AttachmentStoreOp> outputStoreOps;
    // pass 输入描述符集, 统一使用Set3。inputDescriptorImageInfos 按 binding 下标保存，
    // binding 关系来自 CompiledRenderGraph，而不是运行时的输入数组顺序推断。
    vk::DescriptorPool descriptorPool;
    VL::RHIDescriptorPoolHandle descriptorPoolHandle;
    std::vector<std::vector<vk::DescriptorImageInfo>> inputDescriptorImageInfos;
    VL::RHIDescriptorSetLayoutHandle emptyDescriptorSetLayoutHandle;
    vk::DescriptorSetLayout emptyDescriptorSetLayout;
    VL::RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle;
    vk::DescriptorSetLayout descriptorSetLayout;
    std::vector<std::vector<VL::RHIDescriptorSetHandle>> descriptorSetHandles;
    std::vector<std::vector<vk::DescriptorSet>> descriptorSets;
    std::vector<std::vector<vk::WriteDescriptorSet>> writeDescriptorSets;

    // materialInstance 相关
    bool needsMaterial = false;
    std::string materialInstancePath;
    std::weak_ptr<MaterialInstance> materialInstance;
};

/**
 * @class RenderGraph
 * @brief 管理渲染管线的相关资源的创建、销毁和管理
 */
class RenderGraph
{
public:
    struct TestFaultInjection
    {
        // Zero disables numbered creation failures. These faults are only
        // configured on isolated candidate graphs by runtime validation tests.
        size_t failResourceCreationAt = 0;
        size_t failRenderPassCreationAt = 0;
        size_t failFramebufferCreationAt = 0;
        size_t failDescriptorCreationAt = 0;
        bool failPassMaterialContract = false;
    };

    static RenderGraph& GetInstance()
    {
        static RenderGraph instance;
        return instance;
    }
    ~RenderGraph();
    RenderGraph() = default;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;
    RenderGraph(RenderGraph&&) = delete;
    RenderGraph& operator=(RenderGraph&&) = delete;

    // Candidate graphs are built in an isolated instance. The final runtime
    // transaction swaps only already-created state at the render-thread safe
    // point, so no Vulkan creation or validation can fail after publication.
    void SwapState(RenderGraph& other) noexcept;
    bool HasState() const noexcept;
    void SetTestFaultInjection(
        TestFaultInjection injection) noexcept;
    void SetOwnerGeneration(uint64_t generation) noexcept
    {
        ownerGeneration = generation;
    }
    uint64_t GetOwnerGeneration() const noexcept
    {
        return ownerGeneration;
    }
    void LoadRenderGraph(
        const nlohmann::json& renderGraphJson,
        VL::RendererBackendVulkan& rendererBackend);
    void Shutdown(
        VL::RendererBackendVulkan& rendererBackend,
        VL::RenderGraphReleaseMode releaseMode = VL::RenderGraphReleaseMode::Immediate);
    const std::vector<std::string>& GetRenderpassesOrdered() const { return renderpassesOrdered; }
    std::unordered_map<std::string, Renderpass>& GetRenderpasses() { return renderpasses; }
    const std::unordered_map<std::string, Renderpass>& GetRenderpasses() const { return renderpasses; }
    // 返回构图阶段已经校验过的第一条 Shadow pass；当前图没有 Shadow pass
    // 时返回 nullptr。调用方不得跨 RenderGraph shutdown/reload 保存该指针。
    Renderpass* FindCanonicalShadowPass();
    const VL::CompiledRenderGraph& GetCompiledRenderGraph() const { return compiledFrameGraph; }
    const std::unordered_map<std::string, std::vector<RenderResource>>& GetResourcesMsaa() const { return resourcesMsaa; }
    const std::unordered_map<std::string, std::vector<RenderResource>>& GetResourcesResolve() const { return resourcesResolve; }
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
    std::unordered_map<std::string, std::shared_ptr<MaterialInstance>> CapturePassMaterialInstances() const;
    void RestorePassMaterialInstances(const std::unordered_map<std::string, std::shared_ptr<MaterialInstance>>& passMaterials);
    
private:
    RenderResource CreateRenderResource(
        const VL::CompiledRenderGraphResource& resourceDesc,
        VL::RendererBackendVulkan& rendererBackend,
        bool bIsMsaaSource = false);
    void DestroyRenderResource(
        RenderResource& resource,
        VL::RendererBackendVulkan& rendererBackend,
        VL::RenderGraphReleaseMode releaseMode);
    
    Renderpass CreateRenderpass(
        const VL::CompiledRenderGraphPass& passDesc,
        VL::RendererBackendVulkan& rendererBackend);
    void ValidateAndResolveCanonicalShadowPass();
    void DestroyRenderpass(
        Renderpass& renderpass,
        VL::RendererBackendVulkan& rendererBackend,
        VL::RenderGraphReleaseMode releaseMode);

    vk::RenderPass CreateVkRenderPass(
        Renderpass& renderpass,
        VL::RendererBackendVulkan& rendererBackend,
        bool bUseMsaa);

    std::vector<vk::Framebuffer> CreateVkFrameBuffers(
        const Renderpass& renderpass,
        const std::vector<std::string>& outputResources,
        VL::RendererBackendVulkan& rendererBackend,
        bool bUseMsaa);

    vk::Format GetFormat(const std::string& formatStr);
    vk::ImageUsageFlags GetImageUsage(const std::vector<std::string>& usageStr);

    vk::AttachmentLoadOp GetAttachmentLoadOp(const std::string& loadOpStr);
    vk::AttachmentStoreOp GetAttachmentStoreOp(const std::string& storeOpStr);
    bool IsDepthResource(const std::string& resourceName) const;
    
    std::vector<vk::ClearValue> GetClearValues(const std::vector<std::string>& outputResources, bool bUseMsaa);
    void MaybeFailResourceCreation();
    void MaybeFailRenderPassCreation();
    void MaybeFailFramebufferCreation();
    void MaybeFailDescriptorCreation();
private:
    std::unordered_map<std::string, std::vector<RenderResource>> resourcesMsaa;
    std::unordered_map<std::string, std::vector<RenderResource>> resourcesResolve;

    std::vector<std::string> renderpassesOrdered;
    std::unordered_map<std::string, Renderpass> renderpasses;
    std::string canonicalShadowPassName;
    VL::CompiledRenderGraph compiledFrameGraph;
    VL::RendererDescriptorPlanCache descriptorPlanCache;
    uint64_t ownerGeneration = 0;
    TestFaultInjection testFaultInjection;
    size_t testResourceCreationCount = 0;
    size_t testRenderPassCreationCount = 0;
    size_t testFramebufferCreationCount = 0;
    size_t testDescriptorCreationCount = 0;
};
