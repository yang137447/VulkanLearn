#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"
#include "render/rhi/rhiResourceHandles.h"

class PipelineFactory;
struct SDL_Window;

namespace VL
{

class RHIDeviceVulkan;

struct RendererFrameContext
{
    // frameIndex addresses the frame-in-flight sync ring; swapchainImageIndex
    // addresses per-swapchain-image command buffers, framebuffers, descriptors,
    // and UBO resources.
    uint32_t frameIndex = 0;
    uint32_t swapchainImageIndex = 0;
    uint32_t swapchainImageCount = 0;
    vk::CommandBuffer commandBuffer;
};

// Vulkan backend facade. It owns frame policy and delegates low-level
// Vulkan device/resource work to RHIDeviceVulkan while RenderSystem
// coordinates frame orchestration.
class RendererBackendVulkan
{
public:
    RendererBackendVulkan();
    ~RendererBackendVulkan();

    void Initialize(std::vector<const char*>& instanceExtensions, SDL_Window* window);
    void WaitIdle();
    void RecreateSwapchain(int width, int height);
    std::unique_ptr<PipelineFactory> CreatePipelineFactory();

    uint32_t GetSwapchainImageCount() const;
    vk::Extent2D GetSwapchainExtent() const;
    vk::Format GetSwapchainImageFormat() const;
    const std::vector<vk::ImageView>& GetSwapchainImageViews() const;
    std::pair<vk::Buffer, vk::DeviceMemory> CreateBuffer(
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName);
    void* MapMemory(vk::DeviceMemory memory, vk::DeviceSize size);
    void UnmapMemory(vk::DeviceMemory memory);
    void DestroyBuffer(vk::Buffer buffer, vk::DeviceMemory memory);
    void CopyBufferToBuffer(vk::Buffer source, vk::Buffer destination, vk::DeviceSize size);
    void CreatePerSwapchainBufferSet(
        Buffer& bufferSet,
        vk::DeviceSize bufferSize,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugNamePrefix);
    void DestroyBufferSet(Buffer& bufferSet);
    void SetupDescriptorBufferInfos(Buffer& bufferSet);
    std::pair<vk::Image, vk::DeviceMemory> CreateImage(
        uint32_t width,
        uint32_t height,
        uint32_t mipLevels,
        vk::SampleCountFlagBits samples,
        vk::Format format,
        vk::ImageTiling tiling,
        vk::ImageUsageFlags usage,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName);
    std::pair<vk::Image, vk::DeviceMemory> CreateImage(
        const vk::ImageCreateInfo& createInfo,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName);
    void TransitionImageLayout(
        vk::Image image,
        uint32_t mipLevels,
        vk::Format format,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout);
    vk::ImageView Create2DImageView(
        vk::Image image,
        uint32_t mipLevels,
        vk::Format format,
        vk::ImageAspectFlagBits aspectMask,
        const std::string& debugName);
    vk::ImageView CreateImageView(
        vk::Image image,
        vk::ImageViewType viewType,
        vk::Format format,
        vk::ImageAspectFlagBits aspectMask,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount,
        uint32_t baseArrayLayer,
        uint32_t layerCount,
        const std::string& debugName);
    vk::ImageView CreateCubeImageView(
        vk::Image image,
        uint32_t mipLevels,
        vk::Format format,
        const std::string& debugName);
    vk::ImageView CreateCubeStorageImageView(
        vk::Image image,
        vk::Format format,
        const std::string& debugName);
    void DestroyImageView(vk::ImageView& imageView);
    vk::Sampler Create2DSampler(const std::string& debugName);
    vk::Sampler Create2DSampler(
        vk::Filter filter,
        vk::SamplerAddressMode addressMode,
        bool enableMipmaps,
        const std::string& debugName);
    vk::Sampler CreateSampler(const vk::SamplerCreateInfo& createInfo, const std::string& debugName);
    vk::Sampler CreateCubeSampler(float maxLod, const std::string& debugName);
    void DestroySampler(vk::Sampler& sampler);
    vk::Sampler CreateDepthSampler(const std::string& debugName);
    vk::Sampler CreateDepthCompareSampler(const std::string& debugName);
    vk::CommandBuffer BeginSingleTimeCommands();
    void EndSingleTimeCommands(vk::CommandBuffer& commandBuffer);
    void CopyBufferToImage(
        vk::Buffer buffer,
        vk::Image image,
        uint32_t width,
        uint32_t height);
    void CopyImageToBuffer(
        vk::Image image,
        vk::Buffer buffer,
        uint32_t width,
        uint32_t height,
        bool flipY = false,
        vk::DeviceSize rowBytes = 0);
    void GenerateMipmaps(
        vk::Image image,
        uint32_t width,
        uint32_t height,
        uint32_t mipLevels);
    RHIImageHandle GetImageHandle(vk::Image image) const;
    RHIImageViewHandle GetImageViewHandle(vk::ImageView imageView) const;
    RHISamplerHandle GetSamplerHandle(vk::Sampler sampler) const;
    void DestroyImageResource(
        vk::Image& image,
        vk::DeviceMemory& memory,
        vk::ImageView& imageView,
        vk::Sampler& sampler);
    void DestroyImageResource(
        RHIImageHandle& imageHandle,
        RHIImageViewHandle& imageViewHandle,
        RHISamplerHandle& samplerHandle);
    vk::DescriptorSetLayout CreateDescriptorSetLayout(
        const vk::DescriptorSetLayoutCreateInfo& createInfo,
        const std::string& debugName);
    RHIDescriptorSetLayoutHandle GetDescriptorSetLayoutHandle(vk::DescriptorSetLayout descriptorSetLayout) const;
    void DestroyDescriptorSetLayout(vk::DescriptorSetLayout& descriptorSetLayout);
    void DestroyDescriptorSetLayout(RHIDescriptorSetLayoutHandle& descriptorSetLayoutHandle);
    vk::DescriptorPool CreateDescriptorPool(
        const vk::DescriptorPoolCreateInfo& createInfo,
        const std::string& debugName);
    RHIDescriptorPoolHandle GetDescriptorPoolHandle(vk::DescriptorPool descriptorPool) const;
    void DestroyDescriptorPool(vk::DescriptorPool& descriptorPool);
    void DestroyDescriptorPool(RHIDescriptorPoolHandle& descriptorPoolHandle);
    RHIDescriptorSetHandle GetDescriptorSetHandle(vk::DescriptorSet descriptorSet) const;
    void AllocateDescriptorSets(
        const vk::DescriptorSetAllocateInfo& allocateInfo,
        std::vector<vk::DescriptorSet>& descriptorSets);
    void FreeDescriptorSet(vk::DescriptorPool descriptorPool, vk::DescriptorSet& descriptorSet);
    void UpdateDescriptorSets(const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets);
    void SetDescriptorSetDebugName(vk::DescriptorSet descriptorSet, const std::string& debugName);
    vk::RenderPass CreateRenderPass(
        const vk::RenderPassCreateInfo2& createInfo,
        const std::string& debugName);
    RHIRenderPassHandle GetRenderPassHandle(vk::RenderPass renderPass) const;
    void DestroyRenderPass(vk::RenderPass& renderPass);
    void DestroyRenderPass(RHIRenderPassHandle& renderPassHandle);
    vk::Framebuffer CreateFramebuffer(
        const vk::FramebufferCreateInfo& createInfo,
        const std::string& debugName);
    RHIFramebufferHandle GetFramebufferHandle(vk::Framebuffer framebuffer) const;
    void DestroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers);
    void DestroyFramebuffers(std::vector<RHIFramebufferHandle>& framebufferHandles);
    RendererFrameContext BeginFrame(uint32_t currentFrame);
    void SubmitFrame(const RendererFrameContext& frameContext, uint32_t currentFrame);

private:
    RHIBufferHandle RequireBufferHandle(vk::Buffer buffer) const;
    RHIBufferHandle RequireBufferMemoryHandle(vk::DeviceMemory memory) const;
    void BindBufferHandle(RHIBufferHandle bufferHandle);
    void UnbindBufferHandle(RHIBufferHandle bufferHandle);
    RHIImageHandle RequireImageHandle(vk::Image image) const;
    RHIImageHandle RequireImageMemoryHandle(vk::DeviceMemory memory) const;
    void BindImageHandle(RHIImageHandle imageHandle);
    void UnbindImageHandle(RHIImageHandle imageHandle);
    RHIImageViewHandle RequireImageViewHandle(vk::ImageView imageView) const;
    void BindImageViewHandle(RHIImageViewHandle imageViewHandle);
    void UnbindImageViewHandle(RHIImageViewHandle imageViewHandle);
    RHISamplerHandle RequireSamplerHandle(vk::Sampler sampler) const;
    void BindSamplerHandle(RHISamplerHandle samplerHandle);
    void UnbindSamplerHandle(RHISamplerHandle samplerHandle);
    RHIDescriptorSetLayoutHandle RequireDescriptorSetLayoutHandle(vk::DescriptorSetLayout descriptorSetLayout) const;
    RHIDescriptorSetLayoutHandle ResolveDescriptorSetLayoutHandle(vk::DescriptorSetLayout descriptorSetLayout);
    void BindDescriptorSetLayoutHandle(RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle);
    void UnbindDescriptorSetLayoutHandle(RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle);
    RHIDescriptorPoolHandle RequireDescriptorPoolHandle(vk::DescriptorPool descriptorPool) const;
    void BindDescriptorPoolHandle(RHIDescriptorPoolHandle descriptorPoolHandle);
    void UnbindDescriptorPoolHandle(RHIDescriptorPoolHandle descriptorPoolHandle);
    RHIDescriptorSetHandle RequireDescriptorSetHandle(vk::DescriptorSet descriptorSet) const;
    void BindDescriptorSetHandle(
        RHIDescriptorSetHandle descriptorSetHandle,
        RHIDescriptorPoolHandle descriptorPoolHandle);
    void UnbindDescriptorSetHandle(RHIDescriptorSetHandle descriptorSetHandle);
    std::vector<RHIDescriptorWrite> BuildDescriptorWrites(
        const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets) const;
    RHIRenderPassHandle RequireRenderPassHandle(vk::RenderPass renderPass) const;
    void BindRenderPassHandle(RHIRenderPassHandle renderPassHandle);
    void UnbindRenderPassHandle(RHIRenderPassHandle renderPassHandle);
    RHIFramebufferHandle RequireFramebufferHandle(vk::Framebuffer framebuffer) const;
    void BindFramebufferHandle(RHIFramebufferHandle framebufferHandle);
    void UnbindFramebufferHandle(RHIFramebufferHandle framebufferHandle);

