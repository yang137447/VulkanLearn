#include "VulkanManager.h"
#include "SDL3/SDL_vulkan.h"
#include <cstdint>
#include <iostream>
#include "settings.h"
#include "DrawableObject.h"
#include "TriangleData.h"
#include "RenderPipline.h"
#include "CommonFunction.h"

VulkanManager::VulkanManager()
{
}

VulkanManager::VulkanManager(std::vector<const char *> &extensions, SDL_Window *window)
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
    CreateVkDepthBuffer();
    CreateVkRenderPass();
    CreateVkFrameBuffers();
    CreateDrawableObject();
    CreateVkPipline();
    CreateVkFence();
    InitializePresentInfo();
    InitializeMVP();
    
}

VulkanManager::~VulkanManager()
{
    DestroyVkFence();
    DestroyVkPipline();
    DestroyDrawableObject();
    DestroyVkFrameBuffers();
    DestroyVkRenderPass();
    DestroyVkDepthBuffer();
    DestroyVkSwapChain();
    DestroyVkCommandBuffer();
    DestroyVkDevice();
    DestroyVkSurface();
    DestroyVkInstance();
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
        .setDepthClamp(VK_TRUE);
    
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

    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        .setCommandPool(commandPool)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(1);
    vk::Result result = device.allocateCommandBuffers(&commandBufferAllocateInfo, &commandBuffer);

    commandBufferBeginInfo
        .setPInheritanceInfo(nullptr);
    commandBuffers[0] = commandBuffer;

    vk::PipelineStageFlags* piplineStageFlags = new vk::PipelineStageFlags();
    *piplineStageFlags = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    submitInfo[0]
        .setPWaitDstStageMask(piplineStageFlags)
        .setCommandBuffers(commandBuffers)
        .setSignalSemaphores(nullptr);
}

void VulkanManager::DestroyVkCommandBuffer()
{
    device.freeCommandBuffers(commandPool, 1, commandBuffers);
    device.destroyCommandPool(commandPool);
    std::cout << "Destroy VkCommandBuffer" << std::endl;
}

