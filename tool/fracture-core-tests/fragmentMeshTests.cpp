#include <gtest/gtest.h>

#include <cmath>

#include "physics/fracture/fragmentMesh.h"

namespace
{
using namespace VL::Physics::Fracture;

PlanarShard SquareShard(double size)
{
    PlanarShard shard;
    shard.outline = {
        {0.0, 0.0},
        {size, 0.0},
        {size, size},
        {0.0, size}};
    shard.area = size * size;
    shard.centroid = {size * 0.5, size * 0.5};
    return shard;
}

} // namespace

TEST(FragmentMesh, ExtrusionPreservesAreaTimesThickness)
{
    const PlanarShard shard = SquareShard(10.0);
    ExtrusionSettings settings;
    settings.thickness = 0.6;
    settings.centerZ = 4.0;
    const FragmentMesh mesh = ExtrudePlanarShard(shard, settings);

    EXPECT_NEAR(mesh.volume, 60.0, 1e-12);
    EXPECT_EQ(mesh.indices.size() % 3, 0u);
    EXPECT_EQ(mesh.positions.size(), mesh.normals.size());
    EXPECT_EQ(mesh.convexHullPoints.size(), 8u);
    EXPECT_TRUE(mesh.localCenter.isApprox(Eigen::Vector3f(5.0f, 5.0f, 4.0f), 1e-6f));
}

TEST(FragmentMesh, OutwardWindingMatchesPositiveSignedVolume)
{
    const PlanarShard shard = SquareShard(10.0);
    ExtrusionSettings settings;
    settings.thickness = 0.6;
    const FragmentMesh mesh = ExtrudePlanarShard(shard, settings);
    // FragmentMesh 以 float 保存顶点，体积核对应按 float 精度比较。
    EXPECT_NEAR(SignedMeshVolume(mesh.positions, mesh.indices), 60.0, 1e-4);
}

TEST(FragmentMesh, FaceNormalsAreUnitLengthAndFaceOutward)
{
    const PlanarShard shard = SquareShard(10.0);
    ExtrusionSettings settings;
    settings.thickness = 0.6;
    const FragmentMesh mesh = ExtrudePlanarShard(shard, settings);
    for (const Eigen::Vector3f& normal : mesh.normals)
        EXPECT_NEAR(normal.norm(), 1.0f, 1e-6f);

    size_t upward = 0;
    size_t downward = 0;
    for (const Eigen::Vector3f& normal : mesh.normals)
    {
        if (normal.z() > 0.99f) ++upward;
        if (normal.z() < -0.99f) ++downward;
    }
    EXPECT_EQ(upward, 4u);
    EXPECT_EQ(downward, 4u);
}

TEST(FragmentMesh, RejectsEmptyShardAndNonPositiveThickness)
{
    PlanarShard empty;
    ExtrusionSettings settings;
    EXPECT_THROW(ExtrudePlanarShard(empty, settings), std::invalid_argument);

    PlanarShard shard = SquareShard(2.0);
    settings.thickness = 0.0;
    EXPECT_THROW(ExtrudePlanarShard(shard, settings), std::invalid_argument);
}

TEST(FragmentMesh, SignedVolumeRejectsNonTriangleInput)
{
    const std::vector<Eigen::Vector3f> positions = {
        {0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f}};
    EXPECT_THROW(SignedMeshVolume(positions, {0, 1}), std::invalid_argument);
    EXPECT_THROW(SignedMeshVolume(positions, {0, 1, 3}), std::out_of_range);
}
