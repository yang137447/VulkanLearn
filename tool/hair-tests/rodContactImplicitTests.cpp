#include "hair/rod/rodContactImplicit.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodSectionInertia.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>

#include <Eigen/LU>

namespace
{
using namespace VL::Hair::Rod;

RestState ContactRest()
{
    std::vector<Vec3> points;
    for (size_t point = 0; point < 4; ++point)
        points.emplace_back(0.0, 0.0, 0.04 * point);
    return BuildSectionInertiaRestState(points, {}, {}, {0.0, 0.0, 1.0, 1.0});
}

ContactConstraint Plane(const std::string& identity, const Vec3& normal,
    double offset, double scale = 1.0, const Vec3& velocity = Vec3::Zero())
{
    return {identity, [normal, offset, scale, velocity](const Vec3& position)
    {
        ContactGeometry geometry;
        geometry.valid = true;
        geometry.distance = scale * (normal.dot(position) - offset);
        geometry.gradient = scale * normal;
        geometry.surfaceVelocity = velocity;
        return geometry;
    }, scale};
}

StepInput ContactInput(const RestState& rest)
{
    StepInput input;
    input.root.position = rest.positions.front();
    input.root.orientation = rest.orientations.front();
    input.supportEnabled = false;
    return input;
}

void ExportContact(const std::string& name, const ContactImplicitDiagnostics& diagnostics)
{
    const char* directory = std::getenv("ROD_CONTACT_EXPORT_DIR");
    if (!directory)
        return;
    std::filesystem::create_directories(directory);
    std::ofstream output(std::filesystem::path(directory) / (name + ".csv"));
    output << std::setprecision(17);
    output << "point,identity,active,pinnedExcluded,gap,geometricGap,lambda,gx,gy,gz,fx,fy,fz,vcx,vcy,vcz,complementarity,rx,rq,contactPower,status\n";
    for (const auto& record : diagnostics.contacts)
        output << record.point << ',' << record.identity << ',' << record.active << ','
               << record.pinnedExcluded << ',' << record.rawGap << ',' << record.geometricGap
               << ',' << record.reactionMultiplier << ',' << record.gradient.x() << ','
               << record.gradient.y() << ',' << record.gradient.z() << ',' << record.force.x()
               << ',' << record.force.y() << ',' << record.force.z() << ',' << record.surfaceVelocity.x()
               << ',' << record.surfaceVelocity.y() << ',' << record.surfaceVelocity.z()
               << ',' << record.complementarity << ',' << diagnostics.maximumLinearResidual
               << ',' << diagnostics.maximumAngularResidual << ',' << diagnostics.contactPower
               << ',' << static_cast<int>(diagnostics.status) << '\n';
}

TEST(RodContactImplicit, PlaneReactionBalancesGravityAndImplicitFunctionScale)
{
    const auto rest = ContactRest();
    const auto profile = BuildProgressiveSupport(rest);
    const auto input = ContactInput(rest);
    for (double scale : {0.3, 1.0, 3.5})
    {
        auto state = MakeAuthoredState(rest);
        const auto result = StepImplicitMomentumContact(rest, state, input, profile,
            {Plane("plane", Vec3::UnitY(), 0.0, scale)});
        ASSERT_EQ(result.status, Status::Success) << result.failureReason;
        EXPECT_TRUE(result.converged);
        EXPECT_LT(result.maximumLinearResidual, 1.0e-10);
        EXPECT_LT(result.maximumGeometricPenetration, 1.0e-12);
        EXPECT_LT(result.maximumComplementarity, 1.0e-12);
        for (const auto& record : result.contacts)
            if (!record.pinnedExcluded)
            {
                EXPECT_NEAR(record.force.y(), rest.masses[record.point] * 9.8, 1.0e-11);
                EXPECT_NEAR(record.reactionMultiplier * scale,
                    rest.masses[record.point] * 9.8, 1.0e-11);
            }
        ExportContact("plane-scale-" + std::to_string(scale), result);
    }
}

TEST(RodContactImplicit, SeparatingLoadDeletesNegativeActiveReaction)
{
    const auto rest = ContactRest();
    const auto profile = BuildProgressiveSupport(rest);
    auto input = ContactInput(rest);
    input.gravity = Vec3(0.0, 9.8, 0.0);
    auto state = MakeAuthoredState(rest);
    const auto result = StepImplicitMomentumContact(rest, state, input, profile,
        {Plane("plane", Vec3::UnitY(), 0.0)});
    ASSERT_EQ(result.status, Status::Success) << result.failureReason;
    EXPECT_GT(state.positions.back().y(), 0.0);
    for (const auto& record : result.contacts)
        if (!record.pinnedExcluded)
            EXPECT_NEAR(record.reactionMultiplier, 0.0, 1.0e-12);
    ExportContact("separating", result);
}

TEST(RodContactImplicit, PhysicalMarginIsUnchangedByRawImplicitFunctionScale)
{
    const auto rest = ContactRest();
    const auto profile = BuildProgressiveSupport(rest);
    auto input = ContactInput(rest);
    input.contactMargin = 0.001;
    std::vector<State> results;
    for (double scale : {0.3, 1.0, 3.5})
    {
        auto state = MakeAuthoredState(rest);
        const auto result = StepImplicitMomentumContact(rest, state, input, profile,
            {Plane("plane", Vec3::UnitY(), 0.0, scale)});
        ASSERT_EQ(result.status, Status::Success) << result.failureReason;
        for (size_t point = 2; point < state.positions.size(); ++point)
            EXPECT_NEAR(state.positions[point].y(), input.contactMargin, 1.0e-12);
        results.push_back(state);
        ExportContact("nonzero-margin-scale-" + std::to_string(scale), result);
    }
    for (size_t tier = 1; tier < results.size(); ++tier)
        for (size_t point = 0; point < rest.positions.size(); ++point)
            EXPECT_LT((results[tier].positions[point] - results[0].positions[point]).norm(), 1.0e-10);
}

TEST(RodContactImplicit, MovingPlaneRecordsPhysicalReactionPower)
{
    const auto rest = ContactRest();
    const auto profile = BuildProgressiveSupport(rest);
    const auto input = ContactInput(rest);
    auto state = MakeAuthoredState(rest);
    const double speed = 0.02;
    const auto result = StepImplicitMomentumContact(rest, state, input, profile,
        {Plane("moving", Vec3::UnitY(), speed * input.stepSeconds, 1.0,
            Vec3(0.0, speed, 0.0))});
    ASSERT_EQ(result.status, Status::Success) << result.failureReason;
    EXPECT_LT(result.maximumLinearResidual, 1.0e-9);
    EXPECT_GT(result.contactPower, 0.0);
    for (size_t point = 2; point < state.positions.size(); ++point)
    {
        EXPECT_NEAR(state.positions[point].y(), speed * input.stepSeconds, 1.0e-12);
        EXPECT_NEAR(state.velocities[point].y(), speed, 1.0e-10);
    }
    for (const auto& record : result.contacts)
        if (record.pinnedExcluded)
            EXPECT_LT(record.rawGap, 0.0);
    ExportContact("moving-plane", result);
}

TEST(RodContactImplicit, CornerKeepsDuplicateLabelsAndRejectsConflictingSurfaces)
{
    const auto rest = ContactRest();
    const auto profile = BuildProgressiveSupport(rest);
    auto input = ContactInput(rest);
    input.gravity = Vec3(-9.8, -9.8, 0.0);
    auto state = MakeAuthoredState(rest);
    const auto corner = StepImplicitMomentumContact(rest, state, input, profile,
        {Plane("x", Vec3::UnitX(), 0.0), Plane("duplicate-x", Vec3::UnitX(), 0.0),
            Plane("y", Vec3::UnitY(), 0.0)});
    ASSERT_EQ(corner.status, Status::Success) << corner.failureReason;
    EXPECT_EQ(corner.contacts.size(), rest.positions.size() * 3);
    EXPECT_GT(corner.dependentNormals, 0u);
    EXPECT_LT(corner.maximumLinearResidual, 1.0e-10);
    ExportContact("corner-duplicate", corner);
    state = MakeAuthoredState(rest);
    const auto before = state;
    const auto conflict = StepImplicitMomentumContact(rest, state, input, profile,
        {Plane("upper", Vec3::UnitY(), 0.01), Plane("lower", -Vec3::UnitY(), 0.01)});
    EXPECT_NE(conflict.status, Status::Success);
    EXPECT_EQ(state.positions.back(), before.positions.back());
}

TEST(RodContactImplicit, InitialOverlapRepairPreservesPhysicalVelocityAndOmega)
{
    auto rest = ContactRest();
    for (auto& position : rest.positions)
        position.y() -= 0.01;
    rest = BuildSectionInertiaRestState(rest.positions, {}, {}, {0.0, 0.0, 1.0, 1.0});
    const auto profile = BuildProgressiveSupport(rest);
    const auto input = ContactInput(rest);
    auto state = MakeAuthoredState(rest);
    state.velocities[2] = Vec3(0.1, -0.2, 0.3);
    state.angularVelocities[2] = Vec3(0.2, 0.3, -0.1);
    const auto before = state;
    const auto result = RepairInitialContactOverlap(rest, state, input, profile,
        {Plane("plane", Vec3::UnitY(), 0.0)});
    ASSERT_EQ(result.status, Status::Success);
    EXPECT_EQ(state.velocities[2], before.velocities[2]);
    EXPECT_EQ(state.angularVelocities[2], before.angularVelocities[2]);
    EXPECT_EQ(state.positions[0], before.positions[0]);
    EXPECT_EQ(state.positions[1], before.positions[1]);
    EXPECT_NEAR(state.positions[2].y(), 0.0, 1.0e-14);
    EXPECT_TRUE(std::isfinite(result.potentialEnergyChange));
    EXPECT_GT(result.corrections[2].norm(), 0.0);
}

TEST(RodContactImplicit, CurvedNineBlockJacobianAndThomasMatchWholeFiniteDifferences)
{
    const auto rest = ContactRest();
    const auto profile = BuildProgressiveSupport(rest);
    const auto old = MakeAuthoredState(rest);
    auto candidate = old;
    candidate.positions[2] += Vec3(0.002, -0.001, 0.001);
    candidate.orientations[2] = ApplyBodyIncrement(candidate.orientations[2], Vec3(0.03, -0.02, 0.04));
    const auto input = ContactInput(rest);
    const std::vector<ContactConstraint> constraints{
        MakeSphereContact("sphere", {Vec3(0.0, -0.10, 0.05), 0.1, Vec3(0.0, 0.02, 0.0)})};
    std::vector<ContactActiveNode> active(rest.positions.size());
    for (size_t point = 2; point < rest.positions.size(); ++point)
    {
        active[point].identities[0] = 0;
        active[point].multipliers.x() = 0.001;
    }
    const auto system = AssembleContactSystem(rest, old, candidate, input, profile, constraints, active);
    std::ofstream matrixOutput;
    if (const char* directory = std::getenv("ROD_CONTACT_EXPORT_DIR"))
    {
        std::filesystem::create_directories(directory);
        matrixOutput.open(std::filesystem::path(directory) / "curved-nine-kkt-matrices.csv");
        matrixOutput << std::setprecision(17);
        matrixOutput << "kind,epsilon,row,column,value\n";
    }
    const Eigen::Index dimension = static_cast<Eigen::Index>(9 * rest.positions.size());
    Eigen::MatrixXd matrix = Eigen::MatrixXd::Zero(dimension, dimension);
    Eigen::VectorXd residual(dimension);
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        const auto offset = static_cast<Eigen::Index>(9 * point);
        matrix.block<9, 9>(offset, offset) = system.diagonal[point];
        residual.segment<9>(offset) = system.residual[point];
        if (point > 0) matrix.block<9, 9>(offset, offset - 9) = system.lower[point];
        if (point + 1 < rest.positions.size()) matrix.block<9, 9>(offset, offset + 9) = system.upper[point];
    }
    double maximumError = 0.0;
    if (matrixOutput)
        for (Eigen::Index row = 0; row < dimension; ++row)
        {
            matrixOutput << "residual,0," << row << ",0," << residual[row] << '\n';
            for (Eigen::Index column = 0; column < dimension; ++column)
                matrixOutput << "assembled,0," << row << ',' << column << ',' << matrix(row, column) << '\n';
        }
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
      for (size_t point = 1; point < rest.positions.size(); ++point)
        for (int component = 0; component < 7; ++component)
        {
            if ((component < 3 && rest.inverseMasses[point] == 0.0) ||
                (component >= 3 && component < 6 && point == rest.lengths.size()) ||
                (component == 6 && active[point].identities[0] < 0))
                continue;
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
            else
            {
                plusActive[point].multipliers.x() += epsilon * system.forceScale;
                minusActive[point].multipliers.x() -= epsilon * system.forceScale;
            }
            const auto positive = AssembleContactSystem(rest, old, plus, input, profile, constraints, plusActive);
            const auto negative = AssembleContactSystem(rest, old, minus, input, profile, constraints, minusActive);
            Eigen::VectorXd numerical(dimension);
            for (size_t row = 0; row < rest.positions.size(); ++row)
                numerical.segment<9>(static_cast<Eigen::Index>(9 * row)) =
                    (positive.residual[row] - negative.residual[row]) / (2.0 * epsilon);
            const auto column = static_cast<Eigen::Index>(9 * point + component);
            const double error = (numerical - matrix.col(column)).norm() / std::max(1.0, numerical.norm());
            if (matrixOutput)
                for (Eigen::Index row = 0; row < dimension; ++row)
                    matrixOutput << "wholeFD," << epsilon << ',' << row << ',' << column
                                 << ',' << numerical[row] << '\n';
            maximumError = std::max(maximumError, error);
            EXPECT_LT(error, 2.0e-6);
        }
    const auto direction = SolveContactSystem(system);
    Eigen::VectorXd flattened(dimension);
    for (size_t point = 0; point < direction.size(); ++point)
        flattened.segment<9>(static_cast<Eigen::Index>(9 * point)) = direction[point];
    const Eigen::VectorXd dense = matrix.fullPivLu().solve(-residual);
    if (matrixOutput)
        for (Eigen::Index row = 0; row < dimension; ++row)
        {
            matrixOutput << "blockDirection,0," << row << ",0," << flattened[row] << '\n';
            matrixOutput << "denseDirection,0," << row << ",0," << dense[row] << '\n';
        }
    EXPECT_LT((flattened - dense).norm() / std::max(1.0, dense.norm()), 1.0e-8);
    EXPECT_LT((matrix * flattened + residual).norm(), 1.0e-8);
    std::cout << "[RodContactKktOracle] maxJacobianRelative=" << maximumError
              << " directionError=" << (flattened - dense).norm()
              << " linearResidual=" << (matrix * flattened + residual).norm() << std::endl;
}

