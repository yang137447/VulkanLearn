#include "hair/rod/rodAirDrag.h"
#include "hair/rod/rodImplicit.h"
#include "hair/rod/rodSectionInertia.h"

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>

namespace
{
using namespace VL::Hair::Rod;

RestState AirRest(size_t segments)
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

TEST(RodImplicitAir, CompleteVelocityDerivativeMatchesSameForceLaw)
{
    const auto rest = AirRest(3);
    const auto profile = BuildProgressiveSupport(rest);
    const auto old = MakeAuthoredState(rest);
    auto candidate = old;
    const double step = 1.0 / 240.0;
    candidate.positions[2] += step * Vec3(0.3, -0.2, 0.1);
    const Vec3 air(20.0, -0.4, 0.2);
    const auto drag = EvaluateFrozenTangentAirDrag(old, candidate, 2, rest.masses[2], step, profile, air);
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
        for (int axis = 0; axis < 3; ++axis)
        {
            auto positive = candidate;
            auto negative = candidate;
            positive.positions[2][axis] += epsilon * step;
            negative.positions[2][axis] -= epsilon * step;
            const auto first = EvaluateFrozenTangentAirDrag(old, positive, 2, rest.masses[2], step, profile, air);
            const auto second = EvaluateFrozenTangentAirDrag(old, negative, 2, rest.masses[2], step, profile, air);
            const Vec3 numerical = (first.force - second.force) / (2.0 * epsilon);
            EXPECT_LT((numerical + drag.negativeVelocityDerivative.col(axis)).norm() /
                std::max(1.0e-10, drag.negativeVelocityDerivative.norm()), 2.0e-6);
        }
}

TEST(RodImplicitAir, ScalarBackwardEulerDragHasExactDiscretePositiveRoot)
{
    const double coefficient = EffectiveBundleDragAccelerationPerSpeed();
    const double oldSpeed = 20.0;
    for (double step : {1.0 / 240.0, 1.0 / 480.0, 1.0 / 960.0})
    {
        const double nextSpeed = 2.0 * oldSpeed /
            (1.0 + std::sqrt(1.0 + 4.0 * step * coefficient * oldSpeed));
        EXPECT_NEAR((nextSpeed - oldSpeed) / step + coefficient * nextSpeed * nextSpeed, 0.0, 1.0e-10);
        EXPECT_GT(nextSpeed, 0.0);
        EXPECT_LT(nextSpeed, oldSpeed);
        const double exactContinuous = oldSpeed / (1.0 + coefficient * oldSpeed * step);
        EXPECT_GT(nextSpeed, exactContinuous);
    }
}

TEST(RodImplicitAir, FullMomentumJacobianIncludesActualEndVelocityDrag)
{
    const auto rest = AirRest(8);
    const auto profile = BuildProgressiveSupport(rest);
    const auto old = MakeAuthoredState(rest);
    auto candidate = old;
    candidate.positions[3] += Vec3(0.0003, -0.0002, 0.0001);
    StepInput input;
    input.root.position = rest.positions.front();
    input.root.orientation = rest.orientations.front();
    const FrozenTangentAir air{Vec3(20.0, 0.0, 0.0), 1.0};
    const auto system = AssembleProgressiveAirSystem(rest, old, candidate, input, profile, air);
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
        for (int axis = 0; axis < 3; ++axis)
        {
            auto positive = candidate;
            auto negative = candidate;
            positive.positions[3][axis] += epsilon * rest.totalLength;
            negative.positions[3][axis] -= epsilon * rest.totalLength;
            const Vec3 numerical = ((EvaluateProgressiveAirResidual(rest, old, positive, input, profile, air)[3] -
                EvaluateProgressiveAirResidual(rest, old, negative, input, profile, air)[3]) /
                (2.0 * epsilon)).head<3>() / system.forceScale;
            EXPECT_LT((numerical - system.diagonal[3].block<3, 1>(0, axis)).norm() /
                std::max(1.0, numerical.norm()), 2.0e-6);
        }
}

TEST(RodImplicitAir, ShortActualAirUsesNewResidualAndUnchangedWorkGate)
{
    for (size_t segments : {3u, 8u})
    {
        const auto rest = AirRest(segments);
        const auto profile = BuildProgressiveSupport(rest);
        for (unsigned level = 0; level < 3; ++level)
        {
            auto state = MakeAuthoredState(rest);
            StepInput input;
            input.root.position = rest.positions.front();
            input.root.orientation = rest.orientations.front();
            input.stepSeconds = 1.0 / (240.0 * (1u << level));
            double maximumResidual = 0.0;
            unsigned maximumNewton = 0;
            for (unsigned tick = 0; tick < (6u << level); ++tick)
            {
                const State old = state;
                ResidualWorkCertificate certificate;
                const auto result = StepImplicitMomentumProgressiveAirContinuation(
                    rest, state, input, profile, Vec3(20.0, 0.0, 0.0), certificate);
                ASSERT_EQ(result.status, Status::Success)
                    << "segments=" << segments << " level=" << level << " tick=" << tick
                    << " residual=" << result.normalizedResidual;
                EXPECT_TRUE(certificate.accepted);
                EXPECT_LE(result.normalizedResidual, 1.0e-8);
                EXPECT_LE(certificate.balanceScaled, 1.0e-10);
                maximumResidual = std::max(maximumResidual, result.normalizedResidual);
                maximumNewton = std::max(maximumNewton, result.newtonIterations);
                const auto residual = EvaluateProgressiveAirResidual(rest, old, state, input,
                    profile, {Vec3(20.0, 0.0, 0.0), 1.0});
                for (size_t point = 1; point < rest.positions.size(); ++point)
                    EXPECT_TRUE(residual[point].allFinite());
            }
            std::cout << "[RodImplicitAirShort] segments=" << segments << " hz=" << (240u << level)
                      << " maximumResidual=" << maximumResidual << " maximumNewton=" << maximumNewton << std::endl;
        }
    }
}
}
