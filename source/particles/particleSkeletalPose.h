#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <Eigen/Dense>

namespace VL::Animation
{
struct SkeletalAsset;
}

namespace VL
{

// 骨骼网格粒子每帧姿态快照的共享契约。
//
// 为什么需要它：SkeletalMesh 完整实例、SkeletalMeshTris、SkeletalMeshBones
// 三种 renderMode 可能同时引用同一份 SkeletalAsset，而 CPU 蒙皮、三角面源、
// 骨段源都必须看到同一时刻的关节模型矩阵，否则同一骨骼网格拆出的三角面
// 会与骨段错位。姿态时间由发射器自己的 elapsedSeconds 驱动，因此按
// (资产, 片段, 时间) 缓存：同一帧命中缓存，不与其它发射器共享时钟。
struct ParticleSkeletalPoseKey
{
    const Animation::SkeletalAsset* asset = nullptr;
    std::size_t clipIndex = 0;
    float time = 0.0f;

    bool operator==(const ParticleSkeletalPoseKey& other) const
    {
        // 时间比较用位级相等：姿态采样是确定性的，同一 float 输入必须命中，
        // 不同输入宁可多算一次也不能复用错误姿态。
        return asset == other.asset &&
            clipIndex == other.clipIndex &&
            (time == other.time ||
             (std::isnan(time) && std::isnan(other.time)));
    }
};

// bind/rest pose 或片段采样后的关节模型矩阵（模型空间，cm）。
std::shared_ptr<const std::vector<Eigen::Matrix4f>>
SampleParticleSkeletalModels(
    const Animation::SkeletalAsset& asset,
    std::size_t clipIndex,
    float time,
    bool loop);

// 把累计时钟折回片段时长；loop=false 时夹到片段末尾。
float WrapParticleSkeletalPoseTime(
    float baseTime,
    float elapsedSeconds,
    float rate,
    float duration,
    bool loop);

} // namespace VL
