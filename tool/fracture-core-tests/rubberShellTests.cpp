#include <gtest/gtest.h>

#include <cmath>

#include <Eigen/Geometry>

#include "physics/softbody/rubberShell.h"

namespace
{
using namespace VL::Physics::Softbody;
using namespace VL::Physics::Fracture;

RubberShellSettings Settings()
{
    RubberShellSettings settings;
    settings.radius = 20.0;
    settings.subdivisions = 2;
    settings.totalMass = 4.0;
    settings.gravity.setZero();
    settings.damping = 1.0;
    return settings;
}

double KineticEnergy(const RubberShell& shell)
{
    double energy = 0.0;
    for (const ShellParticle& particle : shell.Particles())
        if (!particle.pinned && particle.inverseMass > 0.0)
            energy += 0.5 * particle.velocity.squaredNorm() / particle.inverseMass;
    return energy;
}

} // namespace

TEST(RubberShell, RuntimeIcosphereHasClosedOutwardTopology)
{
    const RubberShell shell = RubberShell::BuildIcosphere(Settings());
    EXPECT_EQ(shell.Particles().size(), 162u);
    EXPECT_EQ(shell.Triangles().size(), 320u);
    ASSERT_FALSE(shell.Constraints().empty());
    EXPECT_GT(shell.ComputeVolume(), 0.0);
    for (const ShellParticle& particle : shell.Particles())
        EXPECT_NEAR(particle.position.norm(), 20.0, 1e-9);
}

TEST(RubberShell, PressureInflatesWhileStructuralStrainStaysBounded)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    const double initialVolume = shell.ComputeVolume();
    for (int step = 0; step < 120; ++step) shell.Step(1.0 / 60.0, 12.0);
    const double finalVolume = shell.ComputeVolume();
    EXPECT_GT(finalVolume, initialVolume);
    EXPECT_LT(shell.MaximumStructuralStrain(), 0.35);
    for (const ShellParticle& particle : shell.Particles())
        EXPECT_TRUE(particle.position.allFinite());
}

TEST(RubberShell, StructuralEdgeLengthsRemainCloseToRestLength)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    for (int step = 0; step < 60; ++step) shell.Step(1.0 / 60.0, 8.0);
    for (const ShellDistanceConstraint& constraint : shell.Constraints())
    {
        if (!constraint.structural) continue;
        const double length = (shell.Particles()[constraint.first].position -
            shell.Particles()[constraint.second].position).norm();
        EXPECT_NEAR(length, constraint.restLength, constraint.restLength * 0.35);
    }
}

TEST(RubberShell, HighPressureCrossesBurstStrain)
{
    RubberShellSettings settings = Settings();
    settings.burstStrain = 0.05;
    settings.edgeCompliance = 2.0e-3;
    RubberShell shell = RubberShell::BuildIcosphere(settings);
    for (int step = 0; step < 120 && !shell.ShouldBurst(); ++step)
        shell.Step(1.0 / 60.0, 120.0);
    EXPECT_TRUE(shell.ShouldBurst());
}

TEST(RubberShell, DampingAndGravityAreFiniteAndDeterministic)
{
    RubberShellSettings settings = Settings();
    settings.gravity = {0.0, -981.0, 0.0};
    RubberShell first = RubberShell::BuildIcosphere(settings);
    RubberShell second = RubberShell::BuildIcosphere(settings);
    for (int step = 0; step < 30; ++step)
    {
        first.Step(1.0 / 60.0, 4.0);
        second.Step(1.0 / 60.0, 4.0);
    }
    ASSERT_EQ(first.Particles().size(), second.Particles().size());
    for (size_t index = 0; index < first.Particles().size(); ++index)
    {
        EXPECT_TRUE(first.Particles()[index].position.isApprox(
            second.Particles()[index].position, 1e-12));
        EXPECT_TRUE(first.Particles()[index].velocity.allFinite());
    }
}

TEST(RubberShell, InvalidSettingsAreRejected)
{
    RubberShellSettings settings = Settings();
    settings.radius = 0.0;
    EXPECT_THROW(RubberShell::BuildIcosphere(settings), std::invalid_argument);
    settings = Settings();
    settings.subdivisions = 5;
    EXPECT_THROW(RubberShell::BuildIcosphere(settings), std::invalid_argument);
    settings = Settings();
    settings.totalMass = 0.0;
    EXPECT_THROW(RubberShell::BuildIcosphere(settings), std::invalid_argument);
}

