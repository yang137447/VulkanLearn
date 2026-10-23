#pragma once

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <vulkan/vulkan.hpp>

#include "baseStructs.h"
#include "presentTarget.h"
#include "render/backend/swapchainFrameResult.h"
#include "render/rhi/rhiResourceHandles.h"

struct SDL_Window;
class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;

struct VulkanBufferResource
{
    vk::Buffer buffer;
    vk::DeviceMemory memory;
};

struct VulkanImageResource
{
    vk::Image image;
    vk::DeviceMemory memory;
};

struct VulkanImageViewResource
{
    vk::ImageView imageView;
};

struct VulkanSamplerResource
{
    vk::Sampler sampler;
};

struct VulkanDescriptorSetLayoutResource
{
    vk::DescriptorSetLayout descriptorSetLayout;
    bool ownsResource = true;
};

struct VulkanDescriptorPoolResource
{
    vk::DescriptorPool descriptorPool;
};

struct VulkanDescriptorSetResource
{
    vk::DescriptorSet descriptorSet;
    RHIDescriptorPoolHandle descriptorPoolHandle;
};

struct VulkanRenderPassResource
{
    vk::RenderPass renderPass;
};

struct VulkanFramebufferResource
{
    vk::Framebuffer framebuffer;
};

struct VulkanFrameSyncResources
{
    vk::Fence taskFinishedFence;
    vk::Semaphore imageAcquiredSemaphore;
};

struct VulkanSwapchainImageSyncResources
{
    vk::CommandBuffer commandBuffer;
    vk::Semaphore renderFinishedSemaphore;
};

// Vulkan device boundary for swapchain, queues, and raw GPU object helpers.
// RendererBackendVulkan owns frame policy; this class owns the low-level
// VulkanManager calls that create, destroy, and name Vulkan resources.
class RHIDeviceVulkan
{
public:
    void Initialize(std::vector<const char*>& instanceExtensions, SDL_Window* window);
    bool IsInitialized() const { return initialized; }

    void WaitIdle();
    // 呈现函数接受目标：“哪个窗口”是显式参数，而不是藏在单例里的隐式默认目标。
    // Initialize 不在此列：它是引导入口，负责创建设备与目标 0。
    void RecreateSwapchain(VL::PresentTarget& target, int width, int height);
    void WaitForFence(vk::Fence fence);
    void ResetFence(vk::Fence fence);
    SwapchainFrameResult AcquireNextSwapchainImage(
        VL::PresentTarget& target,
        vk::Semaphore imageAcquiredSemaphore,
        uint32_t& swapchainImageIndex);
    void SubmitToGraphicsQueue(const vk::SubmitInfo& submitInfo, vk::Fence signalFence);
    SwapchainFrameResult PresentSwapchainImage(
        VL::PresentTarget& target,
        vk::Semaphore renderFinishedSemaphore,
        uint32_t swapchainImageIndex);

    uint32_t GetFrameFenceCount() const;
    VulkanFrameSyncResources GetFrameSyncResources(uint32_t frameIndex) const;
    VulkanSwapchainImageSyncResources GetSwapchainImageSyncResources(
        const VL::PresentTarget& target, uint32_t swapchainImageIndex) const;
    vk::Fence GetImageInFlightFence(const VL::PresentTarget& target, uint32_t swapchainImageIndex) const;
    void SetImageInFlightFence(VL::PresentTarget& target, uint32_t swapchainImageIndex, vk::Fence fence);
    uint32_t GetSwapchainImageCount(const VL::PresentTarget& target) const;
    vk::Extent2D GetSwapchainExtent(const VL::PresentTarget& target) const;
    vk::Format GetSwapchainImageFormat(const VL::PresentTarget& target) const;
    const std::vector<vk::ImageView>& GetSwapchainImageViews(const VL::PresentTarget& target) const;
    const std::vector<vk::Image>& GetSwapchainImages(const VL::PresentTarget& target) const;

    RHIBufferHandle CreateBuffer(
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName);
    void* MapBufferMemory(RHIBufferHandle bufferHandle, vk::DeviceSize size);
    void UnmapBufferMemory(RHIBufferHandle bufferHandle);
    void DestroyBuffer(RHIBufferHandle bufferHandle);
    void CopyBufferToBuffer(
        RHIBufferHandle source,
        RHIBufferHandle destination,
        vk::DeviceSize size);
    const VulkanBufferResource& GetVulkanBufferResource(RHIBufferHandle bufferHandle) const;

