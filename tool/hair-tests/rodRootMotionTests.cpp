#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <string>

#include <nlohmann/json.hpp>

#include "hair/rod/rodImplicit.h"
#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodSectionInertia.h"
#include "hair/rod/rodRootSupport.h"

namespace
{
using namespace VL::Hair::Rod;
using Json = nlohmann::json;

std::filesystem::path EvidenceDirectory()
{
    const char* directory = std::getenv("VL_ROOT_MOTION_OUTPUT");
    if (directory == nullptr || *directory == '\0')
        return {};
    std::filesystem::create_directories(directory);
    return directory;
}

Json VectorJson(const Vec3& vector)
{
    return {vector.x(), vector.y(), vector.z()};
}

Json QuaternionJson(const Quat& orientation)
{
    return {orientation.w(), orientation.x(), orientation.y(), orientation.z()};
}

Json StateJson(const State& state)
{
    Json result;
    for (const auto& position : state.positions) result["x"].push_back(VectorJson(position));
    for (const auto& velocity : state.velocities) result["v"].push_back(VectorJson(velocity));
    for (const auto& orientation : state.orientations) result["q"].push_back(QuaternionJson(orientation));
    for (const auto& velocity : state.angularVelocities) result["omegaBody"].push_back(VectorJson(velocity));
    return result;
}

void SaveJson(const std::string& name, const Json& value)
{
    const auto directory = EvidenceDirectory();
    if (directory.empty())
        return;
    std::ofstream output(directory / name);
    output << value.dump(2) << '\n';
    ASSERT_TRUE(output.good());
}

RestState Fixture(size_t segments, bool doublePin)
{
    std::vector<Vec3> positions;
    std::vector<double> markers(segments + 1, 1.0);
    markers[0] = 0.0;
    if (doublePin) markers[1] = 0.0;
    for (size_t point = 0; point <= segments; ++point)
    {
        const double parameter = static_cast<double>(point) / static_cast<double>(segments);
        positions.emplace_back(0.12 + 0.025 * std::sin(2.0 * parameter),
            -0.08 + 0.02 * (1.0 - std::cos(2.0 * parameter)), 0.2 + 0.3 * parameter);
    }
    return BuildSectionInertiaRestState(positions, {}, {}, markers);
}

RootFrame RestRoot(const RestState& rest)
{
    RootFrame root;
    root.position = rest.positions.front();
    root.orientation = rest.orientations.front();
    return root;
}

double OrientationError(const Quat& first, const Quat& second)
{
    const Quat difference = (first.conjugate() * second).normalized();
    return 2.0 * std::atan2(difference.vec().norm(), std::abs(difference.w()));
}

double FreeResidual(const RestState& rest, const State& old,
    const State& state, const StepInput& input, const ProgressiveSupport& profile)
{
    const auto residual = EvaluateProgressiveImplicitResidual(rest, old, state, input, profile);
    const double mass = std::accumulate(rest.masses.begin(), rest.masses.end(), 0.0);
    const double stiffness = *std::max_element(rest.supportStiffness.begin(), rest.supportStiffness.end());
    const double forceScale = mass * 9.8 + stiffness * rest.totalLength;
    double maximum = 0.0;
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        if (rest.inverseMasses[point] > 0.0)
            maximum = std::max(maximum, residual[point].head<3>().cwiseAbs().maxCoeff() / forceScale);
        if (point > 0 && point < rest.lengths.size())
            maximum = std::max(maximum,
                residual[point].tail<3>().cwiseAbs().maxCoeff() / (forceScale * rest.totalLength));
    }
    return maximum;
}

