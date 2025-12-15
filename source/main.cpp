#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include <iostream>
#include <vector>
#include <chrono>
#include "shaderCompiler.h"
#include "vulkanManager.h"
#include "sceneLoader.h"
#include "renderSystem.h"
#include "commonFunction.h"
#include "fpsTool.h"
#include "controller.h"
#include "sceneObject.h"

int main(int argc, char **argv)
{
    // 做一些初始化设置
    if(!SDL_Init(SDL_INIT_VIDEO)){
        std::cout << "SDL_Init failed" << std::endl;
        exit(1);
    }
    if(!SDL_Vulkan_LoadLibrary(nullptr)){
        std::cout << "SDL_Vulkan_LoadLibrary failed" << std::endl;
        exit(1);
    }    
    CommonFunction::InitConfigJson();

    SDL_Window *window = SDL_CreateWindow(
        "VulkanRenderer",
        CommonFunction::GetWindowSize().x(), CommonFunction::GetWindowSize().y(),
        SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);

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

    //编译shader
    ShaderCompiler shaderCompiler;
    std::string shaderFolderPath = CommonFunction::Path("shader");
    std::cout << "shaderFolderPath: " << shaderFolderPath << std::endl;
    shaderCompiler.StartCompile(shaderFolderPath);

    //初始化VulkanManager
    VulkanManager& vulkanManager = VulkanManager::GetInstance();
    vulkanManager.Init(extensionsVec, window);
    //加载场景
    SceneLoader& sceneLoader = SceneLoader::GetInstance();
    sceneLoader.LoadScence(CommonFunction::Path(CommonFunction::GetInitScene()));
    //初始化渲染系统
    RenderSystem& renderSystem = RenderSystem::GetInstance();
    renderSystem.InitRenderObject();
    //初始化FPS计算工具
    FpsTool fpsTool;
    //玩家控制器
    Controller controller(window);
    controller.SetMoveVelocity(10.0f);
    controller.SetRotationSpeed(10.0f);
    auto camera = sceneLoader.GetCamera();
    controller.SetSceneObject(camera);

    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    while (!shouldClose)
    {
        //delta time
        float deltaTime = CommonFunction::GetDeltaTime();
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
                case SDL_EVENT_WINDOW_RESIZED:
                    std::cout << "Window resized to " << event.window.data1 << "x" << event.window.data2 << std::endl;
                    vulkanManager.ReCreateSwapChain(event.window.data1, event.window.data2);
                    break;
                case SDL_EVENT_QUIT:
                    std::cout << "Quit event received" << std::endl;
                    shouldClose = true;
                    break;
            }
        }
        controller.Update(deltaTime);
        renderSystem.Render();

        //FPS计算
        fpsTool.Calculate(deltaTime);
        SDL_SetWindowTitle(window, fpsTool.getTitle().c_str());
    }
    vulkanManager.GetDevice().waitIdle();
    SDL_DestroyWindow(window);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();

    return 0;
}