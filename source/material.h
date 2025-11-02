#pragma once
#include <memory>
#include <Eigen/Dense>
#include <vulkan/vulkan.hpp>

class MaterialInstance; // Forward declaration
class RenderPipline;
class Material: public std::enable_shared_from_this<Material>
{
public:
    ~Material();
    Material(vk::Device* device, vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties, vk::RenderPass* renderPass, const std::string& shaderName, vk::SampleCountFlagBits samples);

    std::shared_ptr<MaterialInstance> CreateInstance();

    const std::string& GetShaderName() const{ return shaderName; }
    const std::shared_ptr<RenderPipline>& GetRenderPipline() const { return renderPipline; }
    
private:
    Material();

    std::string shaderName;
    std::shared_ptr<RenderPipline> renderPipline;
};