#pragma once

#include <Eigen/Dense>
#include <string>

namespace VL::Animation
{
struct RootOffsetState
{
    bool enabled = false;
    bool active = false;
    Eigen::Vector3f worldTranslation = Eigen::Vector3f::Zero();
    float yawRadians = 0;
    std::string reason = "disabled";
};

// 对物理后的根速度变化产生有限视觉滞后；只输出世界 XZ/yaw，不写 Actor/Jolt 或 MM 姿态。
class RootOffsetSolver
{
public:
    void SetEnabled(bool enabled);
    void Reset(const char* reason);
    // 调用入口已验证刚性正均匀 component、有限速度及非负 delta；本模块不重复作者校验。
    void Update(const Eigen::Matrix4f& component, const Eigen::Vector3f& achievedVelocity, float deltaSeconds);
    const RootOffsetState& State() const { return state; }

private:
    RootOffsetState state;
    Eigen::Vector3f velocity = Eigen::Vector3f::Zero();
    Eigen::Vector3f previousVelocity = Eigen::Vector3f::Zero();
    Eigen::Vector3f previousPosition = Eigen::Vector3f::Zero();
    float yawVelocity = 0, previousYaw = 0, previousYawVelocity = 0;
    bool historyValid = false;
};
}
