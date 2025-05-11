#include "renderCore.h"
#include "SDL3/SDL_vulkan.h"
#include "swapchain.h"
#include "shaderModule.h"
#include "renderProcess.h"
#include "render.h"
#include "settings.h"
#include <iostream>

RenderCore::RenderCore(std::vector<const char *> &extensions, SDL_Window *window)
{
    Init(extensions, window);
    CreateVkInstace();
    PickupPhysicalDevice();
    CreateVkSurface();
    CreateVkDevice();
    CreateVkSwapchain();
    CreateVkRenderPass();
    CreateVkFramebuffers();
    CreateVkShaderModule();
    CreateVkRenderProcess();
    CreateVkRender();
}

RenderCore::~RenderCore()
{
    DestroyVkRender();
    DestroyVkRenderProcess();
    DestroyVkShaderModule();
    DestroyVkFramebuffers();
    DestroyVkRenderPass();
    DestroyVkSwapchain();
    DestroyVkDevice();
    DestroyVkSurface();
    DestroyVkInstance();
}

void RenderCore::Init(std::vector<const char *> &extensions, SDL_Window *window)
{
    instanceExtensions.resize(extensions.size());
    std::copy(extensions.begin(), extensions.end(), instanceExtensions.begin());
    sdlWindow = window;
    for (auto extension : extensions)
    {
        std::cout << extension << std::endl;
    }
}

void RenderCore::draw()
{
    render->Draw();
}

void RenderCore::CreateVkInstace()
{
    vk::ApplicationInfo applicationInfo;
    applicationInfo.setApiVersion(VK_API_VERSION_1_3);
    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo
        .setPApplicationInfo(&applicationInfo)
        .setPEnabledLayerNames(instanceLayers)
        .setPEnabledExtensionNames(instanceExtensions);
    instance = vk::createInstance(instanceCreateInfo);
}

void RenderCore::DestroyVkInstance()
{
    instance.destroy();
}

void RenderCore::PickupPhysicalDevice()
{
    auto physicalDevices = instance.enumeratePhysicalDevices();
    physicalDevice = physicalDevices[GPUIndex];
    std::cout << "Info : "
              << "Used GPU : "
              << physicalDevice.getProperties().deviceName
              << std::endl;
}

void RenderCore::CreateVkSurface()
{
    VkSurfaceKHR vksurface;
    if (!SDL_Vulkan_CreateSurface(sdlWindow, instance, &vksurface))
    {
        std::cout << "Info : "
                  << "CreateSurface failed"
                  << std::endl;
    };

    surface = vksurface;
}

void RenderCore::DestroyVkSurface()
{
    instance.destroySurfaceKHR(surface);
}

void RenderCore::CreateVkDevice()
{
    std::vector<vk::DeviceQueueCreateInfo> deviceQueueCreateInfos;

    vk::DeviceQueueCreateInfo deviceGraphicsQueueCreateInfo;
    float graohicsQueuePriorities = 1.0f;
    queueFamilyIndices.graphicsQueue = QueryQueueFamilyIndices(vk::QueueFlagBits::eGraphics);
    if (!queueFamilyIndices.graphicsQueue.has_value())
    {
        std::cout << "Info : "
                  << "Not QueryGraphicsQueueFamilyIndices : "
                  << std::endl;
    };
    deviceGraphicsQueueCreateInfo
        .setPQueuePriorities(&graohicsQueuePriorities)
        .setQueueCount(1)
        .setQueueFamilyIndex(queueFamilyIndices.graphicsQueue.value());

    vk::DeviceQueueCreateInfo devicePresentQueueCreateInfo;
    float presentQueuePriorities = 1.0f;
    queueFamilyIndices.presentQueue = QueryQueueFamilyIndices(true);
    if (!queueFamilyIndices.presentQueue.has_value())
    {
        std::cout << "Info : "
                  << "Not QueryPresentQueueFamilyIndices : "
                  << std::endl;
    };
    devicePresentQueueCreateInfo
        .setPQueuePriorities(&presentQueuePriorities)
        .setQueueCount(1)
        .setQueueFamilyIndex(queueFamilyIndices.presentQueue.value());

    if (queueFamilyIndices.graphicsQueue == queueFamilyIndices.presentQueue)
    {
        deviceQueueCreateInfos.emplace_back(deviceGraphicsQueueCreateInfo);
    }
    else
    {
        deviceQueueCreateInfos.emplace_back(deviceGraphicsQueueCreateInfo);
        deviceQueueCreateInfos.emplace_back(devicePresentQueueCreateInfo);
    }

    vk::DeviceCreateInfo deviceCreateInfo;
    deviceCreateInfo
        .setQueueCreateInfoCount(deviceQueueCreateInfos.size())
        .setQueueCreateInfos(deviceQueueCreateInfos)
        .setEnabledExtensionCount(deviceExtensions.size())
        .setPEnabledExtensionNames(deviceExtensions);

    device = physicalDevice.createDevice(deviceCreateInfo);

    graphicQueue = device.getQueue(queueFamilyIndices.graphicsQueue.value(), 0);
    presentQueue = device.getQueue(queueFamilyIndices.presentQueue.value(), 0);
}

