#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "world/worldSnapshot.h"

namespace VL
{

inline constexpr uint32_t kMaxLocalShadowLights = 4;
inline constexpr uint32_t kInvalidLocalShadowSlice = 0xFFFFFFFFu;
// 作者未给半径时仍需要一个确定的远平面，避免 0 半径把整盏灯判成无阴影。
inline constexpr float kLocalShadowFallbackFarCm = 2000.0f;

struct LocalShadowOctant
{
    float sx;
    float sy;
    float sz;
};

// 固定的八象限顺序：索引 bit0/bit1/bit2 分别对应 sx/sy/sz 的符号位。
// shader 用同一位序解码 localShadowParams.y，CPU/GPU 不能各自改顺序。
const std::array<LocalShadowOctant, 8>& LocalShadowOctantOrder();

// 点光标准八面体投影：clip.xy/w = 采样端 EncodeOctahedralDirection 的
// [-1,1]^2 坐标。矩阵第 2 行恒为 0，深度由 shadowLocal.frag 的 gl_FragDepth
// 写入，不依赖 gl_Position.z。
Eigen::Matrix4f BuildOctahedralProjectionMatrix(
    const Eigen::Vector3f& lightPosition,
    uint32_t octantIndex);

struct LocalShadowSpotMatrices
{
    Eigen::Matrix4f view = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f projection = Eigen::Matrix4f::Identity();
};

// 聚光使用与相机相同的 lookAt 约定和 Vulkan NDC 重映射：
// fov = 2 * coneAngleOuter，near = 1 cm，far = radius。
LocalShadowSpotMatrices BuildSpotShadowMatrices(
    const Eigen::Vector3f& lightPosition,
    const Eigen::Vector3f& lightDirection,
    float coneAngleOuterRadians,
    float nearCm,
    float farCm);

// 每帧局部阴影选灯的可测试部分：按 intensity/(1 + normalizedDistance²) 降序，
// 同分用稳定 lightId 升序，避免相机微小移动时槽位互换。
std::vector<size_t> RankLocalShadowLights(
    const std::vector<LightSnapshot>& lights,
    const Eigen::Vector3f& cameraPosition,
    uint32_t maxCount);

float ComputeLocalShadowFar(float lightRadius);

} // namespace VL
