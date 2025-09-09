#pragma once
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>

class DrawableObject
{
public:
    DrawableObject(std::vector<struct Vertex>& vertices, std::vector<uint32_t> &indices, vk::Device* device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::CommandPool& commandPool, vk::CommandBuffer& commandBuffer, vk::Queue &GraphicsQueue);
    ~DrawableObject();

    void Draw(vk::CommandBuffer& commandBuffer, vk::PipelineLayout& pipelineLayout, vk::Pipeline& pipeline, vk::DescriptorSet& descriptorSet);
    inline const vk::VertexInputBindingDescription& GetVertexInputBindingDescription() const { return vertexInputBindingDescription; }
    inline const std::vector<vk::VertexInputAttributeDescription>& GetVertexInputAttributeDescriptions() const { return vertexInputAttributeDescriptions; }
private:
    DrawableObject();

    void CreateVertexBuffer();
    void DestroyVertexBuffer();

    void CreateIndexBuffer();
    void DestroyIndexBuffer();
private:
    vk::Device* device;
    vk::CommandPool* commandPool;
    vk::CommandBuffer* commandBuffer;
    vk::Queue* graphicsQueue;
    vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties;
    std::vector<struct Vertex>* vertices;
    std::vector<uint32_t>* indices;
    vk::Buffer vertexBuffer;
    vk::DeviceMemory vertexBufferMemory;
    vk::DescriptorBufferInfo vertexBufferInfo;
    vk::Buffer indexBuffer;
    vk::DeviceMemory indexBufferMemory;
    vk::DescriptorBufferInfo indexBufferInfo;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
};