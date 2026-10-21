#pragma once

#include "motionHistory.h"
#include "motionRuntimeDatabase.h"
#include "skeletalPlayback.h"

namespace VL::Animation
{
// 原生步频修正的只读值反馈；速度为世界水平 cm/s，残差保留 actual-source*rate 的符号。
// 不代表脚部接触测量，也不拥有额外播放时钟。
struct MotionSpeedMatchState
{
    bool enabled = false;
    bool active = false;
    float sourceSpeed = 0;
    float actualSpeed = 0;
    float targetRate = 1;
    float rate = 1;
    float residualSpeed = 0;
    std::string reason = "disabled";
};

// GT 组件实例的原生搜索状态；业务筛选由 CLR 传入，历史与实际混合姿态由 World 提供。
// 不持有 World/Jolt/CLR，也不把数据库可写引用发布到渲染线程。
struct MotionRuntime
{
    std::shared_ptr<const MotionRuntimeDatabase> database;
    std::vector<std::string> allowedClips;
    bool actionLocked = false;
    bool matchSpeed = false;
    MotionSpeedMatchState speedMatch;
    uint64_t playbackIdentity = 0;
    MotionMatcher matcher;
    MotionSearchResult result;
    std::vector<float> rawQuery;
    std::vector<float> normalizedQuery;
    Eigen::Matrix4f queryRoot = Eigen::Matrix4f::Identity();
    std::vector<Eigen::Vector3f> queryFutureRoots;
    bool queryUsesAchievedVelocity = false;
    uint64_t searchCount = 0;
    bool queryReady = false;
};

// 输入来自已验证资产、物理反馈和唯一播放器；只算稳态有界倍率，不修改搜索器/时钟。
MotionSpeedMatchState ComputeMotionSpeedMatch(const MotionLoopVelocity& source,
    const MotionTrack& track, const Eigen::Matrix4f& component, bool enabled,
    bool blending, bool actionLocked, float currentRate, float deltaSeconds);

bool BuildRuntimeMotionQuery(const MotionTrack& track, std::vector<float>& raw,
    const MotionTrack* displayedHistory = nullptr);
MotionObservation ObserveMotion(const SkeletalPlayback& player, const Eigen::Matrix4f& component,
    const std::vector<size_t>& joints, double time);
bool MotionHistoryDiscontinuous(const MotionTrack& track, const MotionObservation& observation);
void TickMotionRuntime(MotionRuntime& runtime, MotionTrack& track, SkeletalPlayback& player,
    const Eigen::Matrix4f& component, float deltaSeconds);
}
