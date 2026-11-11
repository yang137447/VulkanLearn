#include <cstdint>

#include <gtest/gtest.h>

#include "particles/particleRibbon.h"

namespace
{

using namespace VL;

} // namespace

TEST(ParticleRibbon, SegmentsClampToSupportedRange)
{
    EXPECT_EQ(ClampParticleRibbonSegments(0), 1u);
    EXPECT_EQ(ClampParticleRibbonSegments(1), 1u);
    EXPECT_EQ(ClampParticleRibbonSegments(2), 2u);
    EXPECT_EQ(ClampParticleRibbonSegments(3), 3u);
    EXPECT_EQ(ClampParticleRibbonSegments(4), 4u);
    EXPECT_EQ(ClampParticleRibbonSegments(32), 32u);
    EXPECT_EQ(ClampParticleRibbonSegments(100), 32u);
}

TEST(ParticleRibbon, VertexCountScalesWithSegments)
{
    EXPECT_EQ(ParticleRibbonVerticesPerParticle(1), 6u);
    EXPECT_EQ(ParticleRibbonVerticesPerParticle(2), 12u);
    EXPECT_EQ(ParticleRibbonVerticesPerParticle(3), 18u);
    // 越界段数按 clamp 处理，不能把 draw 顶点数放大到任意值。
    EXPECT_EQ(ParticleRibbonVerticesPerParticle(9), 54u);
    EXPECT_EQ(ParticleRibbonVerticesPerParticle(64), 192u);

    EXPECT_EQ(ParticleRibbonVertexCount(2048, 3), 36864u);
    EXPECT_EQ(ParticleRibbonVertexCount(0, 3), 0u);
    EXPECT_EQ(
        ParticleRibbonVertexCount(65536, 3),
        static_cast<uint64_t>(65536) * 18u);
}

TEST(ParticleRibbon, WidthCurveClampsAndInterpolates)
{
    std::vector<ParticleRibbonWidthKey> keys{
        ParticleRibbonWidthKey{0.0f, 0.5f},
        ParticleRibbonWidthKey{1.0f, 1.5f}};
    EXPECT_FLOAT_EQ(EvaluateParticleRibbonWidth(keys, -1.0f), 0.5f);
    EXPECT_FLOAT_EQ(EvaluateParticleRibbonWidth(keys, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleRibbonWidth(keys, 2.0f), 1.5f);
    EXPECT_FLOAT_EQ(
        EvaluateParticleRibbonWidth({}, 0.5f),
        1.0f);
}

TEST(ParticleRibbon, WidthAndColorCurvesSupportHermiteTangents)
{
    std::vector<ParticleRibbonWidthKey> widths{
        ParticleRibbonWidthKey{0.0f, 0.0f, 0.0f, 3.0f},
        ParticleRibbonWidthKey{1.0f, 1.0f, 0.0f, 0.0f}};
    EXPECT_NEAR(
        EvaluateParticleRibbonWidth(
            widths, 0.5f, ParticleCurveInterpolation::Hermite),
        0.875f,
        1.0e-5f);
    EXPECT_NEAR(
        EvaluateParticleRibbonWidth(
            widths, 0.5f, ParticleCurveInterpolation::Linear),
        0.5f,
        1.0e-5f);

    std::vector<ParticleRibbonColorKey> colors{
        ParticleRibbonColorKey{
            0.0f, Eigen::Vector4f(0.0f, 0.0f, 0.0f, 1.0f),
            Eigen::Vector4f::Zero(),
            Eigen::Vector4f(3.0f, 0.0f, 0.0f, 0.0f)},
        ParticleRibbonColorKey{
            1.0f, Eigen::Vector4f(1.0f, 1.0f, 1.0f, 1.0f)}};
    const Eigen::Vector4f middle = EvaluateParticleRibbonColor(
        colors, 0.5f, Eigen::Vector4f::Zero(),
        ParticleCurveInterpolation::Hermite);
    EXPECT_NEAR(middle.x(), 0.875f, 1.0e-5f);
}

TEST(ParticleRibbon, LocalVertexMapsToSegment)
{
    for (uint32_t segment = 0; segment < 3; ++segment)
    {
        for (uint32_t corner = 0; corner < 6; ++corner)
        {
            EXPECT_EQ(
                ParticleRibbonSegmentForLocalVertex(
                    segment * 6u + corner, 3),
                segment);
        }
    }
    // 段数小于 3 时越界局部索引夹到最后一个有效段。
    EXPECT_EQ(ParticleRibbonSegmentForLocalVertex(17, 1), 0u);
    EXPECT_EQ(ParticleRibbonSegmentForLocalVertex(17, 2), 1u);
}

TEST(ParticleRibbon, TailParameterSpansCurrentToOldestPoint)
{
    EXPECT_FLOAT_EQ(ParticleRibbonTailParameter(0, 0.0f, 3), 0.0f);
    EXPECT_FLOAT_EQ(ParticleRibbonTailParameter(0, 1.0f, 3), 1.0f / 3.0f);
    EXPECT_FLOAT_EQ(ParticleRibbonTailParameter(1, 1.0f, 3), 2.0f / 3.0f);
    EXPECT_FLOAT_EQ(ParticleRibbonTailParameter(2, 1.0f, 3), 1.0f);
    // 单段条带仍覆盖完整的 0..1 衰减区间。
    EXPECT_FLOAT_EQ(ParticleRibbonTailParameter(0, 0.5f, 1), 0.5f);
}