TEST(RubberShell, RuntimeVoronoiTearKeepsEveryTriangleInOneSoftFragment)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    for (int step = 0; step < 30; ++step) shell.Step(1.0 / 60.0, 10.0);

    VL::Physics::Fracture::SphericalFractureSettings patternSettings;
    patternSettings.seed = 31;
    patternSettings.siteCount = 16;
    patternSettings.radius = 20.0;
    patternSettings.thickness = 0.2;
    const VL::Physics::Fracture::SphericalPattern pattern =
        VL::Physics::Fracture::GenerateSphericalBurstPattern(patternSettings);
    const std::vector<RubberShell> fragments =
        RubberShell::TearBySphericalPattern(shell, pattern);

    ASSERT_GE(fragments.size(), 4u);
    size_t triangleCount = 0;
    for (const RubberShell& fragment : fragments)
    {
        EXPECT_FALSE(fragment.Particles().empty());
        EXPECT_FALSE(fragment.Triangles().empty());
        EXPECT_FALSE(fragment.Constraints().empty());
        triangleCount += fragment.Triangles().size();
        for (const ShellParticle& particle : fragment.Particles())
            EXPECT_TRUE(particle.position.allFinite());
    }
    EXPECT_EQ(triangleCount, shell.Triangles().size());
}

TEST(RubberShell, TearContractsStructuralRestLengthsForRubberSnapback)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    for (int step = 0; step < 30; ++step) shell.Step(1.0 / 60.0, 10.0);

    VL::Physics::Fracture::SphericalFractureSettings patternSettings;
    patternSettings.seed = 7;
    patternSettings.siteCount = 12;
    patternSettings.radius = 20.0;
    patternSettings.thickness = 0.2;
    const VL::Physics::Fracture::SphericalPattern pattern =
        VL::Physics::Fracture::GenerateSphericalBurstPattern(patternSettings);
    const std::vector<RubberShell> fragments =
        RubberShell::TearBySphericalPattern(shell, pattern);

    for (const RubberShell& fragment : fragments)
        for (const ShellDistanceConstraint& constraint : fragment.Constraints())
        {
            if (!constraint.structural) continue;
            const double currentLength = (fragment.Particles()[constraint.first].position -
                fragment.Particles()[constraint.second].position).norm();
            EXPECT_LT(constraint.restLength, currentLength);
        }
}

TEST(RubberShell, TearIsDeterministicForTheSamePattern)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    for (int step = 0; step < 20; ++step) shell.Step(1.0 / 60.0, 8.0);

    VL::Physics::Fracture::SphericalFractureSettings patternSettings;
    patternSettings.seed = 99;
    patternSettings.siteCount = 12;
    patternSettings.radius = 20.0;
    patternSettings.thickness = 0.2;
    const VL::Physics::Fracture::SphericalPattern pattern =
        VL::Physics::Fracture::GenerateSphericalBurstPattern(patternSettings);
    const auto first = RubberShell::TearBySphericalPattern(shell, pattern);
    const auto second = RubberShell::TearBySphericalPattern(shell, pattern);
    ASSERT_EQ(first.size(), second.size());
    for (size_t fragment = 0; fragment < first.size(); ++fragment)
    {
        ASSERT_EQ(first[fragment].Particles().size(),
            second[fragment].Particles().size());
        for (size_t particle = 0; particle < first[fragment].Particles().size(); ++particle)
            EXPECT_TRUE(first[fragment].Particles()[particle].position.isApprox(
                second[fragment].Particles()[particle].position, 1e-12));
    }
}

