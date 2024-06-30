#include "vulkan/vulkan.hpp"
#include <memory>
#include <optional>

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsQueue;
};

class RenderCore
{
public:
    ~RenderCore();

    static void Init();
    static void Quit();
    static RenderCore &GetInstance();

    void CreateVkInstace();
    void DestroyVkInstance();
    void PickupPhysicalDevice();
    void CreateVkDevice();
    void DestroyVkDevice();

    vk::Instance instance;
    vk::PhysicalDevice physicalDevice;
    vk::Device device;
    QueueFamilyIndices queueFamilyIndices;
    vk::Queue graphicQueue;

private:
    RenderCore();

    std::vector<const char *> instanceLayers = {
        "VK_LAYER_KHRONOS_validation"};
    const int GPUIndex = 0;

    inline static std::unique_ptr<RenderCore> _instance = nullptr;

    std::optional<uint32_t> QueryQueueFamilyIndices(vk::QueueFlagBits queryQueueflagbits=vk::QueueFlagBits::eGraphics);
};