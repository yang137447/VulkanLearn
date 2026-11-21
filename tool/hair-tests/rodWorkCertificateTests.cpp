#include "hair/rod/rodWorkCertificate.h"
#include "hair/rod/rodImplicit.h"
#include "hair/rod/rodSectionInertia.h"

#include <gtest/gtest.h>

#include <iostream>
#include <stdexcept>

namespace
{
using namespace VL::Hair::Rod;

RestState WorkRest()
{
    std::vector<Vec3> positions;
    for (size_t point = 0; point <= 3; ++point)
        positions.emplace_back(0.003 * point * point, 0.001 * point, 0.04 * point);
    return BuildSectionInertiaRestState(positions);
}

StepInput WorkInput(const RestState& rest)
{
    StepInput input;
    input.root.position = rest.positions.front();
    input.root.orientation = rest.orientations.front();
    for (double mass : rest.masses)
        input.externalForces.push_back(mass * Vec3(20.0, 0.0, 0.0));
    return input;
}

TEST(RodWorkCertificate, FrozenTargetBalanceMatchesActualFreeResidualWork)
{
    const auto rest = WorkRest();
    const auto profile = BuildProgressiveSupport(rest);
    auto old = MakeAuthoredState(rest);
    auto candidate = old;
    const auto input = WorkInput(rest);
    for (size_t point = 1; point < rest.positions.size(); ++point)
    {
        old.velocities[point] = Vec3(0.1, -0.2, 0.05);
        candidate.positions[point] += Vec3(0.0003, -0.0002, 0.0001);
    }
    for (size_t segment = 1; segment < rest.lengths.size(); ++segment)
    {
        old.angularVelocities[segment] = Vec3(0.2, -0.1, 0.3);
        candidate.orientations[segment] =
            ApplyBodyIncrement(candidate.orientations[segment], Vec3(0.002, -0.001, 0.003));
    }
    const auto certificate = EvaluateProgressiveResidualWork(rest, old, candidate, input, profile);
    EXPECT_TRUE(certificate.finite);
    EXPECT_GE(certificate.freeResidualWorkUpper, std::abs(certificate.freeResidualWork));
    EXPECT_LT(certificate.identityScaled, 1.0e-12);
    EXPECT_FALSE(certificate.accepted);
    EXPECT_GT(certificate.workBoundScaled, 5.0e-11);
}

TEST(RodWorkCertificate, ExplicitGuardKeepsOriginalEquationAndEnergyBudget)
{
    const auto rest = WorkRest();
    const auto profile = BuildProgressiveSupport(rest);
    auto state = MakeAuthoredState(rest);
    auto input = WorkInput(rest);
    for (unsigned step = 0; step < 6; ++step)
    {
        const State old = state;
        ResidualWorkCertificate certificate;
        const auto result = StepImplicitMomentumProgressiveWorkGuard(rest, state, input, profile, certificate);
        ASSERT_EQ(result.status, Status::Success);
        EXPECT_LE(result.newtonIterations, 12u);
        EXPECT_LE(result.normalizedResidual, 1.0e-8);
        EXPECT_TRUE(certificate.accepted);
        EXPECT_LE(certificate.workBoundScaled, 5.0e-11);
        EXPECT_LE(certificate.balanceScaled, 1.0e-10);
        EXPECT_LE(certificate.identityScaled, 1.0e-10);
        const auto independentlyRecomputed = EvaluateProgressiveResidualWork(rest, old, state, input, profile);
        EXPECT_DOUBLE_EQ(certificate.workBoundScaled, independentlyRecomputed.workBoundScaled);
        std::cout << "[RodWorkGuard] step=" << step << " Newton=" << result.newtonIterations
                  << " residual=" << result.normalizedResidual << " Wupper/Eref=" << certificate.workBoundScaled
                  << " B/SB=" << certificate.balanceScaled << std::endl;
    }
}

TEST(RodWorkCertificate, FrozenTargetMathSeparatesPrescribedRootConstraintWork)
{
    const auto rest = WorkRest();
    const auto profile = BuildProgressiveSupport(rest);
    const auto old = MakeAuthoredState(rest);
    auto candidate = old;
    auto input = WorkInput(rest);
    input.root.position += Vec3(0.001, -0.002, 0.003);
    input.root.orientation = ApplyBodyIncrement(input.root.orientation, Vec3(0.002, -0.003, 0.001));
    input.root.linearVelocity = Vec3(0.1, -0.2, 0.3);
    input.root.angularVelocityWorld = Vec3(-0.1, 0.2, 0.1);
    candidate.positions.front() = input.root.position;
    candidate.orientations.front() = input.root.orientation;
    candidate.positions.back() += Vec3(0.0002, 0.0003, -0.0001);
    const auto certificate = EvaluateProgressiveResidualWork(rest, old, candidate, input, profile);
    EXPECT_TRUE(certificate.finite);
    EXPECT_GT(std::abs(certificate.rootConstraintWork), 0.0);
    EXPECT_LT(certificate.identityScaled, 1.0e-12);
}

TEST(RodWorkCertificate, MalformedCandidateIsRejectedAtPublicBoundary)
{
    const auto rest = WorkRest();
    const auto profile = BuildProgressiveSupport(rest);
    const auto old = MakeAuthoredState(rest);
    auto candidate = old;
    candidate.velocities.clear();
    const auto input = WorkInput(rest);
    EXPECT_THROW(EvaluateProgressiveResidualWork(rest, old, candidate, input, profile), std::invalid_argument);
}
}
