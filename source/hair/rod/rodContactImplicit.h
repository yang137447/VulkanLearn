#pragma once

#include <functional>
#include <string>

#include "hair/rod/rodImplicit.h"

namespace VL::Hair::Rod
{
struct ContactGeometry
{
    bool valid = false;
    double distance = 0.0;
    Vec3 gradient = Vec3::Zero();
    Mat3 hessian = Mat3::Zero();
    Vec3 surfaceVelocity = Vec3::Zero();
    int feature = 0;
};

struct ContactConstraint
{
    std::string identity;
    std::function<ContactGeometry(const Vec3&)> sample;
    double distanceScale = 1.0;
};

struct ContactRecord
{
    size_t point = 0;
    std::string identity;
    bool active = false;
    bool pinnedExcluded = false;
    bool valid = true;
    double rawGap = 0.0;
    double geometricGap = 0.0;
    double reactionMultiplier = 0.0;
    double complementarity = 0.0;
    Vec3 gradient = Vec3::Zero();
    Vec3 surfaceVelocity = Vec3::Zero();
    Vec3 force = Vec3::Zero();
};

struct ContactImplicitDiagnostics : ImplicitDiagnostics
{
    std::vector<ContactRecord> contacts;
    double maximumGeometricPenetration = 0.0;
    double maximumComplementarity = 0.0;
    double contactPower = 0.0;
    unsigned activeSetChanges = 0;
    unsigned dependentNormals = 0;
    std::string failureReason;
};

struct ContactRepairDiagnostics
{
    Status status = Status::Success;
    std::vector<Vec3> corrections;
    double potentialEnergyChange = 0.0;
    unsigned iterations = 0;
};

using ContactVector9 = Eigen::Matrix<double, 9, 1>;
using ContactMatrix9 = Eigen::Matrix<double, 9, 9>;

struct ContactActiveNode
{
    std::array<int, 3> identities{-1, -1, -1};
    Vec3 multipliers = Vec3::Zero();
};

struct ContactSystem
{
    std::vector<ContactMatrix9> lower;
    std::vector<ContactMatrix9> diagonal;
    std::vector<ContactMatrix9> upper;
    std::vector<ContactVector9> residual;
    double forceScale = 0.0;
    double lengthScale = 0.0;
};

struct ContactIterationTrace
{
    State candidate;
    std::vector<ContactActiveNode> previousActive;
    std::vector<ContactActiveNode> active;
    std::vector<std::vector<bool>> excluded;
    ContactSystem system;
    std::vector<ContactVector9> direction;
    std::vector<ImplicitTrialTrace> trials;
    unsigned changedNodes = 0;
    unsigned dependentNormals = 0;
    double normalizedResidual = 0.0;
};

struct ContactTrace
{
    State old;
    State finalCandidate;
    std::vector<ContactActiveNode> finalActive;
    std::vector<ContactIterationTrace> iterations;
    std::string failureReason;
};

std::vector<ContactActiveNode> SelectContactActiveSet(const RestState& rest, const State& candidate,
    const StepInput& input, const std::vector<ContactConstraint>& constraints,
    const std::vector<ContactActiveNode>& previous, std::vector<std::vector<bool>>& excluded,
    unsigned& dependent);

ContactSystem AssembleContactSystem(const RestState& rest, const State& old, const State& candidate,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, const std::vector<ContactActiveNode>& active);
std::vector<ContactVector9> SolveContactSystem(const ContactSystem& system);

ContactConstraint MakeSphereContact(const std::string& identity, const CollisionSphere& sphere);
ContactConstraint MakeCapsuleContact(const std::string& identity, const CollisionCapsule& capsule);
ContactConstraint MakeRawSdfContact(const std::string& identity,
    const CollisionSdfQuery& query, double differenceStep, double distanceScale = 1.0);
ContactRepairDiagnostics RepairInitialContactOverlap(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints);
ContactImplicitDiagnostics StepImplicitMomentumContact(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints);
ContactImplicitDiagnostics StepImplicitMomentumContactWithTrace(const RestState& rest, State& state,
    const StepInput& input, const ProgressiveSupport& profile,
    const std::vector<ContactConstraint>& constraints, ContactTrace& trace);
}
