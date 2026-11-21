#pragma once

#include "hair/rod/rodTypes.h"

#include <functional>

namespace VL::Hair::Rod
{
struct CollisionSphere
{
    Vec3 center = Vec3::Zero();
    double radius = 0.0;
    Vec3 velocity = Vec3::Zero();
};

struct CollisionCapsule
{
    Vec3 a = Vec3::Zero();
    Vec3 b = Vec3::Zero();
    double radius = 0.0;
    Vec3 velocityA = Vec3::Zero();
    Vec3 velocityB = Vec3::Zero();
};

struct CollisionSample
{
    bool valid = false;
    double distance = 0.0;
    Vec3 gradient = Vec3::UnitY();
    Vec3 colliderVelocity = Vec3::Zero();
};

using CollisionSdfQuery = std::function<CollisionSample(const Vec3&)>;

struct ContactProjection
{
    Vec3 position = Vec3::Zero();
    Vec3 velocity = Vec3::Zero();
    Vec3 normal = Vec3::Zero();
    Vec3 correction = Vec3::Zero();
    double gap = 0.0;
    double linearizedGap = 0.0;
    double lambda = 0.0;
    double deltaLambda = 0.0;
    double normalImpulse = 0.0;
    bool sampled = false;
    bool active = false;
};

CollisionSample SampleCollisionSphere(const Vec3& position, const CollisionSphere& sphere);
CollisionSample SampleCollisionCapsule(const Vec3& position, const CollisionCapsule& capsule);
CollisionSample SampleCollisionSdf(const Vec3& position, const CollisionSdfQuery& query);

Vec3 StabilizeContactVelocity(
    const Vec3& velocity, double inverseMass, const CollisionSample& sample,
    bool active, double* normalImpulse = nullptr);

Vec3 ReconstructContactVelocity(
    const Vec3& previousPosition, const Vec3& projectedPosition,
    const Vec3& accumulatedContactCorrection, double timeStep);

ContactProjection ProjectContact(
    const Vec3& position, const Vec3& velocity, double inverseMass, double timeStep,
    double margin, double compliance, double lambda, const CollisionSample& sample);
}
