#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <Eigen/Dense>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/memory/unique_ptr.h>
#include <ozz/base/containers/vector.h>
#include <ozz/base/maths/soa_transform.h>

#include "../vertexDataStruct.h"
#include "animationSequenceData.h"

namespace VL::Animation
{
// 导入器拥有的 CPU 数据；骨架索引同时用于 ozz 轨道和独立权重流，不创建 World/GPU 对象。
struct Joint
{
    std::string name;
    int parent = -1;
    Eigen::Matrix4f restLocal = Eigen::Matrix4f::Identity();
    Eigen::Matrix4f restModel = Eigen::Matrix4f::Identity();
};

// 独立蒙皮流中的关节索引与原始正权重；渲染顶点结构不承担该数据。
struct Influence
{
    uint32_t joint = 0;
    float weight = 0;
};

// 每个顶点保留全部正权重；CSR 范围随 MikkTSpace 的源顶点映射复制，不截断或重新归一化。
struct SkinSection
{
    std::string name;
    std::string materialSlot;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<uint32_t> sourceVertices;
    std::vector<uint32_t> influenceOffsets;
    std::vector<Influence> influences;
    std::vector<Eigen::Matrix4f> inverseBinds;
    Eigen::Matrix4f meshToModel = Eigen::Matrix4f::Identity();
    float maxBindPositionErrorCm = 0;
    float maxBindLinearError = 0;
    float maxBindSkinErrorCm = 0;
    float maxWeightSumError = 0;
    uint32_t maxInfluences = 0;
};

// 网格导入结果共同拥有参考骨架、section 与 ozz 骨架，供片段映射和 CPU 求值使用。
struct SkeletonMesh
{
    std::vector<Joint> joints;
    std::vector<SkinSection> sections;
    ozz::unique_ptr<ozz::animation::Skeleton> runtime;
};

// 同一骨架下的命名片段；保留原始轨道用于根精度和压缩采样对照，不拥有播放时钟。
struct Clip
{
    std::string id;
    std::vector<std::string> restFallbackJoints;
    ozz::animation::offline::RawAnimation raw;
    ozz::unique_ptr<ozz::animation::Animation> runtime;
    // 可选 Sequence sidecar；空对象（duration==0/curves 为空）保持旧资源行为：
    // 没有曲线/notify，采样与导入路径与引入 sidecar 前完全一致。
    SequenceData sequence;
};

// 固定 UE FBX 导出策略：FBX(x,y,z) -> VL(x,z,-y)，厘米，Y-up；UE 源 +Y 前向变为 VL +Z。
// 不叠加 Actor/组件朝向；组件与角色朝向由调用方在放置阶段显式承担。
Eigen::Matrix4f FbxToVlBasis();

// 局部 TRS 保留 ozz 的对齐与 SoA 布局；播放器先混合局部姿态，再统一做层级求值。
using LocalPose = ozz::vector<ozz::math::SoaTransform>;
LocalPose SampleLocals(const SkeletonMesh& mesh, const Clip& clip, float time);
std::vector<Eigen::Matrix4f> LocalToModels(const SkeletonMesh& mesh, const LocalPose& locals);

// 只接受已导入并匹配的骨架/片段；调用边界验证时刻，支持 seek，既不拥有也不推进游戏时钟。
std::vector<std::vector<Eigen::Matrix4f>> SampleModels(
    const SkeletonMesh& mesh, const Clip& clip, const std::vector<float>& times);
}