void RenderCore::DestroyVkDevice()
{
    device.destroy();
}

void RenderCore::CreateVkSwapchain()
{
    int width, height;
    SDL_GetWindowSize(sdlWindow, &width, &height);
    swapchain = new Swapchain(
        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        physicalDevice, device, surface, renderPass, queueFamilyIndices);
}

void RenderCore::DestroyVkSwapchain()
{
    delete swapchain;
}

void RenderCore::CreateVkRenderPass()
{
    vk::AttachmentDescription colorAttachmentDescription;
    colorAttachmentDescription
        .setFormat(swapchain->GetSwapchainFormat())
        .setSamples(vk::SampleCountFlagBits::e1)
        .setLoadOp(vk::AttachmentLoadOp::eClear)
        .setStoreOp(vk::AttachmentStoreOp::eStore)
        .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
        .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
        .setInitialLayout(vk::ImageLayout::eUndefined)
        .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);

    vk::AttachmentReference colorAttachmentReference;
    colorAttachmentReference
        .setAttachment(0)
        .setLayout(vk::ImageLayout::eColorAttachmentOptimal);

    vk::SubpassDescription subpassDescription;
    subpassDescription
        .setPipelineBindPoint(vk::PipelineBindPoint::eGraphics)
        .setColorAttachmentCount(1)
        .setPColorAttachments(&colorAttachmentReference);

    // vk::SubpassDependency subpassDependency;
    // subpassDependency
    //     .setSrcSubpass(VK_SUBPASS_EXTERNAL)
    //     .setDstSubpass(0)
    //     .setDstAccessMask(vk::AccessFlagBits::eColorAttachmentWrite)
    //     .setSrcStageMask(vk::PipelineStageFlagBits::eColorAttachmentOutput);

    vk::RenderPassCreateInfo renderPassCreateInfo;
    renderPassCreateInfo
        .setAttachmentCount(1)
        .setPAttachments(&colorAttachmentDescription)
        .setSubpassCount(1)
        .setPSubpasses(&subpassDescription);
        // .setDependencyCount(1)
        // .setPDependencies(&subpassDependency);
    renderPass = device.createRenderPass(renderPassCreateInfo);
}

void RenderCore::DestroyVkRenderPass()
{
    device.destroyRenderPass(renderPass);
}

void RenderCore::CreateVkFramebuffers()
{
    framebuffers.resize(swapchain->GetImageCount());
    for (int i = 0; i < framebuffers.size(); i++)
    {
        vk::FramebufferCreateInfo framebufferCreateInfo;
        framebufferCreateInfo
            .setRenderPass(renderPass)
            .setAttachments(swapchain->GetImageViews()[i])
            .setWidth(swapchain->GetExtent().width)
            .setHeight(swapchain->GetExtent().height)
            .setLayers(1);
        framebuffers[i] = device.createFramebuffer(framebufferCreateInfo);
    }
}

void RenderCore::DestroyVkFramebuffers()
{
    for (auto &framebuffer : framebuffers)
    {
        device.destroyFramebuffer(framebuffer);
    }
}

void RenderCore::CreateVkShaderModule()
{
    std::string vertexShaderPath = filePath + "shader/spv/test_vert.spv";
    std::string fragmentShaderPath = filePath + "shader/spv/test_frag.spv";
    shaderModule = new ShaderModule(
        device,
        vertexShaderPath,
        fragmentShaderPath);
}

void RenderCore::DestroyVkShaderModule()
{
    delete shaderModule;
}

void RenderCore::CreateVkRenderProcess()
{
    renderProcess = new RenderProcess(
        device,
        renderPass,
        *shaderModule,
        *swapchain);
}

void RenderCore::DestroyVkRenderProcess()
{
    delete renderProcess;
}

void RenderCore::CreateVkRender()
{
    render = new Render(
        device,
        queueFamilyIndices,
        *swapchain,
        framebuffers,
        renderProcess->GetGraphicsPipeline(),
        renderPass,
        graphicQueue,
        presentQueue);
}

void RenderCore::DestroyVkRender()
{
    delete render;
}

RenderCore::RenderCore()
{
}

std::optional<uint32_t> RenderCore::QueryQueueFamilyIndices(vk::QueueFlagBits queryQueueflagbits)
{
    auto properties = physicalDevice.getQueueFamilyProperties();
    for (int i = 0; i < properties.size(); i++)
    {
        const auto &property = properties[i];
        if (property.queueFlags | queryQueueflagbits)
        {
            return i;
            break;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> RenderCore::QueryQueueFamilyIndices(bool findPresentQueue)
{
    auto properties = physicalDevice.getQueueFamilyProperties();
    for (int i = 0; i < properties.size(); i++)
    {
        const bool result = physicalDevice.getSurfaceSupportKHR(i, surface);
        if (result)
        {
            return i;
            break;
        }
    }
    return std::nullopt;
}
