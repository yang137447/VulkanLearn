#pragma once

#include <vector>
#include "vulkan/vulkan.hpp"

class DrawableObject;
struct ShaderBinding;

class RenderPipline
{
public:
    
    RenderPipline(vk::Device *device,
                  vk::PhysicalDeviceMemoryProperties *gpuMemoryProperties,
                  vk::RenderPass *renderPass,
                  const std::string& shaderName,
                  vk::SampleCountFlagBits sampleCount);
    ~RenderPipline();

    inline const vk::PipelineLayout& GetPipelineLayout() const { return pipelineLayout; }
    inline const vk::Pipeline& GetGraphicsPipeline() const { return graphicsPipeline; }
    inline const vk::DescriptorSetLayout& GetDescriptorSetLayout() const { return descriptorSetLayout; }
    inline const std::vector<ShaderBinding>& GetShaderBindings() const { return shaderBindings; }
private:
    RenderPipline();

    void CreatePipelineLayout();
    void DestroyPipelineLayout();

    void CreateShader();
    void DestroyShader();
    
    void initVertexAttribute();

    void CreateGraphicsPipeline();
    void DestroyGraphicsPipeline();
private: 
    
    vk::Device* device;
    vk::RenderPass* renderPass;
    vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties;
    vk::SampleCountFlagBits sampleCount;

    std::string shaderName;

    std::vector<ShaderBinding> shaderBindings;

    vk::DescriptorSetLayout descriptorSetLayout;
    vk::PipelineLayout pipelineLayout;

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
    
    vk::PipelineCache pipelineCache;
    vk::Pipeline graphicsPipeline;
};