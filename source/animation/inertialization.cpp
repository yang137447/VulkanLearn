#include "inertialization.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
Eigen::Matrix3f CrossMatrix(const Eigen::Vector3f& vector)
{
    Eigen::Matrix3f result;
    result << 0, -vector.z(), vector.y(), vector.z(), 0, -vector.x(), -vector.y(), vector.x(), 0;
    return result;
}

Eigen::Matrix3f LeftJacobian(const Eigen::Vector3f& vector, bool inverse)
{
    // Solà 等《A micro Lie theory》SO(3) 左雅可比及逆；小角度使用同阶展开避免消减。
    // https://arxiv.org/abs/1812.01537
    const float angle = vector.norm();
    const auto cross = CrossMatrix(vector);
    if (angle < 0.001f)
        return Eigen::Matrix3f::Identity() + (inverse ? -0.5f : 0.5f) * cross +
            (inverse ? 1.0f / 12 : 1.0f / 6) * cross * cross;
    const float square = angle * angle;
    if (inverse)
        return Eigen::Matrix3f::Identity() - 0.5f * cross +
            ((1 - 0.5f * angle / std::tan(angle * 0.5f)) / square) * cross * cross;
    return Eigen::Matrix3f::Identity() + ((1 - std::cos(angle)) / square) * cross +
        ((angle - std::sin(angle)) / (square * angle)) * cross * cross;
}

void EvaluateOffset(const Eigen::Vector3f& position, const Eigen::Vector3f& velocity,
    float duration, float elapsed, Eigen::Vector3f& result, Eigen::Vector3f& derivative)
{
    const float u = elapsed / duration;
    const float square = u * u, cube = square * u, fourth = cube * u, fifth = fourth * u;
    // 六个 Hermite 边界：起点值/速度指定、起点差加速度零；终点值/速度/加速度全零。
    result = (1 - 10 * cube + 15 * fourth - 6 * fifth) * position +
        duration * (u - 6 * cube + 8 * fourth - 3 * fifth) * velocity;
    derivative = ((-30 * square + 60 * cube - 30 * fourth) / duration) * position +
        (1 - 18 * square + 32 * cube - 15 * fourth) * velocity;
}
}

Eigen::Vector3f RotationLog(Eigen::Quaternionf rotation)
{
    if (rotation.w() < 0) rotation.coeffs() *= -1;
    const float sine = rotation.vec().norm();
    if (sine < 0.000001f) return 2 * rotation.vec();
    return rotation.vec() * (2 * std::atan2(sine, rotation.w()) / sine);
}

Eigen::Quaternionf RotationExp(const Eigen::Vector3f& vector)
{
    const float angle = vector.norm();
    if (angle < 0.000001f)
        return Eigen::Quaternionf(1, vector.x() * 0.5f, vector.y() * 0.5f, vector.z() * 0.5f).normalized();
    return Eigen::Quaternionf(Eigen::AngleAxisf(angle, vector / angle));
}

void FiniteInertialization::Begin(const std::vector<JointKinematics>& source,
    const std::vector<JointKinematics>& target, float seconds)
{
    if (source.empty() || source.size() != target.size() || !std::isfinite(seconds) || seconds <= 0)
        throw std::invalid_argument("Inertialization requires equal nonempty poses and positive finite duration");
    // Holden 的源/目标差值原则；衰减曲线刻意不同：这里在指定终点精确归零。
    // https://theorangeduck.com/page/spring-roll-call#inertialization
    std::vector<Offset> next(source.size());
    for (size_t joint = 0; joint < source.size(); ++joint)
    {
        const auto& from = source[joint];
        const auto& to = target[joint];
        auto& offset = next[joint];
        const Eigen::Quaternionf rotation = (from.rotation * to.rotation.conjugate()).normalized();
        offset.position = from.translation - to.translation;
        offset.positionVelocity = from.linearVelocity - to.linearVelocity;
        offset.rotation = RotationLog(rotation);
        // R = Exp(r) R_target，因此 ω = J_left(r) r' + Exp(r) ω_target；
        // 必须搬运目标角速度，直接相减在多轴大角度过渡时不满足 C1。
        offset.rotationVelocity = LeftJacobian(offset.rotation, true) *
            (from.angularVelocity - rotation * to.angularVelocity);
        offset.logScale = from.scale.array().log().matrix() - to.scale.array().log().matrix();
        offset.logScaleVelocity = from.logScaleVelocity - to.logScaleVelocity;
    }
    offsets.swap(next);
    duration = seconds;
}

std::vector<JointKinematics> FiniteInertialization::Evaluate(
    const std::vector<JointKinematics>& target, float elapsed) const
{
    if (offsets.empty()) return target;
    if (target.size() != offsets.size() || !std::isfinite(elapsed) || elapsed < 0)
        throw std::invalid_argument("Inertialization target or elapsed time is invalid");
    if (elapsed >= duration) return target;
    auto result = target;
    for (size_t joint = 0; joint < result.size(); ++joint)
    {
        auto& output = result[joint];
        const auto& offset = offsets[joint];
        Eigen::Vector3f value, derivative;
        EvaluateOffset(offset.position, offset.positionVelocity, duration, elapsed, value, derivative);
        output.translation += value;
        output.linearVelocity += derivative;
        EvaluateOffset(offset.rotation, offset.rotationVelocity, duration, elapsed, value, derivative);
        const auto correction = RotationExp(value);
        output.rotation = (correction * output.rotation).normalized();
        output.angularVelocity = LeftJacobian(value, false) * derivative + correction * output.angularVelocity;
        EvaluateOffset(offset.logScale, offset.logScaleVelocity, duration, elapsed, value, derivative);
        output.scale = (output.scale.array().log() + value.array()).exp().matrix();
        output.logScaleVelocity += derivative;
    }
    return result;
}
}
