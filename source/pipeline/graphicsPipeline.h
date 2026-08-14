#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include "vulkan/vulkan.hpp"
#include "pipelineBase.h"
#include "graphicsPipelineBuilder.h"
#include "graphicsPipelineLayoutDesc.h"
#include "graphicsShaderVariantArtifact.h"

struct ShaderBinding;
namespace VL
{
class RendererBackendVulkan;
}

class GraphicsPipeline : public PipelineBase
{
public:
    ~GraphicsPipeline();

    inline vk::PipelineBindPoint GetBindPoint() const override { return vk::PipelineBindPoint::eGraphics; }
    inline const vk::Pipeline& GetPipeline() const override { return graphicsPipeline; }
    inline const vk::PipelineLayout& GetPipelineLayout() const override { return pipelineLayout; }
    inline const std::vector<vk::DescriptorSetLayout>& GetDescriptorSetLayouts() const override { return descriptorSetLayouts; }
    inline const std::vector<ShaderBinding>& GetShaderBindings() const override { return shaderBindings; }
    inline const std::vector<ShaderBinding>& GetDescriptorLayoutBindings() const override { return descriptorLayoutBindings; }
private:
    friend class PipelineFactory;

    GraphicsPipeline(
        VL::RendererBackendVulkan* rendererBackend,
        vk::Device& device,
        vk::RenderPass* renderPass,
        const GraphicsShaderVariantArtifact& shaderArtifact,
        vk::SampleCountFlagBits sampleCount,
        uint32_t colorAttachmentCount,
        const GraphicsPipelineStateDesc& pipelineStateDesc = {},
        bool bIsShadowPass = false,
        const GraphicsPipelineLayoutDesc& pipelineLayoutDesc = {});

    void CreateDescriptorSetLayouts(const GraphicsPipelineLayoutDesc& pipelineLayoutDesc);
    void DestroyDescriptorSetLayouts();

    void CreatePipelineLayout();
    void DestroyPipelineLayout();

    void CreateShader();
    void DestroyShader();
    
    void initVertexAttribute();

    void CreateGraphicsPipeline(
        vk::RenderPass& renderPass,
        vk::SampleCountFlagBits sampleCount,
        uint32_t colorAttachmentCount,
        const GraphicsPipelineStateDesc& pipelineStateDesc,
        bool bIsShadowPass);
    void DestroyGraphicsPipeline();
private: 
    VL::RendererBackendVulkan* rendererBackend = nullptr;
    vk::Device* device;

    std::string shaderDisplayName;
    std::vector<uint32_t> vertexSpirv;
    std::vector<uint32_t> fragmentSpirv;

    std::vector<ShaderBinding> shaderBindings;
    std::vector<ShaderBinding> descriptorLayoutBindings;

    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    vk::PipelineLayout pipelineLayout;

    std::vector<vk::PipelineShaderStageCreateInfo> shaderStages;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
    
    vk::PipelineCache pipelineCache;
    vk::Pipeline graphicsPipeline;
};

