#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodRootSupport.h"
#include "hair/rod/rodXpbd.h"
#include "hair/rod/rodImplicit.h"
#include "hair/rod/rodProgressiveSupport.h"
#include "hair/rod/rodAnalytic.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>

#include <Eigen/LU>

namespace
{
using namespace VL::Hair::Rod;

RestState MakeCurve(size_t segments)
{
    std::vector<Vec3> positions;
    for (size_t point = 0; point <= segments; ++point)
    {
        const double parameter = static_cast<double>(point) / static_cast<double>(segments);
        positions.emplace_back(0.025 * std::sin(2.0 * parameter),
            0.02 * (1.0 - std::cos(2.0 * parameter)), 0.30 * parameter);
    }
    return BuildRestState(positions);
}

TEST(RodProduction, FullAngleExponentialLogAndBranchDomain)
{
    const Vec3 angle(0.7, -0.4, 0.9);
    const Quat rotation = Exp(angle);
    EXPECT_LT((Log(rotation) - angle).norm(), 1.0e-14);
    Quat opposite = rotation;
    opposite.coeffs() *= -1.0;
    EXPECT_LT((Log(opposite) - angle).norm(), 1.0e-14);
    EXPECT_THROW(Exp(Vec3(RotationLimit, 0.0, 0.0)), std::domain_error);
    EXPECT_THROW(Log(Quat(Eigen::AngleAxisd(Pi, Vec3::UnitX()))), std::domain_error);
    EXPECT_LT((RightJacobianInverse(Vec3::Zero()) - Mat3::Identity()).norm(), 1.0e-15);
}

TEST(RodProduction, ShearAndBendJacobiansMatchAllRightPerturbations)
{
    const RestState rest = MakeCurve(3);
    State state = MakeAuthoredState(rest);
    state.positions[1] += Vec3(0.003, -0.002, 0.001);
    state.orientations[1] = ApplyBodyIncrement(state.orientations[1], Vec3(0.14, -0.17, 0.21));
    state.orientations[2] = ApplyBodyIncrement(state.orientations[2], Vec3(-0.16, 0.11, -0.19));
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
    {
        for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
        {
            const ShearBlock block = EvaluateShearBlock(rest, state, segment);
            for (int axis = 0; axis < 3; ++axis)
            {
                const Vec3 increment = epsilon * Vec3::Unit(axis);
                for (int endpoint = 0; endpoint < 2; ++endpoint)
                {
                    State plus = state;
                    State minus = state;
                    plus.positions[segment + endpoint] += increment;
                    minus.positions[segment + endpoint] -= increment;
                    const Vec3 numerical = (EvaluateShearBlock(rest, plus, segment).constraint -
                        EvaluateShearBlock(rest, minus, segment).constraint) / (2.0 * epsilon);
                    const Mat3& analytic = endpoint == 0
                        ? block.firstPositionJacobian : block.secondPositionJacobian;
                    EXPECT_LT((numerical - analytic.col(axis)).norm(), 2.0e-7);
                }
                State plus = state;
                State minus = state;
                plus.orientations[segment] = ApplyBodyIncrement(state.orientations[segment], increment);
                minus.orientations[segment] = ApplyBodyIncrement(state.orientations[segment], -increment);
                const Vec3 numerical = (EvaluateShearBlock(rest, plus, segment).constraint -
                    EvaluateShearBlock(rest, minus, segment).constraint) / (2.0 * epsilon);
                EXPECT_LT((numerical - block.orientationJacobian.col(axis)).norm(), 2.0e-7);
            }
        }
        for (size_t joint = 0; joint + 1 < rest.lengths.size(); ++joint)
        {
            const BendBlock block = EvaluateBendBlock(rest, state, joint);
            for (int endpoint = 0; endpoint < 2; ++endpoint)
                for (int axis = 0; axis < 3; ++axis)
                {
                    const Vec3 increment = epsilon * Vec3::Unit(axis);
                    State plus = state;
                    State minus = state;
                    plus.orientations[joint + endpoint] =
                        ApplyBodyIncrement(state.orientations[joint + endpoint], increment);
                    minus.orientations[joint + endpoint] =
                        ApplyBodyIncrement(state.orientations[joint + endpoint], -increment);
                    const Vec3 numerical = (EvaluateBendBlock(rest, plus, joint).constraint -
                        EvaluateBendBlock(rest, minus, joint).constraint) / (2.0 * epsilon);
                    const Mat3& analytic = endpoint == 0
                        ? block.firstOrientationJacobian : block.secondOrientationJacobian;
                    EXPECT_LT((numerical - analytic.col(axis)).norm(), 2.0e-7);
                }
        }
    }
}

TEST(RodProduction, EnergyForceAndMaterialTorqueMatchFiniteDifferences)
{
    const RestState rest = MakeCurve(3);
    State state = MakeAuthoredState(rest);
    state.positions[2] += Vec3(0.002, -0.003, 0.001);
    state.orientations[1] = ApplyBodyIncrement(state.orientations[1], Vec3(0.13, -0.09, 0.2));
    const EnergyEvaluation analytic = EvaluateEnergy(rest, state);
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
    {
        for (size_t point = 0; point < state.positions.size(); ++point)
            for (int axis = 0; axis < 3; ++axis)
            {
                State plus = state;
                State minus = state;
                plus.positions[point][axis] += epsilon;
                minus.positions[point][axis] -= epsilon;
                const double derivative = (EvaluateEnergy(rest, plus).Total() -
                    EvaluateEnergy(rest, minus).Total()) / (2.0 * epsilon);
                EXPECT_NEAR(derivative, -analytic.forces[point][axis], 2.0e-9);
            }
        for (size_t segment = 0; segment < state.orientations.size(); ++segment)
            for (int axis = 0; axis < 3; ++axis)
            {
                State plus = state;
                State minus = state;
                const Vec3 increment = epsilon * Vec3::Unit(axis);
                plus.orientations[segment] = ApplyBodyIncrement(state.orientations[segment], increment);
                minus.orientations[segment] = ApplyBodyIncrement(state.orientations[segment], -increment);
                const double derivative = (EvaluateEnergy(rest, plus).Total() -
                    EvaluateEnergy(rest, minus).Total()) / (2.0 * epsilon);
                EXPECT_NEAR(derivative, -analytic.bodyTorques[segment][axis], 2.0e-10);
            }
    }
    Vec3 totalForce = Vec3::Zero();
    Vec3 totalMoment = Vec3::Zero();
    for (size_t point = 0; point < state.positions.size(); ++point)
    {
        totalForce += analytic.forces[point];
        totalMoment += state.positions[point].cross(analytic.forces[point]);
    }
    for (size_t segment = 0; segment < state.orientations.size(); ++segment)
        totalMoment += state.orientations[segment] * analytic.bodyTorques[segment];
    EXPECT_LT(totalForce.norm(), 1.0e-14);
    EXPECT_LT(totalMoment.norm(), 1.0e-14);
}

TEST(RodProduction, AuthoredRestAndRigidObjectivityAtMultipleResolutions)
{
    const Quat rotation = Exp(Vec3(1.1, -0.6, 0.5));
    const Vec3 translation(0.4, -1.2, 0.8);
    for (size_t segments : {3u, 8u, 16u, 31u, 32u})
    {
        const RestState rest = MakeCurve(segments);
        State state = MakeAuthoredState(rest);
        EXPECT_LT(EvaluateEnergy(rest, state).Total(), 1.0e-24);
        state.positions.back() += Vec3(0.001, -0.002, 0.003);
        state.orientations.back() = ApplyBodyIncrement(state.orientations.back(), Vec3(0.03, -0.04, 0.05));
        const EnergyEvaluation before = EvaluateEnergy(rest, state);
        for (Vec3& position : state.positions)
            position = translation + rotation * position;
        for (Quat& orientation : state.orientations)
            orientation = rotation * orientation;
        const EnergyEvaluation after = EvaluateEnergy(rest, state);
        EXPECT_NEAR(before.Total(), after.Total(), 1.0e-14);
        for (size_t point = 0; point < state.positions.size(); ++point)
            EXPECT_LT((after.forces[point] - rotation * before.forces[point]).norm(), 2.0e-11);
        for (size_t segment = 0; segment < state.orientations.size(); ++segment)
            EXPECT_LT((after.bodyTorques[segment] - before.bodyTorques[segment]).norm(), 2.0e-13);
    }
}

TEST(RodProduction, PureMaterialTorsionMatchesAnalyticEnergyAndEndTorque)
{
    for (size_t segments : {3u, 8u, 16u, 32u})
    {
        std::vector<Vec3> positions;
        for (size_t point = 0; point <= segments; ++point)
            positions.emplace_back(0.0, 0.0, 0.32 * point / segments);
        const RestState rest = BuildRestState(positions);
        State state = MakeAuthoredState(rest);
        const double jointAngle = 0.4 / static_cast<double>(segments - 1);
        for (size_t segment = 0; segment < segments; ++segment)
            state.orientations[segment] = Exp(Vec3(0.0, 0.0, segment * jointAngle));
        const EnergyEvaluation energy = EvaluateEnergy(rest, state);
        const double torque = rest.bendTwist.front().z() * jointAngle / rest.lengths.front();
        EXPECT_LT(energy.stretchShear, 1.0e-25);
        EXPECT_NEAR(energy.bendTwist, 0.5 * (segments - 1) * torque * jointAngle, 1.0e-20);
        EXPECT_NEAR(energy.bodyTorques.front().z(), torque, 1.0e-18);
        EXPECT_NEAR(energy.bodyTorques.back().z(), -torque, 1.0e-18);
    }
}

TEST(RodProduction, FiniteBodyInertiaIncludesGyroAndAnalyticPrincipalTorque)
{
    const Vec3 inertia(2.0, 3.0, 0.5);
    double residual = 0.0;
    const Vec3 principal = PredictBodyAngularVelocity(inertia, Vec3(0.0, 0.0, 2.0),
        Vec3(0.0, 0.0, 0.7), 0.01, residual);
    EXPECT_NEAR(principal.z(), 2.0 + 0.01 * 0.7 / 0.5, 1.0e-14);
    EXPECT_LT(residual, 1.0e-10);
    const Vec3 initial(0.8, -0.4, 1.1);
    const Vec3 predicted = PredictBodyAngularVelocity(inertia, initial, Vec3::Zero(), 0.01, residual);
    EXPECT_GT((predicted - initial).norm(), 1.0e-4);
    EXPECT_LT((inertia.cwiseProduct(predicted - initial) / 0.01 +
        predicted.cross(inertia.cwiseProduct(predicted))).norm(), 1.0e-9);
}

TEST(RodProduction, RootSupportVelocityAndDrivenWorkMatchDerivatives)
{
    const RestState rest = MakeCurve(8);
    State state = MakeAuthoredState(rest);
    RootFrame root;
    root.position = Vec3(0.04, -0.02, 0.03);
    root.orientation = Exp(Vec3(0.2, -0.3, 0.4));
    root.linearVelocity = Vec3(0.3, -0.2, 0.1);
    root.angularVelocityWorld = Vec3(-0.4, 0.3, 0.2);
    const double epsilon = 1.0e-6;
    RootFrame plus = root;
    RootFrame minus = root;
    plus.position += epsilon * root.linearVelocity;
    minus.position -= epsilon * root.linearVelocity;
    plus.orientation = Exp(epsilon * root.angularVelocityWorld) * root.orientation;
    minus.orientation = Exp(-epsilon * root.angularVelocityWorld) * root.orientation;
    double springPower = 0.0;
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        const SupportSample sample = SampleRootSupport(root, rest.rootLocalPositions[point]);
        const Vec3 numericalVelocity = (SampleRootSupport(plus, rest.rootLocalPositions[point]).position -
            SampleRootSupport(minus, rest.rootLocalPositions[point]).position) / (2.0 * epsilon);
        EXPECT_LT((numericalVelocity - sample.velocity).norm(), 1.0e-10);
        springPower += (rest.supportStiffness[point] *
            (sample.position - state.positions[point])).dot(sample.velocity);
    }
    const double numericalPower = (EvaluateRootSupport(rest, state, plus).potential -
        EvaluateRootSupport(rest, state, minus).potential) / (2.0 * epsilon);
    EXPECT_NEAR(numericalPower, springPower, 1.0e-11);
    state.velocities[4] = Vec3(0.2, 0.4, -0.1);
    const SupportEvaluation evaluation = EvaluateRootSupport(rest, state, root);
    double actualPower = springPower;
    for (size_t point = 0; point < state.positions.size(); ++point)
    {
        const SupportSample sample = SampleRootSupport(root, rest.rootLocalPositions[point]);
        const Vec3 springForce = rest.supportStiffness[point] *
            (sample.position - state.positions[point]);
        actualPower += (evaluation.forces[point] - springForce).dot(state.velocities[point]);
    }
    EXPECT_NEAR(actualPower, evaluation.drivePower - evaluation.dissipation, 1.0e-12);
}

