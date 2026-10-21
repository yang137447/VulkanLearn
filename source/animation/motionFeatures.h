#pragma once

#include "animationAssets.h"
#include <nlohmann/json.hpp>

namespace VL::Animation
{
// 版本化特征定义；数据库与运行时查询共用厘米、Y-up、模型 +Z 前向及同一通道顺序。
// 权重只定义距离度量，不参与均值/方差估计；本模块不选择游戏动作。
struct FeatureSchema
{
    int version = 1;
    float sampleRate = 30;
    float velocityStep = 1.0f / 60;
    std::vector<float> trajectoryTimes{-0.4f, -0.2f, 0, 0.2f, 0.4f, 0.6f};
    std::vector<std::string> joints;
    float trajectoryPositionWeight = 1;
    float trajectoryFacingWeight = 1;
    float posePositionWeight = 1;
    float poseVelocityWeight = 1;
};

FeatureSchema ParseFeatureSchema(const nlohmann::json& json);
nlohmann::json FeatureSchemaJson(const FeatureSchema& schema);
FeatureSchema PreviewFeatureSchema(const SkeletonMesh& mesh);
std::vector<size_t> ResolveFeatureJoints(const SkeletonMesh& mesh, const FeatureSchema& schema);
std::vector<std::string> FeatureChannelNames(const FeatureSchema& schema);
std::vector<float> FeatureChannelWeights(const FeatureSchema& schema);
Eigen::Matrix4f HeadingFrame(const Eigen::Matrix4f& root);

// 循环按整周期根刚体变换延拓，非循环越界拒绝；绝不借用下一条动画。
std::vector<Eigen::Matrix4f> SampleMotionPose(
    const SkeletonMesh& mesh, const Clip& clip, double time, bool loop);
bool CanExtractClipFeatures(const Clip& clip, const FeatureSchema& schema, float time, bool loop,
    float playbackRate = 1);
std::vector<float> ExtractClipFeatures(const SkeletonMesh& mesh, const Clip& clip,
    const FeatureSchema& schema, float time, bool loop, float playbackRate = 1);

// 根局部位置和世界位移速度都投到当前 heading 空间；不能先逐帧减根再求速度。
std::vector<float> EncodeMotionFeatures(const FeatureSchema& schema,
    const Eigen::Matrix4f& root, const std::vector<Eigen::Matrix4f>& trajectory,
    const std::vector<Eigen::Vector3f>& jointPositions,
    const std::vector<Eigen::Vector3f>& jointVelocities);
}
