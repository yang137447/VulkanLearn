#include "vulkanManager.h"
#include "SDL3/SDL_vulkan.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <stdexcept>
#include "commonFunction.h"
#include "vulkanDebug.h"

void VulkanManager::Init(std::vector<const char *> &extensions, SDL_Window *window)
{
    instanceExtensions.resize(extensions.size());
    std::copy(extensions.begin(), extensions.end(), instanceExtensions.begin());
#if defined(VULKANLEARN_ENABLE_VULKAN_VALIDATION)
    instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    sdlWindow = window;

    CreateVkInstance();
    EnumeratePhysicalDevices();
    CreateVkSurface();
    CreateVkDevice();
    CreateVkSwapChain();
    CreateVkCommandBuffer();
    CreateSyncObjects();
}

VulkanManager::~VulkanManager()
{
    device.waitIdle(); //等待设备空闲
    DestroySyncObjects();
    DestroyVkCommandBuffer();
    DestroyVkSwapChain();
    DestroyVkDevice();
    DestroyVkSurface();
    DestroyVkInstance();
}

void VulkanManager::ReCreateSwapChain(int newWidth, int newHeight)
{
    if (newWidth <= 0 || newHeight <= 0)
    {
        return;
    }

    device.waitIdle(); //等待设备空闲

    requestedSwapChainExtent = vk::Extent2D{
        static_cast<uint32_t>(newWidth),
        static_cast<uint32_t>(newHeight)
    };

    DestroySyncObjects();
    DestroyVkCommandBuffer();
    DestroyVkSwapChain();

    CreateVkSwapChain();
    CreateVkCommandBuffer();
    CreateSyncObjects();
}

void VulkanManager::CreateVkInstance()
{
    vk::ApplicationInfo applicationInfo;
    applicationInfo.setApiVersion(VK_API_VERSION_1_3);
    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo
        .setPApplicationInfo(&applicationInfo)
        .setEnabledLayerCount(static_cast<uint32_t>(instanceLayers.size()))
        .setPEnabledLayerNames(instanceLayers)
        .setEnabledExtensionCount(static_cast<uint32_t>(instanceExtensions.size()))
        .setPEnabledExtensionNames(instanceExtensions);
    instance = vk::createInstance(instanceCreateInfo);
    if (!instance)
    {
        throw std::runtime_error("Failed to create Vulkan instance.");
    }
    VulkanDebug::Init(instance);
}

void VulkanManager::DestroyVkInstance()
{
    instance.destroy();
}

void VulkanManager::EnumeratePhysicalDevices()
{
    physicalDevices = instance.enumeratePhysicalDevices();

    const uint32_t gpuCount = static_cast<uint32_t>(physicalDevices.size());

    if (gpuCount == 0)
    {
        throw std::runtime_error("No Vulkan physical device found.");
    }
    vk::PhysicalDevice physicalDevice = physicalDevices[0];
    gpuMemoryProperties = physicalDevice.getMemoryProperties();
}

void VulkanManager::CreateVkSurface()
{
    bool result = SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr,&surface);
    if(result == false)
    {
        throw std::runtime_error("Failed to create Vulkan surface.");
    }
}

void VulkanManager::DestroyVkSurface()
{
    instance.destroySurfaceKHR(surface);
}

