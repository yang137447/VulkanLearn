#pragma once

#include <vector>

#include <Eigen/Core>
#include <Eigen/Geometry>

namespace VL::Hair::Rod
{
using Vec3 = Eigen::Vector3d;
using Mat3 = Eigen::Matrix3d;
using Quat = Eigen::Quaterniond;

inline constexpr double Pi = 3.14159265358979323846;
inline constexpr double RotationLimit = Pi - 1.0e-4;

enum class Status
{
    Success = 0,
    InvalidInput,
    DomainFailure,
    PredictorFailure,
    LinearSolveFailure,
    NonFinite
};

struct Material
{
    double radius = 0.0005;
    double density = 1300.0;
    double youngModulus = 1.0e6;
    double shearModulus = 0.4e6;
    double supportOffsetRatio = 0.01;
    double supportDampingRatio = 0.5;
    double referenceGravity = 9.8;
};

struct RestState
{
    std::vector<Vec3> positions;
    std::vector<Quat> orientations;
    std::vector<double> lengths;
    std::vector<Vec3> restStrains;
    std::vector<Vec3> restJointAngles;
    std::vector<Vec3> stretchShear;
    std::vector<Vec3> bendTwist;
    std::vector<double> masses;
    std::vector<double> inverseMasses;
    std::vector<Vec3> inertias;
    std::vector<Vec3> inverseInertias;
    std::vector<Vec3> rootLocalPositions;
    std::vector<double> supportStiffness;
    std::vector<double> supportDamping;
    double totalLength = 0.0;
};

struct State
{
    std::vector<Vec3> positions;
    std::vector<Vec3> velocities;
    std::vector<Quat> orientations;
    std::vector<Vec3> angularVelocities;
};

struct RootFrame
{
    Vec3 position = Vec3::Zero();
    Quat orientation = Quat::Identity();
    Vec3 linearVelocity = Vec3::Zero();
    Vec3 angularVelocityWorld = Vec3::Zero();
};

struct SupportSample
{
    Vec3 position = Vec3::Zero();
    Vec3 velocity = Vec3::Zero();
};

struct EnergyEvaluation
{
    double stretchShear = 0.0;
    double bendTwist = 0.0;
    std::vector<Vec3> forces;
    std::vector<Vec3> bodyTorques;
    double Total() const { return stretchShear + bendTwist; }
};

struct SupportEvaluation
{
    double potential = 0.0;
    double drivePower = 0.0;
    double dissipation = 0.0;
    std::vector<Vec3> forces;
};
}