void WriteState64(std::ofstream& output, double time, const RootFrame& root, const State& state)
{
    if (!output.is_open()) return;
    const auto scalar = [&](double value)
    {
        output.write(reinterpret_cast<const char*>(&value), sizeof(value));
    };
    const auto vector = [&](const Vec3& value)
    {
        scalar(value.x()); scalar(value.y()); scalar(value.z());
    };
    const auto quaternion = [&](const Quat& value)
    {
        scalar(value.w()); scalar(value.x()); scalar(value.y()); scalar(value.z());
    };
    scalar(time);
    vector(root.position);
    quaternion(root.orientation);
    vector(root.linearVelocity);
    vector(root.angularVelocityWorld);
    for (const auto& value : state.positions) vector(value);
    for (const auto& value : state.velocities) vector(value);
    for (const auto& value : state.orientations) quaternion(value);
    for (const auto& value : state.angularVelocities) vector(value);
}

Json RestJson(const RestState& rest, const ProgressiveSupport& profile)
{
    Json result{{"kineticMathVersion", SectionInertiaKineticMathVersion},
        {"totalLength", rest.totalLength}, {"lengths", rest.lengths},
        {"masses", rest.masses}, {"inverseMasses", rest.inverseMasses},
        {"supportStiffness", rest.supportStiffness}, {"supportDamping", rest.supportDamping},
        {"cubicStiffness", profile.cubicStiffness}};
    for (const auto& value : rest.positions) result["x"].push_back(VectorJson(value));
    for (const auto& value : rest.orientations) result["q"].push_back(QuaternionJson(value));
    for (const auto& value : rest.rootLocalPositions) result["rootLocal"].push_back(VectorJson(value));
    for (const auto& value : rest.inertias) result["inertias"].push_back(VectorJson(value));
    for (const auto& value : rest.restStrains) result["restStrains"].push_back(VectorJson(value));
    for (const auto& value : rest.restJointAngles) result["restJointAngles"].push_back(VectorJson(value));
    for (const auto& value : rest.stretchShear) result["stretchShear"].push_back(VectorJson(value));
    for (const auto& value : rest.bendTwist) result["bendTwist"].push_back(VectorJson(value));
    return result;
}

