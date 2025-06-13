#pragma once

#include <vulkan/vulkan.hpp>
#include <SDL3/SDL.h>
#include <vector>
#include <optional>
#include <stack>

#include <Eigen/Dense>

class DrawableObject;
class RenderPipline;

class VulkanManager
{
public:

    VulkanManager(std::vector<const char *> &extensions, SDL_Window *window);
    ~VulkanManager();

    void DrawFrame();

private:
    VulkanManager();

    void CreateVkInstance();
    void DestroyVkInstance();

    void EnumeratePhysicalDevices();

    void CreateVkSurface();
    void DestroyVkSurface();

    void CreateVkDevice();
    void DestroyVkDevice();

    void CreateVkCommandBuffer();
    void DestroyVkCommandBuffer();

    void CreateVkSwapChain();
    void DestroyVkSwapChain();

    void CreateVkDepthBuffer();
    void DestroyVkDepthBuffer();

    void CreateVkRenderPass();
    void DestroyVkRenderPass();

    void CreateVkFrameBuffers();
    void DestroyVkFrameBuffers();

    void CreateDrawableObject();
    void DestroyDrawableObject();

    void CreateVkPipline();
    void DestroyVkPipline();

    void CreateVkFence();
    void DestroyVkFence();

    void InitializePresentInfo();

    void InitializeMVP();

    void FlushUniformBuffer();

    void FlushTextureToDescriptorSet();

private:
    vk::Instance instance;

    std::vector<const char *> instanceLayers = {
            "VK_LAYER_KHRONOS_validation"};
    std::vector<const char *> instanceExtensions = {};
    std::vector<const char *> deviceExtensions = {
            VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    
    const int GPUIndex = 0;
    SDL_Window *sdlWindow = nullptr;
    
    uint32_t gpuCount = 0;
    std::vector<vk::PhysicalDevice> physicalDevices;
    vk::PhysicalDeviceMemoryProperties gpuMemoryProperties;
    uint32_t queueFamilyCount = 0;
    std::vector<vk::QueueFamilyProperties> queueFamilyProperties;
    std::optional<uint32_t> graphicQueueFamilyIndex;
    vk::Queue graphicQueue;
    std::optional<uint32_t> presentQueueFamilyIndex;
    vk::Queue presentQueue;

    vk::Device device;

    vk::CommandPool commandPool;
    vk::CommandBuffer commandBuffer;
    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    vk::CommandBuffer commandBuffers[1];
    std::array<vk::SubmitInfo, 1> submitInfo;

    VkSurfaceKHR surface;
    std::vector<vk::SurfaceFormatKHR> surfaceFormats;
    vk::SurfaceCapabilitiesKHR surfaceCapabilities;
    std::vector<vk::PresentModeKHR> presentModes;
    vk::Extent2D swapChainExtent;
    uint32_t swapChainImageCount = 0;
    vk::SwapchainKHR swapChain;
    std::vector<vk::Image> swapChainImages;
    std::vector<vk::ImageView> swapChainImageViews;

    vk::Format depthFormat;
    vk::FormatProperties depthFormatProperties;
    vk::Image depthImage;
    vk::PhysicalDeviceMemoryProperties depthImageMemoryProperties;
    vk::DeviceMemory depthImageMemory;
    vk::ImageView depthImageView;

    vk::Semaphore imageAcquiredSemaphore;
    uint32_t swapchainImageIndex = 0;
    vk::RenderPass renderPass;
    vk::ClearValue clearValue;
    vk::RenderPassBeginInfo renderPassBeginInfo;

    std::vector<vk::Framebuffer> framebuffers;

    DrawableObject* triangleObject;

    RenderPipline* renderPipline;

    vk::Fence taskFinishedFence;
    
    vk::PresentInfoKHR presentInfo;

//Matrixs
private:
    void InitMatrix();
    void SetTranslation(float x, float y, float z);
    void SetRotation(float x, float y, float z);
    void SetScale(float x, float y, float z);
    void SetCamera(Eigen::Vector3f position, Eigen::Vector3f lookAt, Eigen::Vector3f up);
    void SetProjection(float fov, float aspect, float near, float far);
    
    void GetMVPMatrix();
    void GetFinalMatrix();

    Eigen::Transform<float, 3, Eigen::Affine> modelTransform;

    Eigen::Matrix4f modelMatrix;
    Eigen::Matrix4f viewMatrix;
    Eigen::Matrix4f projectionMatrix;
    Eigen::Matrix4f mvpMatrix;
    Eigen::Matrix4f vulkanClipMatrix;
    Eigen::Matrix4f currentMatrix;

    std::stack<Eigen::Matrix4f> matrixStack;

};
