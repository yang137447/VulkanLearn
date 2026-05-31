#include "platform/platformApplication.h"

#include <iostream>

#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"

namespace VL
{

namespace
{

PlatformKey TranslateKey(SDL_Scancode scancode)
{
    if (scancode == SDL_SCANCODE_ESCAPE)
    {
        return PlatformKey::Escape;
    }

    return PlatformKey::Unknown;
}

} // namespace

PlatformApplication::~PlatformApplication()
{
    Shutdown();
}

RuntimeResult<void> PlatformApplication::Initialize(const PlatformApplicationDesc& desc)
{
    if (desc.windowWidth <= 0 || desc.windowHeight <= 0)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.InvalidWindowSize",
            "PlatformApplication requires a positive window size."));
    }

    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.SdlInitFailed",
            SDL_GetError()));
    }
    sdlInitialized = true;

    if (!SDL_Vulkan_LoadLibrary(nullptr))
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.VulkanLoaderFailed",
            SDL_GetError()));
    }
    vulkanLoaderLoaded = true;

    auto windowResult = window.Create(
        desc.windowTitle.c_str(),
        desc.windowWidth,
        desc.windowHeight,
        desc.hiddenWindow,
        desc.resizableWindow);
    if (windowResult.IsFailure())
    {
        return windowResult;
    }

    unsigned int count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (extensions == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "PlatformApplication.VulkanExtensionsFailed",
            SDL_GetError()));
    }

    vulkanExtensions.clear();
    for (unsigned int i = 0; i < count; ++i)
    {
        vulkanExtensions.push_back(extensions[i]);
        std::cout << "Vulkan extension: " << extensions[i] << std::endl;
    }

    return RuntimeResult<void>::Success();
}

void PlatformApplication::Shutdown()
{
    vulkanExtensions.clear();
    window.Destroy();

    if (vulkanLoaderLoaded)
    {
        SDL_Vulkan_UnloadLibrary();
        vulkanLoaderLoaded = false;
    }

    if (sdlInitialized)
    {
        SDL_Quit();
        sdlInitialized = false;
    }
}

void PlatformApplication::PollEvents(std::vector<PlatformEvent>& outEvents)
{
    outEvents.clear();

    SDL_Event event;
    while (SDL_PollEvent(&event))
    {
        if (event.type == SDL_EVENT_KEY_DOWN)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::KeyDown;
            platformEvent.key = TranslateKey(event.key.scancode);
            platformEvent.repeat = event.key.repeat;
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_RESIZED)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::WindowResized;
            platformEvent.width = static_cast<uint32_t>(event.window.data1);
            platformEvent.height = static_cast<uint32_t>(event.window.data2);
            outEvents.push_back(platformEvent);
            continue;
        }

        if (event.type == SDL_EVENT_QUIT)
        {
            PlatformEvent platformEvent;
            platformEvent.type = PlatformEventType::Quit;
            outEvents.push_back(platformEvent);
        }
    }
}

} // namespace VL
