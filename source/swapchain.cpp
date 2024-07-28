#include "swapchain.h"

Swapchain::Swapchain(/* args */)
{
}

Swapchain::Swapchain(const uint32_t windowWidth, const uint32_t windowHeight, const vk::PhysicalDevice &physicalDevice, const vk::Device &device, const vk::SurfaceKHR &surface, const QueueFamilyIndices &queueFamilyIndices)
{
    // init data
    this->device = device;
    QuerySwapchainData(windowWidth, windowHeight, physicalDevice, surface);

    // Create swapchain
    CreateVkSwapChain(device,surface,queueFamilyIndices);

    //Get Images
    GetVkImages();

    //Create imageViews
    CreateVkImageViews();
}

Swapchain::~Swapchain()
{
    DestroyVkImageViews();
    DestroyVkSwapChain();
}

void Swapchain::QuerySwapchainData(const uint32_t windowWidth, const uint32_t windowHeight, const vk::PhysicalDevice &physicalDevice, const vk::SurfaceKHR &surface)
{
    // Get imageFormat
    auto formats = physicalDevice.getSurfaceFormatsKHR(surface);
    imageFormat = formats[0];
    for (const auto &format : formats)
    {
        if (format.format == vk::Format::eR8G8B8A8Srgb && format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear)
        {
            imageFormat = format;
            break;
        }
    }
    // Get imageCount
    auto capabilities = physicalDevice.getSurfaceCapabilitiesKHR(surface);
    imageCount = std::clamp<uint32_t>(2, capabilities.minImageCount, capabilities.maxImageCount);
    // Get imageExtent
    imageExtent.width = std::clamp<uint32_t>(windowWidth, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
    imageExtent.height = std::clamp<uint32_t>(windowHeight, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    // Get surfaceTransform
    surfaceTransform = capabilities.currentTransform;
    // Get presentMode
    auto PresentModes = physicalDevice.getSurfacePresentModesKHR(surface);
    presentMode = vk::PresentModeKHR::eFifo;
    for (const auto &presentMode : PresentModes)
    {
        if (presentMode == vk::PresentModeKHR::eMailbox)
        {
            this->presentMode = presentMode;
            break;
        }
    }
}

void Swapchain::CreateVkSwapChain(const vk::Device &device, const vk::SurfaceKHR &surface, const QueueFamilyIndices &queueFamilyIndices)
{
    vk::SwapchainCreateInfoKHR swapchainCreateInfo;
    swapchainCreateInfo
        .setClipped(VK_TRUE)
        .setImageArrayLayers(1)
        .setImageUsage(vk::ImageUsageFlagBits::eColorAttachment)
        .setCompositeAlpha(vk::CompositeAlphaFlagBitsKHR::eOpaque)
        .setSurface(surface)
        .setImageExtent(imageExtent)
        .setImageColorSpace(imageFormat.colorSpace)
        .setImageFormat(imageFormat.format)
        .setMinImageCount(imageCount)
        .setPresentMode(presentMode);
    if (queueFamilyIndices.graphicsQueue.value() == queueFamilyIndices.presentQueue.value())
    {
        swapchainCreateInfo
            .setQueueFamilyIndexCount(1)
            .setQueueFamilyIndices(queueFamilyIndices.graphicsQueue.value())
            .setImageSharingMode(vk::SharingMode::eExclusive);
    }
    else
    {
        const std::array indices = {queueFamilyIndices.graphicsQueue.value(), queueFamilyIndices.presentQueue.value()};
        swapchainCreateInfo
            .setQueueFamilyIndexCount(2)
            .setQueueFamilyIndices(indices)
            .setImageSharingMode(vk::SharingMode::eConcurrent);
    }
    swapchain = device.createSwapchainKHR(swapchainCreateInfo);
}

void Swapchain::DestroyVkSwapChain()
{
    device.destroySwapchainKHR(swapchain);
}

void Swapchain::GetVkImages()
{
    images = device.getSwapchainImagesKHR(swapchain);
}

void Swapchain::CreateVkImageViews()
{
    imageViews.resize(images.size());
    for (int i = 0; i < imageViews.size(); i++)
    {
        vk::ComponentMapping ComponentMapping;
        vk::ImageSubresourceRange imageSubresourceRange;
        imageSubresourceRange
            .setLevelCount(1)
            .setBaseMipLevel(0)
            .setLayerCount(1)
            .setBaseArrayLayer(0)
            .setAspectMask(vk::ImageAspectFlagBits::eColor);
        vk::ImageViewCreateInfo imageViewCreateInfo;
        imageViewCreateInfo
            .setImage(images[i])
            .setViewType(vk::ImageViewType::e2D)
            .setComponents(ComponentMapping)
            .setFormat(imageFormat.format)
            .setSubresourceRange(imageSubresourceRange);
        imageViews[i] = device.createImageView(imageViewCreateInfo);
    }
}

void Swapchain::DestroyVkImageViews()
{
    for (auto &imageView : imageViews)
    {
        device.destroyImageView(imageView);
    }
}
