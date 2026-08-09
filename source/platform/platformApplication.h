#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/runtimeResult.h"
#include "platform/platformEvent.h"
#include "platform/platformWindow.h"

struct SDL_Gamepad;

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
    bool OpenGamepad(uint32_t instanceId);
    bool CloseGamepad(uint32_t instanceId);
    void CloseAllGamepads();

    bool sdlInitialized = false;
    bool vulkanLoaderLoaded = false;
    PlatformWindow window;
    std::vector<const char*> vulkanExtensions;
    std::unordered_map<uint32_t, SDL_Gamepad*> gamepads;
};

} // namespace VL
