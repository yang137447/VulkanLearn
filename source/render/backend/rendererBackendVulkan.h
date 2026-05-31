#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"
#include "render/rhi/vulkan/rhiDeviceVulkan.h"

class PipelineFactory;
struct SDL_Window;

namespace VL
{

struct RendererFrameContext
{
    uint32_t frameIndex = 0;
    uint32_t swapchainImageIndex = 0;
    uint32_t swapchainImageCount = 0;
    vk::CommandBuffer commandBuffer;
};

// Transitional Vulkan backend facade. It owns frame policy and delegates raw
// Vulkan device/resource work to the RHI device while RenderSystem still
// records pass commands during the backend split.
class RendererBackendVulkan
{
public:
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
    void DestroyImageResource(
        vk::Image& image,
        vk::DeviceMemory& memory,
        vk::ImageView& imageView,
        vk::Sampler& sampler);
    vk::DescriptorSetLayout CreateDescriptorSetLayout(
        const vk::DescriptorSetLayoutCreateInfo& createInfo,
        const std::string& debugName);
    void DestroyDescriptorSetLayout(vk::DescriptorSetLayout& descriptorSetLayout);
    vk::DescriptorPool CreateDescriptorPool(
        const vk::DescriptorPoolCreateInfo& createInfo,
        const std::string& debugName);
    void DestroyDescriptorPool(vk::DescriptorPool& descriptorPool);
    void AllocateDescriptorSets(
        const vk::DescriptorSetAllocateInfo& allocateInfo,
        std::vector<vk::DescriptorSet>& descriptorSets);
    void FreeDescriptorSet(vk::DescriptorPool descriptorPool, vk::DescriptorSet& descriptorSet);
    void UpdateDescriptorSets(const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets);
    void SetDescriptorSetDebugName(vk::DescriptorSet descriptorSet, const std::string& debugName);
    vk::RenderPass CreateRenderPass(
        const vk::RenderPassCreateInfo2& createInfo,
        const std::string& debugName);
    void DestroyRenderPass(vk::RenderPass& renderPass);
    vk::Framebuffer CreateFramebuffer(
        const vk::FramebufferCreateInfo& createInfo,
        const std::string& debugName);
    void DestroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers);
    RendererFrameContext BeginFrame(uint32_t currentFrame);
    void SubmitFrame(const RendererFrameContext& frameContext, uint32_t currentFrame);

private:
    RHIDeviceVulkan rhiDevice;
    bool initialized = false;
    std::vector<uint64_t> frameFenceEpochs;
};

} // namespace VL
