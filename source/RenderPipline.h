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
                    vk::SampleCountFlagBits sampleCount,
                    bool bIsPostProcess = false,
                    bool bIsShadowPass = false
                );
    ~RenderPipline();

    inline const vk::PipelineLayout& GetPipelineLayout() const { return pipelineLayout; }
    inline const vk::Pipeline& GetGraphicsPipeline() const { return graphicsPipeline; }
    inline const std::vector<vk::DescriptorSetLayout>& GetDescriptorSetLayouts() const { return descriptorSetLayouts; }
    inline const std::vector<ShaderBinding>& GetShaderBindings() const { return shaderBindings; }
private:
    RenderPipline();

    void CreateDescriptorSetLayouts();
    void DestroyDescriptorSetLayouts();

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
    bool bIsPostProcess;
    bool bIsShadowPass;

    std::string shaderName;

    std::vector<ShaderBinding> shaderBindings;

    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    vk::PipelineLayout pipelineLayout;

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
    
    vk::PipelineCache pipelineCache;
    vk::Pipeline graphicsPipeline;
};