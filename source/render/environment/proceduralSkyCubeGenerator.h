#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <vulkan/vulkan.hpp>

class ComputePipeline;
class PipelineFactory;
class Texture;

namespace VL
{
class RendererBackendVulkan;

class ProceduralSkyCubeGenerator
{
public:
    void Initialize(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    bool IsInitialized() const { return initialized; }

    void Record(vk::CommandBuffer commandBuffer, uint32_t swapchainImageIndex);
    
    std::shared_ptr<Texture> GetEnvironmentCube();

private:
    struct SkyCubeResources
    {
        std::shared_ptr<Texture> texture;
        vk::ImageView storageView;
        uint32_t size = 128;
        vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };
    struct BufferResource
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
    };


    void CreateSkyCubeResources(RendererBackendVulkan& rendererBackend);
    void DestroySkyCubeResources(RendererBackendVulkan& rendererBackend);
    void CreateDescriptorResources(
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);
    void DestroyDescriptorResources(RendererBackendVulkan& rendererBackend);

    std::shared_ptr<ComputePipeline> skyToCubemapPipeline;

    vk::DescriptorPool descriptorPool;
    std::vector<vk::DescriptorSet> skyToCubemapDescriptorSets;
    std::vector<vk::DescriptorBufferInfo> globalUniformBufferInfos;

    SkyCubeResources skyCube;

    bool initialized = false;
};
} // namespace VL