TEST(RodContactImplicit, ReleasedReenteredContactClearsOnlySelectedExclusionAndRetainsPositiveReaction)
{
    const auto rest = ContactRest();
    const auto input = ContactInput(rest);
    auto candidate = MakeAuthoredState(rest);
    candidate.positions[2].y() = -1.0e-5;
    const std::vector<ContactConstraint> constraints{
        Plane("selected", Vec3::UnitY(), 0.0),
        Plane("dependent", Vec3::UnitY(), 0.0)};
    std::vector<ContactActiveNode> previous(rest.positions.size());
    previous[2].identities[0] = 0;
    previous[2].multipliers[0] = -1.0e-4;
    std::vector<std::vector<bool>> excluded(rest.positions.size(), std::vector<bool>(2, true));
    unsigned dependent = 0;
    auto active = SelectContactActiveSet(rest, candidate, input, constraints, previous, excluded, dependent);
    ASSERT_EQ(active[2].identities[0], 0);
    EXPECT_DOUBLE_EQ(active[2].multipliers[0], -1.0e-4);
    EXPECT_FALSE(excluded[2][0]);
    EXPECT_TRUE(excluded[2][1]);
    EXPECT_GT(dependent, 0u);
    active[2].multipliers[0] = 7.45e-5;
    candidate.positions[2].y() = 1.0e-8;
    const auto retained = SelectContactActiveSet(rest, candidate, input, constraints, active, excluded, dependent);
    EXPECT_EQ(retained[2].identities[0], 0);
    EXPECT_DOUBLE_EQ(retained[2].multipliers[0], 7.45e-5);
    EXPECT_TRUE(excluded[2][1]);
    excluded[2][0] = true;
    const auto released = SelectContactActiveSet(rest, candidate, input, constraints, retained, excluded, dependent);
    EXPECT_EQ(released[2].identities[0], -1);
    EXPECT_TRUE(excluded[2][0]);
}

