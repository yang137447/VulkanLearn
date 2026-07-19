#pragma once

#include <vector>
#include "vulkan/vulkan.hpp"

struct ShaderBinding;

class PipelineBase
{
public:
    virtual ~PipelineBase() = default;

    virtual vk::PipelineBindPoint GetBindPoint() const = 0;
    virtual const vk::Pipeline& GetPipeline() const = 0;
    virtual const vk::PipelineLayout& GetPipelineLayout() const = 0;
    virtual const std::vector<vk::DescriptorSetLayout>& GetDescriptorSetLayouts() const = 0;
    // Shader bindings are per-pass reflection; layout bindings also contain
    // explicit descriptor contract overrides such as the full material Set 1.
    virtual const std::vector<ShaderBinding>& GetShaderBindings() const = 0;
    virtual const std::vector<ShaderBinding>& GetDescriptorLayoutBindings() const = 0;
};
