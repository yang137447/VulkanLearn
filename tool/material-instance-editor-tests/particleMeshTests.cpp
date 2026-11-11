#include <cmath>

#include <gtest/gtest.h>

#include "particles/particleMesh.h"

namespace
{

using namespace VL;

TEST(ParticleMesh, PathValidationRejectsEscapesAndNonJson)
{
    EXPECT_TRUE(IsValidParticleMeshRelativePath(
        "Common/Meshes/SM_cube.json"));
    EXPECT_TRUE(IsValidParticleMeshRelativePath(
        "Common\\Meshes\\SM_cube.JSON"));
    EXPECT_FALSE(IsValidParticleMeshRelativePath(""));
    EXPECT_FALSE(IsValidParticleMeshRelativePath(
        "../SM_cube.json"));
    EXPECT_FALSE(IsValidParticleMeshRelativePath(
        "Common/../../SM_cube.json"));
    EXPECT_FALSE(IsValidParticleMeshRelativePath(
        "C:/Meshes/SM_cube.json"));
    EXPECT_FALSE(IsValidParticleMeshRelativePath(
        "/Meshes/SM_cube.json"));
    EXPECT_FALSE(IsValidParticleMeshRelativePath(
        "Common/Meshes/cube.obj"));
}

TEST(ParticleMesh, AlignmentCodesAreStable)
{
    EXPECT_EQ(ParticleMeshAlignmentCode("world"), 0);
    EXPECT_EQ(ParticleMeshAlignmentCode("camera"), 1);
    EXPECT_EQ(ParticleMeshAlignmentCode("velocity"), 2);
    EXPECT_TRUE(IsParticleMeshAlignment("world"));
    EXPECT_TRUE(IsParticleMeshAlignment("camera"));
    EXPECT_TRUE(IsParticleMeshAlignment("velocity"));
    EXPECT_FALSE(IsParticleMeshAlignment("normal"));
}

TEST(ParticleMesh, WorldBasisKeepsModelLinearPart)
{
    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 3>(0, 0) =
        Eigen::AngleAxisf(0.5f, Eigen::Vector3f::UnitY())
            .toRotationMatrix() *
        Eigen::Vector3f(2.0f, 3.0f, 4.0f).asDiagonal();
    const Eigen::Matrix3f basis = ComputeParticleMeshBasis(
        "world",
        Eigen::Vector3f(1.0f, 0.0f, 0.0f),
        model,
        Eigen::Vector3f::UnitX(),
        Eigen::Vector3f::UnitY(),
        -Eigen::Vector3f::UnitZ());
    EXPECT_TRUE(basis.isApprox(model.block<3, 3>(0, 0)));
}

TEST(ParticleMesh, CameraBasisFacesTheViewer)
{
    const Eigen::Vector3f right = Eigen::Vector3f::UnitX();
    const Eigen::Vector3f up = Eigen::Vector3f::UnitY();
    const Eigen::Vector3f forward = -Eigen::Vector3f::UnitZ();
    const Eigen::Matrix3f basis = ComputeParticleMeshBasis(
        "camera",
        Eigen::Vector3f::Zero(),
        Eigen::Matrix4f::Identity(),
        right,
        up,
        forward);

    EXPECT_TRUE(basis.col(0).isApprox(right));
    EXPECT_TRUE(basis.col(1).isApprox(up));
    EXPECT_TRUE(basis.col(2).isApprox(-forward));
    EXPECT_NEAR(basis.determinant(), 1.0f, 1.0e-5f);
}

TEST(ParticleMesh, VelocityBasisAlignsLocalZAndIsRightHanded)
{
    const Eigen::Vector3f velocity(3.0f, 4.0f, 0.0f);
    const Eigen::Matrix3f basis = ComputeParticleMeshBasis(
        "velocity",
        velocity,
        Eigen::Matrix4f::Identity(),
        Eigen::Vector3f::UnitX(),
        Eigen::Vector3f::UnitY(),
        -Eigen::Vector3f::UnitZ());

    EXPECT_TRUE(basis.col(2).isApprox(velocity.normalized()));
    EXPECT_NEAR(basis.col(0).dot(basis.col(1)), 0.0f, 1.0e-5f);
    EXPECT_NEAR(basis.col(0).dot(basis.col(2)), 0.0f, 1.0e-5f);
    EXPECT_NEAR(basis.col(1).dot(basis.col(2)), 0.0f, 1.0e-5f);
    EXPECT_NEAR(basis.determinant(), 1.0f, 1.0e-5f);
}

TEST(ParticleMesh, DegenerateVelocityFallsBackToCamera)
{
    const Eigen::Matrix3f camera = ComputeParticleMeshBasis(
        "camera",
        Eigen::Vector3f::Zero(),
        Eigen::Matrix4f::Identity(),
        Eigen::Vector3f::UnitX(),
        Eigen::Vector3f::UnitY(),
        -Eigen::Vector3f::UnitZ());
    const Eigen::Matrix3f degenerate = ComputeParticleMeshBasis(
        "velocity",
        Eigen::Vector3f::Zero(),
        Eigen::Matrix4f::Identity(),
        Eigen::Vector3f::UnitX(),
        Eigen::Vector3f::UnitY(),
        -Eigen::Vector3f::UnitZ());
    const Eigen::Matrix3f parallelToUp = ComputeParticleMeshBasis(
        "velocity",
        Eigen::Vector3f(0.0f, 10.0f, 0.0f),
        Eigen::Matrix4f::Identity(),
        Eigen::Vector3f::UnitX(),
        Eigen::Vector3f::UnitY(),
        -Eigen::Vector3f::UnitZ());

    EXPECT_TRUE(degenerate.isApprox(camera));
    EXPECT_TRUE(parallelToUp.isApprox(camera));
}

TEST(ParticleMesh, DrawVertexCountCountsEveryIndexedInstance)
{
    EXPECT_EQ(ParticleMeshDrawnVertexCount(36, 64), 2304u);
    EXPECT_EQ(ParticleMeshDrawnVertexCount(0, 64), 0u);
}

} // namespace
