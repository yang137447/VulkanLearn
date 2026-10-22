#pragma once

#include "animationAssets.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace VL::Animation
{
struct RetargetJoint
{
    std::string source;
    std::string target;
};

// JSON 的局部旋转偏移按 bindLocal * offset 应用；不改变网格 bind 或 inverse bind。
struct RetargetRotationOffset
{
    std::string joint;
    Eigen::Quaternionf rotation = Eigen::Quaternionf::Identity();
};

// 具名校准参考姿态，供编译后的 retarget 消费；不拥有片段、蒙皮数据或播放时钟。
struct RetargetPose
{
    std::string name = "BindPose";
    std::vector<RetargetRotationOffset> rotationOffsets;
};

// 显式映射源/目标骨名与 SK 身份；不猜测名称，不持有文件或播放器。
// 旋转在校准参考姿态的模型空间转换，目标骨长和局部平移/缩放保留。
struct RetargetProfile
{
    std::string sourceSkeleton;
    std::string targetSkeleton;
    std::string sourceRoot;
    std::string targetRoot;
    std::vector<RetargetJoint> joints;
    RetargetPose sourcePose;
    RetargetPose targetPose;

    void Validate(const SkeletonMesh& source, const SkeletonMesh& target) const;
};

RetargetProfile ParseRetargetProfile(const nlohmann::json& json, std::string_view basePath);

// 在资产/播放器创建边界编译名称和参考旋转，逐帧只消费索引与源姿态。
// 输入骨架需来自正式导入，运行期间保持不可变；不执行链 IK 或接触修正。
class CompiledRetarget
{
public:
    CompiledRetarget(const SkeletonMesh& source, const SkeletonMesh& target,
        const RetargetProfile& profile);
    LocalPose Apply(const LocalPose& sourcePose) const;
    const LocalPose& SourceReference() const { return sourceReference; }
    const LocalPose& TargetReference() const { return targetReference; }

private:
    std::vector<int> sourceParents;
    std::vector<int> targetParents;
    std::vector<int> targetToSource;
    std::vector<Eigen::Quaternionf> sourceReferenceRotations;
    std::vector<Eigen::Quaternionf> targetReferenceRotations;
    LocalPose sourceRest;
    LocalPose sourceReference;
    LocalPose targetReference;
};

LocalPose RetargetLocalPose(const SkeletonMesh& source, const SkeletonMesh& target,
    const RetargetProfile& profile, const LocalPose& sourcePose);
}
