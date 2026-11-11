#pragma once

#include <cmath>

namespace VL
{
// DBuffer decal 法线合成的 CPU 镜像，与
// shader/glsl/engine/decalComposite.glsl 的 DecalBlendWorldNormal 保持同构。
// 运行时合成只在 GPU 上执行；这里存在的目的是用 GoogleTest 对同一公式做
// 数值回归，避免测试复制一份公式后与 shader 漂移。
//
// 语义：
// - DBufferB.rgb 是覆盖率先乘后的 world normal 之和
//   P = Σ N_i * w_i，w_i 是 over 合成的逐层权重；
// - DBufferB.a 是总覆盖率 c = 1 - Π(1 - a_i)；
// - 对单层 decal 有 |P| = c，所以 |P| 同时是"相干强度"；
//   多层相反法线让 |P| 变小，detail 会自然淡出而不是把残余方向
//   放大到整层覆盖率。
struct DecalNormalBlendVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

// 写端：normalStrength 为 0 时不允许覆盖接收表面法线。shader 端用
// step(1e-4, normalStrength) 实现同一门限。
inline float DecalNormalWriteCoverage(
    float coverage,
    float normalStrength)
{
    // 必须与 GLSL step(1.0e-4, normalStrength) 在同一边界取值：
    // step 在 x == edge 时返回 1。
    return normalStrength >= 1.0e-4f ? coverage : 0.0f;
}

inline DecalNormalBlendVec3 DecalBlendWorldNormal(
    DecalNormalBlendVec3 baseNormal,
    DecalNormalBlendVec3 premultipliedDecalNormal,
    float coverage)
{
    constexpr float kEpsilon = 1.0e-6f;
    constexpr float kPi = 3.14159265358979323846f;

    const float baseLengthSq =
        baseNormal.x * baseNormal.x +
        baseNormal.y * baseNormal.y +
        baseNormal.z * baseNormal.z;
    if (!(baseLengthSq > kEpsilon * kEpsilon) ||
        !std::isfinite(baseLengthSq))
    {
        baseNormal = DecalNormalBlendVec3{0.0f, 0.0f, 1.0f};
    }
    else
    {
        const float inverseBaseLength = 1.0f / std::sqrt(baseLengthSq);
        baseNormal.x *= inverseBaseLength;
        baseNormal.y *= inverseBaseLength;
        baseNormal.z *= inverseBaseLength;
    }

    const float weight = std::fmax(0.0f, std::fmin(coverage, 1.0f));
    const float decalLengthSq =
        premultipliedDecalNormal.x * premultipliedDecalNormal.x +
        premultipliedDecalNormal.y * premultipliedDecalNormal.y +
        premultipliedDecalNormal.z * premultipliedDecalNormal.z;
    if (weight <= 0.0f || decalLengthSq <= kEpsilon * kEpsilon ||
        !std::isfinite(decalLengthSq))
    {
        return baseNormal;
    }

    const float decalLength = std::sqrt(decalLengthSq);
    const float inverseDecalLength = 1.0f / decalLength;
    const DecalNormalBlendVec3 decalNormal{
        premultipliedDecalNormal.x * inverseDecalLength,
        premultipliedDecalNormal.y * inverseDecalLength,
        premultipliedDecalNormal.z * inverseDecalLength};

    // 数学上 |P| <= c（固定功能 over 合成的凸组合性质），这里取两者
    // 最小值只是防御 fp16 累积误差。
    const float fade = std::fmin(weight, decalLength);

    // 以接收表面法线为 +Z 建正交基。helper 与 base 的夹角至少约
    // 25.8°，不会因叉积退化。
    const DecalNormalBlendVec3 helper =
        std::fabs(baseNormal.x) < 0.9f
            ? DecalNormalBlendVec3{1.0f, 0.0f, 0.0f}
            : DecalNormalBlendVec3{0.0f, 0.0f, 1.0f};
    DecalNormalBlendVec3 tangent{
        helper.y * baseNormal.z - helper.z * baseNormal.y,
        helper.z * baseNormal.x - helper.x * baseNormal.z,
        helper.x * baseNormal.y - helper.y * baseNormal.x};
    const float tangentLength = std::sqrt(
        tangent.x * tangent.x +
        tangent.y * tangent.y +
        tangent.z * tangent.z);
    if (tangentLength > kEpsilon)
    {
        const float inverseTangentLength = 1.0f / tangentLength;
        tangent.x *= inverseTangentLength;
        tangent.y *= inverseTangentLength;
        tangent.z *= inverseTangentLength;
    }
    else
    {
        tangent = DecalNormalBlendVec3{1.0f, 0.0f, 0.0f};
    }
    const DecalNormalBlendVec3 bitangent{
        baseNormal.y * tangent.z - baseNormal.z * tangent.y,
        baseNormal.z * tangent.x - baseNormal.x * tangent.z,
        baseNormal.x * tangent.y - baseNormal.y * tangent.x};

    const float dz =
        decalNormal.x * baseNormal.x +
        decalNormal.y * baseNormal.y +
        decalNormal.z * baseNormal.z;
    const float dx =
        decalNormal.x * tangent.x +
        decalNormal.y * tangent.y +
        decalNormal.z * tangent.z;
    const float dy =
        decalNormal.x * bitangent.x +
        decalNormal.y * bitangent.y +
        decalNormal.z * bitangent.z;

    if (dz > 0.0f)
    {
        // Whiteout / 坡度叠加（Barré-Brisebois & Hill 2012）：
        // 在该局部基里 base 的坡度为 0，whiteout 的坡度形式为
        // normalize(base + T * slopeX + B * slopeY)。coverage 缩放
        // detail 的坡度；单层时 0 精确回到 base、1 精确回到 decal。
        const float slopeX = (dx / dz) * fade;
        const float slopeY = (dy / dz) * fade;
        DecalNormalBlendVec3 blended{
            baseNormal.x + tangent.x * slopeX + bitangent.x * slopeY,
            baseNormal.y + tangent.y * slopeX + bitangent.y * slopeY,
            baseNormal.z + tangent.z * slopeX + bitangent.z * slopeY};
        const float blendedLengthSq =
            blended.x * blended.x +
            blended.y * blended.y +
            blended.z * blended.z;
        if (blendedLengthSq > kEpsilon)
        {
            const float inverseLength = 1.0f / std::sqrt(blendedLengthSq);
            blended.x *= inverseLength;
            blended.y *= inverseLength;
            blended.z *= inverseLength;
            return blended;
        }
        return decalNormal;
    }

    // 跨过地平线后 gnomonic 坡度无定义；改用单位球上的测地线
    // （great-circle）插值。这正是不再让相反法线在 coverage=0.5 时
    // 相消为 0 的分支：对跖点的 50/50 结果落在与 base 垂直的任意方向，
    // 这里选取由 base 唯一确定的一条切线，并保证结果始终是单位向量。
    const float sinAngle = std::sqrt(dx * dx + dy * dy);
    DecalNormalBlendVec3 axis = tangent;
    float angle = kPi * fade;
    if (sinAngle > kEpsilon)
    {
        const float inverseSinAngle = 1.0f / sinAngle;
        axis = DecalNormalBlendVec3{
            (tangent.x * dx + bitangent.x * dy) * inverseSinAngle,
            (tangent.y * dx + bitangent.y * dy) * inverseSinAngle,
            (tangent.z * dx + bitangent.z * dy) * inverseSinAngle};
        angle = std::acos(std::fmax(-1.0f, std::fmin(dz, 1.0f))) * fade;
    }
    const float cosine = std::cos(angle);
    const float sine = std::sin(angle);
    return DecalNormalBlendVec3{
        baseNormal.x * cosine + axis.x * sine,
        baseNormal.y * cosine + axis.y * sine,
        baseNormal.z * cosine + axis.z * sine};
}
} // namespace VL
