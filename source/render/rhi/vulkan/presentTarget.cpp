#include "render/rhi/vulkan/presentTarget.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace VL
{
namespace
{
struct PresentResourceRegistry
{
    std::mutex mutex;
    std::vector<PresentSwapchainResources*> resources;
};

PresentResourceRegistry& Registry()
{
    static auto* registry = new PresentResourceRegistry;
    return *registry;
}
}

PresentSwapchainResources::PresentSwapchainResources(vk::Device device)
    : device(device)
{
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    registry.resources.push_back(this);
}

PresentSwapchainResources::~PresentSwapchainResources()
{
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    ReleaseNativeResources();
    registry.resources.erase(std::remove(registry.resources.begin(), registry.resources.end(), this),
        registry.resources.end());
}

std::shared_ptr<PresentSwapchainResources> PresentSwapchainResources::Create(
    vk::Device device, vk::SwapchainCreateInfoKHR createInfo,
    std::shared_ptr<const PresentSwapchainResources> oldOwner)
{
    if (!device || (oldOwner && (oldOwner->device != device || !oldOwner->swapchain)))
        throw std::invalid_argument("Swapchain replacement requires the live old leaf on the same device");
    createInfo.setOldSwapchain(oldOwner ? oldOwner->swapchain : vk::SwapchainKHR{});
    auto owner = std::make_shared<PresentSwapchainResources>(device);
    owner->swapchain = device.createSwapchainKHR(createInfo);
    return owner;
}

void PresentSwapchainResources::ReleaseNativeResources()
{
    if (!device)
        return;
    for (const auto view : imageViews)
        if (view)
            device.destroyImageView(view);
    if (swapchain)
        device.destroySwapchainKHR(swapchain);
    imageViews.clear();
    swapchain = nullptr;
    device = nullptr;
}

void ReleasePresentSwapchainsForDevice(vk::Device device)
{
    auto& registry = Registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (auto* owner : registry.resources)
        if (owner->device == device)
            owner->ReleaseNativeResources();
}

void PresentTarget::PublishSwapchain(std::shared_ptr<PresentSwapchainResources> owner,
    std::vector<vk::Image> swapchainImages)
{
    if (swapchainOwner || swapChain || swapchainGeneration == UINT64_MAX ||
        !owner || !owner->device || !owner->swapchain || swapchainImages.empty() ||
        owner->imageViews.size() != swapchainImages.size() ||
        swapchainImages.size() > UINT32_MAX ||
        std::any_of(owner->imageViews.begin(), owner->imageViews.end(),
            [](auto view) { return !view; }) ||
        std::any_of(swapchainImages.begin(), swapchainImages.end(),
            [](auto image) { return !image; }))
        throw std::invalid_argument("Present target needs a complete owned swapchain leaf");
    auto borrowedViews = owner->imageViews;
    images = std::move(swapchainImages);
    imageViews = std::move(borrowedViews);
    imageCount = static_cast<uint32_t>(images.size());
    swapChain = owner->swapchain;
    swapchainOwner = std::move(owner);
    ++swapchainGeneration;
}

std::shared_ptr<PresentSwapchainResources> PresentTarget::DetachSwapchainForReplacement()
{
    auto owner = std::move(swapchainOwner);
    imageViews.clear();
    images.clear();
    swapChain = nullptr;
    imageCount = 0;
    return owner;
}

void PresentTarget::DetachSwapchain()
{
    auto owner = DetachSwapchainForReplacement();
}
}
