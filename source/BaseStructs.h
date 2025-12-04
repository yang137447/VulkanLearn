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

struct UBOGlobal
{
    Eigen::Matrix4f view;
    Eigen::Matrix4f projection;
    Eigen::Vector3f ambient;
    Eigen::Vector3f cameraPosition;
    Eigen::Vector3f pointLightPosition;
    Eigen::Vector4f pointLightColor;
    Eigen::Vector4f pointLightSpecular;
};

struct UBOModel
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
};

struct alignas(16) LightGPU
{
    Eigen::Vector4f colorIntensity;
    Eigen::Vector4f positionRadius;
    Eigen::Vector4f directionPad;
    Eigen::Vector4f coneAngleOuterInnerPadPad;
};