TEST(RodRootMotion, CommonConstantSE3WholeStepWithGeneralStateAndLoads)
{
    const Quat rotation = Exp(Vec3(0.7, -0.5, 0.6));
    const Vec3 translation(0.7, -1.1, 0.4);
    Json results = Json::array();
    for (size_t segments : {3u, 8u, 16u, 32u})
        for (bool doublePin : {false, true})
            for (bool continuation : {false, true})
            {
                SCOPED_TRACE(::testing::Message() << segments << "/" << doublePin << "/" << continuation);
                const auto rest = Fixture(segments, doublePin);
                const auto profile = BuildProgressiveSupport(rest);
                State old = MakeAuthoredState(rest);
                RootFrame root = RestRoot(rest);
                root.linearVelocity = Vec3(0.011, -0.007, 0.005);
                root.angularVelocityWorld = Vec3(0.03, -0.02, 0.015);
                for (size_t point = 0; point < old.positions.size(); ++point)
                {
                    old.velocities[point] = SampleRootSupport(root, rest.rootLocalPositions[point]).velocity;
                    if (rest.inverseMasses[point] > 0.0)
                    {
                        const double index = static_cast<double>(point);
                        old.positions[point] += 2.0e-6 * Vec3(std::sin(index), std::cos(index), std::sin(0.7 * index));
                        old.velocities[point] += Vec3(0.003, -0.002, 0.001);
                    }
                }
                for (size_t segment = 0; segment < segments; ++segment)
                {
                    old.angularVelocities[segment] = old.orientations[segment].conjugate() * root.angularVelocityWorld;
                    if (segment > 0)
                    {
                        old.orientations[segment] = ApplyBodyIncrement(old.orientations[segment],
                            3.0e-4 * Vec3(std::sin(static_cast<double>(segment)), -0.4, 0.7));
                        old.angularVelocities[segment] += Vec3(0.02, -0.01, 0.015);
                    }
                }
                StepInput input;
                input.stepSeconds = 1.0 / 480.0;
                input.gravity = Vec3(0.5, -4.2, 0.8);
                input.root = root;
                input.root.position += input.stepSeconds * root.linearVelocity;
                input.root.orientation = (Exp(input.stepSeconds * root.angularVelocityWorld) * root.orientation).normalized();
                for (size_t point = 0; point < rest.positions.size(); ++point)
                    input.externalForces.push_back(rest.masses[point] *
                        Vec3(0.4 * std::sin(static_cast<double>(point)), 0.3, -0.2));
                for (size_t segment = 0; segment < segments; ++segment)
                    input.externalTorquesWorld.push_back(old.orientations[segment] *
                        rest.inertias[segment].cwiseProduct(Vec3(0.2, -0.1, 0.17)));
                auto transformedRest = rest;
                for (auto& value : transformedRest.positions) value = rotation * value + translation;
                for (auto& value : transformedRest.orientations) value = (rotation * value).normalized();
                auto transformedOld = old;
                for (auto& value : transformedOld.positions) value = rotation * value + translation;
                for (auto& value : transformedOld.velocities) value = rotation * value;
                for (auto& value : transformedOld.orientations) value = (rotation * value).normalized();
                auto transformedInput = input;
                transformedInput.root.position = rotation * input.root.position + translation;
                transformedInput.root.orientation = (rotation * input.root.orientation).normalized();
                transformedInput.root.linearVelocity = rotation * input.root.linearVelocity;
                transformedInput.root.angularVelocityWorld = rotation * input.root.angularVelocityWorld;
                transformedInput.gravity = rotation * input.gravity;
                for (auto& value : transformedInput.externalForces) value = rotation * value;
                for (auto& value : transformedInput.externalTorquesWorld) value = rotation * value;
                auto state = old;
                auto transformed = transformedOld;
                ImplicitTrace trace;
                ImplicitTrace transformedTrace;
                const auto first = continuation
                    ? StepImplicitMomentumProgressiveContinuation(rest, state, input, profile, &trace)
                    : StepImplicitMomentumProgressiveWithTrace(rest, state, input, profile, trace);
                const auto second = continuation
                    ? StepImplicitMomentumProgressiveContinuation(transformedRest, transformed, transformedInput, profile, &transformedTrace)
                    : StepImplicitMomentumProgressiveWithTrace(transformedRest, transformed, transformedInput, profile, transformedTrace);
                Json row{{"segments", segments}, {"doublePin", doublePin}, {"continuation", continuation},
                    {"status", static_cast<int>(first.status)}, {"transformedStatus", static_cast<int>(second.status)},
                    {"failure", trace.failureReason}, {"transformedFailure", transformedTrace.failureReason},
                    {"old", StateJson(old)}, {"new", StateJson(state)}, {"transformedNew", StateJson(transformed)},
                    {"iterations", first.newtonIterations}, {"transformedIterations", second.newtonIterations}};
                EXPECT_EQ(first.status, Status::Success);
                EXPECT_EQ(second.status, Status::Success);
                if (first.status == Status::Success && second.status == Status::Success)
                {
                    double positionError = 0, velocityError = 0, orientationError = 0, angularError = 0;
                    for (size_t point = 0; point < rest.positions.size(); ++point)
                    {
                        positionError = std::max(positionError,
                            (transformed.positions[point] - rotation * state.positions[point] - translation).norm());
                        velocityError = std::max(velocityError,
                            (transformed.velocities[point] - rotation * state.velocities[point]).norm());
                    }
                    for (size_t segment = 0; segment < segments; ++segment)
                    {
                        orientationError = std::max(orientationError,
                            OrientationError(rotation * state.orientations[segment], transformed.orientations[segment]));
                        angularError = std::max(angularError,
                            (transformed.angularVelocities[segment] - state.angularVelocities[segment]).norm());
                    }
                    const double mass = std::accumulate(rest.masses.begin(), rest.masses.end(), 0.0);
                    const double energyError = std::abs(second.energy - first.energy +
                        mass * transformedInput.gravity.dot(translation));
                    row["positionErrorM"] = positionError; row["velocityErrorMps"] = velocityError;
                    row["orientationErrorRad"] = orientationError; row["bodyOmegaError"] = angularError;
                    row["gravityOffsetCorrectedEnergyErrorJ"] = energyError;
                    row["residual"] = FreeResidual(rest, old, state, input, profile);
                    row["transformedResidual"] = FreeResidual(transformedRest, transformedOld, transformed, transformedInput, profile);
                    EXPECT_LE(positionError, 2.0e-9); EXPECT_LE(velocityError, 2.0e-6);
                    EXPECT_LE(orientationError, 2.0e-7); EXPECT_LE(angularError, 2.0e-4);
                    EXPECT_LE(energyError, 1.0e-10);
                    EXPECT_LE(row["residual"].get<double>(), 2.0e-8);
                    EXPECT_LE(row["transformedResidual"].get<double>(), 2.0e-8);
                }
                results.push_back(std::move(row));
            }
    SaveJson("se3-whole-step.json", {{"kineticMathVersion", SectionInertiaKineticMathVersion}, {"cases", results}});
}

