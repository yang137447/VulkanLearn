#pragma once

#include "vulkan/vulkan.hpp"
#include <vector>

class RenderPipline
{
public:
    
    RenderPipline(vk::Device &device,
                  vk::RenderPass &renderPass,
                  vk::PhysicalDeviceMemoryProperties &gpuMemoryProperties);
    ~RenderPipline();

    inline vk::PipelineLayout& GetPipelineLayout() { return pipelineLayout; }
    inline vk::Pipeline& GetGraphicsPipeline() { return graphicsPipeline; }
    inline std::vector<vk::DescriptorSet>& GetDescriptorSet() { return descriptorSets; }
    inline vk::DeviceMemory& GetuniformBufferMemory() { return uniformBufferMemory; }
    inline uint32_t GetuniformBufferSize() { return uniformBufferSize; }
    inline vk::DescriptorBufferInfo& GetuniformBufferInfo() { return uniformBufferInfo; }
    inline std::vector<vk::WriteDescriptorSet>& GetWriteDescriptorSets() { return writeDescriptorSets; }
private:
    RenderPipline();

    void CreateUniformBuffer();
    void DestroyUniformBuffer();

    void CreatePipelineLayout();
    void DestroyPipelineLayout();

    void CreateDescriptorSets();
    void DestroyDescriptorSets();

    void CreateShader();
    void DestroyShader();
    
    void initVertexAttribute();

    void CreateGraphicsPipeline();
    void DestroyGraphicsPipeline();
private: 
    
    vk::Device* device;
    vk::RenderPass* renderPass;
    vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties;

    vk::DeviceMemory uniformBufferMemory;
    uint32_t uniformBufferSize;
    vk::Buffer uniformBuffer;
    vk::DescriptorBufferInfo uniformBufferInfo;

    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    vk::PipelineLayout pipelineLayout;
    std::vector<vk::WriteDescriptorSet> writeDescriptorSets;

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
    
    std::vector<vk::DescriptorSet> descriptorSets;
    vk::DescriptorPool descriptorPool;
    vk::PipelineCache pipelineCache;
    vk::Pipeline graphicsPipeline;
};