#include "vulkanManager.h"
#include "SDL3/SDL_vulkan.h"
#include <cstdint>
#include <iostream>
#include <chrono>
#include "settings.h"
#include "renderPipline.h"
#include "commonFunction.h"
// Loader
#include "textureLoader.h"
#include "modelLoader.h"
#include "sceneLoader.h"

VulkanManager::VulkanManager()
{
}

void VulkanManager::Init(std::vector<const char *> &extensions, SDL_Window *window)
{
    instanceExtensions.resize(extensions.size());
    std::copy(extensions.begin(), extensions.end(), instanceExtensions.begin());
    sdlWindow = window;
    for (auto extension : instanceExtensions)
    {
        std::cout << "instanceExtension:" << extension << std::endl;
    }

    CreateVkInstance();
    EnumeratePhysicalDevices();
    CreateVkSurface();
    CreateVkDevice();
    CreateVkCommandBuffer();
    CreateVkSwapChain();
    CreateColorResource();
    CreateVkDepthBuffer();
    CreateVkRenderPass();
    CreateVkFrameBuffers();
    CreateVkFence();
    InitInstance(); //初始化实例
}

VulkanManager::~VulkanManager()
{
    device.waitIdle(); //等待设备空闲
    DestroyVkFence();
    DestroyVkFrameBuffers();
    DestroyVkRenderPass();
    DestroyVkDepthBuffer();
    DestroyColorResource();
    DestroyVkSwapChain();
    DestroyVkCommandBuffer();
    DestroyVkDevice();
    DestroyVkSurface();
    DestroyVkInstance();
}

void VulkanManager::ReCreateSwapChain(int newWidth, int newHeight)
{
    device.waitIdle(); //等待设备空闲

    //todo: setting.h 需要转化为json配置文件
    width = newWidth;
    height = newHeight;

    DestroyVkFrameBuffers();
    DestroyVkSwapChain();

    CreateVkSwapChain();
    CreateVkFrameBuffers();
}

void VulkanManager::CreateVkInstance()
{
    std::cout << "Valid Vulkan Instance Layers:" << std::endl;
    for(auto& Layer :vk::enumerateInstanceLayerProperties())
    {
        std::cout << "  Layer:" << Layer.layerName << std::endl;
    }
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
        std::cout << "Failed to create Vulkan instance" << std::endl;
        return;
    }
}

void VulkanManager::DestroyVkInstance()
{
    instance.destroy();
    std::cout << "Destroy VkInstance" << std::endl;
}

void VulkanManager::EnumeratePhysicalDevices()
{
    physicalDevices = instance.enumeratePhysicalDevices();

    gpuCount = static_cast<uint32_t>(physicalDevices.size());

    if (gpuCount == 0)
    {
        std::cout << "No GPU found" << std::endl;
        return;
    }
    std::cout << "GPU count: " << gpuCount << std::endl;
    for (uint32_t i = 0; i < gpuCount; i++)
    {
        vk::PhysicalDeviceProperties deviceProperties = physicalDevices[i].getProperties();
        std::cout << "GPU " << i << ": " << deviceProperties.deviceName << std::endl;
    }
    vk::PhysicalDevice physicalDevice = physicalDevices[0];
    gpuMemoryProperties = physicalDevice.getMemoryProperties();
    sampleCount = CommonFunction::GetMaxUsableSampleCount(physicalDevice);
    std::cout << "Max Sample Count: " << (uint32_t)sampleCount << std::endl;
}

void VulkanManager::CreateVkSurface()
{
    bool result = SDL_Vulkan_CreateSurface(sdlWindow, instance, nullptr,&surface);
    if(result == true)
    {
        std::cout << "Create VkSurface" << std::endl;
    }
    
}

void VulkanManager::DestroyVkSurface()
{
    instance.destroySurfaceKHR(surface);
    std::cout << "Destroy VkSurface" << std::endl;
}

