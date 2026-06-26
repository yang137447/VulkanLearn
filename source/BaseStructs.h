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
    std::array<Eigen::Matrix4f, 4> lightViewProj;
    Eigen::Vector4f cascadeSplits = Eigen::Vector4f::Zero();
    std::array<Eigen::Vector4f, 4> shadowBias{};
    alignas(16) Eigen::Vector3f cameraPosition;
    int debugViewMode = 0;
    float environmentIntensity = 1.0f;
    alignas(16) std::array<Eigen::Vector4f, 9> environmentSH{};
    Eigen::Matrix4f previousViewProjection = Eigen::Matrix4f::Identity();
};

static_assert(offsetof(UBOGlobal, cascadeSplits) == 640, "UBOGlobal cascadeSplits must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, shadowBias) == 656, "UBOGlobal shadowBias must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, debugViewMode) == 732, "UBOGlobal debugViewMode must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, environmentIntensity) == 736, "UBOGlobal environmentIntensity must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, environmentSH) == 752, "UBOGlobal environmentSH must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, previousViewProjection) == 896, "UBOGlobal previousViewProjection must match GLSL std140 layout");

struct alignas(16) UBOModel
{
    Eigen::Matrix4f model;
    Eigen::Matrix4f previousModel;
};

struct alignas(16) SkyParametersGPU
{
    Eigen::Vector4f sunDirectionIntensity = Eigen::Vector4f(-0.381299f, 0.421436f, 0.822803f, 1.0f);
    Eigen::Vector4f sunColorAngularRadius = Eigen::Vector4f(22.0f, 17.5f, 10.0f, 0.18f);
    Eigen::Vector4f zenithColor = Eigen::Vector4f(0.09f, 0.32f, 0.95f, 0.0f);
    Eigen::Vector4f horizonColor = Eigen::Vector4f(0.85f, 0.78f, 0.58f, 0.0f);
    Eigen::Vector4f groundColor = Eigen::Vector4f(0.06f, 0.07f, 0.055f, 0.0f);
    Eigen::Vector4f scatteringControls = Eigen::Vector4f(0.42f, 0.35f, 96.0f, 0.45f);
    Eigen::Vector4f cloudControls = Eigen::Vector4f::Zero();
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
