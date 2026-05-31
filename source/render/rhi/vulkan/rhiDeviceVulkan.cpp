#include "render/rhi/vulkan/rhiDeviceVulkan.h"

#include <stdexcept>

#include "commonFunction.h"
#include "vulkanDebug.h"
#include "vulkanManager.h"

namespace VL
{

void RHIDeviceVulkan::Initialize(
    std::vector<const char*>& instanceExtensions,
    SDL_Window* window)
{
    VulkanManager::GetInstance().Init(instanceExtensions, window);
    initialized = true;
}

void RHIDeviceVulkan::WaitIdle()
{
    if (!initialized)
    {
        return;
    }

    GetDevice().waitIdle();
}

void RHIDeviceVulkan::RecreateSwapchain(int width, int height)
{
    if (!initialized)
    {
        return;
    }

    VulkanManager::GetInstance().ReCreateSwapChain(width, height);
}

vk::Device& RHIDeviceVulkan::GetDevice()
{
    return VulkanManager::GetInstance().GetDevice();
}

vk::PhysicalDevice& RHIDeviceVulkan::GetPhysicalDevice()
{
    return VulkanManager::GetInstance().GetPhysicalDevice();
}

vk::PhysicalDeviceMemoryProperties& RHIDeviceVulkan::GetGpuMemoryProperties()
{
    return VulkanManager::GetInstance().GetGpuMemoryProperties();
}

vk::Queue& RHIDeviceVulkan::GetGraphicsQueue()
{
    return VulkanManager::GetInstance().GetGraphicQueue();
}

vk::CommandPool& RHIDeviceVulkan::GetCommandPool()
{
    return VulkanManager::GetInstance().GetCommandPool();
}

std::vector<vk::CommandBuffer>& RHIDeviceVulkan::GetCommandBuffers()
{
    return VulkanManager::GetInstance().GetCommandBuffers();
}

vk::SwapchainKHR& RHIDeviceVulkan::GetSwapchain()
{
    return VulkanManager::GetInstance().GetSwapChain();
}

uint32_t RHIDeviceVulkan::GetSwapchainImageCount() const
{
    return VulkanManager::GetInstance().GetSwapChainImageCount();
}

vk::Extent2D RHIDeviceVulkan::GetSwapchainExtent() const
{
    return VulkanManager::GetInstance().GetSwapChainExtent();
}

vk::Format RHIDeviceVulkan::GetSwapchainImageFormat() const
{
    return VulkanManager::GetInstance().GetSurfaceFormat().format;
}

const std::vector<vk::ImageView>& RHIDeviceVulkan::GetSwapchainImageViews() const
{
    return VulkanManager::GetInstance().GetSwapChainImageViews();
}

std::vector<vk::Fence>& RHIDeviceVulkan::GetTaskFinishedFences()
{
    return VulkanManager::GetInstance().GetTaskFinishedFences();
}

std::vector<vk::Semaphore>& RHIDeviceVulkan::GetImageAcquiredSemaphores()
{
    return VulkanManager::GetInstance().GetImageAcquiredSemaphores();
}

std::vector<vk::Semaphore>& RHIDeviceVulkan::GetRenderFinishedSemaphores()
{
    return VulkanManager::GetInstance().GetRenderFinishedSemaphores();
}

std::vector<vk::Fence>& RHIDeviceVulkan::GetImagesInFlightFences()
{
    return VulkanManager::GetInstance().GetImagesInFlightFences();
}

std::pair<vk::Buffer, vk::DeviceMemory> RHIDeviceVulkan::CreateBuffer(
    vk::DeviceSize size,
    vk::BufferUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    return CommonFunction::CreateBuffer(
        GetDevice(),
        size,
        usage,
        GetGpuMemoryProperties(),
        memoryPropertyFlags,
        debugName);
}

void* RHIDeviceVulkan::MapMemory(vk::DeviceMemory memory, vk::DeviceSize size)
{
    return GetDevice().mapMemory(memory, 0, size);
}

void RHIDeviceVulkan::UnmapMemory(vk::DeviceMemory memory)
{
    GetDevice().unmapMemory(memory);
}

void RHIDeviceVulkan::DestroyBuffer(vk::Buffer buffer, vk::DeviceMemory memory)
{
    vk::Device& device = GetDevice();
    if (buffer)
    {
        device.destroyBuffer(buffer);
    }
    if (memory)
    {
        device.freeMemory(memory);
    }
}

void RHIDeviceVulkan::CopyBufferToBuffer(
    vk::Buffer source,
    vk::Buffer destination,
    vk::DeviceSize size)
{
    CommonFunction::CopyBufferToBuffer(
        GetDevice(),
        GetGraphicsQueue(),
        GetCommandPool(),
        source,
        destination,
        size);
}

std::pair<vk::Image, vk::DeviceMemory> RHIDeviceVulkan::CreateImage(
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels,
    vk::SampleCountFlagBits samples,
    vk::Format format,
    vk::ImageTiling tiling,
    vk::ImageUsageFlags usage,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    return CommonFunction::CreateImage(
        GetDevice(),
        width,
        height,
        mipLevels,
        samples,
        format,
        tiling,
        usage,
        GetGpuMemoryProperties(),
        memoryPropertyFlags,
        debugName);
}

std::pair<vk::Image, vk::DeviceMemory> RHIDeviceVulkan::CreateImage(
    const vk::ImageCreateInfo& createInfo,
    vk::MemoryPropertyFlags memoryPropertyFlags,
    const std::string& debugName)
{
    vk::Device& device = GetDevice();
    vk::Image image = device.createImage(createInfo);
    vk::MemoryRequirements memoryRequirements = device.getImageMemoryRequirements(image);

    vk::MemoryAllocateInfo allocateInfo;
    allocateInfo
        .setAllocationSize(memoryRequirements.size)
        .setMemoryTypeIndex(CommonFunction::FindMemoryType(
            GetGpuMemoryProperties(),
            memoryRequirements.memoryTypeBits,
            memoryPropertyFlags));

    vk::DeviceMemory imageMemory = device.allocateMemory(allocateInfo);
    device.bindImageMemory(image, imageMemory, 0);

    if (!debugName.empty())
    {
        VulkanDebug::SetObjectName(device, image, vk::ObjectType::eImage, debugName);
        VulkanDebug::SetObjectName(
            device,
            imageMemory,
            vk::ObjectType::eDeviceMemory,
            "DeviceMemory: " + debugName);
    }

    return { image, imageMemory };
}

void RHIDeviceVulkan::TransitionImageLayout(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageLayout oldLayout,
    vk::ImageLayout newLayout)
{
    CommonFunction::TransitionImageLayout(
        image,
        mipLevels,
        format,
        GetDevice(),
        GetCommandPool(),
        GetGraphicsQueue(),
        oldLayout,
        newLayout);
}

vk::ImageView RHIDeviceVulkan::Create2DImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    vk::ImageAspectFlagBits aspectMask,
    const std::string& debugName)
{
    return CommonFunction::Create2DImageView(
        GetDevice(),
        image,
        mipLevels,
        format,
        aspectMask,
        debugName);
}