    RHIImageHandle CreateImage(
        uint32_t width,
        uint32_t height,
        uint32_t mipLevels,
        vk::SampleCountFlagBits samples,
        vk::Format format,
        vk::ImageTiling tiling,
        vk::ImageUsageFlags usage,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName);
    RHIImageHandle CreateImage(
        const vk::ImageCreateInfo& createInfo,
        vk::MemoryPropertyFlags memoryPropertyFlags,
        const std::string& debugName);
    void TransitionImageLayout(
        RHIImageHandle imageHandle,
        uint32_t mipLevels,
        vk::Format format,
        vk::ImageLayout oldLayout,
        vk::ImageLayout newLayout);
    RHIImageViewHandle Create2DImageView(
        RHIImageHandle imageHandle,
        uint32_t mipLevels,
        vk::Format format,
        vk::ImageAspectFlagBits aspectMask,
        const std::string& debugName);
    RHIImageViewHandle CreateImageView(
        RHIImageHandle imageHandle,
        vk::ImageViewType viewType,
        vk::Format format,
        vk::ImageAspectFlagBits aspectMask,
        uint32_t baseMipLevel,
        uint32_t mipLevelCount,
        uint32_t baseArrayLayer,
        uint32_t layerCount,
        const std::string& debugName);
    RHIImageViewHandle CreateCubeImageView(
        RHIImageHandle imageHandle,
        uint32_t mipLevels,
        vk::Format format,
        const std::string& debugName);
    RHIImageViewHandle CreateCubeStorageImageView(
        RHIImageHandle imageHandle,
        vk::Format format,
        const std::string& debugName);
    void DestroyImageView(RHIImageViewHandle imageViewHandle);
    const VulkanImageViewResource& GetVulkanImageViewResource(RHIImageViewHandle imageViewHandle) const;
    RHIImageViewHandle RegisterImageView(vk::ImageView imageView);

    RHISamplerHandle Create2DSampler(const std::string& debugName);
    RHISamplerHandle Create2DSampler(
        vk::Filter filter,
        vk::SamplerAddressMode addressMode,
        bool enableMipmaps,
        const std::string& debugName);
    RHISamplerHandle CreateSampler(const vk::SamplerCreateInfo& createInfo, const std::string& debugName);
    RHISamplerHandle CreateCubeSampler(float maxLod, const std::string& debugName);
    void DestroySampler(RHISamplerHandle samplerHandle);
    const VulkanSamplerResource& GetVulkanSamplerResource(RHISamplerHandle samplerHandle) const;
    RHISamplerHandle RegisterSampler(vk::Sampler sampler);
    RHISamplerHandle CreateDepthSampler(const std::string& debugName);
    RHISamplerHandle CreateDepthCompareSampler(const std::string& debugName);
    vk::CommandBuffer BeginSingleTimeCommands();
    // 这是一次性提交的生命周期边界：提交后必须等待完成，未提交时才可 Abort。
    void EndSingleTimeCommands(vk::CommandBuffer& commandBuffer);
    void AbortSingleTimeCommands(vk::CommandBuffer& commandBuffer);
    void CopyBufferToImage(
        RHIBufferHandle bufferHandle,
        RHIImageHandle imageHandle,
        uint32_t width,
        uint32_t height);
    void CopyImageToBuffer(
        RHIImageHandle imageHandle,
        RHIBufferHandle bufferHandle,
        uint32_t width,
        uint32_t height,
        bool flipY = false,
        vk::DeviceSize rowBytes = 0);
    void GenerateMipmaps(
        RHIImageHandle imageHandle,
        uint32_t width,
        uint32_t height,
        uint32_t mipLevels);
    void DestroyImageResource(
        RHIImageHandle imageHandle,
        RHIImageViewHandle imageViewHandle,
        RHISamplerHandle samplerHandle);
    const VulkanImageResource& GetVulkanImageResource(RHIImageHandle imageHandle) const;

    RHIDescriptorSetLayoutHandle CreateDescriptorSetLayout(
        const vk::DescriptorSetLayoutCreateInfo& createInfo,
        const std::string& debugName);
    void DestroyDescriptorSetLayout(RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle);
    const VulkanDescriptorSetLayoutResource& GetVulkanDescriptorSetLayoutResource(
        RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle) const;
    RHIDescriptorSetLayoutHandle RegisterDescriptorSetLayout(
        vk::DescriptorSetLayout descriptorSetLayout,
        bool ownsResource);

