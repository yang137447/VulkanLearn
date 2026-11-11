#include "skeletalPlaybackInternal.h"

#include <cstring>
#include <limits>
#include <stdexcept>

namespace VL::Animation
{
namespace
{
// 把绑定空间 AABB 的 8 个角点用关节 palette 矩阵变换后并入并集。
// 保守性来自“盒子包含顶点 + 仿射变换保持包含关系”，不依赖权重分布。
void MergeJointBounds(const Eigen::Matrix4f& matrix, const JointBindBounds& bounds,
    Eigen::Vector3f& min, Eigen::Vector3f& max)
{
    const Eigen::Matrix3f linear = matrix.block<3, 3>(0, 0);
    const Eigen::Vector3f translation = matrix.block<3, 1>(0, 3);
    for (int corner = 0; corner < 8; ++corner)
    {
        const Eigen::Vector3f point(
            (corner & 1) ? bounds.max.x() : bounds.min.x(),
            (corner & 2) ? bounds.max.y() : bounds.min.y(),
            (corner & 4) ? bounds.max.z() : bounds.min.z());
        const Eigen::Vector3f transformed = linear * point + translation;
        min = min.cwiseMin(transformed);
        max = max.cwiseMax(transformed);
    }
}

// 权重和偏差修正：LBS 的精确位置是 Σ w_i M_i p，而 Σ w_i 只保证在
// [1-e,1+e]（导入器按 WeightTolerance 校验）。把凸组合按该区间绕原点缩放后
// 取包围盒，保证权重未严格归一化时精确结果仍在盒内。
void ExpandForWeightSum(Eigen::Vector3f& min, Eigen::Vector3f& max, float error)
{
    if (!(error > 0)) return;
    const float low = 1.0f - error;
    const float high = 1.0f + error;
    min = min.cwiseMin(min * low).cwiseMin(min * high);
    max = max.cwiseMax(max * low).cwiseMax(max * high);
}

// 全骨架 bind→pose：与 section 无关，跨 section 的代理权重可以直接使用。
// section 的 inverseBind 只对该 section 实际引用的关节有效；其余关节在
// SkinPose::matrices 里退化为绝对 model 矩阵。这里用 restModel 的逆重建。
void FillBindToPoseMatrices(SkinPose& pose,
    const std::vector<Eigen::Matrix4f>& models, const std::vector<Joint>* joints)
{
    if (joints == nullptr || joints->size() != models.size())
        return;
    pose.bindToPoseMatrices.reserve(models.size());
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        const Eigen::Matrix4f bindInverse = joints->at(joint).restModel.inverse();
        const Eigen::Matrix4f bindToPose = models[joint] * bindInverse;
        if (!bindToPose.allFinite())
            throw std::runtime_error("Skin pose has a non-finite bind-to-pose matrix");
        pose.bindToPoseMatrices.push_back(bindToPose);
    }
}
}

Eigen::Matrix4f BlendSkinMatrix(const SkinSection& section, size_t vertex,
    const std::vector<Eigen::Matrix4f>& palette)
{
    Eigen::Matrix4f blended = Eigen::Matrix4f::Zero();
    for (uint32_t i = section.influenceOffsets.at(vertex);
         i < section.influenceOffsets.at(vertex + 1); ++i)
    {
        const auto& influence = section.influences.at(i);
        blended += influence.weight * palette.at(influence.joint);
    }
    return blended;
}

