#pragma once
#include <vulkan/vulkan.hpp>

class DrawableObject
{
public:
    DrawableObject(std::vector<float>& vertices, vk::Device* device, vk::PhysicalDeviceMemoryProperties* physicalDeviceMemoryProperties);
    ~DrawableObject();

    void Draw(vk::CommandBuffer& commandBuffer, vk::PipelineLayout& pipelineLayout, vk::Pipeline& pipeline, vk::DescriptorSet& descriptorSet);
private:
    DrawableObject();
private:
    vk::Device* device;
    std::vector<float>* vertices;
    vk::Buffer vertexBuffer;
    vk::DeviceMemory vertexBufferMemory;
    vk::DescriptorBufferInfo vertexBufferInfo;

};