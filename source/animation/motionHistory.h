#pragma once

#include "motionFeatures.h"
#include <deque>

namespace VL::Animation
{
// 物理后、混合后/IK 前的世界空间观察；固定时间重采样，不把渲染帧序号当时间。
// epoch 隔离硬切/seek 的骨骼差分，真实根轨迹仍可跨正常换片段保留。
struct MotionObservation
{
    double time = 0;
    uint64_t poseEpoch = 0;
    bool poseValid = true;
    Eigen::Matrix4f root = Eigen::Matrix4f::Identity();
    std::vector<Eigen::Vector3f> joints;
};

class MotionHistory
{
public:
    void Reset();
    void Record(const MotionObservation& observation);
    bool Sample(double time, MotionObservation& result) const;
    size_t Size() const { return samples.size(); }
    double LatestTime() const { return latest.time; }
private:
    bool initialized = false;
    MotionObservation latest;
    double nextSample = 0;
    std::deque<MotionObservation> samples;
};

// 对齐当前 CharacterVirtual 规则：水平 walk 瞬时响应、无额外制动或转向平滑；
// 地面锁高，空中积分重力。不作未来碰撞/台阶/移动支撑求解。
Eigen::Matrix4f PredictCharacterRoot(const Eigen::Matrix4f& current,
    const Eigen::Vector3f& achievedVelocity, const Eigen::Vector3f& walkVelocity,
    bool grounded, float futureSeconds);

// 每个实际播放组件独立拥有历史，World/Stop/组件重建通过 playbackIdentity 隔离。
struct MotionTrack
{
    uint64_t playbackIdentity = 0;
    double time = 0;
    MotionHistory history;
    FeatureSchema schema;
    std::vector<size_t> joints;
    Eigen::Vector3f achievedVelocity = Eigen::Vector3f::Zero();
    Eigen::Vector3f walkVelocity = Eigen::Vector3f::Zero();
    bool grounded = true;
};
}
