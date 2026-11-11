#pragma once

#include <algorithm>
#include <cmath>

#include "world/worldWind.h"
#include "world/worldWindSampling.h"

namespace VL
{

// 场景风的场级解析结果。方向已归一化，强度是已经过场景缓动与场景阵风调制的
// 有效值（cm/s）。它主要服务头发等场级消费者；SpeedTree 植被读取作者目标，
// 由每个物种的 CWindStateMgr 语义独立缓动/阵风。
struct ResolvedSceneWind
{
    Eigen::Vector3f direction = Eigen::Vector3f::UnitX();
    float strength = 0.0f;
};

// 场景风的场级运行时平滑层。World 里的 WorldWind 是作者目标值；本类把目标值按
// 固定响应时间缓动，再把场景阵风调制烘进最终强度，供头发等场级消费者使用。
//
// SpeedTree 植被不消费本结果：它们的 response 时间与 gust 包络属于每物种 profile，
// 由 RenderSystem 直接把 WorldWind 目标交给 WindResponseProfileSet。
class SceneWindRuntime
{
public:
    // 场景风强度的指数响应时间（秒）。世界刚加载时第一帧直接落到目标值，避免
    // 打开场景时出现一段从 0 开始的慢启动；之后目标变化才按此时间平滑。
    static constexpr float StrengthResponseSeconds = 0.5f;
    // 风向的指数响应时间（秒）。运行时场景风通常只在 World 换代时变化；该值
    // 保证以后允许运行时改风向时不会瞬间翻转。
    static constexpr float DirectionResponseSeconds = 0.5f;

    void Reset() noexcept
    {
        initialized = false;
        direction = Eigen::Vector3f::UnitX();
        strength = 0.0f;
        lastTimeSeconds = 0.0;
        resolved = ResolvedSceneWind{};
    }

    // 推进到 timeSeconds 并返回本帧解析结果。timeSeconds 由固定风 tick 提供，
    // 不是墙钟；同一帧重复调用不会产生额外推进。
    ResolvedSceneWind Advance(const WorldWind& target, double timeSeconds)
    {
        const double time = std::max(0.0, timeSeconds);
        if (!initialized)
        {
            // 首帧直接采用作者目标，不在加载时从零缓动。
            direction = target.direction.normalized();
            strength = std::max(0.0f, target.strength);
            initialized = true;
        }
        else
        {
            const float deltaSeconds = static_cast<float>(
                std::max(0.0, time - lastTimeSeconds));
            const float strengthBlend = 1.0f - std::exp(
                -deltaSeconds / StrengthResponseSeconds);
            const float directionBlend = 1.0f - std::exp(
                -deltaSeconds / DirectionResponseSeconds);
            const float targetStrength = std::max(0.0f, target.strength);
            strength += (targetStrength - strength) * strengthBlend;
            const Eigen::Vector3f targetDirection = target.direction.normalized();
            const Eigen::Vector3f blended =
                direction * (1.0f - directionBlend) + targetDirection * directionBlend;
            // 反向 180° 时线性混合会过零；退化时直接取目标方向，避免归一化 NaN。
            if (blended.squaredNorm() > 1.0e-12f)
                direction = blended.normalized();
            else
                direction = targetDirection;
        }
        lastTimeSeconds = time;

        // 场级阵风只调制已经缓动后的强度；植被走自己的 profile gust，不读这份结果。
        const float gustFactor = ApplySceneWindGust(1.0f, target.gust, time);
        resolved.direction = direction;
        resolved.strength = strength * gustFactor;
        return resolved;
    }

    const ResolvedSceneWind& GetResolved() const noexcept { return resolved; }

private:
    bool initialized = false;
    Eigen::Vector3f direction = Eigen::Vector3f::UnitX();
    float strength = 0.0f;
    double lastTimeSeconds = 0.0;
    ResolvedSceneWind resolved;
};

} // namespace VL