void VulkanManager::CreateVkSwapChain()
{
    // 获取支持的表面格式
    surfaceFormats = physicalDevices[GPUIndex].getSurfaceFormatsKHR(surface);
    assert(!surfaceFormats.empty());
    for (const auto &surfaceFormat : surfaceFormats)
    {
        std::cout << "Surface format: " << vk::to_string(surfaceFormat.format) << std::endl;
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
    for (const auto &presentMode : presentModes)
    {
        std::cout << "Present mode: " << vk::to_string(presentMode) << std::endl;
    }
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
        .setImageFormat(surfaceFormats[0].format)
        .setImageColorSpace(surfaceFormats[0].colorSpace)
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
            .setFormat(surfaceFormats[0].format)
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
    std::cout << "  Swap chain image format: " << vk::to_string(surfaceFormats[0].format) << std::endl;
    std::cout << "  Swap chain image extent: " << swapChainExtent.width << "x" << swapChainExtent.height << std::endl;
    std::cout << "  Swap chain image color space: " << vk::to_string(surfaceFormats[0].colorSpace) << std::endl;
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

void VulkanManager::CreateVkDepthBuffer()
{
    depthFormat = vk::Format::eD16Unorm;
    depthFormatProperties = physicalDevices[GPUIndex].getFormatProperties(depthFormat);
    // 确定平铺方式
    vk::ImageTiling tiling = vk::ImageTiling::eOptimal;
    if (depthFormatProperties.linearTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment)
    {
        tiling = vk::ImageTiling::eLinear;
    }
    else if (depthFormatProperties.optimalTilingFeatures & vk::FormatFeatureFlagBits::eDepthStencilAttachment)
    {
        tiling = vk::ImageTiling::eOptimal;
    }
    else
    {
        std::cout << "Depth format not supported" << std::endl;
        return;
    }
    // 创建深度图像
    vk::ImageCreateInfo depthImageCreateInfo;
    depthImageCreateInfo
        .setImageType(vk::ImageType::e2D)
        .setFormat(depthFormat)
        .setExtent(vk::Extent3D(swapChainExtent.width, swapChainExtent.height, 1))
        .setMipLevels(1)
        .setArrayLayers(1)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setTiling(tiling)
        .setUsage(vk::ImageUsageFlagBits::eDepthStencilAttachment)
        .setSharingMode(vk::SharingMode::eExclusive)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setQueueFamilyIndexCount(0)
        .setFlags(vk::ImageCreateFlags(0));
    depthImage = device.createImage(depthImageCreateInfo);
    assert(depthImage);

    // 分配深度图像内存
    vk::MemoryRequirements depthImageMemoryRequirements = device.getImageMemoryRequirements(depthImage);
    vk::MemoryAllocateInfo depthImageMemoryAllocateInfo;
    depthImageMemoryAllocateInfo
        .setAllocationSize(depthImageMemoryRequirements.size)
        .setMemoryTypeIndex(FindMemoryType(
            physicalDevices[GPUIndex].getMemoryProperties(), 
            depthImageMemoryRequirements.memoryTypeBits, 
            vk::MemoryPropertyFlagBits::eDeviceLocal));

    depthImageMemory = device.allocateMemory(depthImageMemoryAllocateInfo);
    assert(depthImageMemory);
    device.bindImageMemory(depthImage, depthImageMemory, 0);

    // 创建深度图像视图
    vk::ImageViewCreateInfo depthImageViewCreateInfo;
    depthImageViewCreateInfo
        .setImage(depthImage)
        .setViewType(vk::ImageViewType::e2D)
        .setFormat(depthFormat)
        .setComponents(vk::ComponentMapping(
            vk::ComponentSwizzle::eR, //r
            vk::ComponentSwizzle::eG, //g
            vk::ComponentSwizzle::eB, //b
            vk::ComponentSwizzle::eA  //a
        ))
        .setSubresourceRange(
            vk::ImageSubresourceRange(
                vk::ImageAspectFlagBits::eDepth, 
                0, //baseMipLevel
                1, //MipmaplevelCount
                0, //baseArrayLayer
                1  //layerCount
            ));
    depthImageView = device.createImageView(depthImageViewCreateInfo);
    assert(depthImageView);
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
    vk::SemaphoreCreateInfo imageAcquiredSemaphoreCreateInfo;
    imageAcquiredSemaphore = device.createSemaphore(imageAcquiredSemaphoreCreateInfo);
    assert(imageAcquiredSemaphore);

    //创建渲染通道
    vk::AttachmentDescription attachmentDescription;
    attachmentDescription
        .setFormat(surfaceFormats[0].format)
        .setSamples(vk::SampleCountFlagBits::e1)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setFinalLayout(vk::ImageLayout::ePresentSrcKHR)
        .setFlags(vk::AttachmentDescriptionFlags(0));
    // attachmentDescription[1]
    //     .setFormat(depthFormat)
    //     .setSamples(vk::SampleCountFlagBits::e1)
    //     .setLoadOp(vk::AttachmentLoadOp::eClear)
    //     .setStoreOp(vk::AttachmentStoreOp::eDontCare)
    //     .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
    //     .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
    //     .setInitialLayout(vk::ImageLayout::eUndefined)
    //     .setFinalLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
    //     .setFlags(vk::AttachmentDescriptionFlags(0));

    vk::AttachmentReference colorAttachmentReference;
    colorAttachmentReference
        .setAttachment(0)
        .setLayout(vk::ImageLayout::eColorAttachmentOptimal);
    // vk::AttachmentReference depthAttachmentReference;
    // depthAttachmentReference
    //     .setAttachment(1)
    //     .setLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);

    vk::SubpassDescription subpassDescription;
    subpassDescription
        // .setPInputAttachments(nullptr)
        // .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
        // .setColorAttachmentCount(1)
        // .setPColorAttachments(&colorAttachmentReference)
        // .setPResolveAttachments(nullptr)
        // .setPDepthStencilAttachment(&depthAttachmentReference)
        // .setPreserveAttachmentCount(0)
        // .setPPreserveAttachments(nullptr);
        .setColorAttachments(colorAttachmentReference);

    vk::RenderPassCreateInfo renderPassCreateInfo;
    renderPassCreateInfo
        // .setAttachmentCount(2)
        // .setPAttachments(attachmentDescription)
        // .setSubpassCount(1)
        // .setPSubpasses(&subpassDescription)
        // .setDependencyCount(0)
        // .setPDependencies(nullptr);
        .setAttachments(attachmentDescription)
        .setSubpasses(subpassDescription);

    renderPass = device.createRenderPass(renderPassCreateInfo);
    assert(renderPass);
    std::cout << "Create VkRenderPass" << std::endl;

    // 创建清除值
    clearValue
        .setColor(vk::ClearColorValue(std::array<float, 4>{0.2f, 0.2f, 0.2f, 0.2f}))
        .setDepthStencil(vk::ClearDepthStencilValue(1.0f, 0));

    // 创建渲染通道开始信息
    renderPassBeginInfo
        .setRenderPass(renderPass)
        .setRenderArea(vk::Rect2D(vk::Offset2D(0, 0), swapChainExtent))
        .setClearValueCount(2)
        .setPClearValues(&clearValue);
}

void VulkanManager::DestroyVkRenderPass()
{
    device.destroyRenderPass(renderPass);
    device.destroySemaphore(imageAcquiredSemaphore);
    std::cout << "Destroy VkRenderPass" << std::endl;
}

void VulkanManager::CreateVkFrameBuffers()
{
    vk::ImageView attachments[1];
    //attachments[1] = depthImageView;
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
        attachments[0] = swapChainImageViews[i];
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

void VulkanManager::CreateDrawableObject()
{
    triangleObject = new DrawableObject(TriangleData::GetVertexData(), &device, &gpuMemoryProperties);
    std::cout << "Create DrawableObject" << std::endl;
}

void VulkanManager::DestroyDrawableObject()
{
    delete triangleObject;
    std::cout << "Destroy DrawableObject" << std::endl;
}

void VulkanManager::CreateVkPipline()
{
    renderPipline = new RenderPipline(device, renderPass, gpuMemoryProperties);
    std::cout << "Create VkPipline" << std::endl;
}

void VulkanManager::DestroyVkPipline()
{
    delete renderPipline;
    std::cout << "Destroy VkPipline" << std::endl;
}

void VulkanManager::CreateVkFence()
{
    vk::FenceCreateInfo fenceCreateInfo;
    
    vk::Result result = device.createFence(&fenceCreateInfo, nullptr, &taskFinishedFence);
    assert(result == vk::Result::eSuccess);
    std::cout << "Create VkFence" << std::endl;
}

void VulkanManager::DestroyVkFence()
{
    device.destroyFence(taskFinishedFence);
    std::cout << "Destroy VkFence" << std::endl;
}

void VulkanManager::InitializePresentInfo()
{
    presentInfo
        .setSwapchainCount(1)
        .setPSwapchains(&swapChain);
}

void VulkanManager::InitializeMVP()
{
    InitMatrix();
}

void VulkanManager::DrawFrame()
{
    device.waitForFences(taskFinishedFence, true, UINT64_MAX);
    device.resetFences(taskFinishedFence);

    vk::Result result = device.acquireNextImageKHR(swapChain, UINT64_MAX, imageAcquiredSemaphore, nullptr, &swapchainImageIndex);
    assert(result == vk::Result::eSuccess);

    renderPassBeginInfo.setFramebuffer(framebuffers[swapchainImageIndex]);

    commandBuffer.reset();
    commandBuffer.begin(commandBufferBeginInfo);

    //FlushUniformBuffer();
    //FlushTextureToDescriptorSet();

    commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
    triangleObject->Draw(commandBuffer, renderPipline->GetPipelineLayout(), renderPipline->GetGraphicsPipeline(), renderPipline->GetDescriptorSet()[0]);

    commandBuffer.endRenderPass();

    constexpr vk::ClearColorValue clearColorValue(std::array<float, 4>{42.0f/255.0f, 181.f/255.0f, 116.f/255.0f, 1.0f});
    commandBuffer.clearColorImage(
        swapChainImages[swapchainImageIndex],
        vk::ImageLayout::eUndefined,
        clearColorValue,
        {vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1)}
    );

    commandBuffer.end();

    submitInfo[0]
        .setWaitSemaphoreCount(1)
        .setPWaitSemaphores(&imageAcquiredSemaphore);
    
    graphicQueue.submit(submitInfo, taskFinishedFence);
    
    do
    {
        result = device.waitForFences(1, &taskFinishedFence, VK_TRUE, 100000000);
    } while (result == vk::Result::eTimeout);
    
    result = device.resetFences(1, &taskFinishedFence);
    assert(result == vk::Result::eSuccess);

    presentInfo.pImageIndices = &swapchainImageIndex;
    result = graphicQueue.presentKHR(presentInfo);
    assert(result == vk::Result::eSuccess);
}

void VulkanManager::FlushUniformBuffer()
{
    // 定义一个静态的浮点数变量xAngle，初始值为0.0f
    static float xAngle = 0.0f;

    matrixStack.push(currentMatrix);

    // 每次调用drawFrame函数时，xAngle增加0.01
    xAngle += 0.5f;

    if(xAngle > 360.0f)
    {
        xAngle -= 360.0f;
    }
    //std::cout << "xAngle: " << xAngle << std::endl;

    SetRotation(xAngle, 0.0f, 0.0f);

    GetMVPMatrix();

    static float vertexUniformData[16];
    std::memcpy(vertexUniformData, currentMatrix.transpose().data(), sizeof(float) * 16);

    currentMatrix = matrixStack.top();
    matrixStack.pop();

    void* data = device.mapMemory(renderPipline->GetuniformBufferMemory(), 0, renderPipline->GetuniformBufferSize());
    assert(data != nullptr);
    std::memcpy(data, vertexUniformData, renderPipline->GetuniformBufferSize());
    device.unmapMemory(renderPipline->GetuniformBufferMemory());
}

void VulkanManager::FlushTextureToDescriptorSet()
{
    renderPipline->GetWriteDescriptorSets()[0]
    .setDstSet(renderPipline->GetDescriptorSet()[0])
    .setDstBinding(0)
    .setDstArrayElement(0)
    .setDescriptorType(vk::DescriptorType::eUniformBuffer)
    .setDescriptorCount(1)
    .setPBufferInfo(&renderPipline->GetuniformBufferInfo());

    device.updateDescriptorSets(renderPipline->GetWriteDescriptorSets(), nullptr);
}

void VulkanManager::InitMatrix()
{
    modelTransform.translate(Eigen::Vector3f(0.0f, 0.0f, 0.0f));
    modelTransform.rotate(Eigen::AngleAxisf(0.0f, Eigen::Vector3f::UnitX()));
    modelTransform.rotate(Eigen::AngleAxisf(0.0f, Eigen::Vector3f::UnitY()));
    modelTransform.rotate(Eigen::AngleAxisf(0.0f, Eigen::Vector3f::UnitZ()));
    modelTransform.scale(Eigen::Vector3f(1.0f, 1.0f, 1.0f));
    modelMatrix = modelTransform.matrix();

    SetCamera(Eigen::Vector3f(0.0f, 0.0f, 2000.0f), Eigen::Vector3f(0.0f, 0.0f, 0.0f), Eigen::Vector3f(0.0f, 1.0f, 0.0f));

    SetProjection(90.0f, 1.0f, 1.5f, 1000.0f);

    //Vulkan设备空间XYZ三个轴范围分别是 -1.0～+1.0、+1.0～-1.0、0.0～+1.0
    vulkanClipMatrix = Eigen::Matrix4f::Identity();
    vulkanClipMatrix(0, 0) = 1.0f;
    vulkanClipMatrix(1, 1) = -1.0f;
    vulkanClipMatrix(2, 2) = 0.5f;
    vulkanClipMatrix(3, 2) = 0.5f;
    vulkanClipMatrix(3, 3) = 1.0f;

    currentMatrix = vulkanClipMatrix * projectionMatrix * viewMatrix * modelMatrix;
}

void VulkanManager::SetTranslation(float x, float y, float z)
{
    modelTransform.translate(Eigen::Vector3f(x, y, z));
}

void VulkanManager::SetRotation(float x, float y, float z)
{
    modelTransform.rotate(Eigen::AngleAxisf(x, Eigen::Vector3f::UnitX()));
}

void VulkanManager::SetScale(float x, float y, float z)
{
    modelTransform.scale(Eigen::Vector3f(x, y, z));
}

void VulkanManager::SetCamera(Eigen::Vector3f position, Eigen::Vector3f lookAt, Eigen::Vector3f up)
{
    viewMatrix = Eigen::Affine3f(Eigen::Translation3f(position) * Eigen::Affine3f(Eigen::Quaternionf::FromTwoVectors(lookAt, up))).matrix();
}

void VulkanManager::SetProjection(float fov, float aspect, float near, float far)
{
    float fovy = fov * static_cast<float>(Pi) / 180.0f;
    float f = 1.0f / std::tan(fovy / 2.0f);
    projectionMatrix << 
        f / aspect, 0, 0, 0,
        0, f, 0, 0,
        0, 0, (far + near) / (near - far), (2 * far * near) / (near - far),
        0, 0, -1, 0;
}

void VulkanManager::GetMVPMatrix()
{
    currentMatrix = vulkanClipMatrix * projectionMatrix * viewMatrix * modelMatrix;
}

void VulkanManager::GetFinalMatrix()
{
    modelMatrix = modelTransform.matrix();
    currentMatrix = vulkanClipMatrix * projectionMatrix * viewMatrix * modelMatrix;
}
