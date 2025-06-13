#pragma once

#include "vulkan/vulkan.hpp"

uint32_t FindMemoryType(vk::PhysicalDeviceMemoryProperties memoryProperties, uint32_t typeFilter, vk::MemoryPropertyFlags properties)
{
    for (uint32_t i = 0; i < memoryProperties.memoryTypeCount; i++)
    {
        if ((typeFilter & (1 << i)) && (memoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
        {
            return i;
        }
    }
    throw std::runtime_error("Failed to find suitable memory type!");
}