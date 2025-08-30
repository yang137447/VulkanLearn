#pragma once

#include "vulkan/vulkan.hpp"
#include <vector>

class DrawableObject;

class RenderPipline
{
public:
    
    RenderPipline(vk::Device &device,
                  vk::RenderPass &renderPass,
                  vk::PhysicalDeviceMemoryProperties &gpuMemoryProperties,
                  const DrawableObject& drawableObject);
    ~RenderPipline();

    inline vk::PipelineLayout& GetPipelineLayout() { return pipelineLayout; }
    inline vk::Pipeline& GetGraphicsPipeline() { return graphicsPipeline; }
    inline std::vector<vk::DescriptorSet>& GetDescriptorSets() { return descriptorSets; }
    inline std::vector<vk::DeviceMemory>& GetUniformBufferMemories() { return uniformBufferMemories; }
    inline uint32_t GetUniformBufferSize() { return uniformBufferSize; }
    inline std::vector<vk::DescriptorBufferInfo>& GetUniformBufferInfos() { return uniformBufferInfos; }
    inline void* GetUniformBuffersMapped(uint32_t currentFrame) { return uniformBuffersMapped[currentFrame]; }
    inline std::vector<vk::WriteDescriptorSet>& GetWriteDescriptorSet() { return writeDescriptorSet; }
private:
    RenderPipline();

    void CreateUniformBuffers();
    void DestroyUniformBuffers();

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
    vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties;
    const DrawableObject* drawableObject;

    uint32_t uniformBufferSize;
    std::vector<vk::Buffer> uniformBuffers;
    std::vector<vk::DeviceMemory> uniformBufferMemories;
    std::vector<void*> uniformBuffersMapped;
    std::vector<vk::DescriptorBufferInfo> uniformBufferInfos;
    std::vector<vk::DescriptorImageInfo> ImageInfos;

    vk::DescriptorSetLayout descriptorSetLayout;
    vk::PipelineLayout pipelineLayout;
    std::vector<vk::WriteDescriptorSet> writeDescriptorSet;

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
    
    std::vector<vk::DescriptorSet> descriptorSets;
    vk::DescriptorPool descriptorPool;
    vk::PipelineCache pipelineCache;
    vk::Pipeline graphicsPipeline;
};