vk::ImageView RHIDeviceVulkan::CreateImageView(
    vk::Image image,
    vk::ImageViewType viewType,
    vk::Format format,
    vk::ImageAspectFlagBits aspectMask,
    uint32_t baseMipLevel,
    uint32_t mipLevelCount,
    uint32_t baseArrayLayer,
    uint32_t layerCount,
    const std::string& debugName)
{
    return CommonFunction::CreateImageViewBase(
        GetDevice(),
        image,
        viewType,
        format,
        aspectMask,
        baseMipLevel,
        mipLevelCount,
        baseArrayLayer,
        layerCount,
        debugName);
}

vk::ImageView RHIDeviceVulkan::CreateCubeImageView(
    vk::Image image,
    uint32_t mipLevels,
    vk::Format format,
    const std::string& debugName)
{
    return CommonFunction::CreateCubeImageView(
        GetDevice(),
        image,
        mipLevels,
        format,
        debugName);
}

vk::ImageView RHIDeviceVulkan::CreateCubeStorageImageView(
    vk::Image image,
    vk::Format format,
    const std::string& debugName)
{
    return CommonFunction::CreateCubeStorageImageView(
        GetDevice(),
        image,
        format,
        debugName);
}

void RHIDeviceVulkan::DestroyImageView(vk::ImageView& imageView)
{
    if (!imageView)
    {
        return;
    }

    GetDevice().destroyImageView(imageView);
    imageView = nullptr;
}

