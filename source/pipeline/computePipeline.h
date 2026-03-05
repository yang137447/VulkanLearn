#pragma once

#include <string>
#include <vector>
#include "vulkan/vulkan.hpp"
#include "pipelineBase.h"

struct ShaderBinding;

class ComputePipeline : public PipelineBase
{
public:
    ComputePipeline(vk::Device* device, const std::string& shaderName);
    ~ComputePipeline();

    inline vk::PipelineBindPoint GetBindPoint() const override { return vk::PipelineBindPoint::eCompute; }
    inline const vk::Pipeline& GetPipeline() const override { return computePipeline; }
    inline const vk::PipelineLayout& GetPipelineLayout() const { return pipelineLayout; }
    inline const vk::Pipeline& GetComputePipeline() const { return computePipeline; }
    inline const std::vector<vk::DescriptorSetLayout>& GetDescriptorSetLayouts() const override { return descriptorSetLayouts; }
    inline const std::vector<ShaderBinding>& GetShaderBindings() const override { return shaderBindings; }

    void Bind(vk::CommandBuffer commandBuffer) const;
    void Dispatch(vk::CommandBuffer commandBuffer, uint32_t groupX, uint32_t groupY, uint32_t groupZ) const;
private:
    ComputePipeline();

    void CreateShader();
    void DestroyShader();
    void CreateDescriptorSetLayouts();
    void DestroyDescriptorSetLayouts();
    void CreatePipelineLayout();
    void DestroyPipelineLayout();
    void CreateComputePipeline();
    void DestroyComputePipeline();

    vk::Device* device;
    std::string shaderName;

    vk::ShaderModule shaderModule;
    std::vector<ShaderBinding> shaderBindings;
    std::vector<vk::DescriptorSetLayout> descriptorSetLayouts;
    vk::PipelineLayout pipelineLayout;
    vk::PipelineCache pipelineCache;
    vk::Pipeline computePipeline;
};
