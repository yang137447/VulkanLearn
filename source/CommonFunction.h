#pragma once

#include "vulkan/vulkan.hpp"
#include <vulkan/vulkan_handles.hpp>
#include <Eigen/Dense>
#include <array>

struct UniformBufferObject
{
    Eigen::Matrix4f model;
    Eigen::Matrix4f view;
    Eigen::Matrix4f projection;
};

namespace CommonFunction
{
    inline uint32_t FindMemoryType(vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, uint32_t typeFilter, vk::MemoryPropertyFlags& memoryPropertyFlags)
    {
        for (uint32_t i = 0; i < physicalDeviceMemoryProperties.memoryTypeCount; i++)
        {
            if ((typeFilter & (1 << i)) && (physicalDeviceMemoryProperties.memoryTypes[i].propertyFlags & memoryPropertyFlags) == memoryPropertyFlags)
            {
                return i;
            }
        }
        throw std::runtime_error("Failed to find suitable memory type!");
    }

    inline std::pair<vk::Buffer, vk::DeviceMemory> CreateBuffer(vk::Device& device, vk::DeviceSize& size, vk::BufferUsageFlags& usage, vk::PhysicalDeviceMemoryProperties& physicalDeviceMemoryProperties, vk::MemoryPropertyFlags& memoryPropertyFlags)
    {
        vk::BufferCreateInfo bufferInfo;
        bufferInfo
            .setSize(size)
            .setUsage(usage)
            .setSharingMode(vk::SharingMode::eExclusive);

        vk::Buffer buffer = device.createBuffer(bufferInfo);

        vk::MemoryRequirements memRequirements = device.getBufferMemoryRequirements(buffer);

        vk::MemoryAllocateInfo allocInfo;
        allocInfo
            .setAllocationSize(memRequirements.size)
            .setMemoryTypeIndex(FindMemoryType(physicalDeviceMemoryProperties, memRequirements.memoryTypeBits, memoryPropertyFlags));

        vk::DeviceMemory bufferMemory = device.allocateMemory(allocInfo);
        device.bindBufferMemory(buffer, bufferMemory, 0);

        return { buffer, bufferMemory };
    }

    inline void CopyBufferToBuffer(vk::Queue& GraphicsQueue, vk::CommandBuffer& commandBuffer, vk::Buffer& srcBuffer, vk::Buffer& dstBuffer, vk::DeviceSize& size)
    {
        vk::CommandBufferBeginInfo beginInfo;
        beginInfo.setFlags(vk::CommandBufferUsageFlagBits::eOneTimeSubmit);

        commandBuffer.begin(beginInfo);
        {
            vk::BufferCopy copyRegion;
            copyRegion
                .setSrcOffset(0)
                .setDstOffset(0)
                .setSize(size);
            commandBuffer.copyBuffer(srcBuffer, dstBuffer, 1, &copyRegion);
        }
        commandBuffer.end();

        vk::SubmitInfo submitInfo;
        submitInfo
            .setCommandBuffers(commandBuffer);
            
        GraphicsQueue.submit(submitInfo);
        GraphicsQueue.waitIdle();
    }
}