void VulkanManager::CreateVkDevice()
{
    // 找一下quequeFamilys
    physicalDevices[GPUIndex].getQueueFamilyProperties(&queueFamilyCount, nullptr);
    queueFamilyProperties.resize(queueFamilyCount);
    physicalDevices[GPUIndex].getQueueFamilyProperties(&queueFamilyCount, queueFamilyProperties.data());

    std::vector<uint32_t> presentQueueFamilyIndices;
    std::vector<uint32_t> graphicQueueFamilyIndices;

    for (uint32_t i = 0; i < queueFamilyCount; i++)
    {
        const auto &property = queueFamilyProperties[i];
        if (property.queueFlags & vk::QueueFlagBits::eGraphics)
        {
            graphicQueueFamilyIndices.push_back(i);
        }
        if (physicalDevices[GPUIndex].getSurfaceSupportKHR(i, surface))
        {
            presentQueueFamilyIndices.push_back(i);
        }
    }

    // Check if we have a graphics queue family
    if (graphicQueueFamilyIndices.empty())
    {
        std::cout << "No graphics queue family found" << std::endl;
        return;
    }

    // Check if we have a present queue family
    if (presentQueueFamilyIndices.empty())
    {
        std::cout << "No present queue family found" << std::endl;
        return;
    }
    // 找到一个Graphics和Present都支持的队列
    for (uint32_t i = 0; i < graphicQueueFamilyIndices.size(); i++)
    {
        for (uint32_t j = 0; j < presentQueueFamilyIndices.size(); j++)
        {
            if (graphicQueueFamilyIndices[i] == presentQueueFamilyIndices[j])
            {
                graphicQueueFamilyIndex = graphicQueueFamilyIndices[i];
                presentQueueFamilyIndex = presentQueueFamilyIndices[j];
                std::cout << "Found a queue family that supports both graphics and present: " << graphicQueueFamilyIndex.value() << std::endl;
                break;
            }
        }
    }
    // 如果没有找到，就用第一个Graphics和第一个Present
    if (!graphicQueueFamilyIndex.has_value())
    {
        graphicQueueFamilyIndex = graphicQueueFamilyIndices[0];
        presentQueueFamilyIndex = presentQueueFamilyIndices[0];
        std::cout << "Using the first graphics and present queue family: " << graphicQueueFamilyIndex.value() << presentQueueFamilyIndex.value() << std::endl;
    }

    vk::DeviceQueueCreateInfo deviceGraphicsQueueCreateInfo;
    float graohicsQueuePriorities[1] = {0.0f};
    deviceGraphicsQueueCreateInfo
        .setQueueFamilyIndex(graphicQueueFamilyIndex.value())
        .setQueueCount(1)
        .setPQueuePriorities(graohicsQueuePriorities);

    //TODO: 这里应该先检测是否支持该 Features
    vk::PhysicalDeviceFeatures deviceFeatures;
    deviceFeatures
        .setDepthClamp(VK_TRUE)
        .setSamplerAnisotropy(VK_TRUE);
    
    vk::DeviceCreateInfo deviceCreateInfo;
    deviceCreateInfo
        .setQueueCreateInfoCount(1)
        .setPQueueCreateInfos(&deviceGraphicsQueueCreateInfo)
        .setEnabledExtensionCount(static_cast<uint32_t>(deviceExtensions.size()))
        .setPEnabledExtensionNames(deviceExtensions)
        .setPEnabledFeatures(&deviceFeatures)
        .setEnabledLayerCount(0);
    
    device = physicalDevices[GPUIndex].createDevice(deviceCreateInfo);
    assert(device);
    std::cout << "Create VkDevice" << std::endl;

    // Get the graphics queue
    device.getQueue(graphicQueueFamilyIndex.value(), 0, &graphicQueue);
    // Get the present queue
    device.getQueue(presentQueueFamilyIndex.value(), 0, &presentQueue);
}

void VulkanManager::DestroyVkDevice()
{
    device.destroy();
    std::cout << "Destroy VkDevice" << std::endl;
}

void VulkanManager::CreateVkCommandBuffer()
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        .setQueueFamilyIndex(graphicQueueFamilyIndex.value())
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
    commandPool = device.createCommandPool(commandPoolCreateInfo);
    assert(commandPool);

    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        .setCommandPool(commandPool)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(commandBuffers.size());
    vk::Result result = device.allocateCommandBuffers(&commandBufferAllocateInfo, commandBuffers.data());

    commandBufferBeginInfo
        .setPInheritanceInfo(nullptr);

    vk::PipelineStageFlags* piplineStageFlags = new vk::PipelineStageFlags();
    *piplineStageFlags = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    submitInfo[0]
        .setPWaitDstStageMask(piplineStageFlags)
        .setCommandBuffers(commandBuffers)
        .setSignalSemaphores(nullptr);
}

void VulkanManager::DestroyVkCommandBuffer()
{
    device.freeCommandBuffers(commandPool, commandBuffers.size(), commandBuffers.data());
    device.destroyCommandPool(commandPool);
    std::cout << "Destroy VkCommandBuffer" << std::endl;
}