TEST(RodRootMotion, ConstantVelocityGalileanAuthoredShapeAndRelativeDampingOracle)
{
    const Vec3 velocity(0.37, -0.21, 0.13);
    Json results = Json::array();
    for (size_t segments : {3u, 8u, 32u})
        for (unsigned frequency : {240u, 480u})
        {
            SCOPED_TRACE(::testing::Message() << segments << "/" << frequency);
            const auto rest = Fixture(segments, true);
            const auto profile = BuildProgressiveSupport(rest);
            auto state = MakeAuthoredState(rest);
            for (auto& value : state.velocities) value = velocity;
            StepInput input;
            input.gravity.setZero();
            input.stepSeconds = 1.0 / frequency;
            input.root = RestRoot(rest);
            input.root.linearVelocity = velocity;
            const std::string name = "galilean-n" + std::to_string(segments) + "-h" + std::to_string(frequency);
            std::ofstream stream;
            if (!EvidenceDirectory().empty())
                stream.open(EvidenceDirectory() / (name + ".state64"), std::ios::binary);
            WriteState64(stream, 0.0, input.root, state);
            double positionError = 0, velocityError = 0, orientationError = 0, angularError = 0;
            double dissipation = 0, residual = 0;
            unsigned accepted = 0;
            int failure = 0;
            for (unsigned step = 1; step <= frequency; ++step)
            {
                const double time = step * input.stepSeconds;
                input.root.position = rest.positions.front() + velocity * time;
                const auto old = state;
                const auto diagnostics = StepImplicitMomentumProgressiveContinuation(rest, state, input, profile);
                if (diagnostics.status != Status::Success)
                {
                    failure = static_cast<int>(diagnostics.status);
                    ADD_FAILURE() << "step=" << step << " status=" << failure;
                    break;
                }
                ++accepted;
                residual = std::max(residual, FreeResidual(rest, old, state, input, profile));
                dissipation = std::max(dissipation, EvaluateProgressiveSupport(rest, state, input.root, profile).dissipation);
                for (size_t point = 0; point < rest.positions.size(); ++point)
                {
                    positionError = std::max(positionError,
                        (state.positions[point] - rest.positions[point] - velocity * time).norm());
                    velocityError = std::max(velocityError, (state.velocities[point] - velocity).norm());
                }
                for (size_t segment = 0; segment < segments; ++segment)
                {
                    orientationError = std::max(orientationError,
                        OrientationError(rest.orientations[segment], state.orientations[segment]));
                    angularError = std::max(angularError, state.angularVelocities[segment].norm());
                }
                WriteState64(stream, time, input.root, state);
            }
            SaveJson(name + "-rest.json", RestJson(rest, profile));
            results.push_back({{"segments", segments}, {"frequency", frequency}, {"acceptedSteps", accepted},
                {"status", failure}, {"maxPositionErrorM", positionError}, {"maxVelocityErrorMps", velocityError},
                {"maxOrientationErrorRad", orientationError}, {"maxBodyOmega", angularError},
                {"maxDissipationW", dissipation}, {"maxNormalizedResidual", residual}, {"final", StateJson(state)}});
            EXPECT_EQ(accepted, frequency);
            EXPECT_LE(positionError, 2.0e-8); EXPECT_LE(velocityError, 2.0e-6);
            EXPECT_LE(orientationError, 2.0e-6); EXPECT_LE(angularError, 1.0e-3);
            EXPECT_LE(dissipation, 1.0e-13); EXPECT_LE(residual, 2.0e-8);
        }
    SaveJson("galilean.json", {{"kineticMathVersion", SectionInertiaKineticMathVersion}, {"cases", results}});
}