TEST(RodProduction, InvalidMaterialAndRestFailAtConstructionBoundary)
{
    EXPECT_THROW(BuildRestState({Vec3::Zero(), Vec3::Zero()}), std::invalid_argument);
    Material material;
    material.youngModulus = -1.0;
    EXPECT_THROW(BuildRestState({Vec3::Zero(), Vec3::UnitZ()}, material), std::invalid_argument);
}

TEST(RodProduction, ZeroLoadXpbdPreservesAuthoredStateAndReportsResiduals)
{
    for (size_t segments : {3u, 8u, 16u, 31u, 32u})
    {
        const RestState rest = MakeCurve(segments);
        State state = MakeAuthoredState(rest);
        StepInput input;
        input.gravity.setZero();
        input.root.position = rest.positions.front();
        input.root.orientation = rest.orientations.front();
        const StepDiagnostics result = Step(rest, state, input);
        ASSERT_EQ(result.status, Status::Success);
        EXPECT_LT(result.maximumShearDefect, 1.0e-12);
        EXPECT_LT(result.maximumBendDefect, 1.0e-12);
        EXPECT_LT(result.maximumLinearResidual, 1.0e-10);
        EXPECT_LT(result.maximumAngularResidual, 1.0e-12);
        for (size_t point = 0; point < state.positions.size(); ++point)
            EXPECT_LT((state.positions[point] - rest.positions[point]).norm(), 1.0e-12);
    }
}

TEST(RodProduction, AuthoredDoublePositionPinIsPreservedWithIndependentRootFramePin)
{
    const RestState initial = MakeCurve(31);
    std::vector<double> markers(initial.positions.size(), 1.0);
    markers[0] = 0.0;
    markers[1] = 0.0;
    const RestState rest = BuildRestState(initial.positions, {}, initial.orientations, markers);
    EXPECT_EQ(rest.inverseMasses[1], 0.0);
    EXPECT_GT(rest.inverseInertias[1].minCoeff(), 0.0);
    State state = MakeAuthoredState(rest);
    StepInput input;
    input.root.position = rest.positions.front() + Vec3(0.001, 0.002, -0.001);
    input.root.orientation = Exp(Vec3(0.0, 0.003, 0.0)) * rest.orientations.front();
    input.sweeps = 24;
    const StepDiagnostics result = Step(rest, state, input);
    ASSERT_EQ(result.status, Status::Success);
    for (size_t point : {0u, 1u})
        EXPECT_LT((state.positions[point] -
            SampleRootSupport(input.root, rest.rootLocalPositions[point]).position).norm(), 1.0e-14);
    EXPECT_LT(Log(input.root.orientation.conjugate() * state.orientations.front()).norm(), 1.0e-14);
}

