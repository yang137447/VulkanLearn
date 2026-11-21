#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "render/backend/frameAbandonVulkan.h"

namespace
{
#if defined(VL_FRAME_ABORT_SHORT)
constexpr uint32_t FrameCount = 3;
constexpr uint32_t ExpectedAbandons = 1;
constexpr uint32_t MinimumContinuations = 1;
#else
constexpr uint32_t FrameCount = 14;
constexpr uint32_t ExpectedAbandons = 2;
constexpr uint32_t MinimumContinuations = 4;
#endif

void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}

VKAPI_ATTR VkBool32 VKAPI_CALL Validation(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
        ++*static_cast<std::atomic<uint32_t>*>(user);
    std::cerr << "[VALIDATION] " << data->pMessage << '\n';
    return VK_FALSE;
}

class Harness
{
public:
    ~Harness() { Cleanup(); }
    void Initialize()
    {
        Require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        window = SDL_CreateWindow("framesubmitabort explicit validation", 320, 240, SDL_WINDOW_VULKAN);
        Require(window != nullptr, SDL_GetError());
        uint32_t extensionCount = 0;
        const auto extensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
        Require(extensions != nullptr, SDL_GetError());
        std::vector<const char*> names(extensions, extensions + extensionCount);
        names.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        names.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        const auto layers = vk::enumerateInstanceLayerProperties();
        Require(std::any_of(layers.begin(), layers.end(), [](const auto& layer) {
            return std::string(layer.layerName.data()) == "VK_LAYER_KHRONOS_validation";
        }), "Validation layer is required");
        const char* validationLayer = "VK_LAYER_KHRONOS_validation";
        vk::ApplicationInfo app("framesubmitabort", 1, "VL native recovery harness", 1, VK_API_VERSION_1_2);
        vk::InstanceCreateInfo create;
        const VkValidationFeatureEnableEXT syncValidation =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT features{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        features.enabledValidationFeatureCount = 1;
        features.pEnabledValidationFeatures = &syncValidation;
        create.setPApplicationInfo(&app).setPEnabledExtensionNames(names)
            .setEnabledLayerCount(1).setPpEnabledLayerNames(&validationLayer).setPNext(&features);
        instance = vk::createInstance(create);
        VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debug.pfnUserCallback = Validation;
        debug.pUserData = &validationErrors;
        const auto createDebug = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            instance.getProcAddr("vkCreateDebugUtilsMessengerEXT"));
        Require(createDebug && createDebug(instance, &debug, nullptr, &messenger) == VK_SUCCESS,
            "Could not create validation messenger");
        VkSurfaceKHR rawSurface{};
        Require(SDL_Vulkan_CreateSurface(window, instance, nullptr, &rawSurface), SDL_GetError());
        surface = rawSurface;
        for (const auto candidate : instance.enumeratePhysicalDevices())
        {
            const auto families = candidate.getQueueFamilyProperties();
            for (uint32_t index = 0; index < families.size(); ++index)
                if ((families[index].queueFlags & vk::QueueFlagBits::eGraphics) &&
                    candidate.getSurfaceSupportKHR(index, surface))
                {
                    physical = candidate;
                    family = index;
                    break;
                }
            if (physical) break;
        }
        Require(bool(physical), "No combined graphics/present queue");
        const float priority = 1;
        vk::DeviceQueueCreateInfo queueInfo({}, family, 1, &priority);
        const char* swapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        vk::DeviceCreateInfo deviceInfo;
        deviceInfo.setQueueCreateInfos(queueInfo).setEnabledExtensionCount(1)
            .setPpEnabledExtensionNames(&swapchainExtension);
        device = physical.createDevice(deviceInfo);
        queue = device.getQueue(family, 0);
        const auto caps = physical.getSurfaceCapabilitiesKHR(surface);
        Require(bool(caps.supportedUsageFlags & vk::ImageUsageFlagBits::eTransferDst),
            "Swapchain transfer-destination support is required for real continuation clears");
        const auto formats = physical.getSurfaceFormatsKHR(surface);
        Require(!formats.empty(), "No surface format");
        const auto format = formats.front();
        extent = caps.currentExtent;
        if (extent.width == UINT32_MAX)
            extent = vk::Extent2D(std::clamp(320u, caps.minImageExtent.width, caps.maxImageExtent.width),
                std::clamp(240u, caps.minImageExtent.height, caps.maxImageExtent.height));
        uint32_t count = std::max(2u, caps.minImageCount);
        if (caps.maxImageCount) count = std::min(count, caps.maxImageCount);
        vk::CompositeAlphaFlagBitsKHR alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        for (const auto candidate : {vk::CompositeAlphaFlagBitsKHR::eOpaque,
            vk::CompositeAlphaFlagBitsKHR::ePreMultiplied, vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
            vk::CompositeAlphaFlagBitsKHR::eInherit})
            if (caps.supportedCompositeAlpha & candidate) { alpha = candidate; break; }
        vk::SwapchainCreateInfoKHR swap;
        swap.setSurface(surface).setMinImageCount(count).setImageFormat(format.format)
            .setImageColorSpace(format.colorSpace).setImageExtent(extent).setImageArrayLayers(1)
            .setImageUsage(vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eColorAttachment)
            .setImageSharingMode(vk::SharingMode::eExclusive)
            .setPreTransform(caps.currentTransform).setCompositeAlpha(alpha)
            .setPresentMode(vk::PresentModeKHR::eFifo).setClipped(true);
        swapchain = device.createSwapchainKHR(swap);
        images = device.getSwapchainImagesKHR(swapchain);
        imageFormat = format.format;
        for (const auto image : images)
        {
            vk::ImageViewCreateInfo view;
            view.setImage(image).setViewType(vk::ImageViewType::e2D).setFormat(imageFormat)
                .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
            views.push_back(device.createImageView(view));
        }
        pool = device.createCommandPool({vk::CommandPoolCreateFlagBits::eResetCommandBuffer, family});
        command = device.allocateCommandBuffers({pool, vk::CommandBufferLevel::ePrimary, 1}).front();
        acquired = device.createSemaphore({});
        fence = device.createFence({vk::FenceCreateFlagBits::eSignaled});
        for (size_t index = 0; index < images.size(); ++index)
            finished.push_back(device.createSemaphore({}));
    }

