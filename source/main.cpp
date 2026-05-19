#include "SDL3/SDL.h"
#include "SDL3/SDL_vulkan.h"
#include <iostream>
#include <vector>
#include "shaderCompiler.h"
#include "vulkanManager.h"
#include "sceneLoader.h"
#include "renderSystem.h"
#include "commonFunction.h"
#include "fpsTool.h"
#include "controller.h"
#include "sceneObject.h"
#include "renderGraph.h"
#include "pipeline/pipelineFactory.h"
#include "profiler.h"
#include "debugConsole.h"

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
    PipelineFactory pipelineFactory(&vulkanManager.GetDevice());
    SceneLoader& sceneLoader = SceneLoader::GetInstance();
    sceneLoader.SetPipelineFactory(&pipelineFactory);
    //加载渲染图
    RenderGraph& renderGraph = RenderGraph::GetInstance();
    renderGraph.LoadRenderGraph(CommonFunction::InitRenderGraphJson());
    //加载场景
    sceneLoader.LoadScene(CommonFunction::Path(CommonFunction::GetInitScene()));
    //初始化渲染系统
    RenderSystem& renderSystem = RenderSystem::GetInstance();
    renderSystem.InitRenderObject();
    //初始化调试控制台
    DebugConsole debugConsole(renderSystem);
    debugConsole.Initialize();
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
    controller.SetMouseCaptured(true);

    while (!shouldClose)
    {
        PROFILE_SCOPE("Frame");
        //更新调试控制台
        debugConsole.Update();

        //delta time
        float deltaTime = CommonFunction::GetDeltaTime();
        {
            PROFILE_SCOPE("Events");
            while (SDL_PollEvent(&event))
            {
                switch (event.type)
                {
                    case SDL_EVENT_KEY_DOWN:
                        if (!event.key.repeat && event.key.scancode == SDL_SCANCODE_ESCAPE)
                        {
                            controller.ToggleMouseCaptured();
                            std::cout << "Mouse capture "
                                      << (controller.IsMouseCaptured() ? "enabled" : "disabled")
                                      << " (press Esc to toggle)" << std::endl;
                        }
                        break;
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
        }
        {
            PROFILE_SCOPE("Update");
            controller.Update(deltaTime);
        }
        {
            PROFILE_SCOPE("RenderLoop");
            renderSystem.Render();
        }

        //FPS计算
        {
            PROFILE_SCOPE("FPS");
            fpsTool.Calculate(deltaTime);
            SDL_SetWindowTitle(window, fpsTool.getTitle().c_str());
        }
        PROFILE_FRAME();
    }
    // Wait for device idle before cleanup
    VulkanManager::GetInstance().GetDevice().waitIdle();
    Profiler::Instance().EndSession();

    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
