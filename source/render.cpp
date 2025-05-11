#include "render.h"
#include <iostream>
#include "swapchain.h"

Render::Render()
{
}

Render::Render(const vk::Device &device,
               const QueueFamilyIndices &queueFamilyIndices,
               Swapchain &swapchain,
               std::vector<vk::Framebuffer> &framebuffers,
               const vk::Pipeline &graphicsPipeline,
               const vk::RenderPass &renderPass,
               const vk::Queue &graphicsQueue,
               const vk::Queue &presentQueue)
{
    this->device = &device;
    this->swapchain = &swapchain;
    this->framebuffers = &framebuffers;
    this->graphicsPipeline = &graphicsPipeline;
    this->renderPass = &renderPass;
    this->graphicsQueue = &graphicsQueue;
    this->presentQueue = &presentQueue;

    CreateCommandPool(queueFamilyIndices);
    allocateCommandBuffer();
}

Render::~Render()
{
    device->destroyFence(drawFence);
    device->freeCommandBuffers(commandPool, commandBuffer);
    device->destroyCommandPool(commandPool);
}

void Render::Draw()
{
    uint32_t nextImageIndex = aquireNextImageIndex();

    commandBuffer.reset();

    vk::CommandBufferBeginInfo commandBufferBeginInfo;
    commandBufferBeginInfo
        .setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);
    commandBuffer.begin(vk::CommandBufferBeginInfo());
    {
        vk::ClearValue clearColor = vk::ClearColorValue(std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f});
        vk::Rect2D renderArea;
        renderArea
            .setOffset(vk::Offset2D(0, 0))
            .setExtent(swapchain->GetExtent());
        vk::RenderPassBeginInfo renderPassBeginInfo;
        renderPassBeginInfo
            .setRenderPass(*renderPass)
            .setFramebuffer((*framebuffers)[nextImageIndex])
            .setRenderArea(renderArea)
            .setClearValueCount(1)
            .setPClearValues(&clearColor);
        commandBuffer.beginRenderPass(renderPassBeginInfo, vk::SubpassContents::eInline);
        {
            commandBuffer.bindPipeline(vk::PipelineBindPoint::eGraphics, *graphicsPipeline);
            commandBuffer.draw(3, 1, 0, 0);
        }
        commandBuffer.endRenderPass();
    }
    commandBuffer.end();

    vk::SubmitInfo submitInfo;
    submitInfo
        .setCommandBufferCount(1)
        .setPCommandBuffers(&commandBuffer)
        .setSignalSemaphoreCount(1)
        .setPSignalSemaphores(&renderFinishedSemaphore)
        .setWaitSemaphoreCount(1)
        .setPWaitSemaphores(&imageAvailableSemaphore);
    graphicsQueue->submit(submitInfo, drawFence);

    vk::Result result = device->waitForFences(drawFence, VK_TRUE, UINT64_MAX);
    if (result != vk::Result::eSuccess)
    {
        std::cout << "Info : "
                  << "Failed to wait for fence"
                  << std::endl;
    }

    vk::PresentInfoKHR presentInfo;
    presentInfo
        .setWaitSemaphoreCount(1)
        .setPWaitSemaphores(&renderFinishedSemaphore)
        .setSwapchainCount(1)
        .setSwapchains(swapchain->GetSwapchain());
    vk::Result presentResult = presentQueue->presentKHR(presentInfo);
    if (presentResult != vk::Result::eSuccess)
    {
        std::cout << "Info : "
                  << "Failed to present image"
                  << std::endl;
    }

    device->resetFences(drawFence);
}

void Render::CreateCommandPool(const QueueFamilyIndices &queueFamilyIndices)
{
    vk::CommandPoolCreateInfo commandPoolCreateInfo;
    commandPoolCreateInfo
        .setFlags(vk::CommandPoolCreateFlagBits::eResetCommandBuffer)
        .setQueueFamilyIndex(queueFamilyIndices.graphicsQueue.value());

    commandPool = device->createCommandPool(commandPoolCreateInfo);
}

void Render::allocateCommandBuffer()
{
    vk::CommandBufferAllocateInfo commandBufferAllocateInfo;
    commandBufferAllocateInfo
        .setCommandPool(commandPool)
        .setCommandBufferCount(1)
        .setLevel(vk::CommandBufferLevel::ePrimary);
    commandBuffer = device->allocateCommandBuffers(commandBufferAllocateInfo)[0];
}

uint32_t Render::aquireNextImageIndex()
{
    vk::ResultValue<uint32_t> resultValue = device->acquireNextImageKHR(
        swapchain->GetSwapchain(), UINT64_MAX, imageAvailableSemaphore);
    if (resultValue.result != vk::Result::eSuccess)
    {
        std::cout << "Info : "
                  << "Failed to acquire image"
                  << std::endl;
    }
    return resultValue.value;
}

void Render::CreateFence()
{
    vk::FenceCreateInfo fenceCreateInfo;
    fenceCreateInfo
        .setFlags(vk::FenceCreateFlagBits::eSignaled);
    drawFence = device->createFence(fenceCreateInfo);
}

void Render::CreateSemaphore()
{
    vk::SemaphoreCreateInfo semaphoreCreateInfo;
    imageAvailableSemaphore = device->createSemaphore(semaphoreCreateInfo);
    renderFinishedSemaphore = device->createSemaphore(semaphoreCreateInfo);
}