    std::unique_ptr<RHIDeviceVulkan> rhiDevice;
    bool initialized = false;
    std::vector<uint64_t> frameFenceEpochs;
    std::unordered_map<VkBuffer, RHIBufferHandle> bufferHandlesByBuffer;
    std::unordered_map<VkDeviceMemory, RHIBufferHandle> bufferHandlesByMemory;
    std::unordered_map<VkImage, RHIImageHandle> imageHandlesByImage;
    std::unordered_map<VkDeviceMemory, RHIImageHandle> imageHandlesByMemory;
    std::unordered_map<VkImageView, RHIImageViewHandle> imageViewHandlesByImageView;
    std::unordered_map<VkSampler, RHISamplerHandle> samplerHandlesBySampler;
    std::unordered_map<VkDescriptorSetLayout, RHIDescriptorSetLayoutHandle> descriptorSetLayoutHandlesByLayout;
    std::unordered_map<VkDescriptorPool, RHIDescriptorPoolHandle> descriptorPoolHandlesByPool;
    std::unordered_map<VkDescriptorSet, RHIDescriptorSetHandle> descriptorSetHandlesBySet;
    std::unordered_map<VkDescriptorPool, std::vector<RHIDescriptorSetHandle>> descriptorSetHandlesByPool;
    std::unordered_map<VkRenderPass, RHIRenderPassHandle> renderPassHandlesByRenderPass;
    std::unordered_map<VkFramebuffer, RHIFramebufferHandle> framebufferHandlesByFramebuffer;
};

} // namespace VL
