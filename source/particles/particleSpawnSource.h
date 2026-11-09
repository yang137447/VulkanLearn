#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include <Eigen/Dense>

namespace VL
{

// 骨骼粒子 spawn 源（SkeletalMeshTris / SkeletalMeshBones）的共享约定。
// CPU 侧烘焙 buffer 与 shader/glsl/particle/particleSimulate.comp 必须逐字
// 遵守同一份布局，否则粒子会落在错误的三角面或骨段上：
//   header  = uvec4(kind 1=triangle 2=bone, count, 0, 0)
//   record  = 4 × vec4:
//     [0] xyz = 端点 A（三角形顶点 0 / 骨段起点），w = 累计权重
//     [1] xyz = 端点 B（三角形顶点 1 / 骨段终点），w = 0
//     [2] xyz = 端点 C（三角形顶点 2）或骨段轴向（bone），w = 0
//     [3] xyz = 面法线（triangle）或 0（bone），w = 0
// 权重是三角形面积或骨段长度，CPU 写入的是前缀和，shader 用它对总长做
// 逆 CDF 采样，让大面积三角形/长骨头分到更多粒子。
inline constexpr uint32_t kParticleSpawnSourceBone = 2;
inline constexpr uint32_t kParticleSpawnSourceTriangle = 1;
inline constexpr size_t kParticleSpawnSourceRecordFloats = 16;
inline constexpr uint32_t kMaxParticleSpawnSourceRecords = 65536;

struct ParticleSpawnSourceRecord
{
    std::array<float, kParticleSpawnSourceRecordFloats> data{};
    float weight = 0.0f;
};

// 三角形面积 / 骨段长度；退化时返回 0，调用方据此跳过该源。
inline float ParticleSpawnSourceTriangleArea(
    const Eigen::Vector3f& a,
    const Eigen::Vector3f& b,
    const Eigen::Vector3f& c)
{
    return (b - a).cross(c - a).norm() * 0.5f;
}

inline ParticleSpawnSourceRecord BuildParticleSpawnTriangleRecord(
    const Eigen::Vector3f& a,
    const Eigen::Vector3f& b,
    const Eigen::Vector3f& c)
{
    ParticleSpawnSourceRecord record;
    const Eigen::Vector3f cross = (b - a).cross(c - a);
    const Eigen::Vector3f normal = cross.squaredNorm() > 1.0e-12f
        ? cross.normalized()
        : Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    record.data[0] = a.x();
    record.data[1] = a.y();
    record.data[2] = a.z();
    record.data[4] = b.x();
    record.data[5] = b.y();
    record.data[6] = b.z();
    record.data[8] = c.x();
    record.data[9] = c.y();
    record.data[10] = c.z();
    record.data[12] = normal.x();
    record.data[13] = normal.y();
    record.data[14] = normal.z();
    record.weight = ParticleSpawnSourceTriangleArea(a, b, c);
    return record;
}

inline ParticleSpawnSourceRecord BuildParticleSpawnBoneRecord(
    const Eigen::Vector3f& start,
    const Eigen::Vector3f& end)
{
    ParticleSpawnSourceRecord record;
    const Eigen::Vector3f delta = end - start;
    const float length = delta.norm();
    record.data[0] = start.x();
    record.data[1] = start.y();
    record.data[2] = start.z();
    record.data[4] = end.x();
    record.data[5] = end.y();
    record.data[6] = end.z();
    if (length > 1.0e-6f)
    {
        const Eigen::Vector3f axis = delta / length;
        record.data[8] = axis.x();
        record.data[9] = axis.y();
        record.data[10] = axis.z();
    }
    record.weight = length;
    return record;
}

// 把记录压成 host-visible SSBO 字节：header + 记录，w 写累计权重。
// 返回实际写入的记录数；调用方负责分配 sizeof(uint32_t)*4 + count*64 字节。
inline uint32_t PackParticleSpawnSource(
    std::vector<ParticleSpawnSourceRecord>& records,
    uint32_t kind,
    std::vector<float>& outFloats)
{
    // 只保留有有效权重的源；退化三角形/零长骨段会让逆 CDF 永远选不到，
    // 提前剔除也能让 shader 的 sourceIndex < count 判据保持紧凑。
    records.erase(
        std::remove_if(
            records.begin(),
            records.end(),
            [](const ParticleSpawnSourceRecord& record)
            {
                return !(record.weight > 1.0e-10f);
            }),
        records.end());
    if (records.size() > kMaxParticleSpawnSourceRecords)
    {
        records.resize(kMaxParticleSpawnSourceRecords);
    }
    const uint32_t count = static_cast<uint32_t>(records.size());
    outFloats.assign(4u + static_cast<size_t>(count) * 16u, 0.0f);
    // shader 的 header 是 std430 uvec4；这里必须写整数位模式，不能写
    // float 数值，否则 GPU 会把 1.0f 的位模式当成 kind/count。
    std::memcpy(&outFloats[0], &kind, sizeof(kind));
    std::memcpy(&outFloats[1], &count, sizeof(count));
    float cumulative = 0.0f;
    for (uint32_t index = 0; index < count; ++index)
    {
        cumulative += records[index].weight;
        std::array<float, 16> record = records[index].data;
        record[3] = cumulative;
        record[15] = cumulative;
        std::copy(
            record.begin(),
            record.end(),
            outFloats.begin() + 4 + static_cast<size_t>(index) * 16u);
    }
    return count;
}

} // namespace VL
