#pragma once

#include <optional>
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>

struct QueueFamilyIndices
{
    std::optional<uint32_t> graphicsQueue;
    std::optional<uint32_t> presentQueue;
};

struct Buffer
{
  uint32_t bufferSize;
  std::vector<vk::Buffer> buffers;
  std::vector<vk::DeviceMemory> bufferMemories;
  std::vector<void*> buffersMapped;
  std::vector<vk::DescriptorBufferInfo> bufferInfos;
};

struct alignas(16) UBOGlobal
{
    Eigen::Matrix4f view;
    Eigen::Matrix4f projection;
    alignas(16) Eigen::Vector3f ambient;
    alignas(16) Eigen::Vector3f cameraPosition;
};

struct alignas(16) UBOModel
{
    Eigen::Matrix4f model;
};

struct alignas(4) LightSSBOHeader
{
    int directionalLightOffset;
    int directionalLightCount;
    int pointLightOffset;
    int pointLightCount;
    int spotLightOffset;
    int spotLightCount;
    int pad0;
    int pad1;
};

struct alignas(16) LightGPU
{
    Eigen::Vector4f colorIntensity;
    Eigen::Vector4f positionRadius;
    Eigen::Vector4f directionPad;
    Eigen::Vector4f coneAngleOuterInnerPadPad;
};