void VulkanManager::CreateVkSwapChain()
{
    // 获取支持的表面格式
    surfaceFormats = physicalDevices[GPUIndex].getSurfaceFormatsKHR(surface);
    assert(!surfaceFormats.empty());
    for(const auto &availableFormat : surfaceFormats)
    {
        if(availableFormat.format == vk::Format::eR8G8B8A8Srgb && availableFormat.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
        {
            surfaceFormat = availableFormat;
            break;
        }
    }
    std::cout << "Surface format: " << vk::to_string(surfaceFormat.format) << std::endl;
    for (const auto &surfaceFormat : surfaceFormats)
    {
        std::cout << "Support Surface format: " << vk::to_string(surfaceFormat.format) << std::endl;
    }
    // 获取KHR表面能力
    surfaceCapabilities = physicalDevices[GPUIndex].getSurfaceCapabilitiesKHR(surface);
    std::cout << "Surface capabilities: " << std::endl;
    std::cout << "  minImageCount: " << surfaceCapabilities.minImageCount << std::endl;
    std::cout << "  maxImageCount: " << surfaceCapabilities.maxImageCount << std::endl;
    std::cout << "  currentExtent: " << surfaceCapabilities.currentExtent.width << "x" << surfaceCapabilities.currentExtent.height << std::endl;
    std::cout << "  minImageExtent: " << surfaceCapabilities.minImageExtent.width << "x" << surfaceCapabilities.minImageExtent.height << std::endl;
    std::cout << "  maxImageExtent: " << surfaceCapabilities.maxImageExtent.width << "x" << surfaceCapabilities.maxImageExtent.height << std::endl;
    std::cout << "  maxImageArrayLayers: " << surfaceCapabilities.maxImageArrayLayers << std::endl;
    std::cout << "  supportedTransforms: " << vk::to_string(surfaceCapabilities.supportedTransforms) << std::endl;
    std::cout << "  currentTransform: " << vk::to_string(surfaceCapabilities.currentTransform) << std::endl;
    std::cout << "  supportedCompositeAlpha: " << vk::to_string(surfaceCapabilities.supportedCompositeAlpha) << std::endl;
    std::cout << "  supportedUsageFlags: " << vk::to_string(surfaceCapabilities.supportedUsageFlags) << std::endl;
    //获取支持的显示模式数量
    presentModes = physicalDevices[GPUIndex].getSurfacePresentModesKHR(surface);
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
    std::cout << "Present mode: " << vk::to_string(presentMode) << std::endl;
    for (const auto &presentMode : presentModes)
    {
        std::cout << "  Support present mode: " << vk::to_string(presentMode) << std::endl;
    }
    // 确定surface的高度和宽度
    if(surfaceCapabilities.currentExtent.width == 0xFFFFFFFF)
    {
        //如果surface能力中的尺寸没有定义（宽度为0xFFFFFFFF表示没定义）
        swapChainExtent.width = std::clamp(static_cast<uint32_t>(width), surfaceCapabilities.minImageExtent.width, surfaceCapabilities.maxImageExtent.width);
        swapChainExtent.height = std::clamp(static_cast<uint32_t>(height), surfaceCapabilities.minImageExtent.height, surfaceCapabilities.maxImageExtent.height);
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
        .setOldSwapchain(nullptr);//后续可能用于resize功能
    if(graphicQueueFamilyIndex.value() != presentQueueFamilyIndex.value())
    {
        //如果图形队列和呈现队列不是同一个队列族
        //则需要设置共享模式
        uint32_t queueFamilyIndices[] = { graphicQueueFamilyIndex.value(), presentQueueFamilyIndex.value() };
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
    std::cout << "Swap chain image count: " << swapChainImageCount << std::endl;
    // 获取交换链中的图像
    swapChainImages.resize(swapChainImageCount);
    result = device.getSwapchainImagesKHR(swapChain, &swapChainImageCount, swapChainImages.data());
    assert(result == vk::Result::eSuccess);
    // 创建交换链图像视图
    swapChainImageViews.resize(swapChainImageCount);
    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        vk::ImageViewCreateInfo imageViewCreateInfo;
        imageViewCreateInfo
            .setImage(swapChainImages[i])
            .setViewType(vk::ImageViewType::e2D)
            .setFormat(surfaceFormat.format)
            .setComponents(vk::ComponentMapping())
            .setSubresourceRange(
                vk::ImageSubresourceRange(
                    vk::ImageAspectFlagBits::eColor, 
                    0, //baseMipLevel
                    1, //MipmaplevelCount
                    0, //baseArrayLayer
                    1  //layerCount
                ));
        swapChainImageViews[i] = device.createImageView(imageViewCreateInfo);
        assert(swapChainImageViews[i]);
    }
    std::cout << "Create VkSwapChain" << std::endl;
    std::cout << "  Swap chain image format: " << vk::to_string(surfaceFormat.format) << std::endl;
    std::cout << "  Swap chain image extent: " << swapChainExtent.width << "x" << swapChainExtent.height << std::endl;
    std::cout << "  Swap chain image color space: " << vk::to_string(surfaceFormat.colorSpace) << std::endl;
    std::cout << "  Swap chain image view count: " << swapChainImageCount << std::endl;
}

void VulkanManager::DestroyVkSwapChain()
{
    for(uint32_t i = 0; i < swapChainImageCount; i++)
    {
        device.destroyImageView(swapChainImageViews[i]);
    }
    device.destroySwapchainKHR(swapChain);
    std::cout << "Destroy VkSwapChain" << std::endl;
}

void VulkanManager::CreateColorResource()
{
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eTransientAttachment | vk::ImageUsageFlagBits::eColorAttachment;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    std::tie(colorImage, colorImageMemory) = CommonFunction::CreateImage(
        device, 
        width, height, 1, sampleCount, 
        surfaceFormat.format, tiling, 
        usage,
        gpuMemoryProperties,
        memoryPropertyFlags);

    colorImageView = CommonFunction::CreateImageView(
        device, 
        colorImage,
        1, 
        surfaceFormat.format, 
        vk::ImageAspectFlagBits::eColor
        );
    std::cout << "Create Color Resource" << std::endl;
}
void VulkanManager::DestroyColorResource()
{
    device.destroyImageView(colorImageView);
    device.freeMemory(colorImageMemory);
    device.destroyImage(colorImage);
    std::cout << "Destroy Color Resource" << std::endl;
}

void VulkanManager::CreateVkDepthBuffer()
{
    depthFormat = CommonFunction::FindDepthFormat(physicalDevices[GPUIndex]);
    assert(depthFormat != vk::Format::eUndefined);
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    vk::ImageUsageFlags usage = vk::ImageUsageFlagBits::eDepthStencilAttachment;
    vk::MemoryPropertyFlags memoryPropertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    std::tie(depthImage, depthImageMemory) = CommonFunction::CreateDepthImage(device, physicalDevices[GPUIndex], swapChainExtent.width, swapChainExtent.height, sampleCount, depthFormat, tiling, usage, gpuMemoryProperties, memoryPropertyFlags);
    CommonFunction::TransitionImageLayout(depthImage, 1, depthFormat, device, commandPool, graphicQueue, vk::ImageLayout::eUndefined, vk::ImageLayout::eDepthStencilAttachmentOptimal);
    depthImageView = CommonFunction::CreateDepthImageView(device, physicalDevices[GPUIndex], depthImage, depthFormat);
    std::cout << "Create VkDepthBuffer" << std::endl;
    std::cout << "  Depth format: " << vk::to_string(depthFormat) << std::endl;
}

void VulkanManager::DestroyVkDepthBuffer()
{
    device.destroyImageView(depthImageView);
    device.freeMemory(depthImageMemory);
    device.destroyImage(depthImage);
    std::cout << "Destroy VkDepthBuffer" << std::endl;
}

void VulkanManager::CreateVkRenderPass()
{
    //创建信号量
    imageAcquiredSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vk::SemaphoreCreateInfo imageAcquiredSemaphoreCreateInfo;
        imageAcquiredSemaphores[i] = device.createSemaphore(imageAcquiredSemaphoreCreateInfo);
        assert(imageAcquiredSemaphores[i]);
        renderFinishedSemaphores[i] = device.createSemaphore(imageAcquiredSemaphoreCreateInfo);
        assert(renderFinishedSemaphores[i]);
    }

    //创建渲染通道
    std::vector<vk::AttachmentDescription> attachmentDescriptions;
    vk::AttachmentDescription colorAttachmentDescription;
    colorAttachmentDescription
        .setFormat(surfaceFormat.format)
        .setSamples(sampleCount)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare) 
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setFinalLayout(vk::ImageLayout::ePresentSrcKHR)
        .setFlags(vk::AttachmentDescriptionFlags(0));
    vk::AttachmentDescription depthAttachmentDescription;
    attachmentDescriptions.push_back(colorAttachmentDescription);
    depthAttachmentDescription
        .setFormat(depthFormat)
        .setSamples(sampleCount)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
        .setFlags(vk::AttachmentDescriptionFlags(0));
    attachmentDescriptions.push_back(depthAttachmentDescription);
    vk::AttachmentDescription colorAttachmentResolveDescription;
    colorAttachmentResolveDescription
        .setFormat(surfaceFormat.format)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
    attachmentDescriptions.push_back(colorAttachmentResolveDescription);

    vk::AttachmentReference colorAttachmentReference;
    colorAttachmentReference
        .setAttachment(0)
        .setLayout(vk::ImageLayout::eColorAttachmentOptimal);
    vk::AttachmentReference depthAttachmentReference;
    depthAttachmentReference
        .setAttachment(1)
        .setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
    vk::AttachmentReference colorAttachmentResolveReference;
    colorAttachmentResolveReference
        .setAttachment(2)
        .setLayout(vk::ImageLayout::eColorAttachmentOptimal);

    vk::SubpassDescription subpassDescription;
    subpassDescription
        .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
        .setColorAttachments(colorAttachmentReference)
        .setPDepthStencilAttachment(&depthAttachmentReference)
        .setResolveAttachments(colorAttachmentResolveReference);

    vk::SubpassDependency subpassDependency;
    subpassDependency
        .setSrcSubpass(0)
        .setDstSubpass(0)
        .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eLateFragmentTests)
        .setSrcAccessMask(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDstStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput | vk::PipelineStageFlagBits::eEarlyFragmentTests)
        .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite | vk::AccessFlagBits::eDepthStencilAttachmentWrite)
        .setDependencyFlags(vk::DependencyFlagBits::eByRegion);

    vk::RenderPassCreateInfo renderPassCreateInfo;
    renderPassCreateInfo
        .setAttachments(attachmentDescriptions)
        .setSubpasses(subpassDescription)
        .setDependencies(subpassDependency);

    renderPass = device.createRenderPass(renderPassCreateInfo);
    assert(renderPass);
    std::cout << "Create VkRenderPass" << std::endl;

    // 创建清除值
    clearValues.resize(2);
    clearValues[0]
        .setColor(vk::ClearColorValue(std::array<float, 4>{0.2f, 0.2f, 0.2f, 0.2f}));
    clearValues[1]
        .setDepthStencil(vk::ClearDepthStencilValue(1.0f, 0));

    // 创建渲染通道开始信息
    renderPassBeginInfo
        .setRenderPass(renderPass)
        .setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), swapChainExtent))
        .setClearValues(clearValues);
}

