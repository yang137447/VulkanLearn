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

    // 目标 0 = 主窗口。多窗口时这里会有第二个目标，且设备只创建一次。
    presentTargets.emplace_back();
    defaultTarget().window = window;

    CreateVkInstance();
    EnumeratePhysicalDevices();
    CreateVkSurface(defaultTarget(), window);
    CreateVkDevice();
    CreateVkSwapChain(defaultTarget());
    // 池是共享的，先建一次；随后各目标从它分配自己的每图像缓冲。
    CreateCommandPool();
    CreateTargetCommandBuffers(defaultTarget());
    CreateFrameSyncObjects();
    CreateTargetSyncObjects(defaultTarget());
}

VulkanManager::~VulkanManager()
{
    device.waitIdle(); //等待设备空闲
    for (VL::PresentTarget &target : presentTargets)
    {
        DestroyTargetSyncObjects(target);
        DestroyTargetCommandBuffers(target);
        DestroyVkSwapChain(target);
    }
    DestroyFrameSyncObjects();
    DestroyCommandPool();
    DestroyVkDevice();
    for (VL::PresentTarget &target : presentTargets)
        DestroyVkSurface(target);
    DestroyVkInstance();
}

void VulkanManager::ReCreateSwapChain(int newWidth, int newHeight)
{
    if (newWidth <= 0 || newHeight <= 0)
    {
        return;
    }

    device.waitIdle(); //等待设备空闲

    VL::PresentTarget &target = defaultTarget();
    target.requestedExtent = vk::Extent2D{
        static_cast<uint32_t>(newWidth),
        static_cast<uint32_t>(newHeight)
    };

    // 只动这个目标。共享资源（命令池、帧级信号量与 fence）都不重建：
    // 它们按 MAX_FRAMES_IN_FLIGHT 或设备生命周期分配，与这个窗口的图像数无关，
    // 重建它们会连带影响别的窗口与正在进行的一次性命令。
    DestroyTargetSyncObjects(target);
    DestroyTargetCommandBuffers(target);
    DestroyVkSwapChain(target);

    CreateVkSwapChain(target);
    CreateTargetCommandBuffers(target);
    CreateTargetSyncObjects(target);
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

void VulkanManager::CreateVkSurface(VL::PresentTarget &target, SDL_Window *window)
{
    bool result = SDL_Vulkan_CreateSurface(window, instance, nullptr, &target.surface);
    if(result == false)
    {
        throw std::runtime_error("Failed to create Vulkan surface.");
    }
}

void VulkanManager::DestroyVkSurface(VL::PresentTarget &target)
{
    instance.destroySurfaceKHR(target.surface);
    target.surface = VK_NULL_HANDLE;
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
        if (physicalDevices[kGpuIndex].getSurfaceSupportKHR(i, defaultTarget().surface))
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
                defaultTarget().presentQueueFamilyIndex = presentQueueFamilyIndices[j];
                break;
            }
        }
    }
    // 如果没有找到，就用第一个Graphics和第一个Present
    if (!graphicsQueueFamilyIndex.has_value())
    {
        graphicsQueueFamilyIndex = graphicQueueFamilyIndices[0];
        defaultTarget().presentQueueFamilyIndex = presentQueueFamilyIndices[0];
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

void VulkanManager::CreateVkSwapChain(VL::PresentTarget &target)
{
    // 获取支持的表面格式
    std::vector<vk::SurfaceFormatKHR> surfaceFormats = physicalDevices[kGpuIndex].getSurfaceFormatsKHR(target.surface);
    assert(!surfaceFormats.empty());
    for(const auto &availableFormat : surfaceFormats)
    {
        if(availableFormat.format == vk::Format::eR8G8B8A8Srgb && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
        {
            target.surfaceFormat = availableFormat;
            break;
        }
    }
    // 获取KHR表面能力
    vk::SurfaceCapabilitiesKHR surfaceCapabilities = physicalDevices[kGpuIndex].getSurfaceCapabilitiesKHR(target.surface);
    //获取支持的显示模式数量
    std::vector<vk::PresentModeKHR> presentModes = physicalDevices[kGpuIndex].getSurfacePresentModesKHR(target.surface);
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
        const uint32_t requestedWidth = target.requestedExtent.has_value()
            ? target.requestedExtent->width
            : static_cast<uint32_t>(CommonFunction::GetWindowSize().x());
        const uint32_t requestedHeight = target.requestedExtent.has_value()
            ? target.requestedExtent->height
            : static_cast<uint32_t>(CommonFunction::GetWindowSize().y());
        target.extent.width = std::clamp(requestedWidth,
            surfaceCapabilities.minImageExtent.width, 
            surfaceCapabilities.maxImageExtent.width);
        target.extent.height = std::clamp(requestedHeight,
            surfaceCapabilities.minImageExtent.height, 
            surfaceCapabilities.maxImageExtent.height);
    }
    else
    {
        target.extent = surfaceCapabilities.currentExtent;
    }
    //确定交换链的图像数量
    target.imageCount = surfaceCapabilities.minImageCount + 1;
    if (surfaceCapabilities.maxImageCount > 0 && target.imageCount > surfaceCapabilities.maxImageCount)
    {
        target.imageCount = surfaceCapabilities.maxImageCount;
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
        .setSurface(target.surface)
        .setMinImageCount(target.imageCount)
        .setImageFormat(target.surfaceFormat.format)
        .setImageColorSpace(target.surfaceFormat.colorSpace)
        .setImageExtent(target.extent)
        .setImageArrayLayers(1)
        .setImageUsage(
            vk::ImageUsageFlagBits::eColorAttachment |
            vk::ImageUsageFlagBits::eTransferSrc)
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
    if(graphicsQueueFamilyIndex.value() != target.presentQueueFamilyIndex.value())
    {
        //如果图形队列和呈现队列不是同一个队列族
        //则需要设置共享模式
        uint32_t queueFamilyIndices[] = { graphicsQueueFamilyIndex.value(), target.presentQueueFamilyIndex.value() };
        swapChainCreateInfo
            .setImageSharingMode(vk::SharingMode::eConcurrent)
            .setQueueFamilyIndexCount(2)
            .setPQueueFamilyIndices(queueFamilyIndices);
    }
    target.swapChain = device.createSwapchainKHR(swapChainCreateInfo);
    assert(target.swapChain);

    // 获取交换链中的图像数量
    vk::Result result = device.getSwapchainImagesKHR(target.swapChain, &target.imageCount, nullptr);
    assert(result == vk::Result::eSuccess);
    // 获取交换链中的图像
    target.images.clear();
    target.images.resize(target.imageCount);
    result = device.getSwapchainImagesKHR(target.swapChain, &target.imageCount, target.images.data());
    assert(result == vk::Result::eSuccess);
    // 创建交换链图像视图
    target.imageViews.resize(target.imageCount);
    for (uint32_t i = 0; i < target.imageCount; i++)
    {
        VulkanDebug::SetObjectName(device, target.images[i], vk::ObjectType::eImage, "SwapChainImage: Index " + std::to_string(i));
        target.imageViews[i] = CommonFunction::Create2DImageView(
            device,
            target.images[i],
            1,
            target.surfaceFormat.format,
            vk::ImageAspectFlagBits::eColor,
            "SwapChainImageView_Index_" + std::to_string(i));
        assert(target.imageViews[i]);
    }
}

void VulkanManager::DestroyVkSwapChain(VL::PresentTarget &target)
{
    for(vk::ImageView& imageView : target.imageViews)
    {
        if (imageView)
        {
            device.destroyImageView(imageView);
        }
    }
    target.imageViews.clear();

    if (target.swapChain)
    {
        device.destroySwapchainKHR(target.swapChain);
        target.swapChain = nullptr;
    }
    target.imageCount = 0;
}

void VulkanManager::CreateCommandPool()
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        .setQueueFamilyIndex(graphicsQueueFamilyIndex.value())
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
    commandPool = device.createCommandPool(commandPoolCreateInfo);
    assert(commandPool);
    VulkanDebug::SetObjectName(device, commandPool, vk::ObjectType::eCommandPool, "CommandPool: Main");
}

void VulkanManager::DestroyCommandPool()
{
    if (!commandPool)
    {
        return;
    }
    // 池的销毁会连带释放从它分配出去的命令缓冲，因此调用方必须先确保
    // 各目标的命令缓冲已经不再被引用（本函数只在设备级关闭路径上被调用）。
    device.destroyCommandPool(commandPool);
    commandPool = nullptr;
}

void VulkanManager::CreateTargetCommandBuffers(VL::PresentTarget &target)
{
    // 从**共享**池分配，但缓冲数量随该目标的 swapchain 图像数变化，因此归目标所有。
    target.commandBuffers.resize(target.imageCount);

    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        .setCommandPool(commandPool)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(static_cast<uint32_t>(target.commandBuffers.size()));
    vk::Result result = device.allocateCommandBuffers(
        &commandBufferAllocateInfo, target.commandBuffers.data());
    assert(result == vk::Result::eSuccess);

    for (size_t i = 0; i < target.commandBuffers.size(); i++)
    {
        VulkanDebug::SetObjectName(device, target.commandBuffers[i], vk::ObjectType::eCommandBuffer,
            "CommandBuffer: SwapchainIndex " + std::to_string(i));
    }
}

void VulkanManager::DestroyTargetCommandBuffers(VL::PresentTarget &target)
{
    // 只释放缓冲，**不销毁池**：池是共享的，销毁它会连带影响别的窗口与一次性命令。
    if (!commandPool || target.commandBuffers.empty())
    {
        target.commandBuffers.clear();
        return;
    }
    device.freeCommandBuffers(commandPool,
        static_cast<uint32_t>(target.commandBuffers.size()), target.commandBuffers.data());
    target.commandBuffers.clear();
}

void VulkanManager::CreateFrameSyncObjects()
{
    // 帧级同步对象：按 MAX_FRAMES_IN_FLIGHT 分配，**与 swapchain 图像数无关**，
    // 因此生命周期与设备一致，不随任何窗口的重建而销毁。它与按目标的同步对象混在一个函数里
    // 曾是"重建时把全局对象一起重建"的来源。
    imageAcquiredSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vk::SemaphoreCreateInfo imageAcquiredSemaphoreCreateInfo;
        imageAcquiredSemaphores[i] = device.createSemaphore(imageAcquiredSemaphoreCreateInfo);
        assert(imageAcquiredSemaphores[i]);
        VulkanDebug::SetObjectName(device, imageAcquiredSemaphores[i], vk::ObjectType::eSemaphore, "Semaphore: ImageAcquired (Frame " + std::to_string(i) + ")");
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
}

void VulkanManager::DestroyFrameSyncObjects()
{
    for (vk::Semaphore& semaphore : imageAcquiredSemaphores)
    {
        if (semaphore)
        {
            device.destroySemaphore(semaphore);
        }
    }
    imageAcquiredSemaphores.clear();

    for (vk::Fence& fence : taskFinishedFences)
    {
        if (fence)
        {
            device.destroyFence(fence);
        }
    }
    taskFinishedFences.clear();
}

// 每目标同步对象：按该目标的 swapchain 图像数分配，因此随目标重建。
void VulkanManager::CreateTargetSyncObjects(VL::PresentTarget &target)
{
    target.renderFinishedSemaphores.resize(target.imageCount);
    for (size_t i = 0; i < target.imageCount; i++)
    {
        vk::SemaphoreCreateInfo renderFinishedSemaphoreCreateInfo;
        target.renderFinishedSemaphores[i] = device.createSemaphore(renderFinishedSemaphoreCreateInfo);
        assert(target.renderFinishedSemaphores[i]);
        VulkanDebug::SetObjectName(device, target.renderFinishedSemaphores[i], vk::ObjectType::eSemaphore, "Semaphore_RenderFinished_" + std::to_string(i));
    }

    // 每图像一个 in-flight fence，初值 nullptr（表示该图像还没有在飞的提交）。
    target.imagesInFlightFences.resize(target.imageCount, nullptr);
}

void VulkanManager::DestroyTargetSyncObjects(VL::PresentTarget &target)
{
    for (vk::Semaphore& semaphore : target.renderFinishedSemaphores)
    {
        if (semaphore)
        {
            device.destroySemaphore(semaphore);
        }
    }
    target.renderFinishedSemaphores.clear();
    target.imagesInFlightFences.clear();
}

