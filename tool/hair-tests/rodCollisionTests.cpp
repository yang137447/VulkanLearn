#include "hair/rod/rodCollision.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace
{
using namespace VL::Hair::Rod;

void NearVector(const Vec3& actual, const Vec3& expected, double tolerance = 1.0e-12)
{
    EXPECT_LE((actual - expected).norm(), tolerance);
}

double CapsuleDistance(const Vec3& point, const CollisionCapsule& capsule)
{
    const Vec3 axis = capsule.b - capsule.a;
    const double squaredLength = axis.squaredNorm();
    const double parameter = squaredLength > 0.0
        ? std::max(0.0, std::min(1.0, (point - capsule.a).dot(axis) / squaredLength)) : 0.0;
    return (point - capsule.a - parameter * axis).norm() - capsule.radius;
}

CollisionSample PlaneSample(const Vec3& position)
{
    return CollisionSample{true, position.y(), Vec3::UnitY(), Vec3::Zero()};
}

TEST(RodCollision, SphereHardProjectionAndOutwardMargin)
{
    const CollisionSphere sphere{Vec3(1, 2, 3), .5, Vec3::Zero()};
    for (const Vec3& position : {Vec3(1.1, 2, 3), Vec3(1, 2, 3), Vec3(2, 2, 3)})
    {
        const auto result = ProjectContact(position, Vec3::Zero(), 2, .01, .02, 0, 0,
            SampleCollisionSphere(position, sphere));
        EXPECT_GE((result.position - sphere.center).norm() - sphere.radius, .02 - 1.0e-12);
        EXPECT_GE(result.lambda, 0);
        EXPECT_LE(result.velocity.norm(), 1.0e-12);
    }
    const auto center = SampleCollisionSphere(sphere.center, sphere);
    NearVector(center.gradient, Vec3::UnitY());
}

TEST(RodCollision, CapsuleSideCapsAxisAndCollapsedSphere)
{
    CollisionCapsule capsule{Vec3(0, -1, 0), Vec3(0, 1, 0), .3,
        Vec3(0, 1, 0), Vec3(0, 3, 0)};
    for (const Vec3& position : {Vec3(.1, 0, 0), Vec3(0, 1.1, 0),
        Vec3(0, -1.1, 0), Vec3::Zero().eval()})
    {
        const auto sample = SampleCollisionCapsule(position, capsule);
        const auto result = ProjectContact(position, Vec3::Zero(), 1, .01, .01, 0, 0, sample);
        EXPECT_GE(CapsuleDistance(result.position, capsule), .01 - 1.0e-12);
        EXPECT_NEAR(sample.gradient.norm(), 1, 1.0e-12);
    }
    NearVector(SampleCollisionCapsule(Vec3::Zero(), capsule).gradient, -Vec3::UnitZ());
    NearVector(SampleCollisionCapsule(Vec3(.1, 0, 0), capsule).colliderVelocity, Vec3(0, 2, 0));
    capsule.b = capsule.a;
    const auto sample = SampleCollisionCapsule(capsule.a, capsule);
    NearVector(sample.gradient, Vec3::UnitY());
    NearVector(sample.colliderVelocity, capsule.velocityA);
}

TEST(RodCollision, XpbdPlaneHasIndependentClosedFormAndReleasesLambda)
{
    const Vec3 position(0, -.2, 0);
    const double inverseMass = 2;
    const double step = .1;
    const double compliance = .02;
    const auto result = ProjectContact(position, Vec3::Zero(), inverseMass, step,
        .1, compliance, 0, PlaneSample(position));
    EXPECT_NEAR(result.lambda, .075, 1.0e-14);
    EXPECT_NEAR(result.position.y(), -.05, 1.0e-14);
    EXPECT_NEAR(result.linearizedGap, -.15, 1.0e-14);
    EXPECT_NEAR(result.linearizedGap + compliance / (step * step) * result.lambda, 0, 1.0e-14);
    const Vec3 separated(0, 1, 0);
    const auto released = ProjectContact(separated, Vec3(0, 2, 0), inverseMass, step,
        .1, compliance, result.lambda, PlaneSample(separated));
    EXPECT_DOUBLE_EQ(released.lambda, 0);
    EXPECT_FALSE(released.active);
    NearVector(released.velocity, Vec3(0, 2, 0));
}

TEST(RodCollision, OnlyZeroInverseMassExcludesRoots)
{
    const Vec3 position(0, -1, 0), velocity(1, -2, 3);
    const auto fixed = ProjectContact(position, velocity, 0, .01, 0, 0, 0, PlaneSample(position));
    NearVector(fixed.position, position);
    NearVector(fixed.velocity, velocity);
    EXPECT_DOUBLE_EQ(fixed.lambda, 0);
    EXPECT_FALSE(fixed.active);
    const auto movable = ProjectContact(position, velocity, 1, .01, 0, 0, 0, PlaneSample(position));
    EXPECT_DOUBLE_EQ(movable.position.y(), 0);
    NearVector(movable.velocity, Vec3(1, 0, 3));
}

TEST(RodCollision, MovingColliderUsesRelativeNormalAndPreservesTangents)
{
    const CollisionSample sample{true, 0, Vec3::UnitY(), Vec3(4, 3, -5)};
    double impulse = -1;
    const Vec3 stable = StabilizeContactVelocity(Vec3(7, -2, 8), 2, sample, true, &impulse);
    NearVector(stable, Vec3(7, 3, 8));
    EXPECT_DOUBLE_EQ(impulse, 2.5);
    NearVector(StabilizeContactVelocity(Vec3(7, 6, 8), 2, sample, true), Vec3(7, 6, 8));
    NearVector(StabilizeContactVelocity(Vec3(7, -2, 8), 2, sample, false), Vec3(7, -2, 8));
    NearVector(StabilizeContactVelocity(Vec3(7, -2, 8), 0, sample, true), Vec3(7, -2, 8));
}

TEST(RodCollision, InitialPenetrationRepairDoesNotCreateArtificialRebound)
{
    const Vec3 oldPosition(.5, 0, 0);
    const auto sample = SampleCollisionSphere(oldPosition, CollisionSphere{Vec3::Zero(), 1});
    const auto result = ProjectContact(oldPosition, Vec3::Zero(), 1, .01, 0, 0, 0, sample);
    const Vec3 naive = (result.position - oldPosition) / .01;
    EXPECT_DOUBLE_EQ(naive.x(), 50);
    const Vec3 split = ReconstructContactVelocity(oldPosition, result.position, result.correction, .01);
    NearVector(StabilizeContactVelocity(split, 1, sample, result.active), Vec3::Zero());
    NearVector(result.velocity, Vec3::Zero());
}

TEST(RodCollision, RepeatedGravityContactIsStaticAndRecoveryCanLeave)
{
    const double step = .001;
    Vec3 position(0, .01, 0), velocity = Vec3::Zero();
    for (int tick = 0; tick < 2000; ++tick)
    {
        const Vec3 previous = position;
        velocity.y() -= 9.8 * step;
        const Vec3 predicted = position + step * velocity;
        const auto sample = PlaneSample(predicted);
        const auto result = ProjectContact(predicted, velocity, 1, step, .01, 0, 0, sample);
        position = result.position;
        velocity = StabilizeContactVelocity(
            ReconstructContactVelocity(previous, position, result.correction, step), 1, sample, result.active);
        EXPECT_NEAR(position.y(), .01, 1.0e-12);
        EXPECT_NEAR(velocity.squaredNorm(), 0, 1.0e-20);
    }
    velocity.y() += 20 * step;
    const Vec3 predicted = position + step * velocity;
    const auto result = ProjectContact(predicted, velocity, 1, step, .01, 0, 0, PlaneSample(predicted));
    EXPECT_GT(result.position.y(), .01);
    EXPECT_GT(result.velocity.y(), 0);
    EXPECT_FALSE(result.active);
}

TEST(RodCollision, SdfCallbackNormalizesGradientAndInvalidSampleSkips)
{
    const auto query = [](const Vec3& position)
    {
        return CollisionSample{true, position.x(), Vec3(2, 0, 0), Vec3::Zero()};
    };
    const Vec3 position(-.2, 0, 0);
    const auto result = ProjectContact(position, Vec3(-1, 2, 3), 1, .01, .02, 0, 0,
        SampleCollisionSdf(position, query));
    NearVector(result.position, Vec3(.02, 0, 0));
    NearVector(result.velocity, Vec3(0, 2, 3));
    const CollisionSample unavailable{};
    const auto skipped = ProjectContact(position, Vec3::Ones(), 1, .01, 0, 0, 1, unavailable);
    EXPECT_FALSE(skipped.sampled);
    EXPECT_DOUBLE_EQ(skipped.lambda, 0);
    NearVector(skipped.position, position);
    NearVector(skipped.velocity, Vec3::Ones());
}

TEST(RodCollision, MovingSphereCarriesNormalVelocityWithoutRepairMomentum)
{
    const double step = .001;
    CollisionSphere sphere{Vec3::Zero(), 1, Vec3(0, .2, 0)};
    Vec3 position(0, 1.01, 0), velocity = Vec3::Zero();
    for (int tick = 0; tick < 1000; ++tick)
    {
        sphere.center += step * sphere.velocity;
        const Vec3 previous = position;
        const Vec3 predicted = position + step * velocity;
        const auto sample = SampleCollisionSphere(predicted, sphere);
        const auto result = ProjectContact(predicted, velocity, 1, step, .01, 0, 0, sample);
        position = result.position;
        velocity = StabilizeContactVelocity(
            ReconstructContactVelocity(previous, position, result.correction, step), 1, sample, result.active);
        EXPECT_GE((position - sphere.center).norm() - sphere.radius, .01 - 1.0e-12);
        EXPECT_NEAR(velocity.y(), .2, 1.0e-10);
        EXPECT_NEAR(velocity.x(), 0, 1.0e-12);
        EXPECT_NEAR(velocity.z(), 0, 1.0e-12);
        EXPECT_LE(velocity.squaredNorm(), .04 + 1.0e-10);
    }
}

TEST(RodCollision, SdfSphereMatchesAnalyticProjection)
{
    const CollisionSphere sphere{Vec3(.2, .3, -.4), .6};
    const Vec3 position(.3, .4, -.3);
    const auto analytic = SampleCollisionSphere(position, sphere);
    const auto query = [&](const Vec3& point)
    {
        const Vec3 offset = point - sphere.center;
        return CollisionSample{true, offset.norm() - sphere.radius, offset, sphere.velocity};
    };
    const auto fromSdf = SampleCollisionSdf(position, query);
    const auto first = ProjectContact(position, Vec3::Zero(), 1, .01, .01, 0, 0, analytic);
    const auto second = ProjectContact(position, Vec3::Zero(), 1, .01, .01, 0, 0, fromSdf);
    NearVector(first.position, second.position);
}

TEST(RodCollision, RigidTransformEquivarianceForNondegenerateGeometry)
{
    const Quat rotation(Eigen::AngleAxisd(.8, Vec3(1, 2, 3).normalized()));
    const Vec3 shift(2, -3, 4);
    const CollisionCapsule original{Vec3(0, -1, 0), Vec3(0, 1, 0), .5,
        Vec3(0, .1, 0), Vec3(.2, .3, 0)};
    const Vec3 position(.1, .3, .1), velocity(-2, -3, 4);
    const CollisionCapsule moved{rotation * original.a + shift, rotation * original.b + shift,
        original.radius, rotation * original.velocityA, rotation * original.velocityB};
    const auto baseline = ProjectContact(position, velocity, 2, .01, .02, 0, 0,
        SampleCollisionCapsule(position, original));
    const Vec3 movedPosition = rotation * position + shift;
    const auto transformed = ProjectContact(movedPosition, rotation * velocity, 2, .01, .02, 0, 0,
        SampleCollisionCapsule(movedPosition, moved));
    NearVector(transformed.position, rotation * baseline.position + shift);
    NearVector(transformed.velocity, rotation * baseline.velocity);
}

TEST(RodCollision, InvalidFiniteBoundaryThrowsWithoutChangingOutputs)
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const Vec3 valid = Vec3::Zero();
    EXPECT_THROW(SampleCollisionSphere(Vec3(nan, 0, 0), {}), std::invalid_argument);
    EXPECT_THROW(SampleCollisionSphere(valid, CollisionSphere{valid, -1}), std::invalid_argument);
    EXPECT_THROW(SampleCollisionCapsule(valid, CollisionCapsule{valid, valid, infinity}), std::invalid_argument);
    EXPECT_THROW(SampleCollisionSdf(valid, CollisionSdfQuery{}), std::invalid_argument);
    EXPECT_THROW(SampleCollisionSdf(valid, [](const Vec3&)
        { return CollisionSample{true, 0, Vec3::Zero()}; }), std::invalid_argument);
    const auto sample = PlaneSample(valid);
    for (const double mass : {-1.0, nan, infinity})
        EXPECT_THROW(ProjectContact(valid, valid, mass, .01, 0, 0, 0, sample), std::invalid_argument);
    for (const double step : {0.0, -1.0, nan, infinity})
        EXPECT_THROW(ProjectContact(valid, valid, 1, step, 0, 0, 0, sample), std::invalid_argument);
    EXPECT_THROW(ProjectContact(valid, valid, 1, .01, -1, 0, 0, sample), std::invalid_argument);
    EXPECT_THROW(ProjectContact(valid, valid, 1, .01, 0, -1, 0, sample), std::invalid_argument);
    EXPECT_THROW(ProjectContact(valid, valid, 1, .01, 0, 0, -1, sample), std::invalid_argument);
    EXPECT_THROW(ReconstructContactVelocity(valid, valid, valid, 0), std::invalid_argument);
    double impulse = 123;
    EXPECT_THROW(StabilizeContactVelocity(valid, 1,
        CollisionSample{true, nan, Vec3::UnitY()}, true, &impulse), std::invalid_argument);
    EXPECT_DOUBLE_EQ(impulse, 123);
    EXPECT_THROW(SampleCollisionSphere(Vec3(1.0e308, 0, 0),
        CollisionSphere{Vec3(-1.0e308, 0, 0), 1}), std::invalid_argument);
}
}