TEST(RodProduction, FailedStepKeepsEntireStateUnpublished)
{
    RestState rest = MakeCurve(3);
    const State before = MakeAuthoredState(rest);
    State state = before;
    StepInput input;
    input.stepSeconds = -1.0;
    EXPECT_EQ(Step(rest, state, input).status, Status::InvalidInput);
    input.stepSeconds = 1.0 / 240.0;
    input.root.orientation = Quat(Eigen::AngleAxisd(Pi, Vec3::UnitX())) *
        rest.orientations.front();
    EXPECT_EQ(Step(rest, state, input).status, Status::DomainFailure);
    for (size_t point = 0; point < state.positions.size(); ++point)
        EXPECT_EQ(state.positions[point], before.positions[point]);
}

TEST(RodProduction, RetiredV1ShortStepClassifiesNumericalNonQualification)
{
    size_t nonQualifiedCells = 0;
    bool n32CoarseFailure = false;
    bool n32BudgetOrderFailure = false;
    const char* exportDirectory = std::getenv("ROD_PRODUCTION_EXPORT_DIR");
    std::filesystem::path exportRoot;
    if (exportDirectory)
    {
        exportRoot = exportDirectory;
        ASSERT_FALSE(std::filesystem::exists(exportRoot));
        ASSERT_TRUE(std::filesystem::create_directories(exportRoot));
    }
    auto writeVector = [](std::ostream& output, const Vec3& vector)
        { output << ',' << vector.x() << ',' << vector.y() << ',' << vector.z(); };
    for (size_t segments : {3u, 8u, 16u, 32u})
    {
        const RestState rest = MakeCurve(segments);
        std::vector<std::vector<State>> terminals(3);
        double totalMass = 0.0;
        double maximumStiffness = 0.0;
        for (size_t point = 0; point < rest.masses.size(); ++point)
        {
            totalMass += rest.masses[point];
            maximumStiffness = std::max(maximumStiffness, rest.supportStiffness[point]);
        }
        const double forceReference = totalMass * 9.8 + maximumStiffness * rest.totalLength;
        const double torqueReference = forceReference * rest.totalLength;
        for (unsigned budget = 0; budget < 3; ++budget)
            for (unsigned refinement = 0; refinement < 3; ++refinement)
            {
                std::ofstream nodes;
                std::ofstream frames;
                std::ofstream steps;
                std::ofstream restNodes;
                std::ofstream restFrames;
                if (exportDirectory)
                {
                    const std::filesystem::path tier = exportRoot /
                        ("n" + std::to_string(segments) + "-s" + std::to_string(16u << budget) +
                            "-h" + std::to_string(240u << refinement));
                    ASSERT_TRUE(std::filesystem::create_directories(tier));
                    nodes.open(tier / "nodes.csv");
                    frames.open(tier / "frames.csv");
                    steps.open(tier / "steps.csv");
                    restNodes.open(tier / "rest-nodes.csv");
                    restFrames.open(tier / "rest-frames.csv");
                    for (std::ostream* stream : {static_cast<std::ostream*>(&nodes),
                        static_cast<std::ostream*>(&frames), static_cast<std::ostream*>(&steps),
                        static_cast<std::ostream*>(&restNodes), static_cast<std::ostream*>(&restFrames)})
                        *stream << std::setprecision(17);
                    nodes << "step,time,point,x,y,z,vx,vy,vz,fx,fy,fz,sfx,sfy,sfz,extfx,extfy,extfz,lambdaX,lambdaY,lambdaZ\n";
                    frames << "step,time,segment,qx,qy,qz,qw,wx,wy,wz,tx,ty,tz,ssLambdaX,ssLambdaY,ssLambdaZ,btLambdaX,btLambdaY,btLambdaZ\n";
                    steps << "step,time,status,energy,Tlinear,Tangular,ssDefect,btDefect,supportDefect,rx,rq,predictorResidual,drive,dissipation,rootFx,rootFy,rootFz,rootTx,rootTy,rootTz\n";
                    restNodes << "point,mass,inverseMass,k,c,x,y,z,rx,ry,rz\n";
                    restFrames << "segment,l,qx,qy,qz,qw,nu0x,nu0y,nu0z,phi0x,phi0y,phi0z,Sx,Sy,Sz,Bx,By,Bz,Jx,Jy,Jz\n";
                    for (size_t point = 0; point < rest.positions.size(); ++point)
                    {
                        restNodes << point << ',' << rest.masses[point] << ',' << rest.inverseMasses[point]
                                  << ',' << rest.supportStiffness[point] << ',' << rest.supportDamping[point];
                        writeVector(restNodes, rest.positions[point]);
                        writeVector(restNodes, rest.rootLocalPositions[point]);
                        restNodes << '\n';
                    }
                    for (size_t segment = 0; segment < segments; ++segment)
                    {
                        const Quat& orientation = rest.orientations[segment];
                        restFrames << segment << ',' << rest.lengths[segment] << ',' << orientation.x()
                                   << ',' << orientation.y() << ',' << orientation.z() << ',' << orientation.w();
                        writeVector(restFrames, rest.restStrains[segment]);
                        writeVector(restFrames, segment + 1 < segments ? rest.restJointAngles[segment] : Vec3::Zero().eval());
                        writeVector(restFrames, rest.stretchShear[segment]);
                        writeVector(restFrames, rest.bendTwist[segment]);
                        writeVector(restFrames, rest.inertias[segment]);
                        restFrames << '\n';
                    }
                }
                State state = MakeAuthoredState(rest);
                StepInput input;
                input.stepSeconds = 1.0 / (240.0 * (1u << refinement));
                input.sweeps = 16u << budget;
                input.root.position = rest.positions.front();
                input.root.orientation = rest.orientations.front();
                for (double mass : rest.masses)
                    input.externalForces.push_back(Vec3(20.0 * mass, 0.0, 0.0));
                auto exportState = [&](unsigned stepIndex, const StepDiagnostics& diagnostic)
                {
                    if (!exportDirectory)
                        return;
                    const double time = stepIndex * input.stepSeconds;
                    const EnergyEvaluation energy = EvaluateEnergy(rest, state);
                    const SupportEvaluation support = EvaluateRootSupport(rest, state, input.root);
                    for (size_t point = 0; point < state.positions.size(); ++point)
                    {
                        nodes << stepIndex << ',' << time << ',' << point;
                        writeVector(nodes, state.positions[point]);
                        writeVector(nodes, state.velocities[point]);
                        writeVector(nodes, energy.forces[point]);
                        writeVector(nodes, support.forces[point]);
                        writeVector(nodes, input.externalForces[point] + rest.masses[point] * input.gravity);
                        writeVector(nodes, diagnostic.supportMultipliers.empty() ?
                            Vec3::Zero().eval() : diagnostic.supportMultipliers[point]);
                        nodes << '\n';
                    }
                    for (size_t segment = 0; segment < segments; ++segment)
                    {
                        const Quat& orientation = state.orientations[segment];
                        frames << stepIndex << ',' << time << ',' << segment << ',' << orientation.x()
                               << ',' << orientation.y() << ',' << orientation.z() << ',' << orientation.w();
                        writeVector(frames, state.angularVelocities[segment]);
                        writeVector(frames, energy.bodyTorques[segment]);
                        writeVector(frames, diagnostic.shearMultipliers.empty() ?
                            Vec3::Zero().eval() : diagnostic.shearMultipliers[segment]);
                        writeVector(frames, diagnostic.bendMultipliers.empty() || segment + 1 == segments ?
                            Vec3::Zero().eval() : diagnostic.bendMultipliers[segment]);
                        frames << '\n';
                    }
                    steps << stepIndex << ',' << time << ',' << static_cast<int>(diagnostic.status)
                          << ',' << diagnostic.energy << ',' << diagnostic.linearKineticEnergy
                          << ',' << diagnostic.angularKineticEnergy << ',' << diagnostic.maximumShearDefect
                          << ',' << diagnostic.maximumBendDefect << ',' << diagnostic.maximumSupportDefect
                          << ',' << diagnostic.maximumLinearResidual << ',' << diagnostic.maximumAngularResidual
                          << ',' << diagnostic.predictorResidual << ',' << diagnostic.supportDrivePower
                          << ',' << diagnostic.supportDissipation;
                    writeVector(steps, diagnostic.rootForce);
                    writeVector(steps, diagnostic.rootBodyTorque);
                    steps << '\n';
                };
                exportState(0, StepDiagnostics{});
                StepDiagnostics result;
                double maximumLinearResidual = 0.0;
                double maximumAngularResidual = 0.0;
                double maximumDefect = 0.0;
                for (unsigned step = 0; step < (6u << refinement); ++step)
                {
                    result = Step(rest, state, input);
                    exportState(step + 1, result);
                    ASSERT_EQ(result.status, Status::Success)
                        << "segments=" << segments << " budget=" << input.sweeps
                        << " refinement=" << refinement << " step=" << step;
                    maximumLinearResidual = std::max(maximumLinearResidual, result.maximumLinearResidual);
                    maximumAngularResidual = std::max(maximumAngularResidual, result.maximumAngularResidual);
                    maximumDefect = std::max({maximumDefect, result.maximumShearDefect,
                        result.maximumBendDefect, result.maximumSupportDefect / rest.totalLength});
                }
                const bool qualified = maximumLinearResidual / forceReference <= 1.0e-2 &&
                    maximumAngularResidual / torqueReference <= 1.0e-2 && maximumDefect <= 1.0e-4;
                if (!qualified)
                    ++nonQualifiedCells;
                if (segments == 32 && budget == 0 && refinement == 0)
                {
                    n32CoarseFailure = !qualified;
                    EXPECT_FALSE(qualified);
                }
                std::cout << "[RodRefinement] segments=" << segments << " h=" << input.stepSeconds
                          << " sweeps=" << input.sweeps
                          << " shear=" << result.maximumShearDefect
                          << " bend=" << result.maximumBendDefect
                          << " support=" << result.maximumSupportDefect
                          << " forceResidualNormMax=" << maximumLinearResidual / forceReference
                          << " torqueResidualNormMax=" << maximumAngularResidual / torqueReference
                          << " defectMax=" << maximumDefect
                          << " numerical=" << (qualified ? "QUALIFIED" : "NOT_QUALIFIED") << std::endl;
                EXPECT_TRUE(std::isfinite(maximumLinearResidual));
                EXPECT_TRUE(std::isfinite(maximumAngularResidual));
                terminals[budget].push_back(state);
            }
        auto differences = [](const State& first, const State& second)
        {
            Eigen::Vector2d result = Eigen::Vector2d::Zero();
            for (size_t point = 0; point < first.positions.size(); ++point)
                result.x() = std::max(result.x(), (first.positions[point] - second.positions[point]).norm());
            for (size_t segment = 0; segment < first.orientations.size(); ++segment)
                result.y() = std::max(result.y(), Log(first.orientations[segment].conjugate() *
                    second.orientations[segment]).norm());
            return result;
        };
        for (unsigned comparison = 0; comparison < 4; ++comparison)
        {
            const Eigen::Vector2d coarse = comparison == 0
                ? differences(terminals[2][0], terminals[2][1])
                : differences(terminals[0][comparison - 1], terminals[1][comparison - 1]);
            const Eigen::Vector2d fine = comparison == 0
                ? differences(terminals[2][1], terminals[2][2])
                : differences(terminals[1][comparison - 1], terminals[2][comparison - 1]);
            std::cout << "[RodDifference] segments=" << segments << " comparison=" << comparison
                      << " coarseX=" << coarse.x() << " fineX=" << fine.x()
                      << " coarseQ=" << coarse.y() << " fineQ=" << fine.y() << std::endl;
            const bool positionResolved = coarse.x() > 1.0e-10 * rest.totalLength;
            const bool orientationResolved = coarse.y() > 1.0e-9;
            if (!positionResolved)
                std::cout << "[RodDifference] position=UNRESOLVED_ROUNDOFF" << std::endl;
            if (!orientationResolved)
                std::cout << "[RodDifference] orientation=UNRESOLVED_ROUNDOFF" << std::endl;
            const bool positionDecreasing = positionResolved && fine.x() < coarse.x();
            const bool orientationDecreasing = orientationResolved && fine.y() < coarse.y();
            std::cout << "[RetiredV1OrderClassification] positionDecreasing=" << positionDecreasing
                      << " orientationDecreasing=" << orientationDecreasing << std::endl;
            if (segments == 32 && comparison == 1)
            {
                EXPECT_TRUE(positionResolved);
                EXPECT_TRUE(orientationResolved);
                EXPECT_FALSE(positionDecreasing);
                EXPECT_FALSE(orientationDecreasing);
                n32BudgetOrderFailure = positionResolved && orientationResolved &&
                    !positionDecreasing && !orientationDecreasing;
            }
        }
    }
    EXPECT_GT(nonQualifiedCells, 0u);
    EXPECT_TRUE(n32CoarseFailure);
    EXPECT_TRUE(n32BudgetOrderFailure);
    std::cout << "[RetiredV1Classification] productionQualified=false nonQualifiedCells="
              << nonQualifiedCells << " n32CoarseFailure=" << n32CoarseFailure
              << " n32BudgetOrderFailure=" << n32BudgetOrderFailure << std::endl;
}