    void Run()
    {
        for (uint32_t frame = 0; frame < FrameCount; ++frame)
        {
            Require(device.waitForFences(fence, true, 5000000000ull) == vk::Result::eSuccess,
                "Frame fence did not signal within 5 seconds");
            SDL_Event event;
            while (SDL_PollEvent(&event)) {}
            const auto next = device.acquireNextImageKHR(swapchain, 5000000000ull, acquired);
            Require(next.result == vk::Result::eSuccess || next.result == vk::Result::eSuboptimalKHR,
                "Harness needs a stable acquired swapchain");
            const auto imageIndex = next.value;
            command.reset();
            command.begin({vk::CommandBufferUsageFlagBits::eOneTimeSubmit});
            vk::ImageMemoryBarrier barrier;
            barrier.setOldLayout(vk::ImageLayout::eUndefined).setNewLayout(vk::ImageLayout::eTransferDstOptimal)
                .setSrcQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED).setDstQueueFamilyIndex(VK_QUEUE_FAMILY_IGNORED)
                .setDstAccessMask(vk::AccessFlagBits::eTransferWrite).setImage(images[imageIndex])
                .setSubresourceRange({vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1});
            command.pipelineBarrier(vk::PipelineStageFlagBits::eTopOfPipe,
                vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, barrier);
            command.clearColorImage(images[imageIndex], vk::ImageLayout::eTransferDstOptimal,
                vk::ClearColorValue(std::array<float, 4>{0.1f, 0.4f, 0.7f, 1.0f}), barrier.subresourceRange);
            VL::AcquiredFrameDisposition state;
            if ((FrameCount == 3 && frame == 1) || (FrameCount != 3 && (frame == 4 || frame == 9)))
            {
                command.reset();
                state.Discarded();
                const VL::FrameAbandonVulkanSync sync{
                    device, command, images[imageIndex], acquired, finished[imageIndex], fence,
                    views[imageIndex], imageFormat, extent};
                const auto result = VL::RecoverAcquiredFrame(sync, state,
                    [&](const vk::SubmitInfo& info, vk::Fence signal) { queue.submit(info, signal); },
                    [&](vk::Semaphore signal) { return Present(imageIndex, signal); });
                Require(result == VL::SwapchainFrameResult::Ready, "Unexpected recovery present change");
                ++abandonedFrames;
                std::cout << "{\"event\":\"recovery-submit-present\",\"frame\":" << frame
                    << ",\"discardedSceneSubmits\":0}\n";
            }
            else
            {
                barrier.setOldLayout(vk::ImageLayout::eTransferDstOptimal)
                    .setNewLayout(vk::ImageLayout::ePresentSrcKHR)
                    .setSrcAccessMask(vk::AccessFlagBits::eTransferWrite).setDstAccessMask({});
                command.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                    vk::PipelineStageFlagBits::eBottomOfPipe, {}, {}, {}, barrier);
                command.end();
                const vk::PipelineStageFlags wait = vk::PipelineStageFlagBits::eAllCommands;
                vk::SubmitInfo submit;
                submit.setWaitSemaphores(acquired).setWaitDstStageMask(wait)
                    .setCommandBuffers(command).setSignalSemaphores(finished[imageIndex]);
                device.resetFences(fence);
                queue.submit(submit, fence);
                state.Submitted();
                Require(Present(imageIndex, finished[imageIndex]) == VL::SwapchainFrameResult::Ready,
                    "Unexpected normal present change");
                std::cout << "{\"event\":\"normal-submit-present\",\"frame\":" << frame << "}\n";
                if (abandonedFrames) ++continuationFrames;
            }
        }
        Require(device.waitForFences(fence, true, 5000000000ull) == vk::Result::eSuccess,
            "Final continuation fence did not signal");
        std::cout << "{\"event\":\"final-continuation-fence-completed\",\"frame\":"
            << (FrameCount - 1) << "}\n";
    }

    VL::SwapchainFrameResult Present(uint32_t imageIndex, vk::Semaphore signal)
    {
        vk::PresentInfoKHR info;
        info.setSwapchains(swapchain).setImageIndices(imageIndex).setWaitSemaphores(signal);
        return VL::ClassifySwapchainFrameResult(queue.presentKHR(info));
    }

    void Cleanup() noexcept
    {
        if (device)
        {
            try { device.waitIdle(); } catch (...) {}
            for (const auto signal : finished) device.destroySemaphore(signal);
            if (acquired) device.destroySemaphore(acquired);
            if (fence) device.destroyFence(fence);
            if (pool) device.destroyCommandPool(pool);
            for (const auto view : views) device.destroyImageView(view);
            if (swapchain) device.destroySwapchainKHR(swapchain);
            device.destroy();
            device = nullptr;
        }
        if (instance)
        {
            if (surface) instance.destroySurfaceKHR(surface);
            if (messenger)
            {
                const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                    instance.getProcAddr("vkDestroyDebugUtilsMessengerEXT"));
                if (destroy) destroy(instance, messenger, nullptr);
            }
            instance.destroy();
            instance = nullptr;
        }
        if (window) { SDL_DestroyWindow(window); window = nullptr; }
        SDL_Quit();
    }
    std::atomic<uint32_t> validationErrors{0};
    uint32_t abandonedFrames = 0;
    uint32_t continuationFrames = 0;
