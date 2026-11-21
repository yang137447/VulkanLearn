#include "hair/rod/rodMath.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include <Eigen/LU>

namespace VL::Hair::Rod
{
Mat3 CrossMatrix(const Vec3& value)
{
    Mat3 result;
    result << 0.0, -value.z(), value.y(),
        value.z(), 0.0, -value.x(),
        -value.y(), value.x(), 0.0;
    return result;
}

Quat Exp(const Vec3& angle)
{
    const double magnitude = angle.norm();
    if (!angle.allFinite() || magnitude >= RotationLimit)
        throw std::domain_error("rod rotation increment exceeds principal chart");
    const double squared = magnitude * magnitude;
    const double factor = magnitude < 1.0e-6
        ? 0.5 - squared / 48.0 + squared * squared / 3840.0
        : std::sin(0.5 * magnitude) / magnitude;
    return Quat(std::cos(0.5 * magnitude),
        factor * angle.x(), factor * angle.y(), factor * angle.z());
}

Vec3 Log(const Quat& orientation)
{
    if (!orientation.coeffs().allFinite() || orientation.norm() < 1.0e-15)
        throw std::domain_error("rod orientation is invalid");
    Quat normalized = orientation.normalized();
    if (normalized.w() < 0.0)
        normalized.coeffs() *= -1.0;
    const double sine = normalized.vec().norm();
    const double angle = 2.0 * std::atan2(sine, normalized.w());
    if (angle >= RotationLimit)
        throw std::domain_error("rod relative rotation exceeds principal chart");
    if (sine < 1.0e-10)
        return 2.0 * normalized.vec();
    return normalized.vec() * (angle / sine);
}

Mat3 RightJacobianInverse(const Vec3& angle)
{
    const double squared = angle.squaredNorm();
    if (!angle.allFinite() || squared >= RotationLimit * RotationLimit)
        throw std::domain_error("rod log Jacobian exceeds principal chart");
    const double magnitude = std::sqrt(squared);
    const double factor = magnitude < 1.0e-4
        ? 1.0 / 12.0 + squared / 720.0 + squared * squared / 30240.0
        : (1.0 - 0.5 * magnitude / std::tan(0.5 * magnitude)) / squared;
    const Mat3 cross = CrossMatrix(angle);
    return Mat3::Identity() + 0.5 * cross + factor * cross * cross;
}

Quat ApplyBodyIncrement(const Quat& orientation, const Vec3& angle)
{
    return (orientation * Exp(angle)).normalized();
}

Vec3 PredictBodyAngularVelocity(const Vec3& inertia, const Vec3& oldVelocity,
    const Vec3& bodyTorque, double stepSeconds, double& normalizedResidual)
{
    if (!inertia.allFinite() || (inertia.array() <= 0.0).any() ||
        !oldVelocity.allFinite() || !bodyTorque.allFinite() ||
        !std::isfinite(stepSeconds) || stepSeconds <= 0.0)
        throw std::invalid_argument("rod angular predictor requires positive inertia and step");
    Vec3 predicted = oldVelocity;
    const Mat3 inertiaMatrix = inertia.asDiagonal();
    const double scale = std::max({(inertiaMatrix * oldVelocity).norm() / stepSeconds,
        bodyTorque.norm(), inertia.minCoeff() / (stepSeconds * stepSeconds)});
    for (int iteration = 0; iteration <= 12; ++iteration)
    {
        const Vec3 momentum = inertiaMatrix * predicted;
        const Vec3 residual = inertiaMatrix * (predicted - oldVelocity) / stepSeconds +
            predicted.cross(momentum) - bodyTorque;
        normalizedResidual = residual.norm() / scale;
        if (normalizedResidual <= 1.0e-10)
            return predicted;
        if (iteration == 12)
            break;
        const Mat3 derivative = inertiaMatrix / stepSeconds -
            CrossMatrix(momentum) + CrossMatrix(predicted) * inertiaMatrix;
        const Eigen::FullPivLU<Mat3> factorization(derivative);
        if (!factorization.isInvertible())
            throw std::runtime_error("rod angular predictor linear solve failed");
        predicted -= factorization.solve(residual);
        if (!predicted.allFinite())
            break;
    }
    throw std::runtime_error("rod angular predictor did not converge");
}

RestState BuildRestState(const std::vector<Vec3>& positions,
    const Material& material, const std::vector<Quat>& orientations,
    const std::vector<double>& inverseMassMarkers)
{
    const double values[] = {material.radius, material.density, material.youngModulus,
        material.shearModulus, material.supportOffsetRatio, material.referenceGravity};
    for (double value : values)
        if (!std::isfinite(value) || value <= 0.0)
            throw std::invalid_argument("rod material requires positive finite values");
    if (!std::isfinite(material.supportDampingRatio) || material.supportDampingRatio < 0.0 ||
        positions.size() < 2 || (!orientations.empty() && orientations.size() + 1 != positions.size()) ||
        (!inverseMassMarkers.empty() && inverseMassMarkers.size() != positions.size()))
        throw std::invalid_argument("rod rest dimensions or damping are invalid");
    for (const Vec3& position : positions)
        if (!position.allFinite())
            throw std::invalid_argument("rod rest positions must be finite");
    for (double marker : inverseMassMarkers)
        if (!std::isfinite(marker) || marker < 0.0)
            throw std::invalid_argument("rod inverse mass markers must be finite and nonnegative");
    RestState rest;
    rest.positions = positions;
    rest.masses.assign(positions.size(), 0.0);
    const double area = Pi * material.radius * material.radius;
    const double areaMoment = 0.25 * area * material.radius * material.radius;
    const double polarMoment = 2.0 * areaMoment;
    Quat transported = Quat::Identity();
    Vec3 previousTangent = Vec3::UnitZ();
    for (size_t segment = 0; segment + 1 < positions.size(); ++segment)
    {
        const Vec3 edge = positions[segment + 1] - positions[segment];
        const double length = edge.norm();
        if (!std::isfinite(length) || length <= 1.0e-12)
            throw std::invalid_argument("rod rest segment must have positive length");
        const Vec3 tangent = edge / length;
        if (segment > 0 && tangent.dot(previousTangent) <= -1.0 + 1.0e-8)
            throw std::domain_error("rod rest transport contains a reversal");
        transported = (Quat::FromTwoVectors(previousTangent, tangent) * transported).normalized();
        Quat orientation = orientations.empty() ? transported : orientations[segment];
        if (!orientation.coeffs().allFinite() || orientation.norm() < 1.0e-12)
            throw std::invalid_argument("rod rest frame is invalid");
        orientation.normalize();
        rest.orientations.push_back(orientation);
        rest.lengths.push_back(length);
        rest.totalLength += length;
        rest.restStrains.push_back(orientation.conjugate() * tangent);
        rest.stretchShear.emplace_back(material.shearModulus * area,
            material.shearModulus * area, material.youngModulus * area);
        rest.bendTwist.emplace_back(material.youngModulus * areaMoment,
            material.youngModulus * areaMoment, material.shearModulus * polarMoment);
        const double mass = material.density * area * length;
        rest.masses[segment] += 0.5 * mass;
        rest.masses[segment + 1] += 0.5 * mass;
        const double transverse = mass * (length * length / 12.0 +
            material.radius * material.radius / 4.0);
        const Vec3 inertia(transverse, transverse, 0.5 * mass * material.radius * material.radius);
        rest.inertias.push_back(inertia);
        rest.inverseInertias.push_back(segment == 0 ? Vec3::Zero().eval() :
            inertia.cwiseInverse().eval());
        previousTangent = tangent;
    }
    for (size_t joint = 0; joint + 1 < rest.orientations.size(); ++joint)
        rest.restJointAngles.push_back(Log(
            rest.orientations[joint].conjugate() * rest.orientations[joint + 1]));
    for (size_t point = 0; point < positions.size(); ++point)
    {
        const bool pinned = point == 0 ||
            (!inverseMassMarkers.empty() && inverseMassMarkers[point] == 0.0);
        rest.inverseMasses.push_back(pinned ? 0.0 : 1.0 / rest.masses[point]);
        rest.rootLocalPositions.push_back(rest.orientations.front().conjugate() *
            (positions[point] - positions.front()));
        const double stiffness = pinned ? 0.0 :
            rest.masses[point] * material.referenceGravity /
                (material.supportOffsetRatio * rest.totalLength);
        rest.supportStiffness.push_back(stiffness);
        rest.supportDamping.push_back(2.0 * material.supportDampingRatio *
            std::sqrt(rest.masses[point] * stiffness));
    }
    return rest;
}

State MakeAuthoredState(const RestState& rest)
{
    State state;
    state.positions = rest.positions;
    state.orientations = rest.orientations;
    state.velocities.assign(rest.positions.size(), Vec3::Zero());
    state.angularVelocities.assign(rest.orientations.size(), Vec3::Zero());
    return state;
}
}
