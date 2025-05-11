#include "ShaderModule.h"

ShaderModule::ShaderModule(vk::Device& device,
                           std::string& vertexCode,
                           std::string& fragmentCode)
{
    // init input data
    this->device = device;

    // vertex shader
    vk::ShaderModuleCreateInfo ShaderModuleCreateInfo;
    ShaderModuleCreateInfo
        .setCodeSize(vertexCode.size())
        .setPCode(reinterpret_cast<const uint32_t *>(vertexCode.data()));
    vertexShaderModule = device.createShaderModule(ShaderModuleCreateInfo);

    // fragment shader
    ShaderModuleCreateInfo
        .setCodeSize(fragmentCode.size())
        .setPCode(reinterpret_cast<const uint32_t *>(fragmentCode.data()));
    fragmentShaderModule = device.createShaderModule(ShaderModuleCreateInfo);
}

ShaderModule::~ShaderModule()
{
    device.destroyShaderModule(vertexShaderModule);
    device.destroyShaderModule(fragmentShaderModule);
}
