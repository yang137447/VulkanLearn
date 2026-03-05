#pragma once
#include <memory>
#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

class MaterialInstance; // Forward declaration
class PipelineBase;
class PipelineFactory;
class Material: public std::enable_shared_from_this<Material>
{
public:
    ~Material();
    Material(PipelineFactory& pipelineFactory, vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties, vk::RenderPass* renderPass, const std::string& shaderName, vk::SampleCountFlagBits samples, bool bIsPostProcess = false,
                    bool bIsShadowPass = false);

    std::shared_ptr<MaterialInstance> CreateInstance();

    const std::string& GetShaderName() const{ return shaderName; }
    const std::shared_ptr<PipelineBase>& GetRenderPipeline() const { return renderPipeline; }
    
private:
    Material();

    std::string shaderName;
    std::shared_ptr<PipelineBase> renderPipeline;
};