TEST(RodContactImplicit, CurvedTangentialSlideKeepsPositiveReactionActive)
{
    std::vector<Vec3> points;
    for (size_t point = 0; point < 4; ++point)
    {
        const double angle = 0.12 * static_cast<double>(point);
        points.emplace_back(0.1 * std::cos(angle), 0.1 * std::sin(angle), 0.0);
    }
    const auto rest = BuildSectionInertiaRestState(points, {}, {}, {0.0, 0.0, 1.0, 1.0});
    const auto profile = BuildProgressiveSupport(rest);
    auto input = ContactInput(rest);
    input.gravity.setZero();
    for (size_t point = 0; point < points.size(); ++point)
    {
        const Vec3 normal = points[point].normalized();
        const Vec3 tangent(-normal.y(), normal.x(), 0.0);
        input.externalForces.push_back(rest.masses[point] * (100.0 * tangent - 200.0 * normal));
    }
    const std::vector<ContactConstraint> constraints{
        MakeSphereContact("curved", {Vec3::Zero(), 0.1, Vec3::Zero()})};
    auto state = MakeAuthoredState(rest);
    ContactTrace trace;
    const auto result = StepImplicitMomentumContactWithTrace(rest, state, input, profile, constraints, trace);
    unsigned droppedPositive = 0;
    for (size_t iteration = 0; iteration < trace.iterations.size(); ++iteration)
    {
        const auto& record = trace.iterations[iteration];
        for (size_t point = 0; point < points.size(); ++point)
            for (size_t slot = 0; slot < 3; ++slot)
            {
                const int identity = record.previousActive[point].identities[slot];
                if (identity < 0 || record.previousActive[point].multipliers[static_cast<int>(slot)] <= 0.0)
                    continue;
                if (std::find(record.active[point].identities.begin(),
                    record.active[point].identities.end(), identity) == record.active[point].identities.end())
                    ++droppedPositive;
            }
        std::cout << "[RodCurvedActive] iteration=" << iteration
                  << " norm=" << record.normalizedResidual << " changed=" << record.changedNodes
                  << " cumulativeDroppedPositive=" << droppedPositive << std::endl;
    }
    ExportContact("curved-slide", result);
    ASSERT_EQ(result.status, Status::Success) << result.failureReason;
    EXPECT_EQ(droppedPositive, 0u);
    EXPECT_LE(result.maximumGeometricPenetration, 1.0e-9 * rest.totalLength);
    EXPECT_LE(result.maximumComplementarity, 1.0e-9);
}
}
