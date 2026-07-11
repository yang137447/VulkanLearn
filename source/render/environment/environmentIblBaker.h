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

class EnvironmentIblBaker
{
public:
    void Initialize(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);
    void Shutdown(RendererBackendVulkan& rendererBackend);

    bool IsInitialized() const { return initialized; }

    void Record(
        vk::CommandBuffer commandBuffer, 
        const std::shared_ptr<Texture>& environmentCube, 
        uint32_t swapchainImageIndex);

    std::shared_ptr<Texture> GetPrefilteredEnvironmentCube() const { return prefilterCube.texture; }

private:
    struct BufferResource
    {
        vk::Buffer buffer;
        vk::DeviceMemory memory;
    };

    struct PrefilteredCubeResources
    {
        std::shared_ptr<Texture> texture;
        std::vector<vk::ImageView> storageViews;
        uint32_t size = 128;
        uint32_t mipLevels = 1;
        vk::Format format = vk::Format::eR16G16B16A16Sfloat;
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    };

    void CreateDescriptorResources(
        RendererBackendVulkan& rendererBackend,
        const std::vector<vk::DescriptorBufferInfo>& globalUniformBufferInfos);
    void UpdateEnvironmentCubeDescriptors(
        RendererBackendVulkan& rendererBackend,
        const std::shared_ptr<Texture>& environmentCube,
        uint32_t swapchainImageIndex);
    void DestroyDescriptorResources(RendererBackendVulkan& rendererBackend);
    void CreatePrefilteredCubeResources(RendererBackendVulkan& rendererBackend);
    void DestroyPrefilteredCubeResources(RendererBackendVulkan& rendererBackend);


    std::shared_ptr<ComputePipeline> skySHGeneratePipeline;
    std::shared_ptr<ComputePipeline> prefilterEnvMapPipeline;

    vk::DescriptorPool descriptorPool;
    std::vector<vk::DescriptorSet> skySHGenerateDescriptorSets;
    std::vector<vk::DescriptorBufferInfo> globalUniformBufferInfos;
    std::vector<vk::DescriptorSet> prefilterDescriptorSets;
    std::vector<BufferResource> prefilterParamBuffers;
    std::vector<std::shared_ptr<Texture>> boundEnvironmentCubes;

    PrefilteredCubeResources prefilterCube;

    RendererBackendVulkan* rendererBackend = nullptr;

    bool initialized = false;
};
} // namespace VL
