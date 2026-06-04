#pragma once

#include <array>
#include <cstddef>
#include <vector>
#include <vulkan/vulkan.hpp>
#include <Eigen/Dense>

#include "render/rhi/rhiResourceHandles.h"

struct Buffer
{
  uint32_t bufferSize = 0;
  std::vector<VL::RHIBufferHandle> bufferHandles;
  std::vector<vk::Buffer> buffers;
  std::vector<vk::DeviceMemory> bufferMemories;
  std::vector<void*> buffersMapped;
  std::vector<vk::DescriptorBufferInfo> bufferInfos;

  bool HasResources() const
  {
    return !bufferHandles.empty() ||
      !buffers.empty() ||
      !bufferMemories.empty() ||
      !buffersMapped.empty() ||
      !bufferInfos.empty();
  }
};

struct alignas(16) UBOGlobal
{
    Eigen::Matrix4f view;
    Eigen::Matrix4f projection;
    Eigen::Matrix4f invView;
    Eigen::Matrix4f invProjection;
    Eigen::Matrix4f viewProjection;
    Eigen::Matrix4f invViewProjection;
    Eigen::Matrix4f lightViewProj;
    alignas(16) Eigen::Vector3f cameraPosition;
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    alignas(16) std::array<Eigen::Vector4f, 9> environmentSH{};
    Eigen::Matrix4f previousViewProjection = Eigen::Matrix4f::Identity();
};

static_assert(offsetof(UBOGlobal, debugViewMode) == 460, "UBOGlobal debugViewMode must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, environmentIntensity) == 464, "UBOGlobal environmentIntensity must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, environmentSH) == 480, "UBOGlobal environmentSH must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, previousViewProjection) == 624, "UBOGlobal previousViewProjection must match GLSL std140 layout");

struct alignas(16) UBOModel
{
    Eigen::Matrix4f model;
    Eigen::Matrix4f previousModel;
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
