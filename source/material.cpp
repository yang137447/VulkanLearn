#include "material.h"
#include "materialInstance.h"
#include "pipeline/graphicsPipeline.h"
#include "pipeline/pipelineFactory.h"
#include <memory>

Material::Material()
{
}
Material::~Material()
{
}

Material::Material(PipelineFactory& pipelineFactory, vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties, vk::RenderPass* renderPass, const std::string& shaderName, vk::SampleCountFlagBits samples, bool bIsPostProcess, bool bIsShadowPass)
{
    this->shaderName = shaderName;
    renderPipeline = pipelineFactory.CreateGraphicsPipeline(gpuMemoryProperties, renderPass, shaderName, samples, bIsPostProcess, bIsShadowPass);
}
std::shared_ptr<MaterialInstance> Material::CreateInstance()
{
    auto instance = std::make_shared<MaterialInstance>();
    instance->SetBaseMaterial(shared_from_this());
    
    return instance;
}