TEST(RubberShell, ExactVoronoiTearSplitsTrianglesAlongCellPlanes)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    for (int step = 0; step < 30; ++step) shell.Step(1.0 / 60.0, 10.0);

    VL::Physics::Fracture::SphericalFractureSettings patternSettings;
    patternSettings.seed = 101;
    patternSettings.siteCount = 16;
    patternSettings.radius = 20.0;
    patternSettings.thickness = 0.2;
    const VL::Physics::Fracture::SphericalPattern pattern =
        VL::Physics::Fracture::GenerateSphericalBurstPattern(patternSettings);
    const std::vector<RubberShell> fragments =
        RubberShell::TearBySphericalPatternExact(shell, pattern);

    ASSERT_GE(fragments.size(), 4u);
    size_t triangleCount = 0;
    for (const RubberShell& fragment : fragments)
    {
        EXPECT_FALSE(fragment.Particles().empty());
        EXPECT_FALSE(fragment.Triangles().empty());
        EXPECT_FALSE(fragment.Constraints().empty());
        triangleCount += fragment.Triangles().size();
        for (const ShellParticle& particle : fragment.Particles())
            EXPECT_TRUE(particle.position.allFinite());
    }
    // 边界三角形被裁剪后会增加三角形数；只允许增加，不允许丢失覆盖。
    EXPECT_GE(triangleCount, shell.Triangles().size());
}

TEST(RubberShell, ExactTearIsDeterministicAndContractsRestLengths)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    for (int step = 0; step < 20; ++step) shell.Step(1.0 / 60.0, 8.0);

    VL::Physics::Fracture::SphericalFractureSettings patternSettings;
    patternSettings.seed = 202;
    patternSettings.siteCount = 12;
    patternSettings.radius = 20.0;
    patternSettings.thickness = 0.2;
    const VL::Physics::Fracture::SphericalPattern pattern =
        VL::Physics::Fracture::GenerateSphericalBurstPattern(patternSettings);
    const auto first = RubberShell::TearBySphericalPatternExact(shell, pattern);
    const auto second = RubberShell::TearBySphericalPatternExact(shell, pattern);

    ASSERT_EQ(first.size(), second.size());
    for (size_t fragment = 0; fragment < first.size(); ++fragment)
    {
        ASSERT_EQ(first[fragment].Particles().size(),
            second[fragment].Particles().size());
        for (size_t particle = 0; particle < first[fragment].Particles().size(); ++particle)
            EXPECT_TRUE(first[fragment].Particles()[particle].position.isApprox(
                second[fragment].Particles()[particle].position, 1e-12));
        for (const ShellDistanceConstraint& constraint : first[fragment].Constraints())
        {
            if (!constraint.structural) continue;
            const double currentLength =
                (first[fragment].Particles()[constraint.first].position -
                    first[fragment].Particles()[constraint.second].position).norm();
            EXPECT_LT(constraint.restLength, currentLength);
        }
    }
}

TEST(RubberShell, StrainStiffeningReducesPeakStretch)
{
    RubberShellSettings linear = Settings();
    linear.edgeCompliance = 5.0e-3;
    linear.edgeDamping = 0.0;
    linear.strainStiffening = 0.0;
    RubberShell linearShell = RubberShell::BuildIcosphere(linear);

    RubberShellSettings stiffened = linear;
    stiffened.strainStiffening = 60.0;
    RubberShell stiffenedShell = RubberShell::BuildIcosphere(stiffened);

    for (int step = 0; step < 120; ++step)
    {
        linearShell.Step(1.0 / 60.0, 30.0);
        stiffenedShell.Step(1.0 / 60.0, 30.0);
    }
    EXPECT_LT(stiffenedShell.MaximumStructuralStrain(),
        linearShell.MaximumStructuralStrain());
}

TEST(RubberShell, EdgeDampingDissipatesRubberKineticEnergy)
{
    RubberShellSettings elastic = Settings();
    elastic.edgeDamping = 0.0;
    RubberShell elasticShell = RubberShell::BuildIcosphere(elastic);

    RubberShellSettings viscoelastic = elastic;
    viscoelastic.edgeDamping = 40.0;
    RubberShell viscoelasticShell = RubberShell::BuildIcosphere(viscoelastic);

    for (int step = 0; step < 120; ++step)
    {
        elasticShell.Step(1.0 / 60.0, 12.0);
        viscoelasticShell.Step(1.0 / 60.0, 12.0);
    }
    EXPECT_LT(KineticEnergy(viscoelasticShell), KineticEnergy(elasticShell));
}

