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

// Runtime SDK 10 / Games 9 wind state consumed by the shared SpeedTree
// vertex deformation path. The public SDK shader contract is represented by
// the state vectors below, with converted bounding-box min/max retained for
// the height and packed-offset calculations.
struct alignas(16) SpeedTreeWindStateGPU
{
    // Profile state stores a world-space direction. Object UBO assembly replaces
    // xyz with the normalized object-local direction before GPU upload.
    Eigen::Vector4f windVector = Eigen::Vector4f(1.0f, 0.0f, 0.0f, 0.0f);
    Eigen::Vector4f treeExtentsSharedHeightStart = Eigen::Vector4f::Zero();
    Eigen::Vector4f treeBoundsMin = Eigen::Vector4f::Zero();
    Eigen::Vector4f treeBoundsMax = Eigen::Vector4f::Zero();
    // branch 1 limit, branch 2 limit, instance wind independence, import scale
    Eigen::Vector4f branchStretchLimits = Eigen::Vector4f(0.0f, 0.0f, 0.5f, 1.0f);
    Eigen::Vector4f sharedNoisePosTurbulenceIndependence = Eigen::Vector4f::Zero();
    Eigen::Vector4f sharedBendOscillationTurbulenceFlexibility = Eigen::Vector4f::Zero();
    Eigen::Vector4f branch1NoisePosTurbulenceIndependence = Eigen::Vector4f::Zero();
    Eigen::Vector4f branch1BendOscillationTurbulenceFlexibility = Eigen::Vector4f::Zero();
    Eigen::Vector4f branch2NoisePosTurbulenceIndependence = Eigen::Vector4f::Zero();
    Eigen::Vector4f branch2BendOscillationTurbulenceFlexibility = Eigen::Vector4f::Zero();
    Eigen::Vector4f rippleNoisePosTurbulenceIndependence = Eigen::Vector4f::Zero();
    Eigen::Vector4f ripplePlanarDirectionalFlexibilityShimmer = Eigen::Vector4f::Zero();
};

static_assert(sizeof(SpeedTreeWindStateGPU) == 208, "SpeedTreeWindStateGPU must match the 13 GLSL vec4 fields");

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
    Eigen::Matrix4f previousViewProjection = Eigen::Matrix4f::Identity();

    // 环境相关
    float environmentIntensity;
    alignas(16) std::array<Eigen::Vector4f, 9> environmentSH{};
    SkyParametersGPU skyParameters;
};

static_assert(offsetof(SkyParametersGPU, sunDirectionIntensity) == 0, "SkyParametersGPU sunDirectionIntensity must match GLSL std140 layout");
static_assert(offsetof(SkyParametersGPU, sunColorAngularRadius) == 16, "SkyParametersGPU sunColorAngularRadius must match GLSL std140 layout");
static_assert(offsetof(SkyParametersGPU, zenithColor) == 32, "SkyParametersGPU zenithColor must match GLSL std140 layout");
static_assert(offsetof(SkyParametersGPU, horizonColor) == 48, "SkyParametersGPU horizonColor must match GLSL std140 layout");
static_assert(offsetof(SkyParametersGPU, groundColor) == 64, "SkyParametersGPU groundColor must match GLSL std140 layout");
static_assert(offsetof(SkyParametersGPU, scatteringControls) == 80, "SkyParametersGPU scatteringControls must match GLSL std140 layout");
static_assert(offsetof(SkyParametersGPU, cloudControls) == 96, "SkyParametersGPU cloudControls must match GLSL std140 layout");

static_assert(offsetof(UBOGlobal, cascadeSplits) == 640, "UBOGlobal cascadeSplits must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, shadowBias) == 656, "UBOGlobal shadowBias must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, debugViewMode) == 732, "UBOGlobal debugViewMode must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, previousViewProjection) == 736, "UBOGlobal previousViewProjection must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, environmentIntensity) == 800, "UBOGlobal environmentIntensity must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, environmentSH) == 816, "UBOGlobal environmentSH must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) == 960, "UBOGlobal skyParameters must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) + offsetof(SkyParametersGPU, sunDirectionIntensity) == 960, "UBOGlobal sunDirectionIntensity must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) + offsetof(SkyParametersGPU, sunColorAngularRadius) == 976, "UBOGlobal sunColorAngularRadius must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) + offsetof(SkyParametersGPU, zenithColor) == 992, "UBOGlobal zenithColor must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) + offsetof(SkyParametersGPU, horizonColor) == 1008, "UBOGlobal horizonColor must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) + offsetof(SkyParametersGPU, groundColor) == 1024, "UBOGlobal groundColor must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) + offsetof(SkyParametersGPU, scatteringControls) == 1040, "UBOGlobal scatteringControls must match GLSL std140 layout");
static_assert(offsetof(UBOGlobal, skyParameters) + offsetof(SkyParametersGPU, cloudControls) == 1056, "UBOGlobal cloudControls must match GLSL std140 layout");
struct alignas(16) UBOModel
{
    Eigen::Matrix4f model;
    Eigen::Matrix4f previousModel;
    // TODO: 将 SpeedTree 风动状态从所有对象共享的 UBOModel 中拆出，改为
    // SpeedTree Pipeline 才绑定的可选 Profile/Instance Buffer，避免普通 Mesh
    // 分配并重复上传无用的 208 字节风动数据。
    SpeedTreeWindStateGPU speedTreeWind;
};

static_assert(offsetof(UBOModel, speedTreeWind) == 128, "UBOModel speedTreeWind must match GLSL std140 layout");
static_assert(sizeof(UBOModel) == 336, "UBOModel size must match the GLSL std140 block");

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
