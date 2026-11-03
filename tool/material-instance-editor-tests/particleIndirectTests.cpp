#include <gtest/gtest.h>

#include "particles/particleIndirect.h"

namespace
{

using namespace VL;

TEST(ParticleIndirect, BillboardUsesSixVerticesPerInstance)
{
    const ParticleIndirectDrawShape shape =
        BuildParticleIndirectDrawShape(
            "billboard", 128, 3, false, 0);

    EXPECT_FALSE(shape.indexed);
    EXPECT_EQ(shape.vertexCount, 6u);
    EXPECT_EQ(shape.instanceCount, 128u);
}

TEST(ParticleIndirect, RibbonCountFollowsSegments)
{
    const ParticleIndirectDrawShape shape =
        BuildParticleIndirectDrawShape(
            "ribbon", 64, 2, false, 0);

    EXPECT_FALSE(shape.indexed);
    EXPECT_EQ(shape.vertexCount, 2u * 6u);
    EXPECT_EQ(shape.instanceCount, 64u);
    EXPECT_NE(shape,
        BuildParticleIndirectDrawShape(
            "ribbon", 64, 3, false, 0));
}

TEST(ParticleIndirect, UsableMeshUsesIndexedInstances)
{
    const ParticleIndirectDrawShape shape =
        BuildParticleIndirectDrawShape(
            "mesh", 16, 1, true, 36);

    EXPECT_TRUE(shape.indexed);
    EXPECT_EQ(shape.indexCount, 36u);
    EXPECT_EQ(shape.instanceCount, 16u);
    EXPECT_EQ(shape.vertexCount, 0u);
}

TEST(ParticleIndirect, MeshFallbackReturnsBillboardShape)
{
    const ParticleIndirectDrawShape shape =
        BuildParticleIndirectDrawShape(
            "mesh", 32, 1, false, 36);

    EXPECT_FALSE(shape.indexed);
    EXPECT_EQ(shape.vertexCount, 6u);
    EXPECT_EQ(shape.instanceCount, 32u);
}

TEST(ParticleIndirect, BeamUsesSegmentsAndAuthoredArcCount)
{
    const ParticleIndirectDrawShape shape =
        BuildParticleIndirectDrawShape(
            "beam", 5, 3, false, 0, 24, 5);

    EXPECT_FALSE(shape.indexed);
    EXPECT_EQ(shape.vertexCount, 24u * 6u);
    EXPECT_EQ(shape.instanceCount, 5u);
    // beam 的实例数来自 count，而不是 maxParticles。
    EXPECT_NE(shape,
        BuildParticleIndirectDrawShape(
            "beam", 5, 3, false, 0, 24, 4));
}

TEST(ParticleIndirect, BeamBranchesMultiplyInstanceCount)
{
    const ParticleIndirectDrawShape shape =
        BuildParticleIndirectDrawShape(
            "beam", 8, 3, false, 0, 24, 5, 2);

    EXPECT_FALSE(shape.indexed);
    EXPECT_EQ(shape.vertexCount, 24u * 6u);
    // count * (1 + branchCount) = 5 * 3。
    EXPECT_EQ(shape.instanceCount, 15u);
    EXPECT_NE(shape,
        BuildParticleIndirectDrawShape(
            "beam", 8, 3, false, 0, 24, 5, 1));
}

TEST(ParticleBeam, VertexCountIncludesBranchInstances)
{
    EXPECT_EQ(
        ParticleBeamVertexCount(24, 5, 2),
        static_cast<uint64_t>(24 * 6 * 15));
    EXPECT_EQ(
        ParticleBeamVertexCount(24, 5),
        static_cast<uint64_t>(24 * 6 * 5));
}

} // namespace
