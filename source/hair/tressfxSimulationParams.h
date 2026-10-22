#pragma once

#include <cstdint>

#include <Eigen/Dense>

#include "mesh/meshAssetTypes.h"

namespace VL::Hair
{
// 与 shader/glsl/hair/tressfxGuideSim.comp 的 std140 uniform block 一一对应。
// 字段名沿用 CPU 参考 solver：gravityWind 是 gravity + wind，controls 保存 dt、
// damping、pointsPerGuide、rootFixedPoints，limits.x 是 maxDt。
struct GuideSimulationParams
{
    Eigen::Vector4f gravityWind = Eigen::Vector4f::Zero();
    // x = dt, y = damping, z = pointsPerGuide, w = rootFixedPoints
    Eigen::Vector4f controls = Eigen::Vector4f::Zero();
    // x = iterations, y = guideCount, z = pointCount, w = 对齐保留
    uint32_t iterations = 0;
    uint32_t guideCount = 0;
    uint32_t pointCount = 0;
    uint32_t reserved = 0;
    // x = maxDt
    Eigen::Vector4f limits = Eigen::Vector4f::Zero();
};
static_assert(sizeof(GuideSimulationParams) == 64,
    "std140 guide simulation params must stay 64 bytes");
static_assert(alignof(GuideSimulationParams) == 16,
    "std140 guide simulation params require vec4 alignment");

// 每个 guide 一个 compute invocation；与 shader 的 local_size_x 必须一致。
inline constexpr uint32_t GuideSimulationWorkgroupSize = 64;

// 参数 UBO 按 frame-in-flight 槽分开，主机写入当前槽前该槽上一帧的 fence 已回收，
// 因此不需要在设备与主机之间再做额外的 UBO 同步。数量必须等于 MAX_FRAMES_IN_FLIGHT。
inline constexpr uint32_t GuideSimulationParamBufferCount = 2;

// dt 原样上传：GPU kernel 与 CPU Step 一样在内部 clamp 到 [0, maxDt]，
// 不在上传端提前裁剪，否则两者对负值/超限值的处理路径会分叉。
inline GuideSimulationParams MakeGuideSimulationParams(
    const TressfxSimulationConfig& config,
    uint32_t guideCount,
    uint32_t pointsPerGuide,
    uint32_t pointCount,
    float deltaSeconds)
{
    GuideSimulationParams params;
    const Eigen::Vector3f acceleration = config.gravity + config.wind;
    params.gravityWind = Eigen::Vector4f(
        acceleration.x(), acceleration.y(), acceleration.z(), 0.0f);
    params.controls = Eigen::Vector4f(
        deltaSeconds,
        config.damping,
        static_cast<float>(pointsPerGuide),
        static_cast<float>(config.rootFixedPoints));
    params.iterations = config.iterations;
    params.guideCount = guideCount;
    params.pointCount = pointCount;
    params.reserved = 0;
    params.limits = Eigen::Vector4f(config.maxDt, 0.0f, 0.0f, 0.0f);
    return params;
}

} // namespace VL::Hair