TEST(RodProduction, StepCollisionProjectsInsideSweepAndDoesNotCreateRepairVelocity)
{
    const RestState initial = MakeCurve(8);
    std::vector<double> markers(initial.positions.size(), 1.0);
    markers[0] = 0.0;
    markers[1] = 0.0;
    RestState rest = BuildRestState(initial.positions, {}, initial.orientations, markers);
    for (Vec3& stiffness : rest.stretchShear)
        stiffness *= 1.0e-8;
    for (Vec3& stiffness : rest.bendTwist)
        stiffness *= 1.0e-8;
    State state = MakeAuthoredState(rest);
    StepInput input;
    input.supportEnabled = false;
    input.gravity.setZero();
    input.root.position = rest.positions.front();
    input.root.orientation = rest.orientations.front();
    input.sweeps = 24;
    input.collisionFields.push_back([](const Vec3& position)
    {
        CollisionSample sample;
        sample.valid = true;
        sample.distance = position.y() - 0.01;
        sample.gradient = Vec3::UnitY();
        return sample;
    });
    const StepDiagnostics result = Step(rest, state, input);
    ASSERT_EQ(result.status, Status::Success);
    EXPECT_GT(result.maximumContactCorrection, 0.0);
    for (size_t point = 2; point < state.positions.size(); ++point)
    {
        EXPECT_GE(state.positions[point].y(), 0.01 - 1.0e-12);
        EXPECT_LT(std::abs(state.velocities[point].y()), 1.0e-4);
    }
    EXPECT_EQ(state.positions[0], SampleRootSupport(input.root, rest.rootLocalPositions[0]).position);
    EXPECT_EQ(state.positions[1], SampleRootSupport(input.root, rest.rootLocalPositions[1]).position);
}

TEST(RodProduction, CorruptedRestMetricIsRejectedBeforePublishing)
{
    for (int corruption = 0; corruption < 6; ++corruption)
    {
        RestState rest = MakeCurve(3);
        State state = MakeAuthoredState(rest);
        const State before = state;
        if (corruption == 0) rest.inverseMasses[1] = -1.0;
        if (corruption == 1) rest.inverseInertias[1].x() = -1.0;
        if (corruption == 2) rest.stretchShear[1].x() = 0.0;
        if (corruption == 3) rest.bendTwist[1].x() = std::numeric_limits<double>::quiet_NaN();
        if (corruption == 4) rest.lengths[1] = -1.0;
        if (corruption == 5) rest.supportDamping[1] = -1.0;
        StepInput input;
        EXPECT_EQ(Step(rest, state, input).status, Status::InvalidInput);
        EXPECT_EQ(state.positions[1], before.positions[1]);
    }
}