void VulkanManager::CreateVkDevice()
{
    // Find queue families used by the current Vulkan device boundary.
    uint32_t queueFamilyCount = 0;
    physicalDevices[kGpuIndex].getQueueFamilyProperties(&queueFamilyCount, nullptr);
    std::vector<vk::QueueFamilyProperties> queueFamilyProperties;
    queueFamilyProperties.resize(queueFamilyCount);
    physicalDevices[kGpuIndex].getQueueFamilyProperties(&queueFamilyCount, queueFamilyProperties.data());

    std::vector<uint32_t> presentQueueFamilyIndices;
    std::vector<uint32_t> graphicQueueFamilyIndices;

    for (uint32_t i = 0; i < queueFamilyCount; i++)
    {
        const auto &property = queueFamilyProperties[i];
        if (property.queueFlags & vk::QueueFlagBits::eGraphics)
        {
            graphicQueueFamilyIndices.push_back(i);
        }
        if (physicalDevices[kGpuIndex].getSurfaceSupportKHR(i, surface))
        {
            presentQueueFamilyIndices.push_back(i);
        }
    }

    // Check if we have a graphics queue family
    if (graphicQueueFamilyIndices.empty())
    {
        throw std::runtime_error("No Vulkan graphics queue family found.");
    }

    // Check if we have a present queue family
    if (presentQueueFamilyIndices.empty())
    {
        throw std::runtime_error("No Vulkan present queue family found.");
    }
    // 找到一个Graphics和Present都支持的队列
    for (uint32_t i = 0; i < graphicQueueFamilyIndices.size(); i++)
    {
        for (uint32_t j = 0; j < presentQueueFamilyIndices.size(); j++)
        {
            if (graphicQueueFamilyIndices[i] == presentQueueFamilyIndices[j])
            {
                graphicsQueueFamilyIndex = graphicQueueFamilyIndices[i];
                presentQueueFamilyIndex = presentQueueFamilyIndices[j];
                break;
            }
        }
    }
    // 如果没有找到，就用第一个Graphics和第一个Present
    if (!graphicsQueueFamilyIndex.has_value())
    {
        graphicsQueueFamilyIndex = graphicQueueFamilyIndices[0];
        presentQueueFamilyIndex = presentQueueFamilyIndices[0];
    }
    graphicsQueueTimestampValidBits =
        queueFamilyProperties[graphicsQueueFamilyIndex.value()].timestampValidBits;

    vk::DeviceQueueCreateInfo deviceGraphicsQueueCreateInfo;
    float graphicsQueuePriorities[1] = {0.0f};
    deviceGraphicsQueueCreateInfo
        .setQueueFamilyIndex(graphicsQueueFamilyIndex.value())
        .setQueueCount(1)
        .setPQueuePriorities(graphicsQueuePriorities);

    // depthClamp 和 samplerAnisotropy 是现有渲染路径的基础能力。双源混合是可选能力：
    // 支持时在逻辑设备上启用并走彩色透射，缺失时保留设备可用性并走标量 alpha 降级。
    const vk::PhysicalDeviceFeatures supportedFeatures =
        physicalDevices[kGpuIndex].getFeatures();
    dualSourceBlendEnabled =
        supportedFeatures.dualSrcBlend == VK_TRUE;

    vk::PhysicalDeviceFeatures deviceFeatures;
    deviceFeatures
        .setDepthClamp(VK_TRUE)
        .setSamplerAnisotropy(VK_TRUE)
        .setDualSrcBlend(
            dualSourceBlendEnabled ? VK_TRUE : VK_FALSE);
    
    vk::DeviceCreateInfo deviceCreateInfo;
    deviceCreateInfo
        .setQueueCreateInfoCount(1)
        .setPQueueCreateInfos(&deviceGraphicsQueueCreateInfo)
        .setEnabledExtensionCount(static_cast<uint32_t>(deviceExtensions.size()))
        .setPEnabledExtensionNames(deviceExtensions)
        .setPEnabledFeatures(&deviceFeatures)
        .setEnabledLayerCount(0);
    
    device = physicalDevices[kGpuIndex].createDevice(deviceCreateInfo);
    assert(device);

    // Get the graphics queue
    device.getQueue(graphicsQueueFamilyIndex.value(), 0, &graphicsQueue);
}

void VulkanManager::DestroyVkDevice()
{
    device.destroy();
}

