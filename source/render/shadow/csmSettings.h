#pragma once

#include <array>
#include <cstdint>
#include <string_view>

#include <Eigen/Dense>
#include <nlohmann/json.hpp>

#include "core/runtimeResult.h"

namespace VL
{

struct CsmSettings
{
    static constexpr uint32_t MaxCascadeCount = 4;
    static constexpr float MaxReceiverDepthBias = 0.006f;
    static constexpr float MaxSlopeBiasMultiplier = 8.0f;

    bool castShadows = true;
    uint32_t cascadeCount = 4;
    float dynamicShadowDistance = 300.0f;
    float cascadeDistributionExponent = 3.0f;
    float cascadeTransitionFraction = 0.1f;
    float shadowDistanceFadeoutFraction = 0.1f;
    float shadowBias = 0.5f;
    float shadowSlopeBias = 0.5f;
    float shadowCascadeBiasDistribution = 1.0f;
    bool lightSpaceCasterBounds = true;
};

// 对齐 UE 的组件所有权：方向光持有美术调节参数，
// RenderGraph 只持有最大级联资源容量。
RuntimeResult<CsmSettings> BuildDirectionalLightCsmSettings(
    const nlohmann::json& directionalLightJson,
    uint32_t shadowCascadeCapacity,
    std::string_view scenePath);

} // namespace VL