private:
    SDL_Window* window = nullptr;
    vk::Instance instance;
    VkDebugUtilsMessengerEXT messenger{};
    vk::SurfaceKHR surface;
    vk::PhysicalDevice physical;
    uint32_t family = 0;
    vk::Device device;
    vk::Queue queue;
    vk::SwapchainKHR swapchain;
    vk::Extent2D extent;
    std::vector<vk::Image> images;
    std::vector<vk::ImageView> views;
    vk::Format imageFormat = vk::Format::eUndefined;
    vk::CommandPool pool;
    vk::CommandBuffer command;
    vk::Semaphore acquired;
    std::vector<vk::Semaphore> finished;
    vk::Fence fence;
};
}

int main(int argc, char** argv)
{
    if (argc != 2 || std::string(argv[1]) != "--execute")
    {
        std::cout << "GPU harness not executed. Use --execute only in main's allocated GPU window.\n";
        return 2;
    }
    try
    {
        Harness harness;
        harness.Initialize();
        harness.Run();
        harness.Cleanup();
        Require(harness.validationErrors == 0, "Vulkan validation errors observed");
        Require(harness.abandonedFrames == ExpectedAbandons &&
            harness.continuationFrames >= MinimumContinuations, "No real continuation");
        std::cout << "{\"scope\":\"Vulkan recovery only; no RVT World acceptance\","
            "\"abandonedFrames\":" << harness.abandonedFrames << ",\"continuationFrames\":"
            << harness.continuationFrames << ",\"validationErrors\":" << harness.validationErrors << "}\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
