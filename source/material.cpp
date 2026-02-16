#include "material.h"
#include "materialInstance.h"
#include "renderPipline.h"
#include <memory>

Material::Material()
{
}
Material::~Material()
{
}

Material::Material(vk::Device* device, vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties, vk::RenderPass* renderPass, const std::string& shaderName, vk::SampleCountFlagBits samples, bool bIsPostProcess, bool bIsShadowPass)
{
    this->shaderName = shaderName;
    renderPipline = std::make_shared<RenderPipline>(device, gpuMemoryProperties,renderPass, shaderName, samples, bIsPostProcess, bIsShadowPass);
}
std::shared_ptr<MaterialInstance> Material::CreateInstance()
{
    auto instance = std::make_shared<MaterialInstance>();
    instance->SetBaseMaterial(shared_from_this());
    
    return instance;
}