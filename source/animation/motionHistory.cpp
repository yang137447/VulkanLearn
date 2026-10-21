#include "motionHistory.h"
#include <cmath>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
MotionObservation Interpolate(const MotionObservation& a, const MotionObservation& b, double time)
{
    if (time <= a.time + 1e-10) return a;
    if (time >= b.time - 1e-10) return b;
    const float fraction = static_cast<float>((time - a.time) / (b.time - a.time));
    MotionObservation result;
    result.time = time;
    result.poseEpoch = b.poseEpoch;
    // 根轨迹允许跨换片段；该区间骨骼没有连续观测，不能产生有效速度查询。
    result.poseValid = a.poseValid && b.poseValid && a.poseEpoch == b.poseEpoch;
    result.root.block<3, 3>(0, 0) = Eigen::Quaternionf(a.root.block<3, 3>(0, 0))
        .slerp(fraction, Eigen::Quaternionf(b.root.block<3, 3>(0, 0))).toRotationMatrix();
    result.root.block<3, 1>(0, 3) = a.root.block<3, 1>(0, 3) * (1 - fraction) +
        b.root.block<3, 1>(0, 3) * fraction;
    for (size_t i = 0; i < b.joints.size(); ++i)
        result.joints.push_back(result.poseValid ? Eigen::Vector3f(
            a.joints[i] * (1 - fraction) + b.joints[i] * fraction) : b.joints[i]);
    return result;
}
}

void MotionHistory::Reset()
{
    initialized = false;
    samples.clear();
}

void MotionHistory::Record(const MotionObservation& observation)
{
    if (!std::isfinite(observation.time) || !observation.root.allFinite())
        throw std::invalid_argument("Motion observation requires finite time/transform");
    for (const auto& joint : observation.joints)
        if (!joint.allFinite()) throw std::invalid_argument("Nonfinite pose history");
    if (initialized && (observation.time < latest.time || observation.joints.size() != latest.joints.size()))
        throw std::invalid_argument("Motion history requires monotonic time and matching skeleton");
    // 传送阈值 2000 cm；时钟大间隔同样重建，禁止插出横跨旧 World 的轨迹。
    if (initialized && (observation.time - latest.time > 0.5 ||
        (observation.root.block<3, 1>(0, 3) - latest.root.block<3, 1>(0, 3)).norm() > 2000))
        Reset();
    if (!initialized)
    {
        latest = observation;
        samples.push_back(observation);
        nextSample = observation.time + 1.0 / 60;
        initialized = true;
        return;
    }
    if (observation.time == latest.time) return; // Pause/零倍率不产生重复历史。
    while (nextSample <= observation.time + 1e-9)
    {
        samples.push_back(Interpolate(latest, observation, std::min(nextSample, observation.time)));
        nextSample += 1.0 / 60;
    }
    latest = observation;
    while (samples.size() > 2 && samples[1].time < observation.time - 1.1) samples.pop_front();
}

bool MotionHistory::Sample(double time, MotionObservation& result) const
{
    if (!initialized || !std::isfinite(time) || time < samples.front().time - 1e-9 ||
        time > latest.time + 1e-9) return false;
    if (time >= latest.time - 1e-9) { result = latest; return true; }
    for (size_t index = 1; index < samples.size(); ++index)
        if (time <= samples[index].time + 1e-9)
        {
            result = Interpolate(samples[index - 1], samples[index], time);
            return true;
        }
    if (latest.time > samples.back().time)
    {
        result = Interpolate(samples.back(), latest, time);
        return true;
    }
    result = samples.back();
    return true;
}

Eigen::Matrix4f PredictCharacterRoot(const Eigen::Matrix4f& current,
    const Eigen::Vector3f& achieved, const Eigen::Vector3f& walk, bool grounded, float seconds)
{
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1 ||
        !achieved.allFinite() || !walk.allFinite())
        throw std::invalid_argument("Invalid character prediction input");
    Eigen::Matrix4f result = HeadingFrame(current);
    result(0, 3) += walk.x() * seconds;
    result(2, 3) += walk.z() * seconds;
    if (!grounded)
    {
        // 使用与物理相同的半隐式 60 Hz 重力积分，末端不足一步按其长度推进。
        float velocity = achieved.y(), remaining = seconds;
        while (remaining > 1e-7f)
        {
            const float step = std::min(1.0f / 60, remaining);
            velocity -= 981 * step;
            result(1, 3) += velocity * step;
            remaining -= step;
        }
    }
    return result;
}
}