void VulkanManager::DestroyVkRenderPass()
{
    device.destroyRenderPass(renderPass);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        device.destroySemaphore(imageAcquiredSemaphores[i]);
        device.destroySemaphore(renderFinishedSemaphores[i]);
    }
    std::cout << "Destroy VkRenderPass" << std::endl;
}

void VulkanManager::CreateVkFrameBuffers()
{
    std::vector<vk::ImageView> attachments;
    attachments.resize(3);
    attachments[0] = colorImageView;
    attachments[1] = depthImageView;
    attachments[2] = swapChainImageViews[0]; //占位，后面会被替换
    vk::FramebufferCreateInfo framebufferCreateInfo;
    framebufferCreateInfo
        .setRenderPass(renderPass)
        .setAttachments(attachments)
        .setWidth(swapChainExtent.width)
        .setHeight(swapChainExtent.height)
        .setLayers(1);
    framebuffers.resize(swapChainImageCount);
    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        attachments[2] = swapChainImageViews[i];
        framebuffers[i] = device.createFramebuffer(framebufferCreateInfo);
        assert(framebuffers[i]);
    }
    std::cout << "Create VkFrameBuffers" << std::endl;
    std::cout << "  Framebuffer count: " << swapChainImageCount << std::endl;
}

void VulkanManager::DestroyVkFrameBuffers()
{
    for (uint32_t i = 0; i < swapChainImageCount; i++)
    {
        device.destroyFramebuffer(framebuffers[i]);
    }
    std::cout << "Destroy VkFrameBuffers" << std::endl;
}

void VulkanManager::CreateVkFence()
{
    vk::FenceCreateInfo fenceCreateInfo;
    fenceCreateInfo
        .setFlags(vk::FenceCreateFlagBits::eSignaled); // 设置为signaled状态，表示初始状态为完成
    
    taskFinishedFences.resize(MAX_FRAMES_IN_FLIGHT);
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        vk::Result result = device.createFence(&fenceCreateInfo, nullptr, &taskFinishedFences[i]);
        assert(result == vk::Result::eSuccess);
    }
    std::cout << "Create VkFence" << std::endl;
}

void VulkanManager::DestroyVkFence()
{
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++)
    {
        if (taskFinishedFences[i])
        {
            device.destroyFence(taskFinishedFences[i]);
        }
    }
    std::cout << "Destroy VkFence" << std::endl;
}

void VulkanManager::InitInstance()
{
    TextureLoader& textureLoader = TextureLoader::GetInstance();
    textureLoader.Init(&device, &physicalDevices[0], &gpuMemoryProperties, &commandPool, &graphicQueue);
}