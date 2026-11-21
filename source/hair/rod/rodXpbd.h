#pragma once

#include "hair/rod/rodTypes.h"
#include "hair/rod/rodCollision.h"

namespace VL::Hair::Rod
{
struct StepInput
{
    double stepSeconds = 1.0 / 240.0;
    unsigned sweeps = 16;
    Vec3 gravity = Vec3(0.0, -9.8, 0.0);
    RootFrame root;
    bool supportEnabled = true;
    std::vector<Vec3> externalForces;
    std::vector<Vec3> externalTorquesWorld;
    std::vector<CollisionSphere> collisionSpheres;
    std::vector<CollisionCapsule> collisionCapsules;
    std::vector<CollisionSdfQuery> collisionFields;
    double contactMargin = 0.0;
    double contactCompliance = 0.0;
};

struct StepDiagnostics
{
    Status status = Status::Success;
    bool converged = false;
    double energy = 0.0;
    double linearKineticEnergy = 0.0;
    double angularKineticEnergy = 0.0;
    double maximumShearDefect = 0.0;
    double maximumBendDefect = 0.0;
    double maximumSupportDefect = 0.0;
    double maximumForce = 0.0;
    double maximumBodyTorque = 0.0;
    double maximumLinearResidual = 0.0;
    double maximumAngularResidual = 0.0;
    double predictorResidual = 0.0;
    double supportDrivePower = 0.0;
    double supportDissipation = 0.0;
    double maximumPenetration = 0.0;
    double maximumContactCorrection = 0.0;
    Vec3 rootForce = Vec3::Zero();
    Vec3 rootBodyTorque = Vec3::Zero();
    unsigned completedSweeps = 0;
    std::vector<Vec3> shearMultipliers;
    std::vector<Vec3> bendMultipliers;
    std::vector<Vec3> supportMultipliers;
};

StepDiagnostics Step(const RestState& rest, State& state, const StepInput& input);
}
