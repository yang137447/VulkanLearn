#pragma once
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>

class RenderableObject
{
public:
    RenderableObject(std::vector<struct Vertex> vertices, std::vector<uint32_t> indices, vk::Device* device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties, vk::CommandPool* commandPool, vk::CommandBuffer* commandBuffer, vk::Queue* GraphicsQueue);
    ~RenderableObject();

    void Draw(vk::CommandBuffer& commandBuffer, uint32_t width, uint32_t height);
    const Eigen::Vector3f& GetBoundsMin() const { return boundsMin; }
    const Eigen::Vector3f& GetBoundsMax() const { return boundsMax; }

private:
    void UpdateLocalBounds();

    void CreateVertexBuffer();
    void DestroyVertexBuffer();

    void CreateIndexBuffer();
    void DestroyIndexBuffer();
private:
    RenderableObject();

    vk::Device* device;
    vk::CommandPool* commandPool;
    vk::CommandBuffer* commandBuffer;
    vk::Queue* graphicsQueue;
    vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties;

    std::vector<struct Vertex> vertices;
    std::vector<uint32_t> indices;
    Eigen::Vector3f boundsMin;
    Eigen::Vector3f boundsMax;
    vk::Buffer vertexBuffer;
    vk::DeviceMemory vertexBufferMemory;
    vk::DescriptorBufferInfo vertexBufferInfo;
    vk::Buffer indexBuffer;
    vk::DeviceMemory indexBufferMemory;
    vk::DescriptorBufferInfo indexBufferInfo;
};