    RHIDescriptorPoolHandle CreateDescriptorPool(
        const vk::DescriptorPoolCreateInfo& createInfo,
        const std::string& debugName);
    void DestroyDescriptorPool(RHIDescriptorPoolHandle descriptorPoolHandle);
    const VulkanDescriptorPoolResource& GetVulkanDescriptorPoolResource(
        RHIDescriptorPoolHandle descriptorPoolHandle) const;
    std::vector<RHIDescriptorSetHandle> AllocateDescriptorSets(
        RHIDescriptorPoolHandle descriptorPoolHandle,
        const std::vector<RHIDescriptorSetLayoutHandle>& descriptorSetLayoutHandles);
    void FreeDescriptorSet(
        RHIDescriptorPoolHandle descriptorPoolHandle,
        RHIDescriptorSetHandle descriptorSetHandle);
    const VulkanDescriptorSetResource& GetVulkanDescriptorSetResource(
        RHIDescriptorSetHandle descriptorSetHandle) const;
    void UpdateDescriptorSets(const std::vector<RHIDescriptorWrite>& writeDescriptorSets);
    void SetDescriptorSetDebugName(
        RHIDescriptorSetHandle descriptorSetHandle,
        const std::string& debugName);
    RHIRenderPassHandle CreateRenderPass(
        const vk::RenderPassCreateInfo2& createInfo,
        const std::string& debugName);
    void DestroyRenderPass(RHIRenderPassHandle renderPassHandle);
    const VulkanRenderPassResource& GetVulkanRenderPassResource(
        RHIRenderPassHandle renderPassHandle) const;
    RHIFramebufferHandle CreateFramebuffer(
        RHIRenderPassHandle renderPassHandle,
        const vk::FramebufferCreateInfo& createInfo,
        const std::string& debugName);
    void DestroyFramebuffer(RHIFramebufferHandle framebufferHandle);
    const VulkanFramebufferResource& GetVulkanFramebufferResource(
        RHIFramebufferHandle framebufferHandle) const;
    float GetTimestampPeriodNanoseconds();
    uint32_t GetGraphicsTimestampValidBits();
    // 统计功能只依赖能力查询，不向调用方暴露物理设备。
    bool SupportsTimestampQueries();
    // 非阻塞读取：命令已经随 fence 完成时返回 true；VK_NOT_READY 返回 false。
    bool TryReadTimestampQueryPair(
        vk::QueryPool queryPool,
        uint32_t firstQuery,
        std::array<uint64_t, 2>& timestamps);
    // 向上暴露逻辑设备已启用的双源混合能力，供材质 variant 与 pipeline 同步降级。
    bool IsDualSourceBlendEnabled() const;
    vk::QueryPool CreateTimestampQueryPool(
        uint32_t queryCount,
        const std::string& debugName);
    void DestroyQueryPool(vk::QueryPool& queryPool);
    void ReadTimestampQueryPair(
        vk::QueryPool queryPool,
        uint32_t firstQuery,
        std::array<uint64_t, 2>& timestamps);

private:
    friend class RendererBackendVulkan;

    vk::Device& GetDevice();
    vk::PhysicalDevice& GetPhysicalDevice();
    vk::PhysicalDeviceMemoryProperties& GetGpuMemoryProperties();
    vk::CommandPool& GetCommandPool();
    vk::Queue& GetGraphicsQueue();
    // 呈现目标的入口。取代原来的无参 GetSwapchain()：后端要的不是"那个 swapchain"，
    // 而是"这个后端负责的呈现目标"，后者才带得动第二个窗口。
    // 目标 0 之外的接线尚未完成，因此这里暂时只返回主窗口的目标。
    VL::PresentTarget& GetPresentTarget();
    std::vector<vk::Fence>& GetTaskFinishedFences();
    std::vector<vk::Semaphore>& GetImageAcquiredSemaphores();

    const VulkanBufferResource& RequireBufferResource(RHIBufferHandle bufferHandle) const;
    const VulkanImageResource& RequireImageResource(RHIImageHandle imageHandle) const;
    const VulkanImageViewResource& RequireImageViewResource(RHIImageViewHandle imageViewHandle) const;
    const VulkanSamplerResource& RequireSamplerResource(RHISamplerHandle samplerHandle) const;
    const VulkanDescriptorSetLayoutResource& RequireDescriptorSetLayoutResource(
        RHIDescriptorSetLayoutHandle descriptorSetLayoutHandle) const;
    const VulkanDescriptorPoolResource& RequireDescriptorPoolResource(
        RHIDescriptorPoolHandle descriptorPoolHandle) const;
    const VulkanDescriptorSetResource& RequireDescriptorSetResource(
        RHIDescriptorSetHandle descriptorSetHandle) const;
    const VulkanRenderPassResource& RequireRenderPassResource(
        RHIRenderPassHandle renderPassHandle) const;
    const VulkanFramebufferResource& RequireFramebufferResource(
        RHIFramebufferHandle framebufferHandle) const;

    bool initialized = false;
    uint64_t nextBufferId = 1;
    std::unordered_map<uint64_t, VulkanBufferResource> buffers;
    uint64_t nextImageId = 1;
    std::unordered_map<uint64_t, VulkanImageResource> images;
    uint64_t nextImageViewId = 1;
    std::unordered_map<uint64_t, VulkanImageViewResource> imageViews;
    uint64_t nextSamplerId = 1;
    std::unordered_map<uint64_t, VulkanSamplerResource> samplers;
    uint64_t nextDescriptorSetLayoutId = 1;
    std::unordered_map<uint64_t, VulkanDescriptorSetLayoutResource> descriptorSetLayouts;
    uint64_t nextDescriptorPoolId = 1;
    std::unordered_map<uint64_t, VulkanDescriptorPoolResource> descriptorPools;
    uint64_t nextDescriptorSetId = 1;
    std::unordered_map<uint64_t, VulkanDescriptorSetResource> descriptorSets;
    uint64_t nextRenderPassId = 1;
    std::unordered_map<uint64_t, VulkanRenderPassResource> renderPasses;
    uint64_t nextFramebufferId = 1;
    std::unordered_map<uint64_t, VulkanFramebufferResource> framebuffers;
};

} // namespace VL
