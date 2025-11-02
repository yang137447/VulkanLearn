#pragma once

#include <optional>
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsQueue;
    std::optional<uint32_t> presentQueue;
};

struct UBO
{
  uint32_t uniformBufferSize;
  std::vector<vk::Buffer> uniformBuffers;
  std::vector<vk::DeviceMemory> uniformBufferMemories;
  std::vector<void*> uniformBuffersMapped;
  std::vector<vk::DescriptorBufferInfo> uniformBufferInfos;
};

struct UBOGlobal
{
    Eigen::Matrix4f view;
    Eigen::Matrix4f projection;
    Eigen::Vector3f ambient;
};

struct UBOModel
{
    Eigen::Matrix4f model;
};