vk::Sampler RHIDeviceVulkan::Create2DSampler(const std::string& debugName)
{
    return CommonFunction::Create2DSampler(
        GetDevice(),
        GetPhysicalDevice(),
        debugName);
}

vk::Sampler RHIDeviceVulkan::Create2DSampler(
    vk::Filter filter,
    vk::SamplerAddressMode addressMode,
    bool enableMipmaps,
    const std::string& debugName)
{
    return CommonFunction::Create2DSampler(
        GetDevice(),
        GetPhysicalDevice(),
        filter,
        addressMode,
        enableMipmaps,
        debugName);
}

vk::Sampler RHIDeviceVulkan::CreateSampler(
    const vk::SamplerCreateInfo& createInfo,
    const std::string& debugName)
{
    return CommonFunction::CreateSamplerBase(GetDevice(), createInfo, debugName);
}

vk::Sampler RHIDeviceVulkan::CreateCubeSampler(
    float maxLod,
    const std::string& debugName)
{
    return CommonFunction::CreateCubeSampler(GetDevice(), maxLod, debugName);
}

void RHIDeviceVulkan::DestroySampler(vk::Sampler& sampler)
{
    if (!sampler)
    {
        return;
    }

    GetDevice().destroySampler(sampler);
    sampler = nullptr;
}

vk::Sampler RHIDeviceVulkan::CreateDepthSampler(const std::string& debugName)
{
    return CommonFunction::CreateDepthSampler(
        GetDevice(),
        GetPhysicalDevice(),
        debugName);
}

vk::Sampler RHIDeviceVulkan::CreateDepthCompareSampler(const std::string& debugName)
{
    return CommonFunction::CreateDepthCompareSampler(
        GetDevice(),
        GetPhysicalDevice(),
        debugName);
}

vk::CommandBuffer RHIDeviceVulkan::BeginSingleTimeCommands()
{
    return CommonFunction::BeginSingleTimeCommands(
        GetDevice(),
        GetCommandPool());
}

void RHIDeviceVulkan::EndSingleTimeCommands(vk::CommandBuffer& commandBuffer)
{
    CommonFunction::EndSingleTimeCommands(
        GetDevice(),
        commandBuffer,
        GetGraphicsQueue(),
        GetCommandPool());
}

void RHIDeviceVulkan::CopyBufferToImage(
    vk::Buffer buffer,
    vk::Image image,
    uint32_t width,
    uint32_t height)
{
    CommonFunction::CopyBufferToImage(
        GetDevice(),
        GetGraphicsQueue(),
        GetCommandPool(),
        buffer,
        image,
        width,
        height);
}

void RHIDeviceVulkan::CopyImageToBuffer(
    vk::Image image,
    vk::Buffer buffer,
    uint32_t width,
    uint32_t height,
    bool flipY,
    vk::DeviceSize rowBytes)
{
    CommonFunction::CopyImageToBuffer(
        GetDevice(),
        GetGraphicsQueue(),
        GetCommandPool(),
        image,
        buffer,
        width,
        height,
        flipY,
        rowBytes);
}

void RHIDeviceVulkan::GenerateMipmaps(
    vk::Image image,
    uint32_t width,
    uint32_t height,
    uint32_t mipLevels)
{
    CommonFunction::GenerateMipmaps(
        GetDevice(),
        GetGraphicsQueue(),
        GetCommandPool(),
        image,
        width,
        height,
        mipLevels);
}

void RHIDeviceVulkan::DestroyImageResource(
    vk::Image& image,
    vk::DeviceMemory& memory,
    vk::ImageView& imageView,
    vk::Sampler& sampler)
{
    vk::Device& device = GetDevice();
    if (imageView)
    {
        device.destroyImageView(imageView);
        imageView = nullptr;
    }
    if (sampler)
    {
        device.destroySampler(sampler);
        sampler = nullptr;
    }
    if (image)
    {
        device.destroyImage(image);
        image = nullptr;
    }
    if (memory)
    {
        device.freeMemory(memory);
        memory = nullptr;
    }
}

vk::DescriptorSetLayout RHIDeviceVulkan::CreateDescriptorSetLayout(
    const vk::DescriptorSetLayoutCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::DescriptorSetLayout descriptorSetLayout;
    vk::Result result = GetDevice().createDescriptorSetLayout(
        &createInfo,
        nullptr,
        &descriptorSetLayout);
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to create descriptor set layout: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        descriptorSetLayout,
        vk::ObjectType::eDescriptorSetLayout,
        debugName);
    return descriptorSetLayout;
}

