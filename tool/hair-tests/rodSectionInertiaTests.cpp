#include "hair/rod/rodSectionInertia.h"
#include "hair/rod/rodImplicit.h"
#include "hair/rod/rodAnalytic.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace
{
using namespace VL::Hair::Rod;

RestState Straight(size_t segments)
{
    std::vector<Vec3> points;
    for (size_t point = 0; point <= segments; ++point)
        points.emplace_back(0.0, 0.0, -0.16 + 0.32 * static_cast<double>(point) /
            static_cast<double>(segments));
    return BuildSectionInertiaRestState(points);
}

TEST(RodSectionInertia, DirectorInertiaContainsOnlyCrossSectionMassMoment)
{
    const Material material;
    for (size_t segments : {1u, 8u, 16u, 32u})
    {
        const auto rest = Straight(segments);
        const auto reference = BuildRestState(rest.positions);
        for (size_t segment = 0; segment < segments; ++segment)
        {
            const double mass = material.density * Pi * material.radius * material.radius *
                rest.lengths[segment];
            EXPECT_DOUBLE_EQ(rest.inertias[segment].x(), 0.25 * mass * material.radius * material.radius);
            EXPECT_DOUBLE_EQ(rest.inertias[segment].y(), rest.inertias[segment].x());
            EXPECT_DOUBLE_EQ(rest.inertias[segment].z(), 2.0 * rest.inertias[segment].x());
            EXPECT_DOUBLE_EQ(rest.inertias[segment].z(), reference.inertias[segment].z());
            EXPECT_GT(reference.inertias[segment].x(), rest.inertias[segment].x());
        }
        EXPECT_EQ(rest.masses, reference.masses);
        EXPECT_EQ(rest.lengths, reference.lengths);
        EXPECT_EQ(rest.supportStiffness, reference.supportStiffness);
    }
}

TEST(RodSectionInertia, TranslationMassAndPureMaterialRotationHaveDeclaredEnergy)
{
    const Material material;
    const Vec3 velocity(0.4, -0.3, 0.7);
    const Vec3 omega(0.8, -0.4, 1.1);
    const double mass = material.density * Pi * material.radius * material.radius * 0.32;
    for (size_t segments : {1u, 8u, 16u, 32u})
    {
        const auto rest = Straight(segments);
        double translation = 0.0;
        double rotation = 0.0;
        double summedMass = 0.0;
        for (double nodeMass : rest.masses)
        {
            summedMass += nodeMass;
            translation += 0.5 * nodeMass * velocity.squaredNorm();
        }
        for (const Vec3& inertia : rest.inertias)
            rotation += 0.5 * omega.dot(inertia.cwiseProduct(omega));
        const Vec3 exactInertia = SectionRotaryInertia(mass, material.radius);
        EXPECT_NEAR(summedMass, mass, 1.0e-18);
        EXPECT_NEAR(translation, 0.5 * mass * velocity.squaredNorm(), 1.0e-18);
        EXPECT_NEAR(rotation, 0.5 * omega.dot(exactInertia.cwiseProduct(omega)), 1.0e-24);
    }
}

TEST(RodSectionInertia, WholeRigidRotationShowsLumpedSpatialQuadratureWithoutDoubleCount)
{
    const Material material;
    const double totalLength = 0.32;
    const double mass = material.density * Pi * material.radius * material.radius * totalLength;
    const Vec3 omega(1.2, 0.0, 0.0);
    double previousError = 1.0;
    for (size_t segments : {1u, 8u, 16u, 32u})
    {
        const auto rest = Straight(segments);
        double energy = 0.0;
        for (size_t point = 0; point < rest.positions.size(); ++point)
            energy += 0.5 * rest.masses[point] *
                omega.cross(rest.positions[point]).squaredNorm();
        for (const Vec3& inertia : rest.inertias)
            energy += 0.5 * omega.dot(inertia.cwiseProduct(omega));
        const double length = totalLength / static_cast<double>(segments);
        const double expectedMoment = mass * (totalLength * totalLength / 12.0 +
            length * length / 6.0 + material.radius * material.radius / 4.0);
        const double continuumMoment = mass * (totalLength * totalLength / 12.0 +
            material.radius * material.radius / 4.0);
        EXPECT_NEAR(energy, 0.5 * expectedMoment * omega.squaredNorm(), 1.0e-19);
        const double error = 2.0 * energy / omega.squaredNorm() - continuumMoment;
        EXPECT_GT(error, 0.0);
        EXPECT_LT(error, previousError);
        EXPECT_NEAR(error, mass * length * length / 6.0, 1.0e-19);
        previousError = error;
        std::cout << "[RodSectionRigid] segments=" << segments << " energy=" << energy
                  << " continuumError=" << error << " kineticMathVersion="
                  << SectionInertiaKineticMathVersion << std::endl;
    }
}

TEST(RodSectionInertia, BodyEulerAndRightAngularJacobianUseSectionMetric)
{
    const auto rest = Straight(8);
    const Vec3 inertia = rest.inertias[2];
    const Vec3 oldVelocity(0.8, -0.4, 1.1);
    double predictorResidual = 0.0;
    const double step = 1.0 / 240.0;
    const auto predicted = PredictBodyAngularVelocity(inertia, oldVelocity, Vec3::Zero(),
        step, predictorResidual);
    EXPECT_LT(predictorResidual, 1.0e-10);
    EXPECT_NEAR(predicted.z(), oldVelocity.z(), 1.0e-12);
    const Quat oldOrientation = Exp(Vec3(0.2, -0.1, 0.3));
    const Quat orientation = ApplyBodyIncrement(oldOrientation, Vec3(0.03, -0.02, 0.04));
    const Vec3 external(2.0e-9, -1.0e-9, 0.5e-9);
    auto residual = [&](const Quat& current)
    {
        const Vec3 angle = Log(oldOrientation.conjugate() * current);
        const Vec3 transported = current.conjugate() *
            (oldOrientation * inertia.cwiseProduct(oldVelocity));
        return (inertia.cwiseProduct(angle) / (step * step) -
            transported / step - current.conjugate() * external).eval();
    };
    const Vec3 angle = Log(oldOrientation.conjugate() * orientation);
    const Vec3 transported = orientation.conjugate() *
        (oldOrientation * inertia.cwiseProduct(oldVelocity));
    const Mat3 analytic = inertia.asDiagonal().toDenseMatrix() * RightJacobianInverse(angle) /
        (step * step) - CrossMatrix(transported) / step -
        CrossMatrix(orientation.conjugate() * external);
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
        for (int axis = 0; axis < 3; ++axis)
        {
            const Vec3 difference = (residual(ApplyBodyIncrement(orientation, epsilon * Vec3::Unit(axis))) -
                residual(ApplyBodyIncrement(orientation, -epsilon * Vec3::Unit(axis)))) / (2.0 * epsilon);
            EXPECT_LT((difference - analytic.col(axis)).norm() / analytic.norm(), 2.0e-6);
        }
}

TEST(RodSectionInertia, InvalidSectionInputsAreRejected)
{
    EXPECT_THROW(SectionRotaryInertia(0.0, 0.0005), std::invalid_argument);
    EXPECT_THROW(SectionRotaryInertia(0.001, -0.0005), std::invalid_argument);
}

RestState SectionCurve(size_t segments)
{
    std::vector<Vec3> positions;
    for (size_t point = 0; point <= segments; ++point)
    {
        const double parameter = static_cast<double>(point) / static_cast<double>(segments);
        positions.emplace_back(0.025 * std::sin(2.0 * parameter),
            0.02 * (1.0 - std::cos(2.0 * parameter)), 0.30 * parameter);
    }
    return BuildSectionInertiaRestState(positions);
}

TEST(RodSectionInertia, CorrectedCompleteMomentumBlocksMatchFiniteDifferenceGolden)
{
    for (size_t segments : {3u, 8u})
    {
        const auto rest = SectionCurve(segments);
        auto old = MakeAuthoredState(rest);
        auto candidate = old;
        StepInput input;
        input.root.position = rest.positions.front();
        input.root.orientation = rest.orientations.front();
        input.externalTorquesWorld.assign(segments, Vec3(2.0e-9, -1.0e-9, 0.5e-9));
        for (size_t segment = 1; segment < segments; ++segment)
        {
            old.angularVelocities[segment] = Vec3(0.8, -0.4, 1.1);
            candidate.orientations[segment] =
                ApplyBodyIncrement(candidate.orientations[segment], Vec3(0.03, -0.02, 0.04));
            candidate.positions[segment] += Vec3(0.0002, -0.0001, 0.0003);
        }
        const auto analytical = AssembleImplicitSystemAnalytic(rest, old, candidate, input);
        double maximumError = 0.0;
        for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
        {
            const auto numerical = AssembleImplicitSystem(rest, old, candidate, input, epsilon);
            for (size_t block = 0; block < rest.positions.size(); ++block)
                for (int band = 0; band < 3; ++band)
                {
                    const Matrix6& expected = band == 0 ? analytical.lower[block] :
                        (band == 1 ? analytical.diagonal[block] : analytical.upper[block]);
                    const Matrix6& actual = band == 0 ? numerical.lower[block] :
                        (band == 1 ? numerical.diagonal[block] : numerical.upper[block]);
                    const double error = (actual - expected).norm() / std::max(1.0, expected.norm());
                    maximumError = std::max(maximumError, error);
                    EXPECT_LT(error, 2.0e-6);
                }
        }
        std::cout << "[RodSectionJac] segments=" << segments
                  << " maxBlockRelative=" << maximumError << std::endl;
    }
}

TEST(RodSectionInertia, CorrectedProgressiveShortN3N8ReportsTrueResidualAndRefinement)
{
    const char* directory = std::getenv("ROD_SECTION_SHORT_EXPORT_DIR");
    if (directory)
    {
        ASSERT_FALSE(std::filesystem::exists(directory));
        ASSERT_TRUE(std::filesystem::create_directories(directory));
    }
    for (size_t segments : {3u, 8u})
    {
        const auto rest = SectionCurve(segments);
        const auto profile = BuildProgressiveSupport(rest);
        std::vector<State> terminals;
        for (unsigned level = 0; level < 3; ++level)
        {
            auto state = MakeAuthoredState(rest);
            StepInput input;
            input.stepSeconds = 1.0 / (240.0 * (1u << level));
            input.root.position = rest.positions.front();
            input.root.orientation = rest.orientations.front();
            for (double mass : rest.masses)
                input.externalForces.push_back(Vec3(20.0 * mass, 0.0, 0.0));
            std::ofstream output;
            if (directory)
            {
                output.open(std::filesystem::path(directory) /
                    ("n" + std::to_string(segments) + "-h" + std::to_string(240u << level) + ".csv"));
                output << std::setprecision(17);
                output << "tick,time,index,x,y,z,vx,vy,vz,qx,qy,qz,qw,wx,wy,wz,mass,Jx,Jy,Jz,k2,Fw,rx,ry,rz,tx,ty,tz,residual,newton,energy,Tlinear,Tangular\n";
            }
            auto write = [&](unsigned tick, const State& before, const ImplicitDiagnostics& diagnostic)
            {
                if (!directory)
                    return;
                const auto residual = EvaluateProgressiveImplicitResidual(rest, before, state, input, profile);
                for (size_t point = 0; point < rest.positions.size(); ++point)
                {
                    const auto orientation = point < segments ? state.orientations[point] : Quat::Identity();
                    const Vec3 omega = point < segments ? state.angularVelocities[point] : Vec3::Zero().eval();
                    const Vec3 inertia = point < segments ? rest.inertias[point] : Vec3::Zero().eval();
                    output << tick << ',' << tick * input.stepSeconds << ',' << point
                           << ',' << state.positions[point].x() << ',' << state.positions[point].y()
                           << ',' << state.positions[point].z() << ',' << state.velocities[point].x()
                           << ',' << state.velocities[point].y() << ',' << state.velocities[point].z()
                           << ',' << orientation.x() << ',' << orientation.y() << ',' << orientation.z()
                           << ',' << orientation.w() << ',' << omega.x() << ',' << omega.y() << ',' << omega.z()
                           << ',' << rest.masses[point] << ',' << inertia.x() << ',' << inertia.y() << ',' << inertia.z()
                           << ',' << profile.cubicStiffness[point] << ',' << profile.designForces[point];
                    for (int component = 0; component < 6; ++component)
                        output << ',' << residual[point][component];
                    output << ',' << diagnostic.normalizedResidual << ',' << diagnostic.newtonIterations
                           << ',' << diagnostic.energy << ',' << diagnostic.linearKineticEnergy
                           << ',' << diagnostic.angularKineticEnergy << '\n';
                }
            };
            write(0, state, ImplicitDiagnostics{});
            double maximumResidual = 0.0;
            unsigned maximumNewton = 0;
            for (unsigned tick = 1; tick <= (6u << level); ++tick)
            {
                const auto before = state;
                const auto result = StepImplicitMomentumProgressive(rest, state, input, profile);
                write(tick, before, result);
                ASSERT_EQ(result.status, Status::Success) << "segments=" << segments
                    << " level=" << level << " tick=" << tick << " residual=" << result.normalizedResidual;
                EXPECT_LE(result.normalizedResidual, 1.0e-8);
                maximumResidual = std::max(maximumResidual, result.normalizedResidual);
                maximumNewton = std::max(maximumNewton, result.newtonIterations);
            }
            terminals.push_back(state);
            std::cout << "[RodSectionShort] segments=" << segments << " hz=" << (240u << level)
                      << " maxResidual=" << maximumResidual << " maxNewton=" << maximumNewton << std::endl;
        }
        Eigen::Vector2d differences[2] = {Eigen::Vector2d::Zero(), Eigen::Vector2d::Zero()};
        for (size_t level = 0; level < 2; ++level)
        {
            for (size_t point = 0; point < rest.positions.size(); ++point)
                differences[level].x() = std::max(differences[level].x(),
                    (terminals[level].positions[point] - terminals[level + 1].positions[point]).norm());
            for (size_t segment = 0; segment < segments; ++segment)
                differences[level].y() = std::max(differences[level].y(),
                    Log(terminals[level].orientations[segment].conjugate() *
                        terminals[level + 1].orientations[segment]).norm());
        }
        std::cout << "[RodSectionDifference] segments=" << segments
                  << " coarseX=" << differences[0].x() << " fineX=" << differences[1].x()
                  << " coarseQ=" << differences[0].y() << " fineQ=" << differences[1].y() << std::endl;
        EXPECT_LT(differences[1].x(), differences[0].x());
        EXPECT_LT(differences[1].y(), differences[0].y());
    }
}
}