void VulkanManager::CreateVkSwapChain()
{
    // 获取支持的表面格式
    std::vector<vk::SurfaceFormatKHR> surfaceFormats = physicalDevices[kGpuIndex].getSurfaceFormatsKHR(surface);
    assert(!surfaceFormats.empty());
    for(const auto &availableFormat : surfaceFormats)
    {
        if(availableFormat.format == vk::Format::eR8G8B8A8Srgb && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
        {
            surfaceFormat = availableFormat;
            break;
        }
    }
    // 获取KHR表面能力
    vk::SurfaceCapabilitiesKHR surfaceCapabilities = physicalDevices[kGpuIndex].getSurfaceCapabilitiesKHR(surface);
    //获取支持的显示模式数量
    std::vector<vk::PresentModeKHR> presentModes = physicalDevices[kGpuIndex].getSurfacePresentModesKHR(surface);
    assert(!presentModes.empty());
    // 确定一下显示模式
    vk::PresentModeKHR presentMode = vk::PresentModeKHR::eFifo;
    for (const auto &availablePresentMode : presentModes)
    {
        //如果也支持VK_PRESENT_MODE_MAILBOX_KHR模式，由于其效率高，便选用
        if (availablePresentMode == vk::PresentModeKHR::eMailbox)
        {
            presentMode = availablePresentMode;
            break;
        }
        //如果没能用上VK_PRESENT_MODE_MAILBOX_KHR模式，但有VK_PRESENT_MODE_IMMEDIATE_KHR模式
        //也比VK_PRESENT_MODE_FIFO_KHR模式强，故选用
        else if (availablePresentMode == vk::PresentModeKHR::eImmediate)
        {
            presentMode = availablePresentMode;
        }
    }
    // 确定surface的高度和宽度
    if(surfaceCapabilities.currentExtent.width == 0xFFFFFFFF)
    {
        //如果surface能力中的尺寸没有定义（宽度为0xFFFFFFFF表示没定义）
        const uint32_t requestedWidth = requestedSwapChainExtent.has_value()
            ? requestedSwapChainExtent->width
            : static_cast<uint32_t>(CommonFunction::GetWindowSize().x());
        const uint32_t requestedHeight = requestedSwapChainExtent.has_value()
            ? requestedSwapChainExtent->height
            : static_cast<uint32_t>(CommonFunction::GetWindowSize().y());
        swapChainExtent.width = std::clamp(requestedWidth,
            surfaceCapabilities.minImageExtent.width, 
            surfaceCapabilities.maxImageExtent.width);
        swapChainExtent.height = std::clamp(requestedHeight,
            surfaceCapabilities.minImageExtent.height, 
            surfaceCapabilities.maxImageExtent.height);
    }
    else
    {
        swapChainExtent = surfaceCapabilities.currentExtent;
    }
    //确定交换链的图像数量
    swapChainImageCount = surfaceCapabilities.minImageCount + 1;
    if (surfaceCapabilities.maxImageCount > 0 && swapChainImageCount > surfaceCapabilities.maxImageCount)
    {
        swapChainImageCount = surfaceCapabilities.maxImageCount;
    }
    //KHR表面变换标志
    vk::SurfaceTransformFlagBitsKHR preTransform = surfaceCapabilities.currentTransform;
    if (surfaceCapabilities.supportedTransforms & vk::SurfaceTransformFlagBitsKHR::eIdentity)
    {
        /*主要优势在于它不会对图像进行任何变换，确保图像按照原始方式呈现。这意味着：
            避免额外计算：不需要 GPU 进行旋转或镜像处理，提高渲染效率。
            减少延迟：由于不涉及变换，图像可以更快地传输到显示设备。
            保持原始布局：适用于 UI 设计或需要精准像素映射的应用，如 2D 游戏或图像处理软件。
        */
        preTransform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    }
    // 创建交换链
    vk::SwapchainCreateInfoKHR swapChainCreateInfo;
    swapChainCreateInfo
        .setSurface(surface)
        .setMinImageCount(swapChainImageCount)
        .setImageFormat(surfaceFormat.format)
        .setImageColorSpace(surfaceFormat.colorSpace)
        .setImageExtent(swapChainExtent)
        .setImageArrayLayers(1)
        .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
        .setImageSharingMode(vk::SharingMode::eExclusive)
        .setQueueFamilyIndexCount(0)
        .setPQueueFamilyIndices(nullptr)
        .setPreTransform(preTransform)
        .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
        .setPresentMode(presentMode)
        .setClipped(VK_TRUE)
        // Resize currently destroys the old swapchain before creating the new
        // one. If we move to Vulkan oldSwapchain handoff, this becomes the
        // previous handle and ownership must be updated in DestroyVkSwapChain.
        .setOldSwapchain(nullptr);
    if(graphicsQueueFamilyIndex.value() != presentQueueFamilyIndex.value())
    {
        //如果图形队列和呈现队列不是同一个队列族
        //则需要设置共享模式
        uint32_t queueFamilyIndices[] = { graphicsQueueFamilyIndex.value(), presentQueueFamilyIndex.value() };
        swapChainCreateInfo
            .setImageSharingMode(vk::SharingMode::eConcurrent)
            .setQueueFamilyIndexCount(2)
            .setPQueueFamilyIndices(queueFamilyIndices);
    }
    swapChain = device.createSwapchainKHR(swapChainCreateInfo);
    assert(swapChain);

    // 获取交换链中的图像数量
    vk::Result result = device.getSwapchainImagesKHR(swapChain, &swapChainImageCount, nullptr);
    assert(result == vk::Result::eSuccess);
    // 获取交换链中的图像
    std::vector<vk::Image> swapChainImages;
    swapChainImages.resize(swapChainImageCount);
    result = device.getSwapchainImagesKHR(swapChain, &swapChainImageCount, swapChainImages.data());
    assert(result == vk::Result::eSuccess);
    // 创建交换链图像视图
    swapChainImageViews.resize(swapChainImageCount);
    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        VulkanDebug::SetObjectName(device, swapChainImages[i], vk::ObjectType::eImage, "SwapChainImage: Index " + std::to_string(i));
        swapChainImageViews[i] = CommonFunction::Create2DImageView(
            device,
            swapChainImages[i],
            1,
            surfaceFormat.format,
            vk::ImageAspectFlagBits::eColor,
            "SwapChainImageView_Index_" + std::to_string(i));
        assert(swapChainImageViews[i]);
    }
}

