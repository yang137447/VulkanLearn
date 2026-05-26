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

Material::Material(PipelineFactory& pipelineFactory, vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties, vk::RenderPass* renderPass, const ShaderVariantKey& shaderVariantKey, const std::string& materialKey, vk::SampleCountFlagBits samples, uint32_t colorAttachmentCount, const GraphicsPipelineStateDesc& pipelineStateDesc, bool bIsShadowPass)
{
    this->shaderVariantKey = shaderVariantKey;
    this->materialKey = materialKey;
    renderPipeline = pipelineFactory.CreateGraphicsPipeline(gpuMemoryProperties, renderPass, shaderVariantKey, samples, colorAttachmentCount, pipelineStateDesc, bIsShadowPass);
}
std::shared_ptr<MaterialInstance> Material::CreateInstance()
{
    auto instance = std::make_shared<MaterialInstance>();
    instance->SetBaseMaterial(shared_from_this());
    
    return instance;
}
