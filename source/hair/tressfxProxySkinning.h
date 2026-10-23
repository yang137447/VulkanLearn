#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

namespace VL::Hair
{
// 绑定姿态身体顶点的纯 CPU 视图。刻意不依赖 Animation/SkeletalAsset，让纯数学
// 模块（vl_hair_assets 与 hair_asset_tests）可以在没有 Vulkan/ozz 的情况下验证
// 权重转移；调用方负责把 SkinSection 的 CSR 转成这个视图。
struct SdfProxySkinSourceInfluence
{
    uint32_t joint = 0;
    float weight = 0.0f;
};

struct SdfProxySkinSource
{
    const Eigen::Vector3f* positions = nullptr;
    size_t positionCount = 0;
    // CSR 头：size == positionCount + 1；values 是 influences 的绝对下标。
    const uint32_t* influenceOffsets = nullptr;
    const SdfProxySkinSourceInfluence* influences = nullptr;
    // 骨架关节数；>0 时校验每个 joint 下标，并决定转移后 palette 的最小槽数。
    uint32_t jointCount = 0;
};

// `collision.mesh` 代理的逐顶点蒙皮权重。words 布局与 GPU SkinInfluences 一致：
//   words[0..vertexCount] = 每个顶点在 words 内的绝对起始 word
//   其后每两个 word 是 (joint, floatBits(weight))
// 现有 GPU 蒙皮布局是 CSR，不限制影响数；因此这里保留最近身体顶点的完整影响
// 列表并归一化，而不是截断到 4 骨（本资产身体单点最多 4 个正权重，所以实际
// 结果仍是 <= 4）。不重新分配到第二套骨骼下标空间：joint 直接索引
// SkinPose::matrices。
struct SdfProxySkinTransfer
{
    std::vector<uint32_t> words;
    uint32_t vertexCount = 0;
    uint32_t maxInfluences = 0;
    // 转移表引用到的关节上界（骨架关节数）；GPU palette 槽数必须覆盖它。
    uint32_t jointCount = 0;
    // 诊断/资产校验：代理顶点到最近身体顶点的最大距离（厘米）。正式代理实测
    // 均值约 0.22 cm、最大 0.93 cm（2,572 顶点对 235,490 顶点绑定身体）。
    float maxNearestDistanceCm = 0.0f;
    uint32_t uniqueBodyVertices = 0;
};

// 空间哈希最近点转移。对每个代理顶点找绑定姿态下最近的身体顶点，复制其完整
// 影响列表并归一化。等距时选全局下标更小的身体顶点，因此输出只取决于输入
// 顺序，和哈希桶遍历顺序无关。任何代理顶点在 maxNearestDistanceCm 内找不到
// 身体顶点、权重和 <= 0、坐标非有限都会抛 std::runtime_error。
SdfProxySkinTransfer BuildTressfxProxySkinTransfer(
    const std::vector<Eigen::Vector3f>& proxyPositions,
    const SdfProxySkinSource& body,
    float maxNearestDistanceCm);

// 用转移后的权重对一个绑定姿态代理顶点做 LBS。CPU 侧用它重建每帧代理包围盒，
// 也是 GPU splat shader 的逐操作镜像（权重和已归一化）。
Eigen::Vector3f TressfxProxySkinnedPosition(
    const SdfProxySkinTransfer& transfer,
    uint32_t vertex,
    const std::vector<Eigen::Matrix4f>& jointMatrices,
    const Eigen::Vector3f& bindPosition);

} // namespace VL::Hair
