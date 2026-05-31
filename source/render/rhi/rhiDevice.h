#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

struct SDL_Window;

namespace VL
{

// Stable device-facing RHI contract used by the backend facade. V1 still passes
// some Vulkan resource handles through creation helpers, but this interface no
// longer exposes raw device/queue/swapchain escape hatches to higher layers.
class RHIDevice
{
public:
    virtual ~RHIDevice() = default;

    virtual void Initialize(std::vector<const char*>& instanceExtensions, SDL_Window* window) = 0;
    virtual bool IsInitialized() const = 0;

    virtual void WaitIdle() = 0;
    virtual void RecreateSwapchain(int width, int height) = 0;

    virtual uint32_t GetSwapchainImageCount() const = 0;
    virtual vk::Extent2D GetSwapchainExtent() const = 0;
    virtual vk::Format GetSwapchainImageFormat() const = 0;
    virtual const std::vector<vk::ImageView>& GetSwapchainImageViews() const = 0;

    virtual std::pair<vk::Buffer, vk::DeviceMemory> CreateBuffer(
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName) = 0;
    virtual void* MapMemory(vk::DeviceMemory memory, vk::DeviceSize size) = 0;
    virtual void UnmapMemory(vk::DeviceMemory memory) = 0;
    virtual void DestroyBuffer(vk::Buffer buffer, vk::DeviceMemory memory) = 0;
    virtual void CopyBufferToBuffer(vk::Buffer source, vk::Buffer destination, vk::DeviceSize size) = 0;

    virtual std::pair<vk::Image, vk::DeviceMemory> CreateImage(
        uint32_t width,
        uint32_t height,
        uint32_t mipLevels,
        vk::SampleCountFlagBits samples,
        vk::Format format,
        vk::ImageTiling tiling,
        vk::ImageUsageFlags usage,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName) = 0;
    virtual std::pair<vk::Image, vk::DeviceMemory> CreateImage(
        const vk::ImageCreateInfo& createInfo,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName) = 0;
    virtual void TransitionImageLayout(
        vk::Image image,
        uint32_t mipLevels,
        vk::Format format,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout) = 0;
    virtual vk::ImageView Create2DImageView(
        vk::Image image,
        uint32_t mipLevels,
        vk::Format format,
        vk::ImageAspectFlagBits aspectMask,
        const std::string& debugName) = 0;
    virtual vk::ImageView CreateImageView(
        vk::Image image,
        vk::ImageViewType viewType,
        vk::Format format,
        vk::ImageAspectFlagBits aspectMask,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount,
        uint32_t baseArrayLayer,
        uint32_t layerCount,
        const std::string& debugName) = 0;
    virtual vk::ImageView CreateCubeImageView(
        vk::Image image,
        uint32_t mipLevels,
        vk::Format format,
        const std::string& debugName) = 0;
    virtual vk::ImageView CreateCubeStorageImageView(
        vk::Image image,
        vk::Format format,
        const std::string& debugName) = 0;
    virtual void DestroyImageView(vk::ImageView& imageView) = 0;
    virtual vk::Sampler Create2DSampler(const std::string& debugName) = 0;
    virtual vk::Sampler Create2DSampler(
        vk::Filter filter,
        vk::SamplerAddressMode addressMode,
        bool enableMipmaps,
        const std::string& debugName) = 0;
    virtual vk::Sampler CreateSampler(
        const vk::SamplerCreateInfo& createInfo,
        const std::string& debugName) = 0;
    virtual vk::Sampler CreateCubeSampler(float maxLod, const std::string& debugName) = 0;
    virtual void DestroySampler(vk::Sampler& sampler) = 0;
    virtual vk::Sampler CreateDepthSampler(const std::string& debugName) = 0;
    virtual vk::Sampler CreateDepthCompareSampler(const std::string& debugName) = 0;
    virtual vk::CommandBuffer BeginSingleTimeCommands() = 0;
    virtual void EndSingleTimeCommands(vk::CommandBuffer& commandBuffer) = 0;
    virtual void CopyBufferToImage(
        vk::Buffer buffer,
        vk::Image image,
        uint32_t width,
        uint32_t height) = 0;
    virtual void CopyImageToBuffer(
        vk::Image image,
        vk::Buffer buffer,
        uint32_t width,
        uint32_t height,
        bool flipY = false,
        vk::DeviceSize rowBytes = 0) = 0;
    virtual void GenerateMipmaps(
        vk::Image image,
        uint32_t width,
        uint32_t height,
        uint32_t mipLevels) = 0;
    virtual void DestroyImageResource(
        vk::Image& image,
        vk::DeviceMemory& memory,
        vk::ImageView& imageView,
        vk::Sampler& sampler) = 0;

    virtual vk::DescriptorSetLayout CreateDescriptorSetLayout(
        const vk::DescriptorSetLayoutCreateInfo& createInfo,
        const std::string& debugName) = 0;
    virtual void DestroyDescriptorSetLayout(vk::DescriptorSetLayout& descriptorSetLayout) = 0;
    virtual vk::DescriptorPool CreateDescriptorPool(
        const vk::DescriptorPoolCreateInfo& createInfo,
        const std::string& debugName) = 0;
    virtual void DestroyDescriptorPool(vk::DescriptorPool& descriptorPool) = 0;
    virtual void AllocateDescriptorSets(
        const vk::DescriptorSetAllocateInfo& allocateInfo,
        std::vector<vk::DescriptorSet>& descriptorSets) = 0;
    virtual void FreeDescriptorSet(vk::DescriptorPool descriptorPool, vk::DescriptorSet& descriptorSet) = 0;
    virtual void UpdateDescriptorSets(const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets) = 0;
    virtual void SetDescriptorSetDebugName(vk::DescriptorSet descriptorSet, const std::string& debugName) = 0;
    virtual vk::RenderPass CreateRenderPass(
        const vk::RenderPassCreateInfo2& createInfo,
        const std::string& debugName) = 0;
    virtual void DestroyRenderPass(vk::RenderPass& renderPass) = 0;
    virtual vk::Framebuffer CreateFramebuffer(
        const vk::FramebufferCreateInfo& createInfo,
        const std::string& debugName) = 0;
    virtual void DestroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers) = 0;
};

} // namespace VL