std::shared_ptr<const SkinPose> BuildSkinPoseReference(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity,
    uint64_t playbackIdentity, const std::vector<Joint>* joints)
{
    if (models.size() != section.inverseBinds.size() || section.vertices.empty())
        throw std::invalid_argument("Skin pose requires a matching skeleton and nonempty section");
    auto pose = std::make_shared<SkinPose>();
    pose->discontinuity = discontinuity;
    pose->playbackIdentity = playbackIdentity;
    for (size_t i = 0; i < models.size(); ++i)
        pose->matrices.push_back(models[i] * section.inverseBinds[i]);
    FillBindToPoseMatrices(*pose, models, joints);
    pose->boundsMin = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    pose->boundsMax = -pose->boundsMin;
    for (size_t vertex = 0; vertex < section.vertices.size(); ++vertex)
    {
        // 只累加仿射的 3x4，避免 Debug 下逐顶点 Eigen block/表达式构造。
        // 布局仍为列优先，权重次序与参考 BlendSkinMatrix/GPU 完全相同。
        float affine[12]{};
        for (uint32_t i = section.influenceOffsets[vertex];
             i < section.influenceOffsets[vertex + 1]; ++i)
        {
            const auto& influence = section.influences[i];
            const float* matrix = pose->matrices[influence.joint].data();
            for (int column = 0; column < 4; ++column)
                for (int row = 0; row < 3; ++row)
                    affine[column * 3 + row] += influence.weight * matrix[column * 4 + row];
        }
        const auto& source = section.vertices[vertex].position;
        Eigen::Vector3f position;
        for (int row = 0; row < 3; ++row)
            position[row] = affine[row] * source.x() + affine[3 + row] * source.y() +
                affine[6 + row] * source.z() + affine[9 + row];
        const float determinant =
            affine[0] * (affine[4] * affine[8] - affine[7] * affine[5]) -
            affine[3] * (affine[1] * affine[8] - affine[7] * affine[2]) +
            affine[6] * (affine[1] * affine[5] - affine[4] * affine[2]);
        // LBS 的线性部分可能在极端姿态退化；在 CPU 入口拒绝，shader 不做逐顶点修补。
        if (!position.allFinite() || !std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f)
            throw std::runtime_error("Skin pose has a singular or nonfinite blended transform");
        pose->boundsMin = pose->boundsMin.cwiseMin(position);
        pose->boundsMax = pose->boundsMax.cwiseMax(position);
    }
    return pose;
}

std::shared_ptr<const SkinPose> BuildSkinPose(const SkinSection& section,
    const std::vector<Eigen::Matrix4f>& models, uint64_t discontinuity,
    uint64_t playbackIdentity, const std::vector<Joint>* joints)
{
    if (models.size() != section.inverseBinds.size() || section.vertices.empty())
        throw std::invalid_argument("Skin pose requires a matching skeleton and nonempty section");
    // 直接构造的 section（测试/离线工具）没有预计算数据：退回逐顶点精确参考路径，
    // 行为与引入快路径前完全一致。
    if (section.jointBindBounds.size() != section.inverseBinds.size())
        return BuildSkinPoseReference(section, models, discontinuity,
            playbackIdentity, joints);
    auto pose = std::make_shared<SkinPose>();
    pose->discontinuity = discontinuity;
    pose->playbackIdentity = playbackIdentity;
    pose->matrices.reserve(models.size());
    FillBindToPoseMatrices(*pose, models, joints);
    Eigen::Vector3f min = Eigen::Vector3f::Constant(std::numeric_limits<float>::max());
    Eigen::Vector3f max = -min;
    bool hasGeometry = false;
    for (size_t joint = 0; joint < models.size(); ++joint)
    {
        const Eigen::Matrix4f matrix = models[joint] * section.inverseBinds[joint];
        pose->matrices.push_back(matrix);
        const JointBindBounds& bounds = section.jointBindBounds[joint];
        if (bounds.vertexCount == 0) continue;
        // 快路径不再逐顶点混合，改为逐关节检查 palette 仿射行列式：旧路径对
        // 完全归属该关节的顶点会算出同一个矩阵，非有限/奇异仍在这里被拒绝。
        const float determinant = matrix.block<3, 3>(0, 0).determinant();
        if (!matrix.allFinite() || !std::isfinite(determinant) || std::abs(determinant) < 1.0e-8f)
            throw std::runtime_error("Skin pose has a singular or nonfinite blended transform");
        MergeJointBounds(matrix, bounds, min, max);
        hasGeometry = true;
    }
    // 有顶点却没有任何影响引用属于资产契约破坏；导入器已拒绝，这里保留明确失败。
    if (!hasGeometry)
        throw std::runtime_error("Skin pose has vertices without joint influences");
    ExpandForWeightSum(min, max, section.jointBindWeightSumError);
    pose->boundsMin = min;
    pose->boundsMax = max;
    return pose;
}

std::vector<uint32_t> PackSkinInfluences(const SkinSection& section)
{
    const size_t header = section.influenceOffsets.size();
    if (header != section.vertices.size() + 1 ||
        header + section.influences.size() * 2 > std::numeric_limits<uint32_t>::max())
        throw std::invalid_argument("Invalid skin CSR size");
    std::vector<uint32_t> words;
    words.reserve(header + section.influences.size() * 2);
    for (uint32_t offset : section.influenceOffsets)
        words.push_back(static_cast<uint32_t>(header) + 2 * offset);
    for (const auto& influence : section.influences)
    {
        uint32_t bits;
        static_assert(sizeof(bits) == sizeof(influence.weight));
        std::memcpy(&bits, &influence.weight, sizeof(bits));
        words.push_back(influence.joint);
        words.push_back(bits);
    }
    return words;
}
}
