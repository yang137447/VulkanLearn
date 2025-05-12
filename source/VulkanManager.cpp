#include "VulkanManager.h"
#include <iostream>

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
    CreateVkDevice();
    CreateVkCommandBuffer();
}

VulkanManager::~VulkanManager()
{
    DestroyVkCommandBuffer();
    DestroyVkDevice();
    DestroyVkInstance();
}

void VulkanManager::CreateVkInstance()
{
    vk::ApplicationInfo applicationInfo;
    applicationInfo.setApiVersion(VK_API_VERSION_1_3);
    vk::InstanceCreateInfo instanceCreateInfo;
    instanceCreateInfo
        .setPApplicationInfo(&applicationInfo)
        // .setEnabledLayerCount(static_cast<uint32_t>(instanceLayers.size()))
        // .setPEnabledLayerNames(instanceLayers);
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

void VulkanManager::CreateVkDevice()
{
    physicalDevices[GPUIndex].getQueueFamilyProperties(&queueFamilyCount, nullptr);
    queueFamilyProperties.resize(queueFamilyCount);
    physicalDevices[GPUIndex].getQueueFamilyProperties(&queueFamilyCount, queueFamilyProperties.data());
    for (uint32_t i = 0; i < queueFamilyCount; i++)
    {
        const auto &property = queueFamilyProperties[i];
        if (property.queueFlags & vk::QueueFlagBits::eGraphics)
        {
            std::cout << "Found graphics queue family: " << i << std::endl;
            std::cout << "Queue count: " << property.queueCount << std::endl;
            graphicQueueFamilyIndex = i;
        }
    }

    vk::DeviceQueueCreateInfo deviceGraphicsQueueCreateInfo;
    float graohicsQueuePriorities = 0.0f;
    deviceGraphicsQueueCreateInfo
        .setQueueFamilyIndex(graphicQueueFamilyIndex)
        .setQueueCount(1)
        .setPQueuePriorities(&graohicsQueuePriorities);
    
    vk::DeviceCreateInfo deviceCreateInfo;
    deviceCreateInfo
        .setQueueCreateInfoCount(1)
        .setPQueueCreateInfos(&deviceGraphicsQueueCreateInfo)
        .setEnabledExtensionCount(static_cast<uint32_t>(deviceExtensions.size()))
        .setPEnabledExtensionNames(deviceExtensions);
    
    device = physicalDevices[GPUIndex].createDevice(deviceCreateInfo);
    assert(device);
    std::cout << "Create VkDevice" << std::endl;
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
        .setQueueFamilyIndex(graphicQueueFamilyIndex)
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer);
    commandPool = device.createCommandPool(commandPoolCreateInfo);
    assert(commandPool);

    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        .setCommandPool(commandPool)
        .setLevel(vk::CommandBufferLevel::ePrimary)
        .setCommandBufferCount(1);
    commandBuffer = device.allocateCommandBuffers(commandBufferAllocateInfo);

    commandBufferBeginInfo
        .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit)
        .setPInheritanceInfo(nullptr);
    commandBuffers[0] = commandBuffer;

    vk::PipelineStageFlags* piplineStageFlags = new vk::PipelineStageFlags();
    *piplineStageFlags = vk::PipelineStageFlagBits::eColorAttachmentOutput;
    submitInfo[0]
        .setPWaitDstStageMask(piplineStageFlags)
        .setCommandBufferCount(1)
        .setPCommandBuffers(commandBuffers)
        .setSignalSemaphoreCount(0)
        .setPSignalSemaphores(nullptr);
}

void VulkanManager::DestroyVkCommandBuffer()
{
    device.freeCommandBuffers(commandPool, 1, commandBuffers);
    device.destroyCommandPool(commandPool);
    std::cout << "Destroy VkCommandBuffer" << std::endl;
}
