#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <Eigen/Dense>

namespace VL
{

// 骨骼网格粒子（SkeletalMeshTris / SkeletalMeshBones / 骨骼完整实例）的
// 加载期预检。它不是运行时兜底：作者资产错误必须在烘焙 GPU 资源之前就
// 失败并给出可定位的诊断，而不是在渲染期回退到 billboard 或读空 palette。
//
// 预检只做纯数据检查，不依赖 Vulkan，也不依赖 ozz 骨架求值，因此 GTest
// 可以直接构造描述表覆盖；ParticleRenderer 从已导入的 SkeletalAsset
// 组装输入。section 的顶点/影响数据太大，不复制进描述表，而是由调用方
// 通过 SkeletalSectionInspection 提供逐 section 的扫描结果。
struct ParticleSkeletalSectionInspection
{
    size_t vertexCount = 0;
    size_t indexCount = 0;
    size_t influenceCount = 0;
    size_t inverseBindCount = 0;
    size_t influenceOffsetCount = 0;
    uint32_t maxInfluencesPerVertex = 0;
    // 第一个问题的人读描述；空表示该 section 没有结构问题。
    std::string structuralError;
};

struct ParticleSkeletalPreflightInput
{
    size_t jointCount = 0;
    std::vector<int> jointParents;
    std::vector<ParticleSkeletalSectionInspection> sections;
    bool wantTriangles = false;
    bool wantBones = false;
};

struct ParticleSkeletalPreflightLimits
{
    // 每个 swapchain image 的 palette SSBO 按关节数分配（单关节 2×mat4）。
    uint32_t maxJoints = 4096;
    uint32_t maxSections = 16;
    uint32_t maxTriangles = 65536;
    uint32_t maxBones = 65536;
    uint32_t maxInfluencesPerVertex = 16;
};

struct ParticleSkeletalPreflight
{
    bool success = false;
    std::string error;
    uint32_t sectionCount = 0;
    uint32_t jointCount = 0;
    uint32_t triangleCount = 0;
    uint32_t boneCount = 0;
    uint32_t maxInfluencesPerVertex = 0;
    // 骨骼粒子覆盖全部源项所需的池大小：skeletalMeshTris 是三角形数，
    // skeletalMeshBones 是骨段数。池容量不足时 shader 按源项索引静默丢弃，
    // 因此这里给出作者应设置的下限。
    uint32_t requiredMaxParticles = 0;

    bool ok() const { return success; }
};

ParticleSkeletalPreflight ValidateParticleSkeletalPreflight(
    const ParticleSkeletalPreflightInput& input,
    const ParticleSkeletalPreflightLimits& limits = {});

} // namespace VL