RootFrame PeriodicRoot(const RestState& rest, double time)
{
    const Vec3 axis = Vec3(0.3, 0.7, -0.2).normalized();
    const Vec3 amplitude(0.0012, 0.0007, 0.0009);
    const double frequency = 2.0 * Pi;
    const double sine = std::sin(frequency * time);
    const double cosine = std::cos(frequency * time);
    RootFrame root = RestRoot(rest);
    root.position += amplitude * sine;
    root.orientation = (Exp(axis * (0.02 * sine)) * root.orientation).normalized();
    root.linearVelocity = amplitude * (frequency * cosine);
    root.angularVelocityWorld = axis * (0.02 * frequency * cosine);
    return root;
}

std::array<double, 2> KineticEnergy(const RestState& rest, const State& state)
{
    std::array<double, 2> result{0.0, 0.0};
    for (size_t point = 0; point < rest.positions.size(); ++point)
        result[0] += 0.5 * rest.masses[point] * state.velocities[point].squaredNorm();
    for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
        result[1] += 0.5 * state.angularVelocities[segment].dot(
            rest.inertias[segment].cwiseProduct(state.angularVelocities[segment]));
    return result;
}

bool FiniteState(const State& state)
{
    for (const auto& value : state.positions) if (!value.allFinite()) return false;
    for (const auto& value : state.velocities) if (!value.allFinite()) return false;
    for (const auto& value : state.orientations) if (!value.coeffs().allFinite()) return false;
    for (const auto& value : state.angularVelocities) if (!value.allFinite()) return false;
    return true;
}