void RHIDeviceVulkan::DestroyDescriptorSetLayout(
    vk::DescriptorSetLayout& descriptorSetLayout)
{
    if (!descriptorSetLayout)
    {
        return;
    }

    GetDevice().destroyDescriptorSetLayout(descriptorSetLayout, nullptr);
    descriptorSetLayout = nullptr;
}

vk::DescriptorPool RHIDeviceVulkan::CreateDescriptorPool(
    const vk::DescriptorPoolCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::DescriptorPool descriptorPool;
    vk::Result result = GetDevice().createDescriptorPool(&createInfo, nullptr, &descriptorPool);
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to create descriptor pool: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        descriptorPool,
        vk::ObjectType::eDescriptorPool,
        debugName);
    return descriptorPool;
}

void RHIDeviceVulkan::DestroyDescriptorPool(vk::DescriptorPool& descriptorPool)
{
    if (!descriptorPool)
    {
        return;
    }

    GetDevice().destroyDescriptorPool(descriptorPool, nullptr);
    descriptorPool = nullptr;
}

void RHIDeviceVulkan::AllocateDescriptorSets(
    const vk::DescriptorSetAllocateInfo& allocateInfo,
    std::vector<vk::DescriptorSet>& descriptorSets)
{
    if (descriptorSets.empty())
    {
        return;
    }

    vk::Result result = GetDevice().allocateDescriptorSets(&allocateInfo, descriptorSets.data());
    if (result != vk::Result::eSuccess)
    {
        throw std::runtime_error("Failed to allocate descriptor sets");
    }
}

void RHIDeviceVulkan::FreeDescriptorSet(
    vk::DescriptorPool descriptorPool,
    vk::DescriptorSet& descriptorSet)
{
    if (!descriptorPool || !descriptorSet)
    {
        return;
    }

    GetDevice().freeDescriptorSets(descriptorPool, 1, &descriptorSet);
    descriptorSet = nullptr;
}

void RHIDeviceVulkan::UpdateDescriptorSets(
    const std::vector<vk::WriteDescriptorSet>& writeDescriptorSets)
{
    if (writeDescriptorSets.empty())
    {
        return;
    }

    GetDevice().updateDescriptorSets(writeDescriptorSets, nullptr);
}

void RHIDeviceVulkan::SetDescriptorSetDebugName(
    vk::DescriptorSet descriptorSet,
    const std::string& debugName)
{
    if (!descriptorSet)
    {
        return;
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        descriptorSet,
        vk::ObjectType::eDescriptorSet,
        debugName);
}

vk::RenderPass RHIDeviceVulkan::CreateRenderPass(
    const vk::RenderPassCreateInfo2& createInfo,
    const std::string& debugName)
{
    vk::RenderPass renderPass = GetDevice().createRenderPass2(createInfo);
    if (!renderPass)
    {
        throw std::runtime_error("Failed to create render pass: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        renderPass,
        vk::ObjectType::eRenderPass,
        debugName);
    return renderPass;
}

void RHIDeviceVulkan::DestroyRenderPass(vk::RenderPass& renderPass)
{
    if (!renderPass)
    {
        return;
    }

    GetDevice().destroyRenderPass(renderPass);
    renderPass = nullptr;
}

vk::Framebuffer RHIDeviceVulkan::CreateFramebuffer(
    const vk::FramebufferCreateInfo& createInfo,
    const std::string& debugName)
{
    vk::Framebuffer framebuffer = GetDevice().createFramebuffer(createInfo);
    if (!framebuffer)
    {
        throw std::runtime_error("Failed to create framebuffer: " + debugName);
    }

    VulkanDebug::SetObjectName(
        GetDevice(),
        framebuffer,
        vk::ObjectType::eFramebuffer,
        debugName);
    return framebuffer;
}

void RHIDeviceVulkan::DestroyFramebuffers(std::vector<vk::Framebuffer>& framebuffers)
{
    for (vk::Framebuffer& framebuffer : framebuffers)
    {
        if (framebuffer)
        {
            GetDevice().destroyFramebuffer(framebuffer);
            framebuffer = nullptr;
        }
    }
    framebuffers.clear();
}

} // namespace VL
