#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.hpp>
#include "pipeline/graphicsPipelineBuilder.h"
#include "shaderVariant.h"

class MaterialInstance; // Forward declaration
class PipelineBase;
class PipelineFactory;
class Material: public std::enable_shared_from_this<Material>
{
public:
    ~Material() = default;
    Material(PipelineFactory& pipelineFactory, vk::RenderPass* renderPass, const ShaderVariantKey& shaderVariantKey, const std::string& materialKey, vk::SampleCountFlagBits samples, uint32_t colorAttachmentCount, const GraphicsPipelineStateDesc& pipelineStateDesc = {},
                    bool bIsShadowPass = false);

    std::shared_ptr<MaterialInstance> CreateInstance();

    const std::string& GetShaderName() const{ return shaderVariantKey.shaderName; }
    const ShaderVariantKey& GetShaderVariantKey() const { return shaderVariantKey; }
    const std::string& GetMaterialKey() const { return materialKey; }
    const std::shared_ptr<PipelineBase>& GetRenderPipeline() const { return renderPipeline; }
    
private:
    ShaderVariantKey shaderVariantKey;
    std::string materialKey;
    std::shared_ptr<PipelineBase> renderPipeline;
};