TEST(RodRootMotion, OneHertzRootDriveRecordsWorkAndDiscreteEnergyDefects)
{
    Json results = Json::array();
    const auto rest = Fixture(8, true);
    const auto profile = BuildProgressiveSupport(rest);
    for (unsigned frequency : {240u, 480u})
    {
        SCOPED_TRACE(frequency);
        const std::string name = "periodic-n8-h" + std::to_string(frequency);
        State state = MakeAuthoredState(rest);
        const auto initialRoot = PeriodicRoot(rest, 0.0);
        for (size_t point = 0; point < state.positions.size(); ++point)
            state.velocities[point] = SampleRootSupport(initialRoot, rest.rootLocalPositions[point]).velocity;
        for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
            state.angularVelocities[segment] =
                state.orientations[segment].conjugate() * initialRoot.angularVelocityWorld;
        const auto initialKinetic = KineticEnergy(rest, state);
        const double initialEnergy = initialKinetic[0] + initialKinetic[1] +
            EvaluateEnergy(rest, state).Total() +
            EvaluateProgressiveSupport(rest, state, initialRoot, profile).potential;
        const auto directory = EvidenceDirectory();
        std::ofstream raw;
        std::ofstream csv;
        if (!directory.empty())
        {
            raw.open(directory / (name + ".state64"), std::ios::binary);
            csv.open(directory / (name + ".csv"));
            csv << std::setprecision(17);
            csv << "time,status,newton,backtracks,residual,pin_error_m,q0_error_rad,qnorm_error,"
                "elastic_j,support_j,linear_kinetic_j,angular_kinetic_j,energy_change_j,"
                "root_discrete_work_j,root_analytic_power_w,support_drive_power_w,dissipation_w,"
                "linear_be_defect_j,angular_transport_defect_j,elastic_endpoint_defect_j,"
                "support_endpoint_defect_j,free_residual_work_j,ledger_closure_j,root_power_identity_error_w\n";
        }
        WriteState64(raw, 0.0, initialRoot, state);
        SaveJson(name + "-rest.json", RestJson(rest, profile));
        StepInput input;
        input.stepSeconds = 1.0 / frequency;
        input.gravity.setZero();
        double maxResidual = 0, maxPinError = 0, maxRootAngle = 0, maxQuaternionNorm = 0;
        double maxLedgerClosure = 0, maxPowerIdentityError = 0, maxSupportFdError = 0;
        double maxRootPower = 0, maxSupportPower = 0;
        double rootWork = 0, driveWork = 0, dampingWork = 0;
        double linearDefect = 0, angularDefect = 0, elasticDefect = 0, supportDefect = 0, residualWork = 0;
        double finalEnergy = initialEnergy;
        unsigned accepted = 0, totalIterations = 0, totalBacktracks = 0;
        int failure = 0;
        for (unsigned step = 1; step <= 6 * frequency; ++step)
        {
            const double time = step * input.stepSeconds;
            input.root = PeriodicRoot(rest, time);
            const auto oldRoot = PeriodicRoot(rest, time - input.stepSeconds);
            const auto old = state;
            const auto oldElastic = EvaluateEnergy(rest, old);
            const auto oldSupport = EvaluateProgressiveSupport(rest, old, oldRoot, profile);
            const auto oldKinetic = KineticEnergy(rest, old);
            ImplicitTrace trace;
            const auto diagnostics = StepImplicitMomentumProgressiveContinuation(
                rest, state, input, profile, &trace);
            totalIterations += diagnostics.newtonIterations;
            totalBacktracks += diagnostics.backtracks;
            if (diagnostics.status != Status::Success)
            {
                failure = static_cast<int>(diagnostics.status);
                SaveJson(name + "-failure.json", {{"step", step}, {"time", time},
                    {"status", failure}, {"reason", trace.failureReason},
                    {"old", StateJson(old)}, {"failedCandidate", StateJson(trace.finalCandidate)}});
                ADD_FAILURE() << "root drive step=" << step << " status=" << failure << " " << trace.failureReason;
                break;
            }
            ++accepted;
            ASSERT_TRUE(FiniteState(state));
            const auto elastic = EvaluateEnergy(rest, state);
            const auto support = EvaluateProgressiveSupport(rest, state, input.root, profile);
            const auto kinetic = KineticEnergy(rest, state);
            const auto physical = EvaluateProgressiveImplicitResidual(rest, old, state, input, profile);
            const double residual = FreeResidual(rest, old, state, input, profile);
            double pinError = 0, quaternionNorm = 0;
            double stepRootWork = 0, stepFreeWork = 0, analyticRootPower = 0;
            double stepLinearDefect = 0, elasticPower = 0, relativeSpringPower = 0;
            double angularInertiaWork = 0;
            for (size_t point = 0; point < rest.positions.size(); ++point)
            {
                const auto target = SampleRootSupport(input.root, rest.rootLocalPositions[point]);
                const Vec3 displacement = state.positions[point] - target.position;
                const Vec3 spring = -(rest.supportStiffness[point] +
                    profile.cubicStiffness[point] * displacement.squaredNorm()) * displacement;
                relativeSpringPower += spring.dot(state.velocities[point] - target.velocity);
                elasticPower += elastic.forces[point].dot(state.velocities[point]);
                stepLinearDefect += 0.5 * rest.masses[point] *
                    (state.velocities[point] - old.velocities[point]).squaredNorm();
                const double momentumWork = input.stepSeconds *
                    physical[point].head<3>().dot(state.velocities[point]);
                if (rest.inverseMasses[point] == 0.0)
                {
                    pinError = std::max(pinError, (state.positions[point] - target.position).norm());
                    stepRootWork += momentumWork;
                    analyticRootPower += physical[point].head<3>().dot(target.velocity);
                }
                else
                    stepFreeWork += momentumWork;
            }
            for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
            {
                const Vec3 transported = state.orientations[segment].conjugate() *
                    (old.orientations[segment] *
                        rest.inertias[segment].cwiseProduct(old.angularVelocities[segment]));
                angularInertiaWork += state.angularVelocities[segment].dot(
                    rest.inertias[segment].cwiseProduct(state.angularVelocities[segment]) - transported);
                elasticPower += elastic.bodyTorques[segment].dot(state.angularVelocities[segment]);
                quaternionNorm = std::max(quaternionNorm,
                    std::abs(state.orientations[segment].norm() - 1.0));
                const double momentumWork = input.stepSeconds *
                    physical[segment].tail<3>().dot(state.angularVelocities[segment]);
                if (segment == 0)
                {
                    stepRootWork += momentumWork;
                    analyticRootPower += physical[segment].tail<3>().dot(
                        input.root.orientation.conjugate() * input.root.angularVelocityWorld);
                }
                else
                    stepFreeWork += momentumWork;
            }
            const double rootAngle = OrientationError(state.orientations.front(), input.root.orientation);
            const double stepAngularDefect = angularInertiaWork - (kinetic[1] - oldKinetic[1]);
            const double stepElasticDefect = elastic.Total() - oldElastic.Total() +
                input.stepSeconds * elasticPower;
            const double stepSupportDefect = support.potential - oldSupport.potential +
                input.stepSeconds * relativeSpringPower;
            const double stepDriveWork = input.stepSeconds * support.drivePower;
            const double stepDampingWork = input.stepSeconds * support.dissipation;
            const double total = elastic.Total() + support.potential + kinetic[0] + kinetic[1];
            const double previous = oldElastic.Total() + oldSupport.potential + oldKinetic[0] + oldKinetic[1];
            const double closure = total - previous - stepRootWork - stepDriveWork + stepDampingWork +
                stepLinearDefect + stepAngularDefect - stepElasticDefect - stepSupportDefect - stepFreeWork;
            const double apiRootPower = diagnostics.rootForce.dot(input.root.linearVelocity) +
                diagnostics.rootBodyTorque.dot(
                    input.root.orientation.conjugate() * input.root.angularVelocityWorld);
            const double powerIdentityError = std::abs(apiRootPower - analyticRootPower);
            ASSERT_TRUE(std::isfinite(total) && std::isfinite(closure) &&
                std::isfinite(apiRootPower) && std::isfinite(support.drivePower) &&
                std::isfinite(support.dissipation) && std::isfinite(stepAngularDefect) &&
                std::isfinite(residual)) << "non-finite ledger at t=" << time;
            if (step % (frequency / 4) == 0)
                for (double epsilon : {1.0e-5, 1.0e-6})
                {
                    auto positive = state;
                    auto negative = state;
                    double forcePower = 0;
                    for (size_t point = 0; point < rest.positions.size(); ++point)
                    {
                        positive.positions[point] += epsilon * state.velocities[point];
                        negative.positions[point] -= epsilon * state.velocities[point];
                        forcePower += support.forces[point].dot(state.velocities[point]);
                    }
                    const double derivative = (
                        EvaluateProgressiveSupport(rest, positive, PeriodicRoot(rest, time + epsilon), profile).potential -
                        EvaluateProgressiveSupport(rest, negative, PeriodicRoot(rest, time - epsilon), profile).potential) /
                        (2.0 * epsilon);
                    maxSupportFdError = std::max(maxSupportFdError,
                        std::abs(forcePower + derivative - support.drivePower + support.dissipation));
                }
            maxResidual = std::max(maxResidual, residual);
            maxPinError = std::max(maxPinError, pinError);
            maxRootAngle = std::max(maxRootAngle, rootAngle);
            maxQuaternionNorm = std::max(maxQuaternionNorm, quaternionNorm);
            maxLedgerClosure = std::max(maxLedgerClosure, std::abs(closure));
            maxPowerIdentityError = std::max(maxPowerIdentityError, powerIdentityError);
            maxRootPower = std::max(maxRootPower, std::abs(apiRootPower));
            maxSupportPower = std::max(maxSupportPower, std::abs(support.drivePower));
            rootWork += stepRootWork; driveWork += stepDriveWork; dampingWork += stepDampingWork;
            linearDefect += stepLinearDefect; angularDefect += stepAngularDefect;
            elasticDefect += stepElasticDefect; supportDefect += stepSupportDefect; residualWork += stepFreeWork;
            finalEnergy = total;
            if (csv.is_open())
                csv << time << ",0," << diagnostics.newtonIterations << ',' << diagnostics.backtracks << ','
                    << residual << ',' << pinError << ',' << rootAngle << ',' << quaternionNorm << ','
                    << elastic.Total() << ',' << support.potential << ',' << kinetic[0] << ',' << kinetic[1] << ','
                    << total - previous << ',' << stepRootWork << ',' << apiRootPower << ',' << support.drivePower << ','
                    << support.dissipation << ',' << stepLinearDefect << ',' << stepAngularDefect << ','
                    << stepElasticDefect << ',' << stepSupportDefect << ',' << stepFreeWork << ',' << closure << ','
                    << powerIdentityError << '\n';
            WriteState64(raw, time, input.root, state);
        }
        results.push_back({{"frequency", frequency}, {"acceptedSteps", accepted}, {"failure", failure},
            {"newtonIterations", totalIterations}, {"backtracks", totalBacktracks},
            {"maxResidual", maxResidual}, {"maxPinErrorM", maxPinError}, {"maxQ0ErrorRad", maxRootAngle},
            {"maxQuaternionNormError", maxQuaternionNorm}, {"maxLedgerClosureJ", maxLedgerClosure},
            {"maxRootPowerIdentityErrorW", maxPowerIdentityError}, {"maxSupportPowerFdErrorW", maxSupportFdError},
            {"maxAbsRootPowerW", maxRootPower}, {"maxAbsSupportDrivePowerW", maxSupportPower},
            {"initialEnergyJ", initialEnergy}, {"finalEnergyJ", finalEnergy},
            {"rootDiscreteWorkJ", rootWork}, {"supportDriveWorkJ", driveWork}, {"supportDissipationJ", dampingWork},
            {"linearBeDefectJ", linearDefect}, {"angularTransportDefectJ", angularDefect},
            {"elasticEndpointDefectJ", elasticDefect}, {"supportEndpointDefectJ", supportDefect},
            {"freeResidualWorkJ", residualWork}, {"dynamicQuality", "NOT_ASSERTED"}});
        EXPECT_EQ(accepted, 6 * frequency);
        EXPECT_LE(maxResidual, 2.0e-8); EXPECT_LE(maxPinError, 2.0e-12);
        EXPECT_LE(maxRootAngle, 2.0e-10); EXPECT_LE(maxQuaternionNorm, 1.0e-10);
        EXPECT_LE(maxLedgerClosure, 1.0e-13); EXPECT_LE(maxPowerIdentityError, 1.0e-11);
        EXPECT_LE(maxSupportFdError, 1.0e-10);
        EXPECT_GT(maxRootPower, 1.0e-14);
    }
    SaveJson("periodic-root.json", {{"kineticMathVersion", SectionInertiaKineticMathVersion},
        {"frequencyHz", 1}, {"durationSeconds", 6}, {"dynamicQuality", "NOT_ASSERTED"}, {"cases", results}});
}
}
