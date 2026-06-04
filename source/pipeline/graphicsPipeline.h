#pragma once

#include <cstdint>
#include <vector>
#include "vulkan/vulkan.hpp"
#include "pipelineBase.h"
#include "graphicsPipelineBuilder.h"
#include "../shaderVariant.h"

struct ShaderBinding;

class GraphicsPipeline : public PipelineBase
{
public:
    
    GraphicsPipeline(vk::Device *device,
                    vk::RenderPass *renderPass,
                    const ShaderVariantKey& shaderVariantKey,
                    vk::SampleCountFlagBits sampleCount,
                    uint32_t colorAttachmentCount,
                    const GraphicsPipelineStateDesc& pipelineStateDesc = {},
                    bool bIsShadowPass = false
                );
    ~GraphicsPipeline();

    inline vk::PipelineBindPoint GetBindPoint() const override { return vk::PipelineBindPoint::eGraphics; }
    inline const vk::Pipeline& GetPipeline() const override { return graphicsPipeline; }
    inline const vk::PipelineLayout& GetPipelineLayout() const override { return pipelineLayout; }
    inline const std::vector<vk::DescriptorSetLayout>& GetDescriptorSetLayouts() const override { return descriptorSetLayouts; }
    inline const std::vector<ShaderBinding>& GetShaderBindings() const override { return shaderBindings; }
private:
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
    vk::SampleCountFlagBits sampleCount;
    uint32_t colorAttachmentCount = 1;
    GraphicsPipelineStateDesc pipelineStateDesc;
    bool bIsShadowPass;

    ShaderVariantKey shaderVariantKey;

    std::vector<ShaderBinding> shaderBindings;

    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    vk::PipelineLayout pipelineLayout;

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
    
    vk::PipelineCache pipelineCache;
    vk::Pipeline graphicsPipeline;
};

