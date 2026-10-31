#pragma once

#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "animation/skinPose.h"

namespace VL::Hair
{
// 把冻结的角色模型空间姿态转换到发丝模拟空间；只持有值副本，
// 不查询 World、播放器或 GPU；GPU 的世界空间外力由参数构建处单独换基。
struct TressfxFrameContext
{
    Eigen::Matrix4f skinningMatrix = Eigen::Matrix4f::Identity();
    std::vector<Eigen::Matrix4f> jointMatrices;
    float deltaSeconds = 0.0f;
    uint64_t playbackIdentity = 0;
    uint64_t discontinuity = 0;
};

// 模型矩阵来自已校验的场景 TRS，rootJoint 来自加载期的目标骨架解析。
// 三套消费者（根点、绑定胶囊、SDF 代理）必须共用这份相对 palette；
// 只转换根点会让角色整体移动后，碰撞体仍留在原来的模型空间。
inline void UpdateTressfxFrameContext(
    TressfxFrameContext& context,
    const Eigen::Matrix4f& hairModel,
    const Eigen::Matrix4f& sourceModel,
    const Animation::SkinPose& pose,
    uint32_t rootJoint,
    float deltaSeconds)
{
    context.deltaSeconds = deltaSeconds;
    context.playbackIdentity = pose.playbackIdentity;
    context.discontinuity = pose.discontinuity;
    const Eigen::Matrix4f sourceToHair = hairModel.inverse() * sourceModel;
    const std::vector<Eigen::Matrix4f>& sourcePalette =
        pose.bindToPoseMatrices.empty() ? pose.matrices : pose.bindToPoseMatrices;
    // ObjectDescriptors 持有 context，resize 复用稳定骨架的容量，不逐帧分配 palette。
    context.jointMatrices.resize(sourcePalette.size());
    for (size_t joint = 0; joint < sourcePalette.size(); ++joint)
        context.jointMatrices[joint] = sourceToHair * sourcePalette[joint];
    context.skinningMatrix = context.jointMatrices.at(rootJoint);
}

inline TressfxFrameContext BuildTressfxFrameContext(
    const Eigen::Matrix4f& hairModel,
    const Eigen::Matrix4f& sourceModel,
    const Animation::SkinPose& pose,
    uint32_t rootJoint,
    float deltaSeconds)
{
    TressfxFrameContext context;
    UpdateTressfxFrameContext(context, hairModel, sourceModel, pose,
        rootJoint, deltaSeconds);
    return context;
}

// SSBO 仍由 VF 按当前 hairModel 绘制；换基保持三份历史的世界位置，
// 让 Actor 整体平移/旋转像世界空间 solver 一样产生惯性。缩放只保证点换基，
// 不保证局部各向同性长度、碰撞半径和速度上限等度量与世界空间 solver 等价。
class TressfxModelHistory
{
public:
    bool HasPlacementChange(const Eigen::Matrix4f& currentModel) const
    {
        return initialized && (lastModel.array() != currentModel.array()).any();
    }

    Eigen::Matrix4f TransformToCurrentLocal(
        const Eigen::Matrix4f& currentModel) const
    {
        if (!HasPlacementChange(currentModel))
            return Eigen::Matrix4f::Identity();
        return currentModel.inverse() * lastModel;
    }

    // 必须在录制 Integration 后提交；HasWork/暂停查询不能提前吞掉换基。
    void Commit(const Eigen::Matrix4f& currentModel)
    {
        lastModel = currentModel;
        initialized = true;
    }

private:
    bool initialized = false;
    Eigen::Matrix4f lastModel = Eigen::Matrix4f::Identity();
};

// RT 可重绘上一份有效 RenderScene；冻结 dt 只属于它对应的 GT 帧，不能
// 每次重绘都再推进一次。当前 GT 等待 RT，每帧提交一份快照；若 mailbox
// 覆盖了未消费帧，保留 latest-wins 语义，只推进最新帧，不补算旧姿态。
class TressfxFrameClock
{
public:
    float DeltaForFrame(
        uint64_t worldGeneration, uint64_t frameIndex, float deltaSeconds) const
    {
        return initialized && consumedWorldGeneration == worldGeneration &&
            consumedFrameIndex == frameIndex ? 0.0f : deltaSeconds;
    }

    float Consume(uint64_t worldGeneration, uint64_t frameIndex, float deltaSeconds)
    {
        const float result = DeltaForFrame(worldGeneration, frameIndex, deltaSeconds);
        initialized = true;
        consumedWorldGeneration = worldGeneration;
        consumedFrameIndex = frameIndex;
        return result;
    }

private:
    bool initialized = false;
    uint64_t consumedWorldGeneration = 0;
    uint64_t consumedFrameIndex = 0;
};
}
