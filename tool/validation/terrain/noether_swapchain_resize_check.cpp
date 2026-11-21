#include "render/backend/rendererBackendVulkan.h"
#include "render/frame/terrainCompositedLoading.h"
#include "render/frame/terrainCompositedFrameLifetime.h"
#include "render/rhi/vulkan/presentTarget.h"
#include "vulkanManager.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <vector>

namespace
{
using namespace VL;
using Json = nlohmann::json;
std::shared_ptr<const void> lateShutdownOwner;

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct IdleBeforeResources
{
    RendererBackendVulkan& backend;
    ~IdleBeforeResources()
    {
        try { backend.WaitIdle(); } catch (...) {}
    }
};

struct SeedPass
{
    RendererBackendVulkan* backend = nullptr;
    std::shared_ptr<const void> owner;
    vk::RenderPass pass;
    std::vector<vk::Framebuffer> framebuffers;
    ~SeedPass()
    {
        if (!backend)
            return;
        backend->DestroyFramebuffers(framebuffers);
        if (pass)
            backend->DestroyRenderPass(pass);
    }
};

Json Run(RendererBackendVulkan& backend, SDL_Window* window,
    const std::filesystem::path& output)
{
    TerrainRvtBudgetCaps caps;
    caps.workspaceGpuBytes = caps.totalGpuBytes = 32 * 1024 * 1024;
    caps.workspaceCpuBytes = caps.totalCpuBytes = 4 * 1024 * 1024;
    auto budget = std::make_shared<TerrainRvtBudgetOwner>(caps);
    auto loading = std::make_unique<TerrainCompositedLoading>(
        budget, TerrainRvtCost{128 * 1024, 32 * 1024, 0, 0});
    TerrainCompositedFrameLifetime frames;
    std::vector<std::shared_ptr<const void>> borrowedOldOwners;
    std::vector<std::weak_ptr<const void>> oldWeakOwners;
    std::vector<std::shared_ptr<SeedPass>> seedPasses;
    IdleBeforeResources idle{backend};
    Json report{{"scope", "Native swapchain old-owner resize validation; no terrain quality or P3 claim"},
        {"gpuExecuted", true}, {"validationRequested", true}, {"frames", Json::array()}};
    const std::array<std::array<int, 2>, 4> sizes{
        std::array<int, 2>{1280, 720}, {960, 540}, {800, 600}, {1280, 720}};
    for (uint32_t serial = 0; serial < sizes.size(); ++serial)
    {
        uint64_t oldGeneration = 0;
        if (serial)
        {
            auto oldOwner = backend.CapturePresentTargetLifetime();
            Require(bool(oldOwner), "Resize fixture did not capture the actual old present owner");
            oldGeneration = backend.GetPresentTargetGeneration();
            borrowedOldOwners.push_back(oldOwner);
            oldWeakOwners.push_back(oldOwner);
            Require(SDL_SetWindowSize(window, sizes[serial][0], sizes[serial][1]), SDL_GetError());
            SDL_PumpEvents();
            SDL_Delay(25);
            int width = 0, height = 0;
            Require(SDL_GetWindowSizeInPixels(window, &width, &height), SDL_GetError());
            backend.RecreateSwapchain(width, height);
            frames.Complete(uint64_t(serial));
            Require(backend.GetPresentTargetGeneration() > oldGeneration, "Resize generation did not advance");
            Require(!oldWeakOwners.back().expired(), "Old image views died while the fixture still borrowed them");
        }
        auto frame = backend.BeginFrame(serial);
        Require(frame.ready, "Acquire did not return a usable frame after resize");
        frames.Complete(frame.completedEpoch);
        frames.Prepare(frame.frameIndex);
        auto command = frame.commandBuffer;
        TerrainCompositedLoadingTarget black;
        black.view = backend.GetSwapchainImageViews().at(frame.swapchainImageIndex);
        black.format = backend.GetSwapchainImageFormat();
        black.extent = backend.GetSwapchainExtent();
        black.layout = vk::ImageLayout::ePresentSrcKHR;
        black.owner = backend.CapturePresentTargetLifetime();
        black.ownerGeneration = backend.GetPresentTargetGeneration();
        auto seed = std::make_shared<SeedPass>();
        seed->backend = &backend;
        seed->owner = black.owner;
        seedPasses.push_back(seed);
        vk::AttachmentDescription2 color;
        color.setFormat(black.format).setSamples(vk::SampleCountFlagBits::e1)
            .setLoadOp(vk::AttachmentLoadOp::eClear).setStoreOp(vk::AttachmentStoreOp::eStore)
            .setStencilLoadOp(vk::AttachmentLoadOp::eDontCare)
            .setStencilStoreOp(vk::AttachmentStoreOp::eDontCare)
            .setInitialLayout(vk::ImageLayout::eUndefined)
            .setFinalLayout(vk::ImageLayout::ePresentSrcKHR);
        vk::AttachmentReference2 reference;
        reference.setAttachment(0).setLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setAspectMask(vk::ImageAspectFlagBits::eColor);
        vk::SubpassDescription2 subpass;
        subpass.setPipelineBindPoint(vk::PipelineBindPoint::eGraphics).setColorAttachments(reference);
        vk::RenderPassCreateInfo2 create;
        create.setAttachments(color).setSubpasses(subpass);
        seed->pass = backend.CreateRenderPass(create, "ResizeOwnerWhiteSeed");
        vk::FramebufferCreateInfo framebuffer;
        framebuffer.setRenderPass(seed->pass).setAttachments(black.view)
            .setWidth(black.extent.width).setHeight(black.extent.height).setLayers(1);
        seed->framebuffers.push_back(backend.CreateFramebuffer(framebuffer, "ResizeOwnerWhiteSeed"));
        const vk::ClearValue white{vk::ClearColorValue{std::array<float, 4>{1, 1, 1, 1}}};
        vk::RenderPassBeginInfo begin;
        begin.setRenderPass(seed->pass).setFramebuffer(seed->framebuffers.front())
            .setRenderArea(vk::Rect2D{{0, 0}, black.extent}).setClearValues(white);
        command.beginRenderPass(begin, vk::SubpassContents::eInline);
        command.endRenderPass();
        auto lifetime = loading->Record(backend, command, black, uint64_t(serial) + 1, true, false);
        Require(bool(lifetime), "Loading clear did not retain its real framebuffer/target");
        const auto screenshot = output / ("black-" + std::to_string(serial) + ".bmp");
        std::string error;
        Require(backend.RecordSwapchainScreenshot(frame, screenshot.string(), error), error);
        backend.SubmitFrame(frame, serial);
        frames.Submitted(frame.frameIndex, uint64_t(serial) + 1, {}, std::move(lifetime));
        Require(backend.CompleteSwapchainScreenshot(error), error);
        frames.Complete(uint64_t(serial) + 1);
        report["frames"].push_back({{"serial", serial}, {"generation", black.ownerGeneration},
            {"oldGeneration", oldGeneration}, {"extent", {black.extent.width, black.extent.height}},
            {"screenshot", screenshot.generic_string()}, {"fenceCompleted", true}});
    }
    seedPasses.clear();
    borrowedOldOwners.clear();
    for (const auto& old : oldWeakOwners)
        Require(!old.expired(), "The loading cache did not exercise retained old swapchain ownership");
    loading.reset();
    for (const auto& old : oldWeakOwners)
        Require(old.expired(), "Old swapchain leaf leaked after the cache and completed frame refs dropped");
    report["oldOwnersReleasedAfterCacheReset"] = true;
    lateShutdownOwner = backend.CapturePresentTargetLifetime();
    report["finalOwnerHeldPastManagerShutdown"] = true;
    report["resizeCount"] = sizes.size() - 1;
    report["status"] = "passed-native-steps";
    return report;
}
}

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    const auto output = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(output);
    Json report;
    SDL_Window* window = nullptr;
    try
    {
        Require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError());
        window = SDL_CreateWindow("VL native swapchain owner resize validation", 1280, 720,
            SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
        Require(window != nullptr, SDL_GetError());
        uint32_t count = 0;
        const auto extensions = SDL_Vulkan_GetInstanceExtensions(&count);
        Require(extensions != nullptr, SDL_GetError());
        std::vector<const char*> instanceExtensions(extensions, extensions + count);
        VulkanManager::GetInstance().SetValidationEnabled(true);
        {
            RendererBackendVulkan backend;
            backend.Initialize(instanceExtensions, window, PresentModePreference::Immediate);
            report = Run(backend, window, output);
        }
    }
    catch (const std::exception& error)
    {
        report = {{"status", "failed"}, {"error", error.what()}, {"gpuExecuted", true}};
    }
    if (window)
        SDL_DestroyWindow(window);
    SDL_Quit();
    std::ofstream(output / "report.json") << report.dump(2);
    std::cout << report.dump(2) << std::endl;
    return report.value("status", "") == "passed-native-steps" ? 0 : 1;
}
