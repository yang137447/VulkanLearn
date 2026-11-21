#include "hair/rod/rodContactImplicit.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

#include <Eigen/LU>

#include "hair/rod/rodEnergy.h"
#include "hair/rod/rodMath.h"
#include "hair/rod/rodRootSupport.h"

namespace VL::Hair::Rod
{
namespace
{
using Vector9 = Eigen::Matrix<double, 9, 1>;
using Matrix9 = Eigen::Matrix<double, 9, 9>;

using ActiveNode = ContactActiveNode;

void ValidateGeometry(const ContactGeometry& geometry)
{
    if (geometry.valid && (!std::isfinite(geometry.distance) || !geometry.gradient.allFinite() ||
        !geometry.hessian.allFinite() || !geometry.surfaceVelocity.allFinite() ||
        !std::isfinite(geometry.gradient.norm()) || geometry.gradient.norm() <= 1.0e-14))
        throw std::domain_error("contact geometry is not differentiable");
}

double Gap(const ContactConstraint& constraint, const ContactGeometry& geometry, const StepInput& input)
{
    return geometry.distance - constraint.distanceScale * input.contactMargin;
}

ContactGeometry SphereGeometry(const Vec3& position, const CollisionSphere& sphere)
{
    const Vec3 offset = position - sphere.center;
    const double radius = offset.norm();
    if (!position.allFinite() || !sphere.center.allFinite() || !sphere.velocity.allFinite() ||
        !std::isfinite(sphere.radius) || sphere.radius < 0.0)
        throw std::invalid_argument("contact sphere is invalid");
    if (!std::isfinite(radius) || radius <= 1.0e-14)
        throw std::domain_error("contact sphere axis has no unique normal");
    ContactGeometry geometry;
    geometry.valid = true;
    geometry.distance = radius - sphere.radius;
    geometry.gradient = offset / radius;
    geometry.hessian = (Mat3::Identity() - geometry.gradient * geometry.gradient.transpose()) / radius;
    geometry.surfaceVelocity = sphere.velocity;
    return geometry;
}

double Potential(const RestState& rest, const State& state,
    const StepInput& input, const ProgressiveSupport& profile)
{
    double result = EvaluateEnergy(rest, state).Total();
    if (input.supportEnabled)
        result += EvaluateProgressiveSupport(rest, state, input.root, profile).potential;
    for (size_t point = 0; point < rest.positions.size(); ++point)
        result -= rest.masses[point] * input.gravity.dot(state.positions[point]);
    return result;
}

bool SameSet(const ActiveNode& first, const ActiveNode& second)
{
    return first.identities == second.identities;
}

std::vector<ActiveNode> ChooseActive(const RestState& rest, const State& candidate,
    const StepInput& input, const std::vector<ContactConstraint>& constraints,
    const std::vector<ActiveNode>& previous, std::vector<std::vector<bool>>& excluded,
    unsigned& dependent)
{
    std::vector<ActiveNode> result(rest.positions.size());
    for (size_t point = 0; point < rest.positions.size(); ++point)
    {
        if (rest.inverseMasses[point] == 0.0)
            continue;
        struct Entry
        {
            double geometricGap;
            size_t identity;
            ContactGeometry geometry;
            bool retained;
        };
        std::vector<Entry> entries;
        for (size_t identity = 0; identity < constraints.size(); ++identity)
        {
            const auto geometry = constraints[identity].sample(candidate.positions[point]);
            ValidateGeometry(geometry);
            if (!geometry.valid)
                continue;
            const double gap = Gap(constraints[identity], geometry, input) / geometry.gradient.norm();
            if (excluded[point][identity] && gap >= -1.0e-9 * rest.totalLength)
                continue;
            bool retained = false;
            for (int slot = 0; slot < 3; ++slot)
                if (previous[point].identities[static_cast<size_t>(slot)] == static_cast<int>(identity) &&
                    previous[point].multipliers[slot] > 0.0 && !excluded[point][identity])
                    retained = true;
            if (gap <= 1.0e-9 * rest.totalLength || retained)
                entries.push_back({gap, identity, geometry, retained});
        }
        std::sort(entries.begin(), entries.end(), [](const Entry& first, const Entry& second)
        {
            if (first.retained != second.retained)
                return first.retained;
            if (first.retained)
                return first.identity < second.identity;
            return first.geometricGap == second.geometricGap ?
                first.identity < second.identity : first.geometricGap < second.geometricGap;
        });
        std::array<Vec3, 3> basis{Vec3::Zero(), Vec3::Zero(), Vec3::Zero()};
        size_t count = 0;
        for (const auto& entry : entries)
        {
            Vec3 normal = entry.geometry.gradient.normalized();
            for (size_t axis = 0; axis < count; ++axis)
                normal -= basis[axis] * basis[axis].dot(normal);
            if (normal.norm() <= 1.0e-10 || count == 3)
            {
                ++dependent;
                continue;
            }
            basis[count] = normal.normalized();
            result[point].identities[count] = static_cast<int>(entry.identity);
            if (entry.geometricGap < -1.0e-9 * rest.totalLength)
                excluded[point][entry.identity] = false;
            for (size_t old = 0; old < 3; ++old)
                if (previous[point].identities[old] == static_cast<int>(entry.identity))
                    result[point].multipliers[static_cast<int>(count)] =
                        previous[point].multipliers[static_cast<int>(old)];
            ++count;
        }
    }
    return result;
}

ContactSystem AssembleContact(const RestState& rest, const State& old, const State& candidate,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, const std::vector<ActiveNode>& active)
{
    const auto base = AssembleProgressiveImplicitSystem(rest, old, candidate, input, profile);
    ContactSystem system;
    system.forceScale = base.forceScale;
    system.lengthScale = base.lengthScale;
    const size_t count = rest.positions.size();
    system.lower.assign(count, Matrix9::Zero());
    system.diagonal.assign(count, Matrix9::Zero());
    system.upper.assign(count, Matrix9::Zero());
    system.residual.assign(count, Vector9::Zero());
    for (size_t point = 0; point < count; ++point)
    {
        system.lower[point].topLeftCorner<6, 6>() = base.lower[point];
        system.diagonal[point].topLeftCorner<6, 6>() = base.diagonal[point];
        system.upper[point].topLeftCorner<6, 6>() = base.upper[point];
        system.residual[point].head<6>() = base.residual[point];
        for (int slot = 0; slot < 3; ++slot)
        {
            const int identity = active[point].identities[static_cast<size_t>(slot)];
            if (identity < 0)
            {
                system.diagonal[point](6 + slot, 6 + slot) = 1.0;
                continue;
            }
            const auto geometry = constraints[static_cast<size_t>(identity)].sample(candidate.positions[point]);
            ValidateGeometry(geometry);
            if (!geometry.valid)
                throw std::domain_error("active contact geometry became unavailable");
            const double multiplier = active[point].multipliers[slot];
            system.residual[point].head<3>() -= multiplier * geometry.gradient / system.forceScale;
            system.residual[point][6 + slot] =
                Gap(constraints[static_cast<size_t>(identity)], geometry, input) / system.lengthScale;
            system.diagonal[point].topLeftCorner<3, 3>() -=
                multiplier * geometry.hessian * (system.lengthScale / system.forceScale);
            system.diagonal[point].block<3, 1>(0, 6 + slot) = -geometry.gradient;
            system.diagonal[point].block<1, 3>(6 + slot, 0) = geometry.gradient.transpose();
        }
    }
    return system;
}

std::vector<Vector9> SolveContact(const ContactSystem& system)
{
    const size_t count = system.diagonal.size();
    std::vector<Matrix9> upper(count, Matrix9::Zero());
    std::vector<Vector9> right(count, Vector9::Zero());
    for (size_t point = 0; point < count; ++point)
    {
        Matrix9 diagonal = system.diagonal[point];
        Vector9 value = -system.residual[point];
        if (point > 0)
        {
            diagonal -= system.lower[point] * upper[point - 1];
            value -= system.lower[point] * right[point - 1];
        }
        const Eigen::FullPivLU<Matrix9> factorization(diagonal);
        if (!factorization.isInvertible())
            throw std::runtime_error("contact KKT pivot lost rank");
        upper[point] = factorization.solve(system.upper[point]);
        right[point] = factorization.solve(value);
    }
    for (size_t point = count - 1; point > 0; --point)
        right[point - 1] -= upper[point - 1] * right[point];
    return right;
}

std::pair<double, double> ContactMerit(const ContactSystem& system)
{
    double squared = 0.0;
    double maximum = 0.0;
    for (const auto& row : system.residual)
    {
        if (!row.allFinite())
            return {std::numeric_limits<double>::infinity(), std::numeric_limits<double>::infinity()};
        squared += row.squaredNorm();
        maximum = std::max(maximum, row.cwiseAbs().maxCoeff());
    }
    return {squared, maximum};
}
}

std::vector<ContactActiveNode> SelectContactActiveSet(const RestState& rest, const State& candidate,
    const StepInput& input, const std::vector<ContactConstraint>& constraints,
    const std::vector<ContactActiveNode>& previous, std::vector<std::vector<bool>>& excluded,
    unsigned& dependent)
{
    return ChooseActive(rest, candidate, input, constraints, previous, excluded, dependent);
}

ContactConstraint MakeSphereContact(const std::string& identity, const CollisionSphere& sphere)
{
    return {identity, [sphere](const Vec3& position) { return SphereGeometry(position, sphere); }};
}

ContactConstraint MakeCapsuleContact(const std::string& identity, const CollisionCapsule& capsule)
{
    return {identity, [capsule](const Vec3& position)
    {
        const Vec3 edge = capsule.b - capsule.a;
        const double length = edge.norm();
        if (!capsule.a.allFinite() || !capsule.b.allFinite() || !capsule.velocityA.allFinite() ||
            !capsule.velocityB.allFinite() || !std::isfinite(capsule.radius) || capsule.radius < 0.0)
            throw std::invalid_argument("contact capsule is invalid");
        if (length <= 1.0e-14)
            return SphereGeometry(position, {capsule.a, capsule.radius, capsule.velocityA});
        const Vec3 direction = edge / length;
        const double fraction = std::clamp((position - capsule.a).dot(direction) / length, 0.0, 1.0);
        const Vec3 center = capsule.a + fraction * edge;
        auto geometry = SphereGeometry(position, {center, capsule.radius,
            (1.0 - fraction) * capsule.velocityA + fraction * capsule.velocityB});
        if (fraction > 0.0 && fraction < 1.0)
        {
            const double radius = (position - center).norm();
            geometry.hessian -= direction * direction.transpose() / radius;
            geometry.feature = 1;
        }
        else
            geometry.feature = fraction == 0.0 ? 2 : 3;
        return geometry;
    }};
}

ContactConstraint MakeRawSdfContact(const std::string& identity,
    const CollisionSdfQuery& query, double differenceStep, double distanceScale)
{
    if (!query || !std::isfinite(differenceStep) || differenceStep <= 0.0 ||
        !std::isfinite(distanceScale) || distanceScale <= 0.0)
        throw std::invalid_argument("raw SDF contact requires query and positive difference step");
    return {identity, [query, differenceStep](const Vec3& position)
    {
        const auto raw = query(position);
        ContactGeometry geometry;
        geometry.valid = raw.valid;
        if (!raw.valid)
            return geometry;
        geometry.distance = raw.distance;
        geometry.gradient = raw.gradient;
        geometry.surfaceVelocity = raw.colliderVelocity;
        for (int axis = 0; axis < 3; ++axis)
        {
            const auto positive = query(position + differenceStep * Vec3::Unit(axis));
            const auto negative = query(position - differenceStep * Vec3::Unit(axis));
            if (!positive.valid || !negative.valid)
                throw std::domain_error("raw SDF Hessian crosses unavailable domain");
            geometry.hessian.col(axis) = (positive.gradient - negative.gradient) / (2.0 * differenceStep);
        }
        ValidateGeometry(geometry);
        return geometry;
    }, distanceScale};
}

ContactRepairDiagnostics RepairInitialContactOverlap(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints)
{
    ContactRepairDiagnostics diagnostics;
    if (!ValidateImplicitStepInput(rest, state, input) ||
        profile.cubicStiffness.size() != rest.positions.size() ||
        !std::isfinite(input.contactMargin) || input.contactMargin < 0.0)
    {
        diagnostics.status = Status::InvalidInput;
        return diagnostics;
    }
    for (const auto& constraint : constraints)
        if (constraint.identity.empty() || !constraint.sample ||
            !std::isfinite(constraint.distanceScale) || constraint.distanceScale <= 0.0)
        {
            diagnostics.status = Status::InvalidInput;
            return diagnostics;
        }
    State candidate = state;
    diagnostics.corrections.assign(state.positions.size(), Vec3::Zero());
    try
    {
        const double before = Potential(rest, candidate, input, profile);
        for (unsigned iteration = 0; iteration < 24; ++iteration)
        {
            double maximum = 0.0;
            for (size_t point = 0; point < state.positions.size(); ++point)
                for (const auto& constraint : constraints)
                {
                    if (rest.inverseMasses[point] == 0.0)
                        continue;
                    const auto geometry = constraint.sample(candidate.positions[point]);
                    ValidateGeometry(geometry);
                    if (!geometry.valid)
                        continue;
                    const double gap = Gap(constraint, geometry, input);
                    maximum = std::max(maximum, -gap / geometry.gradient.norm());
                    if (gap >= 0.0)
                        continue;
                    const Vec3 correction = -gap * geometry.gradient / geometry.gradient.squaredNorm();
                    candidate.positions[point] += correction;
                    diagnostics.corrections[point] += correction;
                }
            ++diagnostics.iterations;
            if (maximum <= 1.0e-9 * rest.totalLength)
            {
                diagnostics.potentialEnergyChange = Potential(rest, candidate, input, profile) - before;
                state = std::move(candidate);
                return diagnostics;
            }
        }
        diagnostics.status = Status::PredictorFailure;
    }
    catch (const std::domain_error&)
    {
        diagnostics.status = Status::DomainFailure;
    }
    catch (const std::invalid_argument&)
    {
        diagnostics.status = Status::InvalidInput;
    }
    return diagnostics;
}

static ContactImplicitDiagnostics StepContactImpl(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, ContactTrace* trace)
{
    ContactImplicitDiagnostics diagnostics;
    if (!ValidateImplicitStepInput(rest, state, input) ||
        profile.cubicStiffness.size() != rest.positions.size() ||
        !input.collisionSpheres.empty() || !input.collisionCapsules.empty() ||
        !input.collisionFields.empty() || !std::isfinite(input.contactMargin) ||
        input.contactMargin < 0.0 || rest.positions.size() != state.positions.size() ||
        state.positions.size() != state.velocities.size() ||
        rest.lengths.size() != state.orientations.size() ||
        state.orientations.size() != state.angularVelocities.size())
    {
        diagnostics.status = Status::InvalidInput;
        return diagnostics;
    }
    for (const auto& constraint : constraints)
        if (constraint.identity.empty() || !constraint.sample ||
            !std::isfinite(constraint.distanceScale) || constraint.distanceScale <= 0.0)
        {
            diagnostics.status = Status::InvalidInput;
            return diagnostics;
        }
    if (constraints.empty())
    {
        static_cast<ImplicitDiagnostics&>(diagnostics) =
            StepImplicitMomentumProgressive(rest, state, input, profile);
        return diagnostics;
    }
    const State old = state;
    State candidate = state;
    std::vector<ActiveNode> active(rest.positions.size());
    struct ContactTraceFinalizer
    {
        ContactTrace* trace;
        State* candidate;
        ContactImplicitDiagnostics* diagnostics;
        std::vector<ActiveNode>* active;
        ~ContactTraceFinalizer()
        {
            if (trace)
            {
                if (!candidate->positions.empty())
                    trace->finalCandidate = *candidate;
                trace->finalActive = *active;
                trace->failureReason = diagnostics->failureReason;
            }
        }
    };
    const ContactTraceFinalizer traceFinalizer{trace, &candidate, &diagnostics, &active};
    std::vector<std::vector<bool>> excluded(rest.positions.size(),
        std::vector<bool>(constraints.size(), false));
    auto recordFailure = [&]()
    {
        diagnostics.contacts.clear();
        for (size_t point = 0; point < candidate.positions.size(); ++point)
            for (size_t identity = 0; identity < constraints.size(); ++identity)
            {
                ContactRecord record;
                record.point = point;
                record.identity = constraints[identity].identity;
                record.pinnedExcluded = rest.inverseMasses[point] == 0.0;
                try
                {
                    const auto geometry = constraints[identity].sample(candidate.positions[point]);
                    ValidateGeometry(geometry);
                    record.valid = geometry.valid;
                    if (geometry.valid)
                    {
                        record.rawGap = Gap(constraints[identity], geometry, input);
                        record.geometricGap = record.rawGap / geometry.gradient.norm();
                        record.gradient = geometry.gradient;
                        record.surfaceVelocity = geometry.surfaceVelocity;
                        for (int slot = 0; slot < 3; ++slot)
                            if (active[point].identities[static_cast<size_t>(slot)] == static_cast<int>(identity))
                            {
                                record.active = true;
                                record.reactionMultiplier = active[point].multipliers[slot];
                            }
                        record.force = record.reactionMultiplier * record.gradient;
                    }
                }
                catch (const std::exception&)
                {
                    record.valid = false;
                    record.rawGap = std::numeric_limits<double>::quiet_NaN();
                    record.geometricGap = std::numeric_limits<double>::quiet_NaN();
                }
                diagnostics.contacts.push_back(record);
            }
    };
    for (size_t point = 0; point < rest.positions.size(); ++point)
        if (rest.inverseMasses[point] == 0.0)
            candidate.positions[point] = SampleRootSupport(input.root, rest.rootLocalPositions[point]).position;
    candidate.orientations.front() = input.root.orientation;
    try
    {
        for (unsigned iteration = 0; iteration < 24; ++iteration)
        {
            const unsigned previousChanges = diagnostics.activeSetChanges;
            const unsigned previousDependent = diagnostics.dependentNormals;
            const auto previousActive = trace ? active : std::vector<ActiveNode>{};
            const auto selected = ChooseActive(rest, candidate, input, constraints, active,
                excluded, diagnostics.dependentNormals);
            for (size_t point = 0; point < active.size(); ++point)
                if (!SameSet(active[point], selected[point]))
                    ++diagnostics.activeSetChanges;
            active = selected;
            const auto system = AssembleContact(rest, old, candidate, input, profile, constraints, active);
            const auto merit = ContactMerit(system);
            diagnostics.normalizedResidual = merit.second;
            if (trace)
            {
                trace->iterations.emplace_back();
                auto& record = trace->iterations.back();
                record.candidate = candidate;
                record.previousActive = previousActive;
                record.active = active;
                record.excluded = excluded;
                record.system = system;
                record.normalizedResidual = merit.second;
                record.changedNodes = diagnostics.activeSetChanges - previousChanges;
                record.dependentNormals = diagnostics.dependentNormals - previousDependent;
            }
            if (!std::isfinite(merit.first))
            {
                diagnostics.status = Status::NonFinite;
                diagnostics.failureReason = "contact-nonfinite-residual";
                recordFailure();
                return diagnostics;
            }
            bool feasible = true;
            for (size_t point = 0; point < active.size(); ++point)
            {
                if (rest.inverseMasses[point] == 0.0)
                    continue;
                for (const auto& constraint : constraints)
                {
                    const auto geometry = constraint.sample(candidate.positions[point]);
                    ValidateGeometry(geometry);
                    if (geometry.valid && Gap(constraint, geometry, input) /
                        geometry.gradient.norm() < -1.0e-9 * rest.totalLength)
                        feasible = false;
                }
                for (int slot = 0; slot < 3; ++slot)
                    if (active[point].identities[static_cast<size_t>(slot)] >= 0)
                    {
                        const auto identity = static_cast<size_t>(active[point].identities[static_cast<size_t>(slot)]);
                        const auto geometry = constraints[identity].sample(candidate.positions[point]);
                        ValidateGeometry(geometry);
                        if (!geometry.valid || std::abs(active[point].multipliers[slot] *
                            Gap(constraints[identity], geometry, input)) /
                            (system.forceScale * system.lengthScale) > 1.0e-9)
                            feasible = false;
                        if (active[point].multipliers[slot] < -1.0e-9 * system.forceScale)
                        {
                            excluded[point][identity] = true;
                            feasible = false;
                        }
                    }
            }
            if (merit.second <= 1.0e-8 && feasible)
            {
                diagnostics.converged = true;
                break;
            }
            const auto direction = SolveContact(system);
            if (trace)
                trace->iterations.back().direction = direction;
            for (size_t point = 0; point < direction.size(); ++point)
            {
                Vector9 residual = system.diagonal[point] * direction[point] + system.residual[point];
                if (point > 0)
                    residual += system.lower[point] * direction[point - 1];
                if (point + 1 < direction.size())
                    residual += system.upper[point] * direction[point + 1];
                diagnostics.linearSolveResidual = std::max(diagnostics.linearSolveResidual,
                    residual.cwiseAbs().maxCoeff());
            }
            bool accepted = false;
            double alpha = 1.0;
            for (unsigned trialIndex = 0; trialIndex <= 20; ++trialIndex)
            {
                try
                {
                    State trial = candidate;
                    auto trialActive = active;
                    for (size_t point = 0; point < direction.size(); ++point)
                    {
                        trial.positions[point] += alpha * system.lengthScale * direction[point].head<3>();
                        if (point < rest.lengths.size())
                            trial.orientations[point] = ApplyBodyIncrement(trial.orientations[point],
                                alpha * direction[point].segment<3>(3));
                        trialActive[point].multipliers += alpha * system.forceScale * direction[point].tail<3>();
                    }
                    const auto trialSystem = AssembleContact(rest, old, trial, input, profile, constraints, trialActive);
                    const double trialMerit = ContactMerit(trialSystem).first;
                    if (trace)
                        trace->iterations.back().trials.push_back(
                            {alpha, trialMerit, ContactMerit(trialSystem).second, false, false});
                    if (std::isfinite(trialMerit) &&
                        trialMerit <= (1.0 - 1.0e-4 * alpha) * merit.first)
                    {
                        candidate = std::move(trial);
                        active = std::move(trialActive);
                        if (trace)
                            trace->iterations.back().trials.back().accepted = true;
                        accepted = true;
                        break;
                    }
                }
                catch (const std::domain_error&)
                {
                    if (trace)
                        trace->iterations.back().trials.push_back(
                            {alpha, std::numeric_limits<double>::infinity(),
                                std::numeric_limits<double>::infinity(), true, false});
                }
                alpha *= 0.5;
                ++diagnostics.backtracks;
            }
            ++diagnostics.newtonIterations;
            if (!accepted)
            {
                diagnostics.status = Status::PredictorFailure;
                diagnostics.failureReason = "contact-armijo-or-active-set";
                recordFailure();
                return diagnostics;
            }
        }
        if (!diagnostics.converged)
        {
            diagnostics.status = Status::PredictorFailure;
            diagnostics.failureReason = "contact-budget-rank-or-infeasible";
            recordFailure();
            return diagnostics;
        }
        for (size_t point = 0; point < state.positions.size(); ++point)
            candidate.velocities[point] = (candidate.positions[point] - old.positions[point]) / input.stepSeconds;
        for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
            candidate.angularVelocities[segment] =
                Log(old.orientations[segment].conjugate() * candidate.orientations[segment]) / input.stepSeconds;
        auto physical = EvaluateProgressiveImplicitResidual(rest, old, candidate, input, profile);
        const auto scaled = AssembleProgressiveImplicitSystem(rest, old, candidate, input, profile);
        for (size_t point = 0; point < state.positions.size(); ++point)
        {
            for (size_t identity = 0; identity < constraints.size(); ++identity)
            {
                const auto geometry = constraints[identity].sample(candidate.positions[point]);
                ValidateGeometry(geometry);
                if (!geometry.valid)
                    continue;
                ContactRecord record;
                record.point = point;
                record.identity = constraints[identity].identity;
                record.pinnedExcluded = rest.inverseMasses[point] == 0.0;
                record.rawGap = Gap(constraints[identity], geometry, input);
                record.geometricGap = record.rawGap / geometry.gradient.norm();
                record.gradient = geometry.gradient;
                record.surfaceVelocity = geometry.surfaceVelocity;
                for (int slot = 0; slot < 3; ++slot)
                    if (active[point].identities[static_cast<size_t>(slot)] == static_cast<int>(identity))
                    {
                        record.active = true;
                        record.reactionMultiplier = active[point].multipliers[slot];
                    }
                record.force = record.reactionMultiplier * record.gradient;
                record.complementarity = std::abs(record.reactionMultiplier * record.rawGap) /
                    (scaled.forceScale * scaled.lengthScale);
                if (!record.pinnedExcluded)
                {
                    physical[point].head<3>() -= record.force;
                    diagnostics.maximumGeometricPenetration =
                        std::max(diagnostics.maximumGeometricPenetration, -record.geometricGap);
                    diagnostics.maximumComplementarity =
                        std::max(diagnostics.maximumComplementarity, record.complementarity);
                    diagnostics.contactPower += record.force.dot(record.surfaceVelocity);
                }
                diagnostics.contacts.push_back(record);
            }
            if (rest.inverseMasses[point] == 0.0)
            {
                diagnostics.rootForce += physical[point].head<3>();
                diagnostics.rootBodyTorque += input.root.orientation.conjugate() *
                    ((candidate.positions[point] - input.root.position).cross(physical[point].head<3>()));
            }
            else
                diagnostics.maximumLinearResidual = std::max(diagnostics.maximumLinearResidual,
                    physical[point].head<3>().norm());
            diagnostics.linearKineticEnergy +=
                0.5 * rest.masses[point] * candidate.velocities[point].squaredNorm();
        }
        for (size_t segment = 0; segment < rest.lengths.size(); ++segment)
        {
            if (segment == 0)
                diagnostics.rootBodyTorque += physical[segment].tail<3>();
            else
                diagnostics.maximumAngularResidual =
                    std::max(diagnostics.maximumAngularResidual, physical[segment].tail<3>().norm());
            diagnostics.angularKineticEnergy += 0.5 * candidate.angularVelocities[segment].dot(
                rest.inertias[segment].cwiseProduct(candidate.angularVelocities[segment]));
        }
        const auto support = input.supportEnabled ?
            EvaluateProgressiveSupport(rest, candidate, input.root, profile) : SupportEvaluation{};
        diagnostics.energy = Potential(rest, candidate, input, profile);
        diagnostics.supportDrivePower = support.drivePower;
        diagnostics.supportDissipation = support.dissipation;
        diagnostics.maximumShearDefect = std::numeric_limits<double>::quiet_NaN();
        diagnostics.maximumBendDefect = std::numeric_limits<double>::quiet_NaN();
        diagnostics.maximumSupportDefect = std::numeric_limits<double>::quiet_NaN();
        bool finiteState = std::isfinite(diagnostics.energy) &&
            std::isfinite(diagnostics.linearKineticEnergy) &&
            std::isfinite(diagnostics.angularKineticEnergy) &&
            std::isfinite(diagnostics.supportDrivePower) &&
            std::isfinite(diagnostics.supportDissipation) &&
            std::isfinite(diagnostics.contactPower) &&
            std::isfinite(diagnostics.maximumLinearResidual) &&
            std::isfinite(diagnostics.maximumAngularResidual);
        for (size_t point = 0; point < candidate.positions.size(); ++point)
            finiteState = finiteState && candidate.positions[point].allFinite() &&
                candidate.velocities[point].allFinite();
        for (size_t segment = 0; segment < candidate.orientations.size(); ++segment)
        {
            finiteState = finiteState && candidate.orientations[segment].coeffs().allFinite() &&
                std::abs(candidate.orientations[segment].norm() - 1.0) <= 1.0e-8 &&
                candidate.angularVelocities[segment].allFinite();
            Log(old.orientations[segment].conjugate() * candidate.orientations[segment]);
            if (segment + 1 < candidate.orientations.size())
                Log(candidate.orientations[segment].conjugate() * candidate.orientations[segment + 1]);
        }
        if (!finiteState || diagnostics.maximumComplementarity > 1.0e-9 ||
            diagnostics.maximumGeometricPenetration > 1.0e-9 * rest.totalLength)
        {
            diagnostics.status = finiteState ? Status::PredictorFailure : Status::NonFinite;
            diagnostics.failureReason = finiteState ? "contact-final-complementarity-or-gap" :
                "contact-final-state-nonfinite";
            diagnostics.converged = false;
            recordFailure();
            return diagnostics;
        }
        if (trace)
            trace->finalCandidate = candidate;
        state = std::move(candidate);
    }
    catch (const std::domain_error&)
    {
        diagnostics.status = Status::DomainFailure;
        diagnostics.converged = false;
        diagnostics.failureReason = "contact-geometry-domain";
    }
    catch (const std::invalid_argument&)
    {
        diagnostics.status = Status::InvalidInput;
        diagnostics.converged = false;
        diagnostics.failureReason = "contact-input";
    }
    catch (const std::runtime_error&)
    {
        diagnostics.status = Status::LinearSolveFailure;
        diagnostics.converged = false;
        diagnostics.failureReason = "contact-KKT-rank";
    }
    if (diagnostics.status != Status::Success)
        recordFailure();
    return diagnostics;
}

ContactImplicitDiagnostics StepImplicitMomentumContact(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints)
{
    return StepContactImpl(rest, state, input, profile, constraints, nullptr);
}

ContactImplicitDiagnostics StepImplicitMomentumContactWithTrace(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, ContactTrace& trace)
{
    trace = ContactTrace{};
    trace.old = state;
    trace.finalCandidate = state;
    auto result = StepContactImpl(rest, state, input, profile, constraints, &trace);
    trace.failureReason = result.failureReason;
    return result;
}

ContactSystem AssembleContactSystem(const RestState& rest, const State& old, const State& candidate,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, const std::vector<ContactActiveNode>& active)
{
    if (!ValidateImplicitStepInput(rest, old, input) ||
        active.size() != rest.positions.size() ||
        candidate.positions.size() != old.positions.size() ||
        candidate.orientations.size() != old.orientations.size())
        throw std::invalid_argument("contact oracle dimensions are invalid");
    for (const auto& node : active)
        for (int slot = 0; slot < 3; ++slot)
            if (node.identities[static_cast<size_t>(slot)] < -1 ||
                node.identities[static_cast<size_t>(slot)] >= static_cast<int>(constraints.size()) ||
                !std::isfinite(node.multipliers[slot]))
                throw std::invalid_argument("contact oracle active identity is invalid");
    return AssembleContact(rest, old, candidate, input, profile, constraints, active);
}

std::vector<ContactVector9> SolveContactSystem(const ContactSystem& system)
{
    if (system.diagonal.empty() || system.lower.size() != system.diagonal.size() ||
        system.upper.size() != system.diagonal.size() || system.residual.size() != system.diagonal.size())
        throw std::invalid_argument("contact system block counts are invalid");
    return SolveContact(system);
}
}
