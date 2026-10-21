#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "ui/uiRenderSnapshot.h"
#include "shader/reload/uiOverlayReloadParticipant.h"

namespace VL
{

class RendererBackendVulkan;

// Records immutable UI snapshots into the final swapchain image after render-graph passes.
// It owns Vulkan overlay resources but does not own widget state or input.
class UiOverlayRendererVulkan
    : public UiOverlayReloadParticipant
{
public:
    void Initialize(
        RendererBackendVulkan& rendererBackend,
        std::string vertexShaderPath,
        std::string fragmentShaderPath);
    void Shutdown();

    void ReleaseSwapchainDependentResources();
    void RebuildSwapchainDependentResources();
    void SynchronizeTextures(const UiRenderSnapshot& snapshot);
    void CollectRetiredTextures();
    void Record(
        vk::CommandBuffer commandBuffer,
        uint32_t swapchainImageIndex,
        const UiRenderSnapshot& snapshot,
        vk::ImageView sceneViewportView = {},
        vk::Sampler sceneViewportSampler = {},
        vk::ImageView sceneDepthView = {},
        vk::Sampler sceneDepthSampler = {});

    bool IsInitialized() const { return initialized; }

    std::string GetShaderName() const override;
    const GraphicsShaderVariantArtifact& GetActiveArtifact() const override
    {
        return activeShaderArtifact;
    }
    void SetActiveShaderArtifact(
        const GraphicsShaderVariantArtifact& artifact)
    {
        activeShaderArtifact = artifact;
    }
    UiOverlayPipelineReplacement PrepareReplacementPipelines(
        const GraphicsShaderVariantArtifact& candidate) override;
    void CommitReplacement(
        GraphicsShaderVariantArtifact committedArtifact,
        UiOverlayPipelineReplacement&& replacement) noexcept override;

private:
    struct DynamicBuffer
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
        vk::DeviceSize capacity = 0;
        void* mappedMemory = nullptr;
    };

    struct FrameBuffers
    {
        DynamicBuffer vertices;
        DynamicBuffer indices;
    };

    struct GpuTexture
    {
        UiTextureId id = 0;
        uint64_t generation = 0;
        vk::Image image;
        vk::DeviceMemory memory;
        vk::ImageView imageView;
        vk::Sampler sampler;
        vk::DescriptorSet descriptorSet;
        // descriptor 退休完成前固定 token；借用的成图不会进入本类的图像销毁路径。
        std::shared_ptr<const RendererGpuImageToken> gpuImage;
    };

    struct RetiredTexture
    {
        GpuTexture texture;
        uint64_t lastUsedEpoch = 0;
    };

    void CreateDescriptorResources();
    void DestroyDescriptorResources();
    void CreateSwapchainResources();
    void DestroySwapchainResources();
    void CreateRenderPassAndFramebuffers();
    void DestroyRenderPassAndFramebuffers();
    void CreatePipelines();
    UiOverlayPipelineReplacement BuildUiOverlayPipelinePair(
        const std::vector<uint32_t>& vertexSpirv,
        const std::vector<uint32_t>& fragmentSpirv);
    void DestroyPipelines();
    void CreateFrameBuffers();
    void DestroyFrameBuffers();
    void EnsureBufferCapacity(
        DynamicBuffer& buffer,
        vk::DeviceSize requiredSize,
        vk::BufferUsageFlags usage,
        const std::string& debugName);
    void DestroyDynamicBuffer(DynamicBuffer& buffer);
    GpuTexture CreateTexture(const UiTextureSnapshot& snapshot);
    GpuTexture CreateThumbnailTexture(const UiTextureSnapshot& snapshot);
    void DestroyTexture(GpuTexture& texture);
    void RetireTexture(GpuTexture& texture);
    void FlushRetiredTextures();
    vk::DescriptorSet ResolveTextureDescriptor(UiTextureId textureId, uint32_t imageIndex) const;
    void EnsureWhiteTexture();

    RendererBackendVulkan* backend = nullptr;
    std::string vertexShaderPath;
    std::string fragmentShaderPath;
    vk::DescriptorSetLayout descriptorSetLayout;
    vk::DescriptorPool descriptorPool;
    vk::PipelineLayout pipelineLayout;
    vk::RenderPass renderPass;
    vk::RenderPass editorPresentRenderPass;
    std::vector<vk::Framebuffer> framebuffers;
    std::vector<FrameBuffers> frameBuffers;
    std::vector<vk::DescriptorSet> sceneViewportDescriptors;
    std::vector<vk::DescriptorSet> sceneDepthDescriptors;
    vk::PipelineCache straightAlphaPipelineCache;
    vk::Pipeline straightAlphaPipeline;
    vk::PipelineCache premultipliedAlphaPipelineCache;
    vk::Pipeline premultipliedAlphaPipeline;
    GraphicsShaderVariantArtifact activeShaderArtifact;
    std::unordered_map<UiTextureId, GpuTexture> textures;
    std::vector<RetiredTexture> retiredTextures;
    bool initialized = false;
};

} // namespace VL