TEST(RubberShell, NegativeStiffeningAndDampingAreRejected)
{
    RubberShellSettings settings = Settings();
    settings.strainStiffening = -1.0;
    EXPECT_THROW(RubberShell::BuildIcosphere(settings), std::invalid_argument);
    settings = Settings();
    settings.edgeDamping = -1.0;
    EXPECT_THROW(RubberShell::BuildIcosphere(settings), std::invalid_argument);
}

TEST(RubberShell, RenderMeshHasSmoothOutwardNormalsAndValidIndices)
{
    const RubberShell shell = RubberShell::BuildIcosphere(Settings());
    const SoftBodyRenderMesh mesh = shell.BuildRenderMesh();
    ASSERT_EQ(mesh.positions.size(), shell.Particles().size());
    ASSERT_EQ(mesh.normals.size(), shell.Particles().size());
    ASSERT_EQ(mesh.indices.size(), shell.Triangles().size() * 3);
    for (uint32_t index : mesh.indices)
        EXPECT_LT(index, mesh.positions.size());
    for (size_t index = 0; index < mesh.normals.size(); ++index)
    {
        EXPECT_NEAR(mesh.normals[index].norm(), 1.0f, 1e-5f);
        EXPECT_GT(mesh.normals[index].dot(mesh.positions[index]), 0.0f);
    }
}

TEST(RubberShell, ExactFragmentsStayInsideTheirRuntimeVoronoiCell)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    for (int step = 0; step < 20; ++step) shell.Step(1.0 / 60.0, 8.0);

    VL::Physics::Fracture::SphericalFractureSettings patternSettings;
    patternSettings.seed = 303;
    patternSettings.siteCount = 12;
    patternSettings.radius = 20.0;
    patternSettings.thickness = 0.2;
    const VL::Physics::Fracture::SphericalPattern pattern =
        VL::Physics::Fracture::GenerateSphericalBurstPattern(patternSettings);
    const auto fragments = RubberShell::TearBySphericalPatternExact(shell, pattern);
    ASSERT_FALSE(fragments.empty());

    for (const RubberShell& fragment : fragments)
    {
        Eigen::Vector3d average = Eigen::Vector3d::Zero();
        for (const ShellParticle& particle : fragment.Particles())
            average += particle.position.normalized();
        average.normalize();
        size_t bestCell = 0;
        double bestDot = -2.0;
        for (size_t cell = 0; cell < pattern.shards.size(); ++cell)
        {
            const double dot = average.dot(pattern.shards[cell].siteDirection);
            if (dot > bestDot)
            {
                bestDot = dot;
                bestCell = cell;
            }
        }
        const VL::Physics::Fracture::SphericalShard& shard = pattern.shards[bestCell];
        for (const ShellParticle& particle : fragment.Particles())
            for (size_t edge = 0; edge < shard.boundary.size(); ++edge)
            {
                Eigen::Vector3d normal = shard.boundary[edge].cross(
                    shard.boundary[(edge + 1) % shard.boundary.size()]).normalized();
                if (normal.dot(shard.siteDirection) < 0.0) normal = -normal;
                EXPECT_GE(normal.dot(particle.position), -1e-4);
            }
    }
}

TEST(RubberShell, GroundPlaneClampPreventsParticlePenetration)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    shell.SetGravity({0.0, -981.0, 0.0});
    for (int step = 0; step < 120; ++step) shell.Step(1.0 / 60.0, 0.0);
    shell.ApplyGroundPlane(-100.0f, 0.1f);
    for (const ShellParticle& particle : shell.Particles())
    {
        EXPECT_GE(particle.position.y(), -100.0 - 1e-9);
        EXPECT_TRUE(particle.velocity.allFinite());
    }
}

TEST(RubberShell, BurstImpulseAddsDeterministicOutwardVelocity)
{
    RubberShell first = RubberShell::BuildIcosphere(Settings());
    RubberShell second = RubberShell::BuildIcosphere(Settings());
    first.ApplyBurstImpulse(Eigen::Vector3d::Zero(), 220.0, 60.0, 17);
    second.ApplyBurstImpulse(Eigen::Vector3d::Zero(), 220.0, 60.0, 17);

    double outward = 0.0;
    for (size_t index = 0; index < first.Particles().size(); ++index)
    {
        const ShellParticle& particle = first.Particles()[index];
        EXPECT_TRUE(particle.velocity.allFinite());
        outward += particle.velocity.dot(particle.position.normalized());
        EXPECT_TRUE(particle.velocity.isApprox(second.Particles()[index].velocity, 1e-12));
    }
    EXPECT_GT(outward, 0.0);
}

