#pragma once

#include <cstdint>
#include <memory>
#include <vector>
#include <Eigen/Dense>

namespace VL::Animation
{
// 快照只共享不可变 palette/边界，不携带播放器、ozz context 或 Vulkan 句柄。
struct SkinPose
{
    std::vector<Eigen::Matrix4f> matrices;
    // 与 section 无关的全局 bind→pose：models[joint] * inverse(restModel[joint])。
    // matrices 是逐 section 的 models[joint] * inverseBind[joint]；section 没有
    // 引用的关节会保留单位 inverseBind，其值退化为绝对 model 矩阵，不能跨
    // section 使用。TressFX 代理权重可能引用多个 body section 的关节，因此
    // 碰撞/胶囊使用这份全骨架矩阵。仅当 BuildSkinPose 收到 joints 时填充。
    std::vector<Eigen::Matrix4f> bindToPoseMatrices;
    Eigen::Vector3f boundsMin = Eigen::Vector3f::Zero();
    Eigen::Vector3f boundsMax = Eigen::Vector3f::Zero();
    uint64_t discontinuity = 0;
    uint64_t playbackIdentity = 0;
};

// 只在成功提交后推进，不能把被 mailbox 丢弃的 GT 姿态当作上一渲染帧。
struct SubmittedSkinPose
{
    std::shared_ptr<const SkinPose> pose;
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    uint64_t submission = 0;

    void RetainIfSubmittedAt(uint64_t latestSubmission)
    {
        // 隐藏后重现或离线模式返回时，旧对象姿态不能充当紧邻上一帧的速度历史。
        if (submission != latestSubmission) pose.reset();
    }

    const SkinPose& Previous(const SkinPose& current) const
    {
        return Matches(current) ? *pose : current;
    }
    Eigen::Matrix4f PreviousModel(const SkinPose& current, const Eigen::Matrix4f& currentModel) const
    {
        return Matches(current) ? model : currentModel;
    }
    void Commit(std::shared_ptr<const SkinPose> submitted,
        const Eigen::Matrix4f& submittedModel, uint64_t serial)
    {
        pose = std::move(submitted);
        model = submittedModel;
        submission = serial;
    }
private:
    bool Matches(const SkinPose& current) const
    {
        // Stop/组件重建会产生新播放器，即使局部 discontinuity 相同也不能继承速度历史。
        return pose && pose->playbackIdentity == current.playbackIdentity &&
            pose->discontinuity == current.discontinuity;
    }
};
}
