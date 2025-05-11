#pragma once
#include "vulkan/vulkan.hpp"

class ShaderModule;
class Swapchain;

class RenderProcess
{
public:
    RenderProcess(const vk::Device &device,
                  const vk::RenderPass &renderPass,
                  const ShaderModule &shaderModule,
                  const Swapchain &swapchain);
                  
    ~RenderProcess();

    inline vk::Pipeline GetGraphicsPipeline() { return graphicsPipeline; }

private:
    RenderProcess();
    // input data
    const vk::Device *device;
    const vk::RenderPass *renderPass;
    const ShaderModule *shaderModule;
    const Swapchain *swapchain;
    // data
    vk::Pipeline graphicsPipeline;
    vk::PipelineLayout pipelineLayout;

    // functions
    void CreateVkGraphicsPipeline();
    void DestroyVkGraphicsPipeline();
    void CreateVkPipelineLayout();
    void DestroyVkPipelineLayout();
};