void VulkanManager::DestroyVkSwapChain()
{
    for(vk::ImageView& imageView : swapChainImageViews)
    {
        if (imageView)
        {
            device.destroyImageView(imageView);
        }
    }
    swapChainImageViews.clear();

    if (swapChain)
    {
        device.destroySwapchainKHR(swapChain);
        swapChain = nullptr;
    }
    swapChainImageCount = 0;
}

void VulkanManager::CreateVkCommandBuffer()
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        .setQueueFamilyIndex(graphicsQueueFamilyIndex.value())
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
    commandPool = device.createCommandPool(commandPoolCreateInfo);
    assert(commandPool);
    VulkanDebug::SetObjectName(device, commandPool, vk::ObjectType::eCommandPool, "CommandPool: Main");

    commandBuffers.resize(swapChainImageCount);

    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        .setCommandPool(commandPool)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(commandBuffers.size());
    vk::Result result = device.allocateCommandBuffers(&commandBufferAllocateInfo, commandBuffers.data());

    for (size_t i = 0; i < commandBuffers.size(); i++)
    {
        VulkanDebug::SetObjectName(device, commandBuffers[i], vk::ObjectType::eCommandBuffer, "CommandBuffer: SwapchainIndex " + std::to_string(i));
    }

}

void VulkanManager::DestroyVkCommandBuffer()
{
    if (commandPool)
    {
        if (!commandBuffers.empty())
        {
            device.freeCommandBuffers(commandPool, commandBuffers.size(), commandBuffers.data());
        }
        device.destroyCommandPool(commandPool);
        commandPool = nullptr;
    }
    commandBuffers.clear();
}

void VulkanManager::CreateSyncObjects()
{
    //创建 Semaphore
    imageAcquiredSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(swapChainImageCount);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vk::SemaphoreCreateInfo imageAcquiredSemaphoreCreateInfo;
        imageAcquiredSemaphores[i] = device.createSemaphore(imageAcquiredSemaphoreCreateInfo);
        assert(imageAcquiredSemaphores[i]);
        VulkanDebug::SetObjectName(device, imageAcquiredSemaphores[i], vk::ObjectType::eSemaphore, "Semaphore: ImageAcquired (Frame " + std::to_string(i) + ")");
    }
    for (size_t i = 0; i < swapChainImageCount; i++)
    {
        vk::SemaphoreCreateInfo renderFinishedSemaphoreCreateInfo;
        renderFinishedSemaphores[i] = device.createSemaphore(renderFinishedSemaphoreCreateInfo);
        assert(renderFinishedSemaphores[i]);
        VulkanDebug::SetObjectName(device, renderFinishedSemaphores[i], vk::ObjectType::eSemaphore, "Semaphore_RenderFinished_" + std::to_string(i));
    }
    //创建 Fence
    vk::FenceCreateInfo fenceCreateInfo;
    fenceCreateInfo
        .setFlags(vk::FenceCreateFlagBits::eSignaled); // 设置为signaled状态，表示初始状态为完成
    
    taskFinishedFences.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vk::Result result = device.createFence(&fenceCreateInfo, nullptr, &taskFinishedFences[i]);
        assert(result == vk::Result::eSuccess);
        VulkanDebug::SetObjectName(device, taskFinishedFences[i], vk::ObjectType::eFence, "Fence: TaskFinished (FrameIndex " + std::to_string(i) + ")");
    }

    imagesInFlightFences.resize(swapChainImageCount, nullptr);
}

void VulkanManager::DestroySyncObjects()
{
    for (vk::Semaphore& semaphore : imageAcquiredSemaphores)
    {
        if (semaphore)
        {
            device.destroySemaphore(semaphore);
        }
    }
    imageAcquiredSemaphores.clear();

    for (vk::Semaphore& semaphore : renderFinishedSemaphores)
    {
        if (semaphore)
        {
            device.destroySemaphore(semaphore);
        }
    }
    renderFinishedSemaphores.clear();
    
    for (vk::Fence& fence : taskFinishedFences)
    {
        if (fence)
        {
            device.destroyFence(fence);
        }
    }
    taskFinishedFences.clear();

    imagesInFlightFences.clear();
}
