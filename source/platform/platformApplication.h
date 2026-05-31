#pragma once

#include <string>
#include <vector>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"

namespace VL
{

struct PlatformApplicationDesc
{
    std::string windowTitle = "VulkanRenderer";
    int windowWidth = 1280;
    int windowHeight = 720;
    bool hiddenWindow = true;
    bool resizableWindow = true;
};

// Owns SDL startup, Vulkan loader setup, the main window, and raw SDL event
// collection. It deliberately returns platform-neutral PlatformEvent values so
// EngineLoop does not branch on SDL_Event directly.
class PlatformApplication
{
public:
    PlatformApplication() = default;
    ~PlatformApplication();

    PlatformApplication(const PlatformApplication&) = delete;
    PlatformApplication& operator=(const PlatformApplication&) = delete;

    RuntimeResult<void> Initialize(const PlatformApplicationDesc& desc);
    void Shutdown();

    void PollEvents(std::vector<PlatformEvent>& outEvents);

    PlatformWindow& GetWindow() { return window; }
    const PlatformWindow& GetWindow() const { return window; }
    std::vector<const char*>& GetVulkanExtensions() { return vulkanExtensions; }

private:
    bool sdlInitialized = false;
    bool vulkanLoaderLoaded = false;
    PlatformWindow window;
    std::vector<const char*> vulkanExtensions;
};

} // namespace VL
