#pragma once
#include <vulkan/vulkan.hpp>

struct Vertex;

class DrawableObject
{
public:
    DrawableObject(std::vector<Vertex>& vertices, vk::Device* device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties);
    ~DrawableObject();

    void Draw(vk::CommandBuffer& commandBuffer, vk::PipelineLayout& pipelineLayout, vk::Pipeline& pipeline, vk::DescriptorSet& descriptorSet);
    inline const vk::VertexInputBindingDescription& GetVertexInputBindingDescription() const { return vertexInputBindingDescription; }
    inline const std::vector<vk::VertexInputAttributeDescription>& GetVertexInputAttributeDescriptions() const { return vertexInputAttributeDescriptions; }
private:
    DrawableObject();
private:
    vk::Device* device;
    std::vector<Vertex>* vertices;
    vk::Buffer vertexBuffer;
    vk::DeviceMemory vertexBufferMemory;
    vk::DescriptorBufferInfo vertexBufferInfo;

    vk::VertexInputBindingDescription vertexInputBindingDescription;
    std::vector<vk::VertexInputAttributeDescription> vertexInputAttributeDescriptions;
};