TEST(RodProduction, PureTwistReleaseReportsFiniteInertiaAndNumericalDissipation)
{
    std::vector<Vec3> positions;
    for (size_t point = 0; point <= 8; ++point)
        positions.emplace_back(0.0, 0.0, 0.04 * point);
    const RestState rest = BuildRestState(positions);
    const char* exportDirectory = std::getenv("ROD_TWIST_EXPORT_DIR");
    std::filesystem::path exportRoot;
    if (exportDirectory)
    {
        exportRoot = exportDirectory;
        ASSERT_FALSE(std::filesystem::exists(exportRoot));
        ASSERT_TRUE(std::filesystem::create_directories(exportRoot));
    }
    for (unsigned budget = 0; budget < 3; ++budget)
        for (unsigned refinement = 0; refinement < 3; ++refinement)
        {
            State state = MakeAuthoredState(rest);
            for (size_t segment = 0; segment < 8; ++segment)
                state.orientations[segment] = Exp(Vec3(0.0, 0.0, 0.2 * segment / 7.0));
            const double initialEnergy = EvaluateEnergy(rest, state).Total();
            StepInput input;
            input.root.position = rest.positions.front();
            input.root.orientation = rest.orientations.front();
            input.gravity.setZero();
            input.supportEnabled = false;
            input.stepSeconds = 1.0 / (240.0 * (1u << refinement));
            input.sweeps = 16u << budget;
            std::ofstream output;
            if (exportDirectory)
            {
                output.open(exportRoot / ("twist-s" + std::to_string(input.sweeps) +
                    "-h" + std::to_string(240u << refinement) + ".csv"));
                output << std::setprecision(17);
                output << "step,time,segment,qx,qy,qz,qw,wx,wy,wz,tx,ty,tz,elastic,Tangular,total,btDefect,rq,status\n";
            }
            double maximumEnergy = initialEnergy;
            double maximumEnergyIncrease = 0.0;
            double previousEnergy = initialEnergy;
            double maximumPositionError = 0.0;
            double maximumAngularResidual = 0.0;
            StepDiagnostics result;
            for (unsigned step = 0; step < (6u << refinement); ++step)
            {
                result = Step(rest, state, input);
                ASSERT_EQ(result.status, Status::Success)
                    << "sweeps=" << input.sweeps << " h=" << input.stepSeconds << " step=" << step;
                const EnergyEvaluation evaluation = EvaluateEnergy(rest, state);
                const double energy = evaluation.Total() + result.angularKineticEnergy +
                    result.linearKineticEnergy;
                maximumEnergyIncrease = std::max(maximumEnergyIncrease, energy - previousEnergy);
                maximumEnergy = std::max(maximumEnergy, energy);
                previousEnergy = energy;
                maximumAngularResidual = std::max(maximumAngularResidual, result.maximumAngularResidual);
                for (size_t point = 0; point < state.positions.size(); ++point)
                    maximumPositionError = std::max(maximumPositionError,
                        (state.positions[point] - rest.positions[point]).norm());
                for (size_t segment = 0; segment < 8; ++segment)
                {
                    EXPECT_NEAR(state.orientations[segment].norm(), 1.0, 1.0e-12);
                    if (exportDirectory)
                    {
                        const Quat& orientation = state.orientations[segment];
                        const Vec3& velocity = state.angularVelocities[segment];
                        const Vec3& torque = evaluation.bodyTorques[segment];
                        output << step + 1 << ',' << (step + 1) * input.stepSeconds << ',' << segment
                               << ',' << orientation.x() << ',' << orientation.y() << ',' << orientation.z()
                               << ',' << orientation.w() << ',' << velocity.x() << ',' << velocity.y()
                               << ',' << velocity.z() << ',' << torque.x() << ',' << torque.y() << ',' << torque.z()
                               << ',' << evaluation.Total() << ',' << result.angularKineticEnergy << ',' << energy
                               << ',' << result.maximumBendDefect << ',' << result.maximumAngularResidual
                               << ',' << static_cast<int>(result.status) << '\n';
                    }
                }
            }
            const double finalTwist = Log(state.orientations.back()).z();
            std::cout << "[RodTwistRelease] sweeps=" << input.sweeps << " h=" << input.stepSeconds
                      << " initialEnergy=" << initialEnergy << " maxEnergyRatio=" << maximumEnergy / initialEnergy
                      << " maxStepEnergyIncrease=" << maximumEnergyIncrease
                      << " finalEnergyRatio=" << previousEnergy / initialEnergy
                      << " finalTipTwist=" << finalTwist << " maxXerror=" << maximumPositionError
                      << " torqueResidualMax=" << maximumAngularResidual
                      << " settle=NOT_CLAIMED" << std::endl;
            EXPECT_TRUE(std::isfinite(previousEnergy));
            EXPECT_LT(maximumPositionError, 1.0e-12);
        }
}

