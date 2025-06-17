#pragma once
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_handles.hpp>

class DrawableObject
{
public:
    DrawableObject(std::vector<struct Vertex>& vertices, vk::Device* device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::CommandBuffer& commandBuffer, vk::Queue &GraphicsQueue);
    ~DrawableObject();

    void Draw(vk::CommandBuffer& commandBuffer, vk::PipelineLayout& pipelineLayout, vk::Pipeline& pipeline, vk::DescriptorSet& descriptorSet);
    inline const vk::VertexInputBindingDescription& GetVertexInputBindingDescription() const { return vertexInputBindingDescription; }
    inline const std::vector<vk::VertexInputAttributeDescription>& GetVertexInputAttributeDescriptions() const { return vertexInputAttributeDescriptions; }
private:
    DrawableObject();

    void CreateVertexBuffer();
private:
    vk::Device* device;
    vk::CommandBuffer* commandBuffer;
    vk::Queue* graphicsQueue;
    vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties;
    std::vector<struct Vertex>* vertices;
    vk::Buffer vertexBuffer;
    vk::DeviceMemory vertexBufferMemory;
    vk::DescriptorBufferInfo vertexBufferInfo;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
};