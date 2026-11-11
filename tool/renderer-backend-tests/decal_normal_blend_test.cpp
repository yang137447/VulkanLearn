#include <cmath>

#include <gtest/gtest.h>

#include "render/decal/decalNormalBlend.h"

namespace VL
{
namespace
{

constexpr float kSqrtHalf = 0.70710678118f;

float Length(const DecalNormalBlendVec3& value)
{
    return std::sqrt(
        value.x * value.x +
        value.y * value.y +
        value.z * value.z);
}

float Dot(
    const DecalNormalBlendVec3& left,
    const DecalNormalBlendVec3& right)
{
    return left.x * right.x +
        left.y * right.y +
        left.z * right.z;
}

void ExpectUnitAndFinite(const DecalNormalBlendVec3& value)
{
    EXPECT_TRUE(std::isfinite(value.x));
    EXPECT_TRUE(std::isfinite(value.y));
    EXPECT_TRUE(std::isfinite(value.z));
    EXPECT_NEAR(Length(value), 1.0f, 1.0e-5f);
}

} // namespace

TEST(DecalNormalBlend, CoverageZeroReturnsReceiverNormal)
{
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 1.0f};
    const DecalNormalBlendVec3 result = DecalBlendWorldNormal(
        base,
        DecalNormalBlendVec3{0.0f, 0.0f, 0.0f},
        0.0f);

    EXPECT_NEAR(result.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(result.y, 0.0f, 1.0e-6f);
    EXPECT_NEAR(result.z, 1.0f, 1.0e-6f);
}

TEST(DecalNormalBlend, FullCoverageReturnsDecalNormalExactly)
{
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 1.0f};
    const DecalNormalBlendVec3 decal{kSqrtHalf, 0.0f, kSqrtHalf};
    const DecalNormalBlendVec3 result =
        DecalBlendWorldNormal(base, decal, 1.0f);

    ExpectUnitAndFinite(result);
    EXPECT_NEAR(result.x, decal.x, 1.0e-5f);
    EXPECT_NEAR(result.y, decal.y, 1.0e-5f);
    EXPECT_NEAR(result.z, decal.z, 1.0e-5f);
}

TEST(DecalNormalBlend, HalfCoverageFadesSameHemisphereSlope)
{
    // 45° decal 在 coverage 0.5 时坡度减半：
    // atan(0.5 * tan(45°)) = 26.565°。
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 1.0f};
    const DecalNormalBlendVec3 premultipliedDecal{
        0.5f * kSqrtHalf,
        0.0f,
        0.5f * kSqrtHalf};
    const DecalNormalBlendVec3 result =
        DecalBlendWorldNormal(base, premultipliedDecal, 0.5f);

    ExpectUnitAndFinite(result);
    EXPECT_NEAR(result.x, 0.4472135955f, 1.0e-5f);
    EXPECT_NEAR(result.y, 0.0f, 1.0e-6f);
    EXPECT_NEAR(result.z, 0.8944271910f, 1.0e-5f);
}

TEST(DecalNormalBlend, OppositeNormalAtHalfCoverageDoesNotCancel)
{
    // 旧实现 mix(base, -base, 0.5) 得到零向量，归一化后产生 NaN。
    // 跨半球分支应在对跖点 50/50 给出与 base 垂直的单位向量。
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 1.0f};
    const DecalNormalBlendVec3 premultipliedDecal{0.0f, 0.0f, -0.5f};
    const DecalNormalBlendVec3 result =
        DecalBlendWorldNormal(base, premultipliedDecal, 0.5f);

    ExpectUnitAndFinite(result);
    EXPECT_NEAR(Dot(result, base), 0.0f, 1.0e-5f);
    EXPECT_GT(Length(result), 0.999f);
}

TEST(DecalNormalBlend, OppositeNormalAtFullCoverageReturnsDecal)
{
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 1.0f};
    const DecalNormalBlendVec3 result = DecalBlendWorldNormal(
        base,
        DecalNormalBlendVec3{0.0f, 0.0f, -1.0f},
        1.0f);

    ExpectUnitAndFinite(result);
    EXPECT_NEAR(result.z, -1.0f, 1.0e-5f);
}

TEST(DecalNormalBlend, CrossHemisphereFullCoverageReturnsDecal)
{
    // 120° 倾斜落在下半球，必须走 great-circle 分支且 c=1 时精确还原。
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 1.0f};
    const DecalNormalBlendVec3 decal{0.8660254038f, 0.0f, -0.5f};
    const DecalNormalBlendVec3 result =
        DecalBlendWorldNormal(base, decal, 1.0f);

    ExpectUnitAndFinite(result);
    EXPECT_NEAR(result.x, decal.x, 1.0e-5f);
    EXPECT_NEAR(result.z, decal.z, 1.0e-5f);
}

TEST(DecalNormalBlend, OpposingAccumulatedNormalsFallBackToReceiver)
{
    // 两个等权、方向相反的 decal 在 DBufferB 里只剩覆盖率，没有可用方向；
    // 这里必须稳定回退到接收表面法线，而不是返回零向量。
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 1.0f};
    const DecalNormalBlendVec3 result = DecalBlendWorldNormal(
        base,
        DecalNormalBlendVec3{0.0f, 0.0f, 0.0f},
        0.75f);

    ExpectUnitAndFinite(result);
    EXPECT_NEAR(result.z, 1.0f, 1.0e-6f);
}

TEST(DecalNormalBlend, ClampsCoverageAndNormalizesInputs)
{
    const DecalNormalBlendVec3 base{0.0f, 0.0f, 4.0f};
    const DecalNormalBlendVec3 decal{0.0f, 0.0f, -3.0f};

    const DecalNormalBlendVec3 clampedHigh =
        DecalBlendWorldNormal(base, decal, 2.0f);
    ExpectUnitAndFinite(clampedHigh);
    EXPECT_NEAR(clampedHigh.z, -1.0f, 1.0e-5f);

    const DecalNormalBlendVec3 clampedLow =
        DecalBlendWorldNormal(base, decal, -1.0f);
    ExpectUnitAndFinite(clampedLow);
    EXPECT_NEAR(clampedLow.z, 1.0f, 1.0e-5f);
}

TEST(DecalNormalBlend, DegenerateBaseNormalFallsBackToUp)
{
    const DecalNormalBlendVec3 result = DecalBlendWorldNormal(
        DecalNormalBlendVec3{0.0f, 0.0f, 0.0f},
        DecalNormalBlendVec3{0.0f, 0.0f, 0.5f},
        1.0f);

    ExpectUnitAndFinite(result);
    EXPECT_NEAR(result.z, 1.0f, 1.0e-6f);
}

TEST(DecalNormalBlend, NormalStrengthZeroWritesNoNormalCoverage)
{
    EXPECT_FLOAT_EQ(DecalNormalWriteCoverage(0.8f, 0.0f), 0.0f);
    EXPECT_FLOAT_EQ(DecalNormalWriteCoverage(0.8f, 1.0e-5f), 0.0f);
    EXPECT_FLOAT_EQ(DecalNormalWriteCoverage(0.8f, 1.0e-4f), 0.8f);
    EXPECT_FLOAT_EQ(DecalNormalWriteCoverage(0.8f, 0.5f), 0.8f);
}

} // namespace VL