TEST(RodImplicit, FullNonsymmetricJacobianAndBlockThomasMatchIndependentOracles)
{
    const char* exportDirectory = std::getenv("ROD_IMPLICIT_STATIC_EXPORT_DIR");
    std::filesystem::path exportRoot;
    if (exportDirectory)
    {
        exportRoot = exportDirectory;
        ASSERT_FALSE(std::filesystem::exists(exportRoot));
        ASSERT_TRUE(std::filesystem::create_directories(exportRoot));
    }
    for (size_t segments : {3u, 8u})
    {
        const RestState rest = MakeCurve(segments);
        State old = MakeAuthoredState(rest);
        State candidate = old;
        StepInput input;
        input.root.position = rest.positions.front();
        input.root.orientation = rest.orientations.front();
        input.externalTorquesWorld.assign(segments, Vec3(1.0e-6, -2.0e-6, 0.5e-6));
        for (size_t segment = 1; segment < segments; ++segment)
        {
            old.angularVelocities[segment] = Vec3(0.8, -0.4, 1.1);
            candidate.orientations[segment] = ApplyBodyIncrement(candidate.orientations[segment],
                Vec3(0.03, -0.02, 0.04));
        }
        for (size_t point = 1; point < rest.positions.size(); ++point)
        {
            candidate.positions[point] += Vec3(0.0002, -0.0001, 0.0003);
            old.velocities[point] = Vec3(0.01, -0.02, 0.03);
        }
        const ImplicitSystem system = AssembleImplicitSystem(rest, old, candidate, input);
        std::ofstream matrixOutput;
        if (exportDirectory)
        {
            const auto tier = exportRoot / ("n" + std::to_string(segments));
            ASSERT_TRUE(std::filesystem::create_directories(tier));
            matrixOutput.open(tier / "matrices.csv");
            matrixOutput << std::setprecision(17);
            matrixOutput << "kind,epsilon,row,column,value\n";
            std::ofstream stateOutput(tier / "state.csv");
            stateOutput << std::setprecision(17);
            stateOutput << "kind,index,c0,c1,c2,c3\n";
            auto write = [&](const char* kind, size_t index, const Vec3& value)
            {
                stateOutput << kind << ',' << index << ',' << value.x()
                            << ',' << value.y() << ',' << value.z() << ",0\n";
            };
            auto writeQuaternion = [&](const char* kind, size_t index, const Quat& value)
            {
                stateOutput << kind << ',' << index << ',' << value.x()
                            << ',' << value.y() << ',' << value.z() << ',' << value.w() << '\n';
            };
            for (size_t point = 0; point < rest.positions.size(); ++point)
            {
                write("restX", point, rest.positions[point]);
                write("oldX", point, old.positions[point]);
                write("oldV", point, old.velocities[point]);
                write("candidateX", point, candidate.positions[point]);
                write("rootLocal", point, rest.rootLocalPositions[point]);
                write("massKc", point, Vec3(rest.masses[point],
                    rest.supportStiffness[point], rest.supportDamping[point]));
                write("inverseMass", point, Vec3(rest.inverseMasses[point], 0.0, 0.0));
            }
            for (size_t segment = 0; segment < segments; ++segment)
            {
                writeQuaternion("restQ", segment, rest.orientations[segment]);
                writeQuaternion("oldQ", segment, old.orientations[segment]);
                writeQuaternion("candidateQ", segment, candidate.orientations[segment]);
                write("oldW", segment, old.angularVelocities[segment]);
                write("nu0", segment, rest.restStrains[segment]);
                write("phi0", segment, segment + 1 < segments ?
                    rest.restJointAngles[segment] : Vec3::Zero().eval());
                write("length", segment, Vec3(rest.lengths[segment], 0.0, 0.0));
                write("S", segment, rest.stretchShear[segment]);
                write("B", segment, rest.bendTwist[segment]);
                write("J", segment, rest.inertias[segment]);
                write("externalTorque", segment, input.externalTorquesWorld[segment]);
            }
            write("rootPosition", 0, input.root.position);
            writeQuaternion("rootOrientation", 0, input.root.orientation);
            write("rootVelocity", 0, input.root.linearVelocity);
            write("rootOmega", 0, input.root.angularVelocityWorld);
            write("gravity", 0, input.gravity);
            write("stepL", 0, Vec3(input.stepSeconds, rest.totalLength, 1.0));
            write("scales", 0, Vec3(system.lengthScale, system.forceScale, system.torqueScale));
        }
        const Eigen::Index dimension = static_cast<Eigen::Index>(6 * rest.positions.size());
        Eigen::MatrixXd dense = Eigen::MatrixXd::Zero(dimension, dimension);
        Eigen::VectorXd residual(dimension);
        for (size_t block = 0; block < system.diagonal.size(); ++block)
        {
            const auto offset = static_cast<Eigen::Index>(6 * block);
            dense.block<6, 6>(offset, offset) = system.diagonal[block];
            residual.segment<6>(offset) = system.residual[block];
            if (block > 0)
                dense.block<6, 6>(offset, offset - 6) = system.lower[block];
            if (block + 1 < system.diagonal.size())
                dense.block<6, 6>(offset, offset + 6) = system.upper[block];
        }
        double maximumError = 0.0;
        if (exportDirectory)
            for (Eigen::Index row = 0; row < dimension; ++row)
            {
                matrixOutput << "residual,0," << row << ",0," << residual[row] << '\n';
                for (Eigen::Index column = 0; column < dimension; ++column)
                    matrixOutput << "assembled,0," << row << ',' << column << ',' << dense(row, column) << '\n';
            }
        for (double relativeStep : {1.0e-5, 1.0e-6, 1.0e-7})
            for (size_t block = 1; block < rest.positions.size(); ++block)
                for (int component = 0; component < 6; ++component)
                {
                    if (component >= 3 && block == segments)
                        continue;
                    const double epsilon = relativeStep *
                        (component < 3 ? system.lengthScale : 1.0);
                    State plus = candidate;
                    State minus = candidate;
                    if (component < 3)
                    {
                        plus.positions[block][component] += epsilon;
                        minus.positions[block][component] -= epsilon;
                    }
                    else
                    {
                        const Vec3 increment = epsilon * Vec3::Unit(component - 3);
                        plus.orientations[block] = ApplyBodyIncrement(candidate.orientations[block], increment);
                        minus.orientations[block] = ApplyBodyIncrement(candidate.orientations[block], -increment);
                    }
                    const auto positive = EvaluateImplicitResidual(rest, old, plus, input);
                    const auto negative = EvaluateImplicitResidual(rest, old, minus, input);
                    Eigen::VectorXd numerical = Eigen::VectorXd::Zero(dimension);
                    for (size_t row = 1; row < rest.positions.size(); ++row)
                    {
                        Vector6 derivative = (positive[row] - negative[row]) / (2.0 * epsilon);
                        derivative.head<3>() /= system.forceScale;
                        derivative.tail<3>() /= system.torqueScale;
                        if (component < 3)
                            derivative *= system.lengthScale;
                        if (row == segments)
                            derivative.tail<3>().setZero();
                        numerical.segment<6>(static_cast<Eigen::Index>(6 * row)) = derivative;
                    }
                    const auto column = static_cast<Eigen::Index>(6 * block + component);
                    const double error = (numerical - dense.col(column)).norm() /
                        std::max(1.0, numerical.norm());
                    maximumError = std::max(maximumError, error);
                    if (exportDirectory)
                        for (Eigen::Index row = 0; row < dimension; ++row)
                            matrixOutput << "wholeFD," << relativeStep << ',' << row << ','
                                         << column << ',' << numerical[row] << '\n';
                    EXPECT_LT(error, 2.0e-6);
                }
        const auto blocks = SolveImplicitBlocks(system);
        Eigen::VectorXd blockDirection(dimension);
        for (size_t block = 0; block < blocks.size(); ++block)
            blockDirection.segment<6>(static_cast<Eigen::Index>(6 * block)) = blocks[block];
        const Eigen::VectorXd denseDirection = dense.fullPivLu().solve(-residual);
        if (exportDirectory)
            for (Eigen::Index row = 0; row < dimension; ++row)
            {
                matrixOutput << "blockDirection,0," << row << ",0," << blockDirection[row] << '\n';
                matrixOutput << "denseDirection,0," << row << ",0," << denseDirection[row] << '\n';
            }
        const double directionError = (blockDirection - denseDirection).norm() /
            std::max(1.0, denseDirection.norm());
        const double linearError = (dense * blockDirection + residual).norm() /
            std::max(1.0, residual.norm());
        std::cout << "[RodImplicitOracle] segments=" << segments
                  << " fullJacobianRelative=" << maximumError
                  << " blockVsDense=" << directionError << " linearResidual=" << linearError
                  << " nonsymmetry=" << (dense - dense.transpose()).norm() << std::endl;
        EXPECT_LT(directionError, 1.0e-8);
        EXPECT_LT(linearError, 1.0e-10);
    }
}

