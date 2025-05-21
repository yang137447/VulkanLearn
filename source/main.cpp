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
    SDL_Init(SDL_INIT_EVERYTHING);

    SDL_Window *window = SDL_CreateWindow(
        "VulkanRenderer",
        width, height,
        SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS);

    if (!window)
    {
        SDL_Log("Create window failed");
        exit(2);
    }
    bool shouldClose = false;
    SDL_Event event;

    unsigned int count;
    SDL_Vulkan_GetInstanceExtensions(&count, nullptr);
    std::vector<const char *> extensions(count);
    SDL_Vulkan_GetInstanceExtensions(&count, extensions.data());

    // ShaderCompiler shaderCompiler;
    // std::string shaderFolderPath = filePath + "/shader";
    // std::cout << "shaderFolderPath: " << shaderFolderPath << std::endl;
    // shaderCompiler.StartCompile(shaderFolderPath);

    std::unique_ptr<VulkanManager> vulkanManager = std::make_unique<VulkanManager>(extensions, window);

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
    SDL_Quit();

    return 0;
}