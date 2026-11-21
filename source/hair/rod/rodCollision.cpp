#include "hair/rod/rodCollision.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL::Hair::Rod
{
namespace
{
void Require(bool condition)
{
    if (!condition)
        throw std::invalid_argument("Invalid SI rod collision input");
}

Vec3 UnitGradient(const CollisionSample& sample)
{
    Require(std::isfinite(sample.distance) && sample.gradient.allFinite() &&
        sample.colliderVelocity.allFinite());
    const double length = sample.gradient.stableNorm();
    Require(std::isfinite(length) && length > 0.0);
    return sample.gradient / length;
}

void ValidateSample(const CollisionSample& sample)
{
    if (sample.valid)
        UnitGradient(sample);
}

Vec3 AxisNormal(const Vec3& axis)
{
    const Vec3 reference = std::abs(axis.y()) < 0.9
        ? Vec3::UnitY() : Vec3::UnitX();
    return axis.cross(reference).normalized();
}
}

CollisionSample SampleCollisionSphere(const Vec3& position, const CollisionSphere& sphere)
{
    Require(position.allFinite() && sphere.center.allFinite() &&
        sphere.velocity.allFinite() && std::isfinite(sphere.radius) && sphere.radius >= 0.0);
    const Vec3 offset = position - sphere.center;
    Require(offset.allFinite());
    const double length = offset.stableNorm();
    Require(std::isfinite(length));
    CollisionSample sample;
    sample.valid = true;
    sample.distance = length - sphere.radius;
    sample.gradient = length > 0.0 ? Vec3(offset / length) : Vec3::UnitY();
    sample.colliderVelocity = sphere.velocity;
    ValidateSample(sample);
    return sample;
}

CollisionSample SampleCollisionCapsule(const Vec3& position, const CollisionCapsule& capsule)
{
    Require(position.allFinite() && capsule.a.allFinite() && capsule.b.allFinite() &&
        capsule.velocityA.allFinite() && capsule.velocityB.allFinite() &&
        std::isfinite(capsule.radius) && capsule.radius >= 0.0);
    const Vec3 axis = capsule.b - capsule.a;
    Require(axis.allFinite());
    const double axisLength = axis.stableNorm();
    Require(std::isfinite(axisLength));
    if (axisLength == 0.0)
        return SampleCollisionSphere(position,
            CollisionSphere{capsule.a, capsule.radius, capsule.velocityA});
    const Vec3 direction = axis / axisLength;
    const Vec3 relative = position - capsule.a;
    Require(relative.allFinite());
    const double projected = relative.dot(direction);
    Require(std::isfinite(projected));
    const double fraction = std::clamp(projected / axisLength, 0.0, 1.0);
    const Vec3 closest = (1.0 - fraction) * capsule.a + fraction * capsule.b;
    const Vec3 offset = position - closest;
    Require(offset.allFinite());
    const double length = offset.stableNorm();
    Require(std::isfinite(length));
    CollisionSample sample;
    sample.valid = true;
    sample.distance = length - capsule.radius;
    sample.gradient = length > 0.0 ? Vec3(offset / length) : AxisNormal(direction);
    sample.colliderVelocity =
        (1.0 - fraction) * capsule.velocityA + fraction * capsule.velocityB;
    ValidateSample(sample);
    return sample;
}

CollisionSample SampleCollisionSdf(const Vec3& position, const CollisionSdfQuery& query)
{
    Require(position.allFinite() && static_cast<bool>(query));
    CollisionSample sample = query(position);
    if (sample.valid)
        sample.gradient = UnitGradient(sample);
    return sample;
}

Vec3 StabilizeContactVelocity(
    const Vec3& velocity, double inverseMass, const CollisionSample& sample,
    bool active, double* normalImpulse)
{
    Require(velocity.allFinite() && std::isfinite(inverseMass) && inverseMass >= 0.0);
    ValidateSample(sample);
    Vec3 result = velocity;
    double impulse = 0.0;
    if (inverseMass > 0.0 && sample.valid && active)
    {
        const Vec3 normal = UnitGradient(sample);
        const Vec3 relativeVelocity = velocity - sample.colliderVelocity;
        Require(relativeVelocity.allFinite());
        const double inwardSpeed = std::min(relativeVelocity.dot(normal), 0.0);
        Require(std::isfinite(inwardSpeed));
        result -= inwardSpeed * normal;
        impulse = -inwardSpeed / inverseMass;
        Require(result.allFinite() && std::isfinite(impulse));
    }
    if (normalImpulse)
        *normalImpulse = impulse;
    return result;
}

Vec3 ReconstructContactVelocity(
    const Vec3& previousPosition, const Vec3& projectedPosition,
    const Vec3& accumulatedContactCorrection, double timeStep)
{
    Require(previousPosition.allFinite() && projectedPosition.allFinite() &&
        accumulatedContactCorrection.allFinite() &&
        std::isfinite(timeStep) && timeStep > 0.0);
    const Vec3 velocity =
        ((projectedPosition - previousPosition) - accumulatedContactCorrection) / timeStep;
    Require(velocity.allFinite());
    return velocity;
}

ContactProjection ProjectContact(
    const Vec3& position, const Vec3& velocity, double inverseMass, double timeStep,
    double margin, double compliance, double lambda, const CollisionSample& sample)
{
    Require(position.allFinite() && velocity.allFinite() &&
        std::isfinite(inverseMass) && inverseMass >= 0.0 &&
        std::isfinite(timeStep) && timeStep > 0.0 &&
        std::isfinite(margin) && margin >= 0.0 &&
        std::isfinite(compliance) && compliance >= 0.0 &&
        std::isfinite(lambda) && lambda >= 0.0);
    ValidateSample(sample);
    ContactProjection result;
    result.position = position;
    result.velocity = velocity;
    if (!sample.valid)
        return result;
    result.sampled = true;
    result.normal = UnitGradient(sample);
    result.gap = sample.distance - margin;
    result.linearizedGap = result.gap;
    Require(std::isfinite(result.gap));
    if (inverseMass == 0.0)
        return result;
    const double scaledCompliance = (compliance / timeStep) / timeStep;
    const double denominator = inverseMass + scaledCompliance;
    Require(std::isfinite(scaledCompliance) && std::isfinite(denominator) && denominator > 0.0);
    const double numerator = -result.gap - scaledCompliance * lambda;
    Require(std::isfinite(numerator));
    const double delta = numerator / denominator;
    const double unclampedLambda = lambda + delta;
    Require(std::isfinite(delta) && std::isfinite(unclampedLambda));
    result.lambda = std::max(0.0, unclampedLambda);
    result.deltaLambda = result.lambda - lambda;
    const double displacement = inverseMass * result.deltaLambda;
    Require(std::isfinite(displacement));
    result.correction = displacement * result.normal;
    result.position += result.correction;
    result.linearizedGap += displacement;
    Require(result.position.allFinite() && std::isfinite(result.linearizedGap));
    result.active = result.gap <= 0.0 || result.lambda > 0.0;
    result.velocity = StabilizeContactVelocity(
        velocity, inverseMass, sample, result.active, &result.normalImpulse);
    return result;
}
}
