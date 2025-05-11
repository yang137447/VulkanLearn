#pragma once

#include "vulkan/vulkan.hpp"

class ShaderModule
{
public:
    ShaderModule(vk::Device& device,
           std::string& vertexCode,
           std::string& fragmentCode);
    ~ShaderModule();

    inline vk::ShaderModule GetVertexShaderModule() const { return vertexShaderModule; }
    inline vk::ShaderModule GetFragmentShaderModule() const { return fragmentShaderModule; }

private:
    ShaderModule();
    // inputdata
    vk::Device device;
    // data
    vk::ShaderModule vertexShaderModule;
    vk::ShaderModule fragmentShaderModule;
};