TEST(RubberShell, PunctureBoostConcentratesBurstImpulseNearImpact)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    const Eigen::Vector3d puncture = Eigen::Vector3d(1.0, 0.0, 0.0);
    shell.ApplyBurstImpulse(
        Eigen::Vector3d::Zero(), 200.0, 0.0, 9, puncture, 1.0);

    double nearSum = 0.0;
    double farSum = 0.0;
    int nearCount = 0;
    int farCount = 0;
    for (const ShellParticle& particle : shell.Particles())
    {
        const Eigen::Vector3d direction = particle.position.normalized();
        const double facing = direction.dot(puncture);
        const double radialSpeed = particle.velocity.dot(direction);
        if (facing > 0.85)
        {
            nearSum += radialSpeed;
            ++nearCount;
            continue;
        }
        if (facing < -0.85)
        {
            farSum += radialSpeed;
            ++farCount;
        }
    }
    ASSERT_GT(nearCount, 0);
    ASSERT_GT(farCount, 0);
    EXPECT_GT(nearSum / nearCount, farSum / farCount * 1.5);
}

TEST(RubberShell, RenderHoleSkipsTrianglesAroundPuncture)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    const size_t fullIndices = shell.BuildRenderMesh().indices.size();
    shell.SetRenderHole(Eigen::Vector3d(0.0, 0.0, 1.0), 0.25);
    const SoftBodyRenderMesh holed = shell.BuildRenderMesh();
    // 洞只影响渲染索引：减少一组三角形，但仍保留大部分球面。
    EXPECT_LT(holed.indices.size(), fullIndices);
    EXPECT_GT(holed.indices.size(), fullIndices / 2);
    for (const Eigen::Vector3f& position : holed.positions)
        EXPECT_TRUE(position.allFinite());
}

TEST(RubberShell, DeflateImpulseMovesParticlesInward)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    shell.ApplyDeflateImpulse(Eigen::Vector3d::Zero(), 50.0, 0.0, 7);
    double inward = 0.0;
    for (const ShellParticle& particle : shell.Particles())
    {
        EXPECT_TRUE(particle.velocity.allFinite());
        inward += particle.velocity.dot(particle.position.normalized());
    }
    // 漏气是整体向内塌陷，与爆裂的外向冲量方向相反。
    EXPECT_LT(inward, 0.0);
}

TEST(RubberShell, PinNearestKeepsNeckParticlesAnchored)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    shell.PinNearest({0.0, -20.0, 0.0}, 2);
    int pinned = 0;
    for (const ShellParticle& particle : shell.Particles())
        if (particle.pinned) ++pinned;
    EXPECT_EQ(pinned, 2);

    std::vector<Eigen::Vector3d> pinnedPositions;
    for (const ShellParticle& particle : shell.Particles())
        if (particle.pinned) pinnedPositions.push_back(particle.position);
    shell.SetGravity({0.0, -981.0, 0.0});
    for (int step = 0; step < 30; ++step) shell.Step(1.0 / 60.0, 20.0);
    for (const ShellParticle& particle : shell.Particles())
        if (particle.pinned)
        {
            bool found = false;
            for (const auto& position : pinnedPositions)
                if (particle.position.isApprox(position, 1e-12)) found = true;
            EXPECT_TRUE(found);
        }
}

TEST(RubberShell, RigidTransformMovesParticlesAndVelocities)
{
    RubberShell shell = RubberShell::BuildIcosphere(Settings());
    const Eigen::Vector3d before = shell.Particles().front().position;
    Eigen::Matrix4f transform = Eigen::Matrix4f::Identity();
    transform.block<3, 1>(0, 3) = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    shell.ApplyRigidTransform(transform);
    EXPECT_TRUE(shell.Particles().front().position.isApprox(before + Eigen::Vector3d(10, 0, 0), 1e-9));
}
