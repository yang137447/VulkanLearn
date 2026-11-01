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

} // namespace
