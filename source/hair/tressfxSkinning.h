#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Eigen/Dense>

#include "hair/tfxBoneLoader.h"
#include "hair/tressfxCollision.h"
#include "hair/tressfxProxySkinning.h"

namespace VL::Hair
{
// GPU 逐 strand 的 4 骨影响，std430 行布局与 shader 的
// `struct GpuStrandBones { uvec4 indices; vec4 weights; }` 对齐。
struct TressfxBoneWeightGpu
{
    uint32_t joints[4] = {0, 0, 0, 0};
    float weights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(TressfxBoneWeightGpu) == 32,
    "TressFX GPU bone weights must stay 32 bytes");

// 逐 strand 的 GPU 4 骨表示。按权重降序取前 4 再归一化；没有可用影响或权重和
// 非正时回退到 fallbackJoint（权重 1），不能留下零矩阵。排序用 stable_sort，
// 等权时保留输入顺序，保证 CPU 与 GPU 两条消费路径拿到同一组骨。
// 抽成纯函数的目的：上传缓冲（RenderableObject）与模块测试共用同一份语义。
inline TressfxBoneWeightGpu TressfxPackStrandBoneWeights(
    const TressfxStrandBoneWeights& weights, uint32_t fallbackJoint)
{
    TressfxBoneWeightGpu packed;
    std::vector<std::pair<float, uint32_t>> ordered;
    ordered.reserve(weights.joints.size());
    const size_t count = std::min(weights.joints.size(), weights.weights.size());
    for (size_t influence = 0; influence < count; ++influence)
        ordered.emplace_back(weights.weights[influence], weights.joints[influence]);
    std::stable_sort(ordered.begin(), ordered.end(),
        [](const auto& left, const auto& right) { return left.first > right.first; });
    if (ordered.size() > 4)
        ordered.resize(4);
    float sum = 0.0f;
    for (const auto& entry : ordered)
        sum += entry.first;
    if (!(sum > 0.0f))
    {
        packed.joints[0] = fallbackJoint;
        packed.weights[0] = 1.0f;
        return packed;
    }
    for (size_t influence = 0; influence < ordered.size(); ++influence)
    {
        packed.joints[influence] = ordered[influence].second;
        packed.weights[influence] = ordered[influence].first / sum;
    }
    return packed;
}

// 把网格转移得到的 CSR word 流转成逐 strand 权重。未匹配查询点得到空权重，
// 由调用方按根回退 rootBone；这里不静默补权，也不改 strand 数量。
inline std::vector<TressfxStrandBoneWeights> TressfxStrandBoneWeightsFromTransfer(
    const SdfProxySkinTransfer& transfer)
{
    std::vector<TressfxStrandBoneWeights> result(transfer.vertexCount);
    if (transfer.vertexCount == 0)
        return result;
    if (transfer.words.size() < static_cast<size_t>(transfer.vertexCount) + 1)
        throw std::runtime_error("TressFX skin transfer word stream is truncated");
    for (uint32_t vertex = 0; vertex < transfer.vertexCount; ++vertex)
    {
        const uint32_t begin = transfer.words[vertex];
        const uint32_t end = transfer.words[vertex + 1];
        if (begin > end || static_cast<size_t>(end) > transfer.words.size())
            throw std::runtime_error("TressFX skin transfer word range is invalid");
        for (uint32_t word = begin; word + 1 < end; word += 2)
        {
            float weight = 0.0f;
            const uint32_t bits = transfer.words[word + 1];
            std::memcpy(&weight, &bits, sizeof(weight));
            result[vertex].joints.push_back(transfer.words[word]);
            result[vertex].weights.push_back(weight);
        }
    }
    return result;
}

// 组件挂载时的根点权重来源：组件显式值 > SM 显式 weights > 组件绑定默认 mesh。
// 显式 .tfxbone 优先于“组件绑定自动升级”：只有显式 mesh 才会与文件冲突。
// 返回 0=rootBone、1=mesh；-2 表示 mesh 与显式 .tfxbone 冲突，由调用方按作者
// 冲突拒绝（纯函数不抛，便于模块测试）。
inline int ResolveTressfxRootWeightChoice(bool componentBound, bool smWeightsMesh,
    bool smWeightsAuthored, std::string_view componentWeights, bool hasBoneWeightsFile)
{
    bool mesh = smWeightsAuthored ? smWeightsMesh
                                  : (componentBound && !hasBoneWeightsFile);
    if (componentWeights == "rootBone") mesh = false;
    else if (componentWeights == "mesh") mesh = true;
    if (mesh && hasBoneWeightsFile) return -2;
    return mesh ? 1 : 0;
}

// 解析完成后的发丝根点绑定。jointIndex 是源角色“目标骨架”的关节下标，
// 与 Animation::SkinPose::matrices 的下标空间一致；sourceSceneObject 是场景对象
// 身份（不是单 section 的 debugName），运行时据此在冻结快照里找 draw packet。
struct TressfxSkinningBinding
{
    bool enabled = false;
    std::string sourceSceneObject;
    std::string rootBone;
    uint32_t jointIndex = 0;
    // 与 config.capsules 同下标的关节下标；-1 表示该胶囊没有 bone、保持静态。
    // 与 jointIndex 共用同一个源对象与骨架，运行时矩阵也来自同一份 SkinPose。
    std::vector<int32_t> capsuleJointIndices;
    // 可选逐 strand 多骨权重（.tfxbone）。空 = 单根骨；非空时长度必须等于 guide
    // 数，元素可为空权重（该 strand 回退根骨）。权重已归一化到 1。
    std::vector<TressfxStrandBoneWeights> strandBones;
};

// 按名字查找目标骨架关节。名字大小写敏感，与 Animation::SkeletonAsset::JointIndex
// 和 FBX 导入器一致。jointNames 为空表示源对象没有 SK：这是硬错误，不是回退。
// usage 描述调用方（如 "simulation.skinning root bone"、"collision.capsules[0].bone"），
// 让错误消息同时带角色、骨名、对象名与发丝 SM 资产路径。未知骨同样是硬错误。
inline uint32_t ResolveTressfxJointByName(
    const std::vector<std::string>& jointNames,
    std::string_view sourceSceneObject,
    std::string_view boneName,
    std::string_view usage,
    std::string_view hairAssetPath)
{
    if (jointNames.empty())
        throw std::runtime_error(
            "TressFX simulation.skinning source object \"" +
            std::string(sourceSceneObject) +
            "\" has no skeleton (SK): " + std::string(hairAssetPath));
    for (size_t index = 0; index < jointNames.size(); ++index)
    {
        if (jointNames[index] == boneName)
            return static_cast<uint32_t>(index);
    }
    throw std::runtime_error(
        "TressFX " + std::string(usage) + " \"" + std::string(boneName) +
        "\" not found in source object \"" + std::string(sourceSceneObject) +
        "\" skeleton: " + std::string(hairAssetPath));
}

// 根骨入口：错误消息保持既有措辞，供加载期与模块测试共用。
inline uint32_t ResolveTressfxRootJoint(
    const std::vector<std::string>& jointNames,
    std::string_view sourceSceneObject,
    std::string_view rootBone,
    std::string_view hairAssetPath)
{
    return ResolveTressfxJointByName(jointNames, sourceSceneObject, rootBone,
        "simulation.skinning root bone", hairAssetPath);
}

// 每个胶囊按自己的关节矩阵变换端点；没有关节（-1）、关节下标越出当前 pose
// 或胶囊数量不匹配时保留作者绑定空间的值。GPU pass 与测试共用这一份语义，
// 避免 Host 与 shader 对“未绑定胶囊”产生两种理解。
inline std::vector<TressfxCollider> TransformTressfxCapsulesByBones(
    const std::vector<TressfxCollider>& capsules,
    const std::vector<int32_t>& capsuleJointIndices,
    const std::vector<Eigen::Matrix4f>& jointMatrices)
{
    std::vector<TressfxCollider> transformed = capsules;
    const size_t boundCount =
        std::min(capsules.size(), capsuleJointIndices.size());
    for (size_t index = 0; index < boundCount; ++index)
    {
        const int32_t joint = capsuleJointIndices[index];
        if (joint < 0 || static_cast<size_t>(joint) >= jointMatrices.size())
            continue;
        transformed[index] = TransformTressfxCapsuleByBone(
            capsules[index], jointMatrices[static_cast<size_t>(joint)]);
    }
    return transformed;
}

// CPU 侧对 TressFXSimulationIntegration InitialPos 的镜像，只用于纯数学测试。
// TressFX 对每个顶点先算 InitialPos = ApplyVertexBoneSkinning(RestPos)，不可移动点
// （根前缀，或 VL 扩展的逆质量 <= 0）整步都写回 InitialPos。可移动点不会调用
// 这个函数；GPU pass 必须保持同一规则。
// 单根骨版本的 InitialPos；声明 .tfxbone 时 shader 改用下面的加权版本。
inline Eigen::Vector3f TressfxInitialPosition(
    const Eigen::Matrix4f& skinningMatrix,
    const Eigen::Vector3f& restPosition)
{
    return (skinningMatrix * restPosition.homogeneous()).head<3>();
}

// 逐 strand 多骨矩阵：Σ palette[joint_i] * weight_i。权重已在导入期归一化；
// 与源 ApplyVertexBoneSkinning 的 BoneMatrix 累加/WeightSum 除法等价。
inline Eigen::Matrix4f TressfxBlendedBoneMatrix(
    const TressfxStrandBoneWeights& weights,
    const std::vector<Eigen::Matrix4f>& palette)
{
    Eigen::Matrix4f blended = Eigen::Matrix4f::Zero();
    const size_t count = std::min(weights.joints.size(), weights.weights.size());
    for (size_t i = 0; i < count; ++i)
        blended += palette[weights.joints[i]] * weights.weights[i];
    return blended;
}

inline Eigen::Vector3f TressfxInitialPosition(
    const TressfxStrandBoneWeights& weights,
    const std::vector<Eigen::Matrix4f>& palette,
    const Eigen::Vector3f& restPosition)
{
    return (TressfxBlendedBoneMatrix(weights, palette) *
        restPosition.homogeneous()).head<3>();
}

} // namespace VL::Hair
