#include "rootOffset.h"

#include <algorithm>
#include <cmath>

namespace VL::Animation
{
namespace
{
constexpr float pi = 3.14159265358979323846f;
constexpr float halfLife = 0.08f;
constexpr float maximumTranslation = 20;
constexpr float maximumYaw = pi / 4;

float WrapYaw(float angle)
{
    angle = std::remainder(angle, 2 * pi);
    return angle <= -pi ? angle + 2 * pi : angle;
}

void Decay(float& position, float& velocity, float deltaSeconds)
{
    // Holden 临界阻尼解析解；halfLife 沿用其阻尼参数约定，并非任意初速下位置恰减半的时间。
    // 使用 std::exp，避免参考实现 fast_negexp 的分步近似误差。
    // https://github.com/orangeduck/Motion-Matching/blob/2b621d3e7eee447057bb974fab85aad4093e5389/spring.h
    const float damping = 2 * std::log(2.0f) / halfLife;
    const float impulse = velocity + damping * position;
    const float decay = std::exp(-damping * deltaSeconds);
    position = decay * (position + impulse * deltaSeconds);
    velocity = decay * (velocity - impulse * damping * deltaSeconds);
}
}

void RootOffsetSolver::SetEnabled(bool enabled)
{
    if (state.enabled == enabled) return;
    state.enabled = enabled;
    Reset(enabled ? "warming-up" : "disabled");
}

void RootOffsetSolver::Reset(const char* reason)
{
    const bool enabled = state.enabled;
    state = {};
    state.enabled = enabled;
    state.reason = reason;
    velocity.setZero();
    yawVelocity = 0;
    historyValid = false;
}

void RootOffsetSolver::Update(const Eigen::Matrix4f& component,
    const Eigen::Vector3f& achievedVelocity, float deltaSeconds)
{
    if (!state.enabled || deltaSeconds <= 0) return;
    if (deltaSeconds > 0.1f) { Reset("large-delta"); return; }
    const Eigen::Vector3f position = component.block<3, 1>(0, 3);
    const float yaw = std::atan2(component(0, 2), component(2, 2));
    const float yawDelta = WrapYaw(yaw - previousYaw);
    if (historyValid && ((position - previousPosition).norm() > 100 || std::abs(yawDelta) > pi / 3))
    {
        Reset("teleported");
        return;
    }
    if (!historyValid)
    {
        previousPosition = position;
        previousVelocity = achievedVelocity;
        previousYaw = yaw;
        previousYawVelocity = 0;
        historyValid = true;
        state.reason = "warming-up";
        return;
    }
    const float currentYawVelocity = yawDelta / deltaSeconds;
    // 只补偿速度跳变，匀速/匀角速度不持续注入新的位移误差；首次观察不猜测旧速度。
    velocity -= achievedVelocity - previousVelocity;
    velocity.y() = 0;
    yawVelocity -= currentYawVelocity - previousYawVelocity;
    for (int axis : {0, 2}) Decay(state.worldTranslation[axis], velocity[axis], deltaSeconds);
    Decay(state.yawRadians, yawVelocity, deltaSeconds);
    bool saturated = false;
    const float distance = state.worldTranslation.norm();
    if (distance > maximumTranslation)
    {
        const Eigen::Vector3f outward = state.worldTranslation / distance;
        state.worldTranslation = outward * maximumTranslation;
        velocity -= outward * std::max(0.0f, outward.dot(velocity));
        saturated = true;
    }
    if (std::abs(state.yawRadians) > maximumYaw)
    {
        state.yawRadians = std::copysign(maximumYaw, state.yawRadians);
        if (state.yawRadians * yawVelocity > 0) yawVelocity = 0;
        saturated = true;
    }
    // 硬误差界优先于任意极端输入的 C1；饱和显式暴露，不能解释为连续性验收通过。
    state.active = state.worldTranslation.norm() > 0.0001f || std::abs(state.yawRadians) > 0.00001f ||
        velocity.norm() > 0.001f || std::abs(yawVelocity) > 0.0001f;
    state.reason = saturated ? "saturated" : state.active ? "smoothing" : "settled";
    previousPosition = position;
    previousVelocity = achievedVelocity;
    previousYaw = yaw;
    previousYawVelocity = currentYawVelocity;
}
}
