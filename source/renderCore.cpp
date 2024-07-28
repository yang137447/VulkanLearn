#include "renderCore.h"
#include "SDL3/SDL_vulkan.h"
#include <iostream>

RenderCore::RenderCore(std::vector<const char *> &extensions, SDL_Window *window)
{
    Init(extensions, window);
    CreateVkInstace();
    PickupPhysicalDevice();
    CreateVkSurface();
    CreateVkDevice();
    CreateVkSwapchain();
}

RenderCore::~RenderCore()
{
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
    swapchain = std::make_unique<Swapchain>(
        static_cast<uint32_t>(width), static_cast<uint32_t>(height),
        physicalDevice, device, surface, queueFamilyIndices);
}

void RenderCore::DestroyVkSwapchain()
{
    swapchain.reset();
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
