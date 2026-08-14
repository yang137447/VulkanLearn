#pragma once

#include <string>
#include <vector>
#include "vulkan/vulkan.hpp"
#include "pipelineBase.h"
#include "shader/reload/computeShaderArtifact.h"

struct ShaderBinding;
namespace VL
{
class RendererBackendVulkan;
}

class ComputePipeline : public PipelineBase
{
public:
    ~ComputePipeline();

    inline vk::PipelineBindPoint GetBindPoint() const override { return vk::PipelineBindPoint::eCompute; }
    inline const vk::Pipeline& GetPipeline() const override { return computePipeline; }
    inline const vk::PipelineLayout& GetPipelineLayout() const override { return pipelineLayout; }
    inline const std::vector<vk::DescriptorSetLayout>& GetDescriptorSetLayouts() const override { return descriptorSetLayouts; }
    inline const std::vector<ShaderBinding>& GetShaderBindings() const override { return shaderBindings; }
    inline const std::vector<ShaderBinding>& GetDescriptorLayoutBindings() const override { return shaderBindings; }

    void Bind(vk::CommandBuffer commandBuffer) const;
    void Dispatch(vk::CommandBuffer commandBuffer, uint32_t groupX, uint32_t groupY, uint32_t groupZ) const;
private:
    friend class PipelineFactory;

    ComputePipeline(
        VL::RendererBackendVulkan* rendererBackend,
        vk::Device& device,
        const std::string& shaderName);
    ComputePipeline(
        VL::RendererBackendVulkan* rendererBackend,
        vk::Device& device,
        const ComputeShaderArtifact& artifact);

    void CreateShader(const std::vector<uint32_t>& spirv);
    void DestroyShader();
    void CreateDescriptorSetLayouts();
    void DestroyDescriptorSetLayouts();
    void CreatePipelineLayout();
    void DestroyPipelineLayout();
    void CreateComputePipeline();
    void DestroyComputePipeline();

    VL::RendererBackendVulkan* rendererBackend = nullptr;
    vk::Device* device = nullptr;
    std::string shaderName;

    vk::ShaderModule shaderModule;
    std::vector<ShaderBinding> shaderBindings;
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    vk::PipelineLayout pipelineLayout;
    vk::PipelineCache pipelineCache;
    vk::Pipeline computePipeline;
};
