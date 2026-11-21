#pragma once

#include "animationAssets.h"

#include <Eigen/Core>
#include <cstdint>

namespace VL::Animation
{
// GT 外部姿态流的一帧。局部 TRS 按目标网格 joint 顺序排列；来源可以是
// MotionBricks、离线烘焙回放或其它生成式组件，SkeletalPlayback 不解释来源。
//
// rootMotionDelta/rootMotionYawDelta 与 clip 根运动使用同一约定：模型空间厘米、
// 绕 VL Y-up 的弧度增量，由调用方交给现有 C#/Jolt 根运动路径。
struct ExternalPoseFrame
{
    LocalPose locals;
    Eigen::Vector3f rootMotionDelta = Eigen::Vector3f::Zero();
    float rootMotionYawDelta = 0.0f;
    // true 表示与上一帧没有连续速度关系（首帧、传送、generation 重建）；
    // 播放器据此推进 discontinuity，避免把跳变算进速度历史。
    bool hardCut = false;
    // 生成这一帧 rootMotionDelta 时实际推进的时间。消费端可能晚一帧才取走
    // 姿态，不能用消费时的 delta 反推速度。
    float deltaSeconds = 0.0f;
    // 仅用于诊断/遥测，不参与姿态求值。
    float sourceTime = 0.0f;
};
}
