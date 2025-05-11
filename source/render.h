#pragma once

#include "vulkan/vulkan.hpp"
#include "BaseStructs.h"

class Swapchain;

class Render
{
public:
    Render(const vk::Device &device,
           const QueueFamilyIndices &queueFamilyIndices,
           Swapchain &swapchain,
           std::vector<vk::Framebuffer> &framebuffers,
           const vk::Pipeline &graphicsPipeline,
           const vk::RenderPass &renderPass,
           const vk::Queue &graphicsQueue,
           const vk::Queue &presentQueue);
    ~Render();
    void Draw();

private:
    Render();
    // input data
    const vk::Device *device;
    Swapchain *swapchain;
    std::vector<vk::Framebuffer> *framebuffers;
    const vk::Pipeline *graphicsPipeline;
    const vk::RenderPass *renderPass;
    const vk::Queue *graphicsQueue;
    const vk::Queue *presentQueue;
    // data
    vk::CommandPool commandPool;
    vk::CommandBuffer commandBuffer;

    vk::Fence drawFence;
    vk::Semaphore imageAvailableSemaphore;
    vk::Semaphore renderFinishedSemaphore;

    // functions
    void CreateCommandPool(const QueueFamilyIndices &queueFamilyIndices);
    void allocateCommandBuffer();
    uint32_t aquireNextImageIndex();
    void CreateFence();
    void CreateSemaphore();
};