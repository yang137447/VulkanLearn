#pragma once

#include <cstdint>

#include <Eigen/Dense>

#include "mesh/meshAssetTypes.h"

namespace VL::Hair
{
// 与 shader/glsl/hair/tressfxGuideSim.comp 的 std140 uniform block 一一对应。
// 固定步 accumulator 由 Host 推进，因此 kernel 只收到单个 fixedStep；maxDt 不进入 GPU。
struct GuideSimulationParams
{
    // xyz = 平均重力，w 未使用。
    Eigen::Vector4f gravity = Eigen::Vector4f::Zero();
    // xyz = 平均风，w 未使用；kernel 叠加每 guide 的有界 gust。
    Eigen::Vector4f wind = Eigen::Vector4f::Zero();
    // x = fixedStep, y = damping, z = pointsPerGuide, w = rootFixedPoints
    Eigen::Vector4f controls = Eigen::Vector4f::Zero();
    // x = iterations, y = guideCount, z = pointCount, w = 对齐保留
    uint32_t iterations = 0;
    uint32_t guideCount = 0;
    uint32_t pointCount = 0;
    uint32_t reserved = 0;
    // x = bendStiffness, y = windGust, z = windFrequency, w = frame-start simulationTime
    Eigen::Vector4f motion = Eigen::Vector4f::Zero();
};
static_assert(sizeof(GuideSimulationParams) == 80,
    "std140 guide simulation params must stay 80 bytes");
static_assert(alignof(GuideSimulationParams) == 16,
    "std140 guide simulation params require vec4 alignment");

// 每个 guide 一个 compute invocation；与 shader 的 local_size_x 必须一致。
inline constexpr uint32_t GuideSimulationWorkgroupSize = 64;

// 参数 UBO 按 frame-in-flight 槽分开，主机写入当前槽前该槽上一帧的 fence 已回收，
// 因此不需要在设备与主机之间再做额外的 UBO 同步。数量必须等于 MAX_FRAMES_IN_FLIGHT。
inline constexpr uint32_t GuideSimulationParamBufferCount = 2;

// simulationTime 是该帧所有子步共享的 frame-start t；dt 固定为 fixedStep，
// 因为 accumulator 已在 Host 与 CPU Step 中用相同算术消费。
inline GuideSimulationParams MakeGuideSimulationParams(
    const TressfxSimulationConfig& config,
    uint32_t guideCount,
    uint32_t pointsPerGuide,
    uint32_t pointCount,
    float simulationTime)
{
    GuideSimulationParams params;
    params.gravity = Eigen::Vector4f(
        config.gravity.x(), config.gravity.y(), config.gravity.z(), 0.0f);
    params.wind = Eigen::Vector4f(
        config.wind.x(), config.wind.y(), config.wind.z(), 0.0f);
    params.controls = Eigen::Vector4f(
        config.fixedStep,
        config.damping,
        static_cast<float>(pointsPerGuide),
        static_cast<float>(config.rootFixedPoints));
    params.iterations = config.iterations;
    params.guideCount = guideCount;
    params.pointCount = pointCount;
    params.reserved = 0;
    params.motion = Eigen::Vector4f(
        config.bendStiffness,
        config.windGust,
        config.windFrequency,
        simulationTime);
    return params;
}

} // namespace VL::Hair
