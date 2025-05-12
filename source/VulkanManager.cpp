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
        std::cout << extension << std::endl;
    }

    createInstance();
}

VulkanManager::~VulkanManager()
{
}

void VulkanManager::createInstance()
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
}

void VulkanManager::destroyInstance()
{
    instance.destroy();
    instance = nullptr;
    sdlWindow = nullptr;
}
