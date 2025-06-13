#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include <iostream>
#include <vector>
#include "renderCore.h"
#include "shaderCompiler.h"
#include "settings.h"
#include "VulkanManager.h"

int main(int argc, char **argv)
{
    if(!SDL_Init(SDL_INIT_VIDEO)){
        std::cout << "SDL_Init failed" << std::endl;
        exit(1);
    }
    if(!SDL_Vulkan_LoadLibrary(nullptr)){
        std::cout << "SDL_Vulkan_LoadLibrary failed" << std::endl;
        exit(1);
    }
    SDL_Window *window = SDL_CreateWindow(
        "VulkanRenderer",
        width, height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN );

    if (!window)
    {
        SDL_Log("Create window failed");
        exit(1);
    }
    bool shouldClose = false;
    SDL_Event event;

    unsigned int count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!extensions) {
        std::cout << "Failed to get Vulkan instance extension count" << std::endl;
        exit(1);
    }
    std::vector<const char *> extensionsVec;
    for (unsigned int i = 0; i < count; i++)
    {
        extensionsVec.push_back(extensions[i]);
        std::cout << "Vulkan extension: " << extensions[i] << std::endl;
    }
    // ShaderCompiler shaderCompiler;
    // std::string shaderFolderPath = filePath + "/shader";
    // std::cout << "shaderFolderPath: " << shaderFolderPath << std::endl;
    // shaderCompiler.StartCompile(shaderFolderPath);

    std::unique_ptr<VulkanManager> vulkanManager = std::make_unique<VulkanManager>(extensionsVec, window);

    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);
    while (!shouldClose)
    {
        while (SDL_PollEvent(&event))
        {
            if (event.type == SDL_EVENT_QUIT)
            {
                shouldClose = true;
            }
        }
        vulkanManager->DrawFrame();
    }

    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();

    return 0;
}