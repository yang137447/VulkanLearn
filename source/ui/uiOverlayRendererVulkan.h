#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "ui/uiRenderSnapshot.h"

namespace VL
{

class RendererBackendVulkan;

// Records immutable UI snapshots into the final swapchain image after render-graph passes.
// It owns Vulkan overlay resources but does not own widget state or input.
class UiOverlayRendererVulkan
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
        const UiRenderSnapshot& snapshot);

    bool IsInitialized() const { return initialized; }

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
    void DestroyTexture(GpuTexture& texture);
    void RetireTexture(GpuTexture& texture);
    void FlushRetiredTextures();
    vk::DescriptorSet ResolveTextureDescriptor(UiTextureId textureId) const;
    void EnsureWhiteTexture();

    RendererBackendVulkan* backend = nullptr;
    std::string vertexShaderPath;
    std::string fragmentShaderPath;
    vk::DescriptorSetLayout descriptorSetLayout;
    vk::DescriptorPool descriptorPool;
    vk::PipelineLayout pipelineLayout;
    vk::RenderPass renderPass;
    std::vector<vk::Framebuffer> framebuffers;
    std::vector<FrameBuffers> frameBuffers;
    vk::PipelineCache straightAlphaPipelineCache;
    vk::Pipeline straightAlphaPipeline;
    vk::PipelineCache premultipliedAlphaPipelineCache;
    vk::Pipeline premultipliedAlphaPipeline;
    std::unordered_map<UiTextureId, GpuTexture> textures;
    std::vector<RetiredTexture> retiredTextures;
    bool initialized = false;
};

} // namespace VL
