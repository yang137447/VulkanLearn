#include "vulkan/vulkan.hpp"
#include <memory>
#include <optional>
#include <vector>
#include "SDL3/SDL.h"

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsQueue;
    std::optional<uint32_t> presentQueue;
};

class RenderCore
{
public:
    ~RenderCore();

    static void Init(std::vector<const char *> &extensions, SDL_Window *window);
    static void Quit();
    static RenderCore &GetInstance();

    void CreateVkInstace();
    void DestroyVkInstance();
    void PickupPhysicalDevice();
    void CreateVkSurface();
    void CreateVkDevice();
    void DestroyVkDevice();

    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::SurfaceKHR surface;
    vk::Device device;
    QueueFamilyIndices queueFamilyIndices;
    vk::Queue graphicQueue;

private:
    RenderCore();
    // data
    std::vector<const char *> instanceLayers = {
        "VK_LAYER_KHRONOS_validation"};
    const int GPUIndex = 0;
    inline static std::vector<const char *> sdlExtensoins = {};

    inline static std::unique_ptr<RenderCore> _instance = nullptr;

    inline static SDL_Window *sdlWindow = nullptr;

    // function
    std::optional<uint32_t> QueryQueueFamilyIndices(vk::QueueFlagBits queryQueueflagbits = vk::QueueFlagBits::eGraphics);
};