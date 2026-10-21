#pragma once

#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <vector>

namespace VL::Animation
{
// 局部父空间 TRS 及其导数；角速度为父空间 rad/s，scale 导数使用 log(scale)/s。
struct JointKinematics
{
    Eigen::Vector3f translation = Eigen::Vector3f::Zero();
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
    Eigen::Vector3f scale = Eigen::Vector3f::Ones();
    Eigen::Vector3f linearVelocity = Eigen::Vector3f::Zero();
    Eigen::Vector3f angularVelocity = Eigen::Vector3f::Zero();
    Eigen::Vector3f logScaleVelocity = Eigen::Vector3f::Zero();
};

Eigen::Vector3f RotationLog(Eigen::Quaternionf rotation);
Eigen::Quaternionf RotationExp(const Eigen::Vector3f& vector);

// 接收已采样局部运动，输出有限时惯性偏移；不采样片段、不推进播放器或 Actor 时钟。
// 沿用 Holden 的差值惯性化思想，改用五次 Hermite 保留现有有限过渡终点；初始差加速度取零。
class FiniteInertialization
{
public:
    void Begin(const std::vector<JointKinematics>& source,
        const std::vector<JointKinematics>& target, float duration);
    std::vector<JointKinematics> Evaluate(const std::vector<JointKinematics>& target, float elapsed) const;
    void Clear() { offsets.clear(); }
    bool Empty() const { return offsets.empty(); }

private:
    struct Offset
    {
        Eigen::Vector3f position, positionVelocity;
        Eigen::Vector3f rotation, rotationVelocity;
        Eigen::Vector3f logScale, logScaleVelocity;
    };
    std::vector<Offset> offsets;
    float duration = 0;
};
}