TEST(RodImplicit, ShortN3N8MomentumResidualAndTimestepRefinement)
{
    const char* exportDirectory = std::getenv("ROD_IMPLICIT_EXPORT_DIR");
    std::filesystem::path exportRoot;
    if (exportDirectory)
    {
        exportRoot = exportDirectory;
        ASSERT_FALSE(std::filesystem::exists(exportRoot));
        ASSERT_TRUE(std::filesystem::create_directories(exportRoot));
    }
    for (size_t segments : {3u, 8u})
    {
        const RestState rest = MakeCurve(segments);
        std::vector<State> terminals;
        for (unsigned refinement = 0; refinement < 3; ++refinement)
        {
            State state = MakeAuthoredState(rest);
            StepInput input;
            input.stepSeconds = 1.0 / (240.0 * (1u << refinement));
            input.root.position = rest.positions.front();
            input.root.orientation = rest.orientations.front();
            for (double mass : rest.masses)
                input.externalForces.push_back(Vec3(20.0 * mass, 0.0, 0.0));
            std::ofstream output;
            if (exportDirectory)
            {
                output.open(exportRoot / ("n" + std::to_string(segments) +
                    "-h" + std::to_string(240u << refinement) + ".csv"));
                output << std::setprecision(17);
                output << "step,time,index,x,y,z,vx,vy,vz,qx,qy,qz,qw,wx,wy,wz,rx,ry,rz,tx,ty,tz,normalizedResidual,newton,backtracks,linearResidual,energy,Tlinear,Tangular\n";
            }
            double maximumResidual = 0.0;
            unsigned maximumIterations = 0;
            for (unsigned step = 0; step < (6u << refinement); ++step)
            {
                const State old = state;
                const auto result = StepImplicitMomentum(rest, state, input);
                ASSERT_EQ(result.status, Status::Success)
                    << "segments=" << segments << " refinement=" << refinement
                    << " step=" << step << " residual=" << result.normalizedResidual;
                EXPECT_TRUE(result.converged);
                EXPECT_LE(result.normalizedResidual, 1.0e-8);
                maximumResidual = std::max(maximumResidual, result.normalizedResidual);
                maximumIterations = std::max(maximumIterations, result.newtonIterations);
                const auto residual = EvaluateImplicitResidual(rest, old, state, input);
                for (size_t point = 0; exportDirectory && point < state.positions.size(); ++point)
                {
                    const Quat orientation = point < segments ? state.orientations[point] : Quat::Identity();
                    const Vec3 omega = point < segments ? state.angularVelocities[point] : Vec3::Zero().eval();
                    output << step + 1 << ',' << (step + 1) * input.stepSeconds << ',' << point
                           << ',' << state.positions[point].x() << ',' << state.positions[point].y()
                           << ',' << state.positions[point].z() << ',' << state.velocities[point].x()
                           << ',' << state.velocities[point].y() << ',' << state.velocities[point].z()
                           << ',' << orientation.x() << ',' << orientation.y() << ',' << orientation.z()
                           << ',' << orientation.w() << ',' << omega.x() << ',' << omega.y() << ',' << omega.z();
                    for (int component = 0; component < 6; ++component)
                        output << ',' << residual[point][component];
                    output << ',' << result.normalizedResidual << ',' << result.newtonIterations
                           << ',' << result.backtracks << ',' << result.linearSolveResidual
                           << ',' << result.energy << ',' << result.linearKineticEnergy
                           << ',' << result.angularKineticEnergy << '\n';
                }
            }
            terminals.push_back(state);
            std::cout << "[RodImplicitShort] segments=" << segments << " h=" << input.stepSeconds
                      << " maxResidual=" << maximumResidual << " maxNewton=" << maximumIterations << std::endl;
        }
        Eigen::Vector2d differences[2] = {Eigen::Vector2d::Zero(), Eigen::Vector2d::Zero()};
        for (size_t tier = 0; tier < 2; ++tier)
        {
            for (size_t point = 0; point < rest.positions.size(); ++point)
                differences[tier].x() = std::max(differences[tier].x(),
                    (terminals[tier].positions[point] - terminals[tier + 1].positions[point]).norm());
            for (size_t segment = 0; segment < segments; ++segment)
                differences[tier].y() = std::max(differences[tier].y(),
                    Log(terminals[tier].orientations[segment].conjugate() *
                        terminals[tier + 1].orientations[segment]).norm());
        }
        std::cout << "[RodImplicitDifference] segments=" << segments
                  << " coarseX=" << differences[0].x() << " fineX=" << differences[1].x()
                  << " coarseQ=" << differences[0].y() << " fineQ=" << differences[1].y() << std::endl;
        EXPECT_LT(differences[1].x(), differences[0].x());
        EXPECT_LT(differences[1].y(), differences[0].y());
    }
}

TEST(RodProgressiveSupport, DesignForceAndCompleteHessianMatchFixedProfile)
{
    const RestState rest = MakeCurve(8);
    const ProgressiveSupport profile = BuildProgressiveSupport(rest);
    const double maximumDisplacement = profile.responseLimitRatio * rest.totalLength;
    EXPECT_DOUBLE_EQ(profile.designAirSpeed, 20.0);
    EXPECT_DOUBLE_EQ(profile.responseLimitRatio, 0.08);
    EXPECT_NEAR(profile.dragAccelerationPerSpeed * 400.0,
        1.225 * 1.5 * 400.0 / (Pi * 1300.0 * 0.0005), 1.0e-12);
    for (size_t point = 1; point < rest.positions.size(); ++point)
    {
        EXPECT_GT(profile.cubicStiffness[point], 0.0);
        const ProgressiveNode design = EvaluateProgressiveNode(
            Vec3(maximumDisplacement, 0.0, 0.0),
            rest.supportStiffness[point], profile.cubicStiffness[point]);
        EXPECT_NEAR(design.springForce.norm(), profile.designForces[point], 1.0e-14);
        const Vec3 aerodynamic = ProgressiveAirDragForce(
            profile, rest.masses[point], Vec3(20.0, 0.0, 0.0), Vec3::UnitZ());
        EXPECT_NEAR(aerodynamic.norm(), profile.designForces[point], 1.0e-14);
    }
    const Vec3 displacement(0.006, -0.009, 0.004);
    const double linear = rest.supportStiffness[4];
    const double cubic = profile.cubicStiffness[4];
    const ProgressiveNode node = EvaluateProgressiveNode(displacement, linear, cubic);
    EXPECT_GT(std::abs(node.hessian(0, 1)), 0.0);
    for (double epsilon : {1.0e-5, 1.0e-6, 1.0e-7})
        for (int axis = 0; axis < 3; ++axis)
        {
            const Vec3 perturbation = epsilon * Vec3::Unit(axis);
            const auto positive = EvaluateProgressiveNode(displacement + perturbation, linear, cubic);
            const auto negative = EvaluateProgressiveNode(displacement - perturbation, linear, cubic);
            const double derivative = (positive.potential - negative.potential) / (2.0 * epsilon);
            const Vec3 forceDerivative = (positive.springForce - negative.springForce) / (2.0 * epsilon);
            EXPECT_NEAR(derivative, -node.springForce[axis], 1.0e-8);
            EXPECT_LT((forceDerivative + node.hessian.col(axis)).norm() /
                std::max(1.0, node.hessian.col(axis).norm()), 2.0e-6);
        }
}

TEST(RodImplicit, AnalyticFullElasticTangentMatchesFrozenFiniteDifferenceAssembly)
{
    for (size_t segments : {3u, 8u, 31u, 32u})
    {
        const RestState rest = MakeCurve(segments);
        State old = MakeAuthoredState(rest);
        State candidate = old;
        StepInput input;
        input.root.position = rest.positions.front();
        input.root.orientation = rest.orientations.front();
        input.externalTorquesWorld.assign(segments, Vec3(1.0e-6, -2.0e-6, 0.5e-6));
        for (size_t point = 1; point < rest.positions.size(); ++point)
            candidate.positions[point] += Vec3(0.0002, -0.0001, 0.0003);
        for (size_t segment = 1; segment < segments; ++segment)
        {
            old.angularVelocities[segment] = Vec3(0.8, -0.4, 1.1);
            candidate.orientations[segment] = ApplyBodyIncrement(candidate.orientations[segment],
                Vec3(0.03, -0.02, 0.04));
        }
        const auto numerical = AssembleImplicitSystem(rest, old, candidate, input);
        const auto analytic = AssembleImplicitSystemAnalytic(rest, old, candidate, input);
        double maximumBlockError = 0.0;
        for (size_t block = 0; block < rest.positions.size(); ++block)
        {
            const Matrix6 pairs[] = {numerical.lower[block], numerical.diagonal[block], numerical.upper[block]};
            const Matrix6 references[] = {analytic.lower[block], analytic.diagonal[block], analytic.upper[block]};
            for (size_t pair = 0; pair < 3; ++pair)
            {
                const double error = (pairs[pair] - references[pair]).norm() /
                    std::max(1.0, pairs[pair].norm());
                maximumBlockError = std::max(maximumBlockError, error);
                EXPECT_LT(error, 2.0e-6);
            }
        }
        const auto referenceDirection = SolveImplicitBlocks(numerical);
        const auto analyticDirection = SolveImplicitBlocks(analytic);
        double directionErrorSquared = 0.0;
        double referenceSquared = 0.0;
        for (size_t block = 0; block < referenceDirection.size(); ++block)
        {
            directionErrorSquared += (referenceDirection[block] - analyticDirection[block]).squaredNorm();
            referenceSquared += referenceDirection[block].squaredNorm();
        }
        const double directionError = std::sqrt(directionErrorSquared / std::max(1.0, referenceSquared));
        std::cout << "[RodAnalyticOracle] segments=" << segments
                  << " maxBlockRelative=" << maximumBlockError
                  << " directionRelative=" << directionError << std::endl;
        EXPECT_LT(directionError, 2.0e-6);
    }
}

