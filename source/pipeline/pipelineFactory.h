#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include "vulkan/vulkan.hpp"

class ComputePipeline;
class GraphicsPipeline;
namespace vk
{
    class Device;
}

struct GraphicsPipelineKey
{
    vk::RenderPass* renderPass;
    std::string shaderName;
    vk::SampleCountFlagBits sampleCount;
    bool bIsPostProcess;
    bool bIsShadowPass;

    bool operator==(const GraphicsPipelineKey& other) const
    {
        return renderPass == other.renderPass &&
            shaderName == other.shaderName &&
            sampleCount == other.sampleCount &&
            bIsPostProcess == other.bIsPostProcess &&
            bIsShadowPass == other.bIsShadowPass;
    }
};

struct GraphicsPipelineKeyHash
{
    size_t operator()(const GraphicsPipelineKey& key) const;
};

class PipelineFactory
{
public:
    PipelineFactory(vk::Device* device);
    std::shared_ptr<ComputePipeline> CreateComputePipeline(const std::string& shaderName);
    std::shared_ptr<GraphicsPipeline> CreateGraphicsPipeline(
        vk::PhysicalDeviceMemoryProperties* gpuMemoryProperties,
        vk::RenderPass* renderPass,
        const std::string& shaderName,
        vk::SampleCountFlagBits sampleCount,
        bool bIsPostProcess,
        bool bIsShadowPass);
private:
    vk::Device* device;
    std::unordered_map<std::string, std::weak_ptr<ComputePipeline>> computePipelines;
    std::unordered_map<GraphicsPipelineKey, std::weak_ptr<GraphicsPipeline>, GraphicsPipelineKeyHash> graphicsPipelines;
};
