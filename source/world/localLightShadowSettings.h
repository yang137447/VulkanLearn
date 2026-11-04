#pragma once

#include <cmath>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace VL
{

// 场景点光/聚光的局部阴影作者字段。castShadows 缺省关闭，bias 缺省 2 cm；
// shadowSlice/shadowFar 不在这里，它们是 RenderSystem 每帧分配的运行时值。
struct LocalLightShadowSettings
{
    bool castShadows = false;
    float bias = 2.0f;
};

inline LocalLightShadowSettings ReadLocalLightShadowSettings(
    const nlohmann::json& lightNode,
    const std::string& owner)
{
    LocalLightShadowSettings settings;
    if (!lightNode.contains("shadow"))
    {
        return settings;
    }

    const nlohmann::json& shadow = lightNode.at("shadow");
    // 允许 `"shadow": true/false` 简写；对象形式再读 castShadows/bias。
    if (shadow.is_boolean())
    {
        settings.castShadows = shadow.get<bool>();
        return settings;
    }
    if (!shadow.is_object())
    {
        throw std::invalid_argument(
            owner + " shadow must be a boolean or object");
    }
    if (shadow.contains("castShadows"))
    {
        if (!shadow.at("castShadows").is_boolean())
        {
            throw std::invalid_argument(
                owner + " shadow.castShadows must be a boolean");
        }
        settings.castShadows = shadow.at("castShadows").get<bool>();
    }
    if (shadow.contains("bias"))
    {
        if (!shadow.at("bias").is_number())
        {
            throw std::invalid_argument(
                owner + " shadow.bias must be a number");
        }
        const double bias = shadow.at("bias").get<double>();
        // bias 是接收面厘米偏移；负值等于反向偏移，直接拒绝以免作者写出
        // 不可见的自遮挡/漏光，上限避免无意义的巨大常量。
        if (!std::isfinite(bias) || bias < 0.0 || bias > 1000.0)
        {
            throw std::invalid_argument(
                owner + " shadow.bias must be finite and in [0, 1000] cm");
        }
        settings.bias = static_cast<float>(bias);
    }
    return settings;
}

} // namespace VL
