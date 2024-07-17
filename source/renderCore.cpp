#include "renderCore.h"
#include "SDL3/SDL_vulkan.h"
#include <iostream>

RenderCore::~RenderCore()
{
    DestroyVkInstance();
    DestroyVkDevice();
}

void RenderCore::Init(std::vector<const char *> &extensions, SDL_Window *window)
{
    sdlExtensoins.resize(extensions.size());
    std::copy(extensions.begin(), extensions.end(), sdlExtensoins.begin());
    sdlWindow = window;
    if (_instance == nullptr)
    {
        _instance.reset(new RenderCore);
    }
    for (auto extension : extensions)
    {
        std::cout << extension << std::endl;
    }
}

void RenderCore::Quit()
{
    _instance.reset();
}

RenderCore &RenderCore::GetInstance()
{
    if (_instance == nullptr)
    {
        _instance.reset(new RenderCore);
    }
    return *_instance; // TODO: insert return statement here
}

void RenderCore::CreateVkInstace()
{
    vk::ApplicationInfo applicationInfo;
    applicationInfo.setApiVersion(VK_API_VERSION_1_3);
    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo
        .setPApplicationInfo(&applicationInfo)
        .setPEnabledLayerNames(instanceLayers)
        .setPEnabledExtensionNames(sdlExtensoins);
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

void RenderCore::CreateVkDevice()
{
    vk::DeviceQueueCreateInfo deviceQueueCreateInfo;
    float queuePriorities = 1.0f;
    queueFamilyIndices.graphicsQueue = QueryQueueFamilyIndices(vk::QueueFlagBits::eGraphics);
    queueFamilyIndices.presentQueue=QueryQueueFamilyIndices(true)
    if (!queueFamilyIndices.graphicsQueue.has_value())
    {
        std::cout << "Info : "
                  << "Not QueryQueueFamilyIndices : "
                  << std::endl;
    };
    deviceQueueCreateInfo
        .setPQueuePriorities(&queuePriorities)
        .setQueueCount(1)
        .setQueueFamilyIndex(queueFamilyIndices.graphicsQueue.value());
    vk::DeviceCreateInfo deviceCreateInfo;
    deviceCreateInfo.setQueueCreateInfos(deviceQueueCreateInfo);
    device = physicalDevice.createDevice(deviceCreateInfo);

    graphicQueue = device.getQueue(queueFamilyIndices.graphicsQueue.value(), 0);
}

void RenderCore::DestroyVkDevice()
{
    device.destroy();
}

RenderCore::RenderCore()
{
    CreateVkInstace();
    PickupPhysicalDevice();
    CreateVkSurface();
    CreateVkDevice();
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
        const bool result = physicalDevice.getSurfaceSupportKHR(i,surface);
        if (result)
        {
            return i;
            break;
        }
    }
    return std::nullopt;
}