TEST(RodProgressiveSupport, NonlinearRootDriveAndDampingKeepCompletePowerIdentity)
{
    const RestState rest = MakeCurve(8);
    const ProgressiveSupport profile = BuildProgressiveSupport(rest);
    State state = MakeAuthoredState(rest);
    RootFrame root;
    root.position = rest.positions.front() + Vec3(0.003, -0.004, 0.002);
    root.orientation = Exp(Vec3(0.01, -0.02, 0.03)) * rest.orientations.front();
    root.linearVelocity = Vec3(0.2, -0.3, 0.1);
    root.angularVelocityWorld = Vec3(0.3, -0.1, 0.2);
    state.velocities[3] = Vec3(0.1, 0.2, -0.1);
    const auto evaluation = EvaluateProgressiveSupport(rest, state, root, profile);
    const double epsilon = 1.0e-6;
    RootFrame positive = root;
    RootFrame negative = root;
    positive.position += epsilon * root.linearVelocity;
    negative.position -= epsilon * root.linearVelocity;
    positive.orientation = Exp(epsilon * root.angularVelocityWorld) * root.orientation;
    negative.orientation = Exp(-epsilon * root.angularVelocityWorld) * root.orientation;
    const double partial = (EvaluateProgressiveSupport(rest, state, positive, profile).potential -
        EvaluateProgressiveSupport(rest, state, negative, profile).potential) / (2.0 * epsilon);
    double springPower = 0.0;
    double dampingPower = 0.0;
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        const auto sample = SampleRootSupport(root, rest.rootLocalPositions[point]);
        const auto node = EvaluateProgressiveNode(state.positions[point] - sample.position,
            rest.supportStiffness[point], profile.cubicStiffness[point]);
        springPower += node.springForce.dot(sample.velocity);
        dampingPower += (evaluation.forces[point] - node.springForce).dot(state.velocities[point]);
    }
    EXPECT_NEAR(partial, springPower, 1.0e-10);
    EXPECT_NEAR(springPower + dampingPower,
        evaluation.drivePower - evaluation.dissipation, 1.0e-12);
}

TEST(RodProgressiveSupport, ImplicitStepUsesQuarticResidualJacobianAndEnergy)
{
    const char* exportDirectory = std::getenv("ROD_PROGRESSIVE_EXPORT_DIR");
    std::filesystem::path exportRoot;
    if (exportDirectory)
    {
        exportRoot = exportDirectory;
        ASSERT_FALSE(std::filesystem::exists(exportRoot));
        ASSERT_TRUE(std::filesystem::create_directories(exportRoot));
    }
    for (size_t segments : {3u, 8u})
    {
        const RestState rest = MakeCurve(segments);
        const ProgressiveSupport profile = BuildProgressiveSupport(rest);
        State old = MakeAuthoredState(rest);
        State candidate = old;
        candidate.positions[2] += Vec3(0.004, -0.006, 0.003);
        StepInput input;
        input.root.position = rest.positions.front();
        input.root.orientation = rest.orientations.front();
        const auto system = AssembleProgressiveImplicitSystem(rest, old, candidate, input, profile);
        for (int axis = 0; axis < 3; ++axis)
        {
            const double epsilon = 1.0e-6 * rest.totalLength;
            State positive = candidate;
            State negative = candidate;
            positive.positions[2][axis] += epsilon;
            negative.positions[2][axis] -= epsilon;
            const Vec3 derivative =
                ((EvaluateProgressiveImplicitResidual(rest, old, positive, input, profile)[2] -
                  EvaluateProgressiveImplicitResidual(rest, old, negative, input, profile)[2]) /
                 (2.0 * epsilon)).head<3>() * (system.lengthScale / system.forceScale);
            EXPECT_LT((derivative - system.diagonal[2].block<3, 1>(0, axis)).norm() /
                std::max(1.0, derivative.norm()), 2.0e-6);
        }
        std::vector<State> terminals;
        for (unsigned refinement = 0; refinement < 3; ++refinement)
        {
            State state = MakeAuthoredState(rest);
            input.stepSeconds = 1.0 / (240.0 * (1u << refinement));
            input.externalForces.clear();
            for (double mass : rest.masses)
                input.externalForces.push_back(Vec3(20.0 * mass, 0.0, 0.0));
            std::ofstream output;
            if (exportDirectory)
            {
                output.open(exportRoot / ("n" + std::to_string(segments) +
                    "-h" + std::to_string(240u << refinement) + ".csv"));
                output << std::setprecision(17);
                output << "step,time,index,x,y,z,vx,vy,vz,qx,qy,qz,qw,wx,wy,wz,rx,ry,rz,tx,ty,tz,k2,Fw,residual,newton,energy,Tlinear,Tangular\n";
            }
            double maximumResidual = 0.0;
            unsigned maximumIterations = 0;
            for (unsigned step = 0; step < (6u << refinement); ++step)
            {
                const State before = state;
                const auto result = StepImplicitMomentumProgressive(rest, state, input, profile);
                ASSERT_EQ(result.status, Status::Success)
                    << "segments=" << segments << " refinement=" << refinement
                    << " step=" << step << " residual=" << result.normalizedResidual;
                EXPECT_TRUE(result.converged);
                EXPECT_LE(result.normalizedResidual, 1.0e-8);
                maximumResidual = std::max(maximumResidual, result.normalizedResidual);
                maximumIterations = std::max(maximumIterations, result.newtonIterations);
                const auto residual = EvaluateProgressiveImplicitResidual(rest, before, state, input, profile);
                for (size_t point = 0; exportDirectory && point < state.positions.size(); ++point)
                {
                    const Quat orientation = point < segments ? state.orientations[point] : Quat::Identity();
                    const Vec3 omega = point < segments ? state.angularVelocities[point] : Vec3::Zero().eval();
                    output << step + 1 << ',' << (step + 1) * input.stepSeconds << ',' << point
                           << ',' << state.positions[point].x() << ',' << state.positions[point].y()
                           << ',' << state.positions[point].z() << ',' << state.velocities[point].x()
                           << ',' << state.velocities[point].y() << ',' << state.velocities[point].z()
                           << ',' << orientation.x() << ',' << orientation.y() << ',' << orientation.z()
                           << ',' << orientation.w() << ',' << omega.x() << ',' << omega.y() << ',' << omega.z();
                    for (int component = 0; component < 6; ++component)
                        output << ',' << residual[point][component];
                    output << ',' << profile.cubicStiffness[point] << ',' << profile.designForces[point]
                           << ',' << result.normalizedResidual << ',' << result.newtonIterations
                           << ',' << result.energy << ',' << result.linearKineticEnergy
                           << ',' << result.angularKineticEnergy << '\n';
                }
            }
            terminals.push_back(state);
            std::cout << "[RodProgressiveShort] segments=" << segments << " h=" << input.stepSeconds
                      << " maxResidual=" << maximumResidual << " maxNewton=" << maximumIterations
                      << " dragPerSpeed=" << profile.dragAccelerationPerSpeed << std::endl;
        }
        Eigen::Vector2d differences[2] = {Eigen::Vector2d::Zero(), Eigen::Vector2d::Zero()};
        for (size_t tier = 0; tier < 2; ++tier)
        {
            for (size_t point = 0; point < rest.positions.size(); ++point)
                differences[tier].x() = std::max(differences[tier].x(),
                    (terminals[tier].positions[point] - terminals[tier + 1].positions[point]).norm());
            for (size_t segment = 0; segment < segments; ++segment)
                differences[tier].y() = std::max(differences[tier].y(),
                    Log(terminals[tier].orientations[segment].conjugate() *
                        terminals[tier + 1].orientations[segment]).norm());
        }
        std::cout << "[RodProgressiveDifference] segments=" << segments
                  << " coarseX=" << differences[0].x() << " fineX=" << differences[1].x()
                  << " coarseQ=" << differences[0].y() << " fineQ=" << differences[1].y() << std::endl;
        EXPECT_LT(differences[1].x(), differences[0].x());
        EXPECT_LT(differences[1].y(), differences[0].y());
    }
}
}
