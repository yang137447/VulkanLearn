#include "SDL3/SDL.h"

#include <iostream>
#include <vector>

int main(int argc, char **argv)
{
    SDL_Init(SDL_INIT_EVERYTHING);

    SDL_Window *window = SDL_CreateWindow(
        "VulkanRenderer",
        1024, 768,
        SDL_WINDOW_VULKAN | SDL_WINDOW_BORDERLESS);

    if(!window)
    {
        SDL_Log("Create window failed");
        exit(2);
    }
    bool shouldClose = false;
    SDL_Event event;

    while (!shouldClose)
    {
        while (SDL_PollEvent(&event))
        {
            if(event.type == SDL_EVENT_QUIT)
            {
                shouldClose =true;
            }
        }
    }

    SDL_DestroyWindow(window);
    SDL_Quit();

    return 0;
}