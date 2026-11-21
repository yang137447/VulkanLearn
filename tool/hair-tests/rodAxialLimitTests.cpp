#include "hair/rod/rodAxialLimit.h"
#include "hair/rod/rodAxialKkt.h"
#include "hair/rod/rodAxialStep.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodSectionInertia.h"

#include <gtest/gtest.h>

#include <iostream>

#include <Eigen/LU>

namespace
{
using namespace VL::Hair::Rod;

TEST(RodAxialLimit, ExactEngineeringBoundFiniteReactionAndNoCompressionBound)
{
    const Vec3 origin = Vec3::Zero();
    const auto active = EvaluateAxialLimit(origin, Vec3(0.1, 0.0, 0.0),
        origin, Vec3(0.101, 0.0, 0.0), 0.1, 0.003);
    EXPECT_NEAR(active.gap, 0.0, 1.0e-16);
    EXPECT_NEAR(active.force.x(), 0.003, 1.0e-16);
    EXPECT_NEAR(active.force[3], -0.003, 1.0e-16);
    EXPECT_NEAR(active.work, -3.0e-6, 1.0e-18);
    const auto compressed = EvaluateAxialLimit(origin, Vec3(0.1, 0.0, 0.0),
        origin, Vec3(0.05, 0.0, 0.0), 0.1, 0.0);
    EXPECT_GT(compressed.gap, 0.0);
    EXPECT_DOUBLE_EQ(compressed.force.norm(), 0.0);
    EXPECT_THROW(EvaluateAxialLimit(origin, origin, origin, origin, 0.1, 0.0), std::domain_error);
}

TEST(RodAxialLimit, GapForceAndFullGeometricHessianMatchFiniteDifferences)
{
    const Vec3 left(0.02, -0.03, 0.04);
    const Vec3 right(0.11, 0.02, 0.07);
    const auto exact = EvaluateAxialLimit(left, right, left, right, 0.1, 0.004);
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
        for (int component = 0; component < 6; ++component)
        {
            Vec3 plusLeft = left;
            Vec3 minusLeft = left;
            Vec3 plusRight = right;
            Vec3 minusRight = right;
            if (component < 3)
            {
                plusLeft[component] += epsilon;
                minusLeft[component] -= epsilon;
            }
            else
            {
                plusRight[component - 3] += epsilon;
                minusRight[component - 3] -= epsilon;
            }
            const auto plus = EvaluateAxialLimit(left, right, plusLeft, plusRight, 0.1, 0.004);
            const auto minus = EvaluateAxialLimit(left, right, minusLeft, minusRight, 0.1, 0.004);
            EXPECT_NEAR((plus.gap - minus.gap) / (2.0 * epsilon),
                exact.gapGradient[component], 2.0e-8);
            EXPECT_LT(((plus.gapGradient - minus.gapGradient) / (2.0 * epsilon) -
                exact.gapHessian.col(component)).norm(), 2.0e-6);
            EXPECT_LT((-(plus.force - minus.force) / (2.0 * epsilon) -
                exact.negativeForceDerivative.col(component)).norm(), 1.0e-8);
        }
    EXPECT_LT((exact.gapHessian - exact.gapHessian.transpose()).norm(), 1.0e-15);
    EXPECT_LT((exact.force.head<3>() + exact.force.tail<3>()).norm(), 1.0e-15);
    EXPECT_LT(((right - left).cross(exact.force.tail<3>())).norm(), 1.0e-15);
}

TEST(RodAxialLimit, RigidFrameAndEndpointWorkAreObjectiveAndNonpositive)
{
    const Vec3 oldLeft(0.01, -0.02, 0.03);
    const Vec3 oldRight = oldLeft + Vec3(0.1, 0.0, 0.0);
    const Vec3 newLeft = oldLeft + Vec3(0.002, 0.003, -0.001);
    const Vec3 newRight = newLeft + 0.101 * Vec3(0.9, 0.4, 0.1).normalized();
    const auto original = EvaluateAxialLimit(oldLeft, oldRight, newLeft, newRight, 0.1, 0.004);
    const Mat3 rotation = Eigen::AngleAxisd(0.7, Vec3(1.0, 2.0, 3.0).normalized()).toRotationMatrix();
    const Vec3 translation(1.0, -3.0, 2.0);
    const auto rotated = EvaluateAxialLimit(rotation * oldLeft + translation, rotation * oldRight + translation,
        rotation * newLeft + translation, rotation * newRight + translation, 0.1, 0.004);
    EXPECT_NEAR(original.gap, rotated.gap, 1.0e-15);
    EXPECT_LT((rotation * original.force.head<3>() - rotated.force.head<3>()).norm(), 1.0e-15);
    EXPECT_NEAR(original.work, rotated.work, 1.0e-17);
    EXPECT_LE(original.work, 0.0);
    EXPECT_NEAR(original.work, -0.004 * original.direction.dot(
        (newRight - newLeft) - (oldRight - oldLeft)), 1.0e-18);
}

TEST(RodAxialLimit, IncomingPinnedEdgeHasInvertibleLocalKktAndAnalyticTensileBalance)
{
    const double inertial = 2.0;
    const double stiffness = 0.8;
    const double restLength = 0.1;
    const double bound = 1.01 * restLength;
    const double force = 0.02;
    const double reaction = force - stiffness * 0.01 - inertial * (bound - restLength);
    ASSERT_GT(reaction, 0.0);
    const auto edge = EvaluateAxialLimit(Vec3::Zero(), Vec3(restLength, 0.0, 0.0),
        Vec3::Zero(), Vec3(bound, 0.0, 0.0), restLength, reaction);
    EXPECT_NEAR(inertial * (bound - restLength) + stiffness * 0.01 -
        force - edge.force[3], 0.0, 1.0e-16);
    Eigen::Matrix2d kkt;
    kkt << inertial + stiffness / restLength, 1.0, -1.0, 0.0;
    EXPECT_TRUE(kkt.fullPivLu().isInvertible());
    const Eigen::Vector2d residual(0.003, -0.0002);
    const Eigen::Vector2d direction = kkt.fullPivLu().solve(-residual);
    EXPECT_LT((kkt * direction + residual).norm(), 1.0e-16);
}

TEST(RodAxialLimit, CombinedN3N8FullJacobianAndIncomingBandSolveMatchDenseOracle)
{
    for (size_t segments : {3u, 8u})
    {
        std::vector<Vec3> points;
        for (size_t point = 0; point <= segments; ++point)
            points.emplace_back(0.0, 0.0, 0.02 * static_cast<double>(point));
        std::vector<double> weights(points.size(), 1.0);
        weights[0] = 0.0;
        weights[1] = 0.0;
        const auto rest = BuildSectionInertiaRestState(points, {}, {}, weights);
        const auto old = MakeAuthoredState(rest);
        auto candidate = old;
        for (size_t point = 2; point < points.size(); ++point)
            candidate.positions[point] += Vec3(0.0001, -0.00005, 0.0002 * static_cast<double>(point - 1));
        StepInput input;
        input.root.position = points.front();
        input.root.orientation = rest.orientations.front();
        const auto profile = BuildProgressiveSupport(rest);
        AxialContactActiveSet active;
        active.contacts.resize(points.size());
        active.tensileActive.assign(segments, true);
        active.tensileActive[0] = false;
        active.tensileMultipliers.assign(segments, 0.0002);
        active.contacts.back().identities[0] = 0;
        active.contacts.back().multipliers[0] = 0.0003;
        const std::vector<ContactConstraint> constraints{{"plane", [](const Vec3& position)
        {
            ContactGeometry geometry;
            geometry.valid = true;
            geometry.distance = position.y();
            geometry.gradient = Vec3::UnitY();
            return geometry;
        }}};
        const FrozenTangentAir air{Vec3(20.0, 0.0, 0.0), 1.0};
        const auto system = AssembleAxialContactSystem(rest, old, candidate, input, profile, constraints, active, &air);
        const Eigen::Index dimension = static_cast<Eigen::Index>(10 * points.size());
        Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(dimension, dimension);
        Eigen::VectorXd residual(dimension);
        for (size_t point = 0; point < points.size(); ++point)
        {
            const auto offset = static_cast<Eigen::Index>(10 * point);
            matrix.block<10, 10>(offset, offset) = system.diagonal[point];
            residual.segment<10>(offset) = system.residual[point];
            if (point > 0)
                matrix.block<10, 10>(offset, offset - 10) = system.lower[point];
            if (point + 1 < points.size())
                matrix.block<10, 10>(offset, offset + 10) = system.upper[point];
        }
        const auto blocks = SolveAxialContactSystem(system);
        Eigen::VectorXd direction(dimension);
        for (size_t point = 0; point < points.size(); ++point)
            direction.segment<10>(static_cast<Eigen::Index>(10 * point)) = blocks[point];
        const Eigen::VectorXd dense = matrix.fullPivLu().solve(-residual);
        EXPECT_LT((direction - dense).norm() / std::max(1.0, dense.norm()), 1.0e-8);
        EXPECT_LT((matrix * direction + residual).norm(), 1.0e-9);
        for (size_t point = 0; point < points.size(); ++point)
            for (int component = 0; component < 10; ++component)
            {
                if ((component < 3 && rest.inverseMasses[point] == 0.0) ||
                    (component >= 3 && component < 6 && (point == 0 || point >= segments)) ||
                    (component >= 6 && component < 9 && active.contacts[point].identities[component - 6] < 0) ||
                    (component == 9 && (point == 0 || !active.tensileActive[point - 1])))
                    continue;
                for (double epsilon : {1.0e-6, 1.0e-7})
                {
                    auto plus = candidate;
                    auto minus = candidate;
                    auto plusActive = active;
                    auto minusActive = active;
                    if (component < 3)
                    {
                        plus.positions[point][component] += epsilon * system.lengthScale;
                        minus.positions[point][component] -= epsilon * system.lengthScale;
                    }
                    else if (component < 6)
                    {
                        plus.orientations[point] = ApplyBodyIncrement(candidate.orientations[point],
                            epsilon * Vec3::Unit(component - 3));
                        minus.orientations[point] = ApplyBodyIncrement(candidate.orientations[point],
                            -epsilon * Vec3::Unit(component - 3));
                    }
                    else if (component < 9)
                    {
                        plusActive.contacts[point].multipliers[component - 6] += epsilon * system.forceScale;
                        minusActive.contacts[point].multipliers[component - 6] -= epsilon * system.forceScale;
                    }
                    else
                    {
                        plusActive.tensileMultipliers[point - 1] += epsilon * system.forceScale;
                        minusActive.tensileMultipliers[point - 1] -= epsilon * system.forceScale;
                    }
                    const auto positive = AssembleAxialContactSystem(rest, old, plus, input, profile,
                        constraints, plusActive, &air);
                    const auto negative = AssembleAxialContactSystem(rest, old, minus, input, profile,
                        constraints, minusActive, &air);
                    Eigen::VectorXd numerical(dimension);
                    for (size_t row = 0; row < points.size(); ++row)
                        numerical.segment<10>(static_cast<Eigen::Index>(10 * row)) =
                            (positive.residual[row] - negative.residual[row]) / (2.0 * epsilon);
                    const auto column = static_cast<Eigen::Index>(10 * point + component);
                    EXPECT_LT((numerical - matrix.col(column)).norm() / std::max(1.0, numerical.norm()),
                        2.0e-6) << segments << ":" << point << ":" << component << ":" << epsilon;
                }
            }
    }
}

TEST(RodAxialLimit, SingularLocalPivotUsesExactTwoNodeSupernodeAndRejectsGlobalRedundancy)
{
    AxialContactSystem system;
    system.lower.assign(3, AxialContactMatrix::Zero());
    system.upper.assign(3, AxialContactMatrix::Zero());
    system.diagonal.assign(3, AxialContactMatrix::Identity());
    system.residual.assign(3, AxialContactVector::Constant(0.002));
    system.diagonal[0](9, 9) = 0.0;
    system.upper[0](9, 0) = 1.0;
    system.lower[1](0, 9) = -1.0;
    system.upper[1](2, 3) = 0.1;
    system.lower[2](3, 2) = -0.2;
    ASSERT_FALSE(system.diagonal[0].fullPivLu().isInvertible());
    Eigen::MatrixXd full = Eigen::MatrixXd::Zero(30, 30);
    Eigen::VectorXd residual(30);
    for (size_t point = 0; point < 3; ++point)
    {
        const auto offset = static_cast<Eigen::Index>(10 * point);
        full.block<10, 10>(offset, offset) = system.diagonal[point];
        residual.segment<10>(offset) = system.residual[point];
        if (point > 0)
            full.block<10, 10>(offset, offset - 10) = system.lower[point];
        if (point < 2)
            full.block<10, 10>(offset, offset + 10) = system.upper[point];
    }
    ASSERT_TRUE(full.fullPivLu().isInvertible());
    const auto blocks = SolveAxialContactSystem(system);
    Eigen::VectorXd direction(30);
    for (size_t point = 0; point < 3; ++point)
        direction.segment<10>(static_cast<Eigen::Index>(10 * point)) = blocks[point];
    const Eigen::VectorXd dense = full.fullPivLu().solve(-residual);
    EXPECT_LT((direction - dense).norm(), 1.0e-15);
    EXPECT_LT((full * direction + residual).norm(), 1.0e-15);
    system.upper[0].setZero();
    system.lower[1].setZero();
    EXPECT_THROW(SolveAxialContactSystem(system), std::runtime_error);
}

TEST(RodAxialLimit, TensileReleaseStepPreservesPinsFiniteMaterialAndOriginalWorkGates)
{
    for (size_t segments : {3u, 8u, 32u})
        for (double step : {1.0 / 240.0, 1.0 / 480.0, 1.0 / 960.0})
        {
            std::vector<Vec3> points;
            for (size_t point = 0; point <= segments; ++point)
                points.emplace_back(0.0, 0.0, 0.16 * static_cast<double>(point) / segments);
            std::vector<double> weights(points.size(), 1.0);
            weights[0] = 0.0;
            weights[1] = 0.0;
            const auto rest = BuildSectionInertiaRestState(points, {}, {}, weights);
            const auto profile = BuildProgressiveSupport(rest);
            auto state = MakeAuthoredState(rest);
            StepInput input;
            input.stepSeconds = step;
            input.root.position = points.front();
            input.root.orientation = rest.orientations.front();
            input.gravity.setZero();
            input.externalForces.assign(points.size(), Vec3::Zero());
            input.externalForces.back() = Vec3(0.0, 0.0, 0.04);
            AxialStepTrace trace;
            const auto loaded = StepAxialContactImplicit(rest, state, input, profile, {}, nullptr, &trace);
            ASSERT_EQ(loaded.status, Status::Success) << segments << ":" << step << ":" << loaded.failureReason;
            EXPECT_TRUE(loaded.workCertificate.accepted);
            EXPECT_LE(loaded.normalizedResidual, 1.0e-8);
            EXPECT_LE(loaded.maximumTensileViolation, 1.0e-9 * rest.totalLength);
            EXPECT_LE(loaded.maximumComplementarity, 1.0e-9);
            EXPECT_LE(loaded.newtonIterations, 132u);
            EXPECT_EQ(state.positions[0], points[0]);
            EXPECT_EQ(state.positions[1], points[1]);
            bool hasReaction = false;
            for (size_t edge = 0; edge < segments; ++edge)
            {
                const double strain = (state.positions[edge + 1] - state.positions[edge]).norm() /
                    rest.lengths[edge] - 1.0;
                EXPECT_LE(strain, 0.01 + 1.0e-7);
                hasReaction = hasReaction || trace.finalActive.tensileMultipliers[edge] > 0.0;
            }
            EXPECT_TRUE(hasReaction);
            EXPECT_LE(loaded.tensileWork, 1.0e-12);
            input.externalForces.back().setZero();
            AxialStepTrace releaseTrace;
            const auto released = StepAxialContactImplicit(rest, state, input, profile, {}, nullptr, &releaseTrace);
            if (released.status != Status::Success)
            {
                std::cout << "[AxialReleaseFailure] segments=" << segments << " h=" << step
                          << " norm=" << released.normalizedResidual
                          << " workUpper=" << released.workCertificate.workBoundScaled
                          << " balance=" << released.workCertificate.balanceScaled << std::endl;
                for (const auto& iteration : releaseTrace.iterations)
                {
                    std::cout << "[AxialReleaseIteration] fraction=" << iteration.loadFraction
                              << " fallback=" << iteration.fallback << " norm=" << iteration.normalizedResidual;
                    for (size_t edge = 0; edge < segments; ++edge)
                        std::cout << " edge" << edge << "=" << iteration.active.tensileActive[edge]
                                  << ":" << iteration.active.tensileMultipliers[edge];
                    for (const auto& trial : iteration.trials)
                        std::cout << " trial=" << trial.alpha << ":" << trial.normalizedResidual << ":" << trial.accepted;
                    std::cout << std::endl;
                }
            }
            ASSERT_EQ(released.status, Status::Success) << segments << ":" << step << ":" << released.failureReason;
            EXPECT_TRUE(released.workCertificate.accepted);
            EXPECT_EQ(state.positions[0], points[0]);
            EXPECT_EQ(state.positions[1], points[1]);
        }
}
}
