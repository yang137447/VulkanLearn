#include <cmath>

#include <gtest/gtest.h>

#include "particles/particleCollision.h"

namespace
{

using namespace VL;

ParticleEmitterAsset PlaneEmitter()
{
    ParticleEmitterAsset emitter;
    emitter.collisionType = "plane";
    emitter.collisionPlaneNormal = Eigen::Vector3f(0.0f, 1.0f, 0.0f);
    emitter.collisionPlaneOffset = 0.0f;
    emitter.collisionBounce = 0.5f;
    emitter.collisionFriction = 0.0f;
    return emitter;
}

ParticleEmitterAsset BoxEmitter()
{
    ParticleEmitterAsset emitter;
    emitter.collisionType = "box";
    emitter.collisionBoxMin = Eigen::Vector3f(-1.0f, -1.0f, -1.0f);
    emitter.collisionBoxMax = Eigen::Vector3f(1.0f, 1.0f, 1.0f);
    emitter.collisionBounce = 0.5f;
    emitter.collisionFriction = 0.5f;
    return emitter;
}

ParticleEmitterAsset CapsuleEmitter()
{
    ParticleEmitterAsset emitter;
    emitter.collisionType = "capsule";
    emitter.collisionCapsuleA = Eigen::Vector3f(0.0f, -10.0f, 0.0f);
    emitter.collisionCapsuleB = Eigen::Vector3f(0.0f, 10.0f, 0.0f);
    emitter.collisionCapsuleRadius = 5.0f;
    emitter.collisionBounce = 1.0f;
    emitter.collisionFriction = 0.0f;
    return emitter;
}

ParticleCollisionState State(
    const Eigen::Vector3f& position,
    const Eigen::Vector3f& velocity)
{
    ParticleCollisionState state;
    state.position = position;
    state.velocity = velocity;
    return state;
}

} // namespace

TEST(ParticleCollision, PlanePushesParticleOutAndReflectsNormal)
{
    ParticleEmitterAsset emitter = PlaneEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, -5.0f, 0.0f),
            Eigen::Vector3f(3.0f, -10.0f, 2.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.y(), 0.0f);
    EXPECT_FLOAT_EQ(state.velocity.x(), 3.0f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 5.0f);
    EXPECT_FLOAT_EQ(state.velocity.z(), 2.0f);
}

TEST(ParticleCollision, PlaneBounceOneReflectsAndBounceZeroStopsNormal)
{
    ParticleEmitterAsset emitter = PlaneEmitter();
    emitter.collisionBounce = 1.0f;
    ParticleCollisionState reflected =
        State(Eigen::Vector3f(0.0f, -1.0f, 0.0f),
            Eigen::Vector3f(4.0f, -8.0f, -3.0f));
    EXPECT_TRUE(ResolveParticleCollision(reflected, emitter));
    EXPECT_FLOAT_EQ(reflected.velocity.x(), 4.0f);
    EXPECT_FLOAT_EQ(reflected.velocity.y(), 8.0f);
    EXPECT_FLOAT_EQ(reflected.velocity.z(), -3.0f);

    emitter.collisionBounce = 0.0f;
    ParticleCollisionState stopped =
        State(Eigen::Vector3f(0.0f, -1.0f, 0.0f),
            Eigen::Vector3f(4.0f, -8.0f, -3.0f));
    EXPECT_TRUE(ResolveParticleCollision(stopped, emitter));
    EXPECT_FLOAT_EQ(stopped.velocity.x(), 4.0f);
    EXPECT_FLOAT_EQ(stopped.velocity.y(), 0.0f);
    EXPECT_FLOAT_EQ(stopped.velocity.z(), -3.0f);
}

TEST(ParticleCollision, PlaneFrictionScalesOnlyTangentialVelocity)
{
    ParticleEmitterAsset emitter = PlaneEmitter();
    emitter.collisionBounce = 0.0f;
    emitter.collisionFriction = 0.25f;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, -1.0f, 0.0f),
            Eigen::Vector3f(4.0f, -8.0f, -2.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.velocity.x(), 3.0f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 0.0f);
    EXPECT_FLOAT_EQ(state.velocity.z(), -1.5f);
}

TEST(ParticleCollision, PlaneRadiusOffsetsSurface)
{
    ParticleEmitterAsset emitter = PlaneEmitter();
    emitter.collisionPlaneOffset = 10.0f;
    emitter.collisionRadius = 2.0f;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, 7.5f, 0.0f),
            Eigen::Vector3f::Zero());

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.y(), 12.0f);
    EXPECT_TRUE(state.velocity.isZero());
}

TEST(ParticleCollision, PlaneKillOnCollisionClearsAlive)
{
    ParticleEmitterAsset emitter = PlaneEmitter();
    emitter.killOnCollision = true;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, -1.0f, 0.0f),
            Eigen::Vector3f(0.0f, -3.0f, 0.0f));

    EXPECT_FALSE(ResolveParticleCollision(state, emitter));
    EXPECT_FALSE(state.alive);
}

TEST(ParticleCollision, DisabledCollisionIsNoOp)
{
    ParticleEmitterAsset emitter;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, -1.0f, 0.0f),
            Eigen::Vector3f(1.0f, -2.0f, 3.0f));
    const Eigen::Vector3f position = state.position;
    const Eigen::Vector3f velocity = state.velocity;

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_TRUE(state.position.isApprox(position));
    EXPECT_TRUE(state.velocity.isApprox(velocity));
}

TEST(ParticleCollision, BoxPushesOnMinimumPenetrationAxis)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(-0.8f, 0.0f, 0.0f),
            Eigen::Vector3f(5.0f, 2.0f, 3.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.x(), -1.0f);
    EXPECT_FLOAT_EQ(state.position.y(), 0.0f);
    EXPECT_FLOAT_EQ(state.position.z(), 0.0f);
    EXPECT_FLOAT_EQ(state.velocity.x(), -2.5f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 1.0f);
    EXPECT_FLOAT_EQ(state.velocity.z(), 1.5f);
}

TEST(ParticleCollision, BoxSelectsMaximumFaceWhenItIsNearest)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.8f, 0.0f, 0.0f),
            Eigen::Vector3f(-5.0f, 2.0f, 3.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.x(), 1.0f);
    EXPECT_FLOAT_EQ(state.velocity.x(), 2.5f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 1.0f);
    EXPECT_FLOAT_EQ(state.velocity.z(), 1.5f);
}

TEST(ParticleCollision, BoxOutwardNormalKeepsDirectionButFrictionsTangents)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.8f, 0.0f, 0.0f),
            Eigen::Vector3f(5.0f, 2.0f, 3.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.x(), 1.0f);
    EXPECT_FLOAT_EQ(state.velocity.x(), 5.0f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 1.0f);
    EXPECT_FLOAT_EQ(state.velocity.z(), 1.5f);
}

TEST(ParticleCollision, BoxRadiusExpandsEveryFace)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    emitter.collisionRadius = 0.5f;
    ParticleCollisionState state =
        State(Eigen::Vector3f(1.4f, 0.0f, 0.0f),
            Eigen::Vector3f(-1.0f, 0.0f, 0.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.x(), 1.5f);
    EXPECT_FLOAT_EQ(state.velocity.x(), 0.5f);
}

TEST(ParticleCollision, BoxOutsideIsUnchanged)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(3.0f, 4.0f, 5.0f),
            Eigen::Vector3f(-1.0f, 2.0f, -3.0f));
    const Eigen::Vector3f position = state.position;
    const Eigen::Vector3f velocity = state.velocity;

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_TRUE(state.position.isApprox(position));
    EXPECT_TRUE(state.velocity.isApprox(velocity));
}

TEST(ParticleCollision, BoxKillOnCollisionClearsAlive)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    emitter.killOnCollision = true;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, 0.0f, 0.0f),
            Eigen::Vector3f::Zero());

    EXPECT_FALSE(ResolveParticleCollision(state, emitter));
    EXPECT_FALSE(state.alive);
}

TEST(ParticleCollision, CapsuleProjectsOntoSegmentAndPushesOut)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(3.0f, 0.0f, 0.0f),
            Eigen::Vector3f(-2.0f, 0.0f, 0.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.x(), 5.0f);
    EXPECT_FLOAT_EQ(state.position.y(), 0.0f);
    EXPECT_FLOAT_EQ(state.position.z(), 0.0f);
    EXPECT_FLOAT_EQ(state.velocity.x(), 2.0f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 0.0f);
}

TEST(ParticleCollision, CapsuleUsesEndCapWhenProjectionClamps)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, 12.0f, 0.0f),
            Eigen::Vector3f(0.0f, -3.0f, 0.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.x(), 0.0f);
    EXPECT_FLOAT_EQ(state.position.y(), 15.0f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 3.0f);
}

TEST(ParticleCollision, CapsuleAddsParticleRadiusToSurface)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    emitter.collisionRadius = 2.0f;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, 0.0f, 6.0f),
            Eigen::Vector3f::Zero());

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.z(), 7.0f);
    EXPECT_TRUE(state.velocity.isZero());
}

TEST(ParticleCollision, CapsuleFrictionScalesTangentialVelocity)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    emitter.collisionBounce = 0.0f;
    emitter.collisionFriction = 0.5f;
    ParticleCollisionState state =
        State(Eigen::Vector3f(3.0f, 0.0f, 0.0f),
            Eigen::Vector3f(-4.0f, 6.0f, 0.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.velocity.x(), 0.0f);
    EXPECT_FLOAT_EQ(state.velocity.y(), 3.0f);
}

TEST(ParticleCollision, CapsuleKillOnCollisionClearsAlive)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    emitter.killOnCollision = true;
    ParticleCollisionState state =
        State(Eigen::Vector3f(1.0f, 0.0f, 0.0f),
            Eigen::Vector3f::Zero());

    EXPECT_FALSE(ResolveParticleCollision(state, emitter));
    EXPECT_FALSE(state.alive);
}

TEST(ParticleCollision, CapsuleWithCoincidentEndpointsDegeneratesToSphere)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    emitter.collisionCapsuleA = Eigen::Vector3f(2.0f, 3.0f, 4.0f);
    emitter.collisionCapsuleB = emitter.collisionCapsuleA;
    ParticleCollisionState state =
        State(Eigen::Vector3f(5.0f, 3.0f, 4.0f),
            Eigen::Vector3f(-1.0f, 0.0f, 0.0f));

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_FLOAT_EQ(state.position.x(), 7.0f);
    EXPECT_FLOAT_EQ(state.velocity.x(), 1.0f);
}

TEST(ParticleCollision, CapsuleOnAxisUsesStableNormal)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, 0.0f, 0.0f),
            Eigen::Vector3f::Zero());

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_TRUE(state.position.allFinite());
    EXPECT_FLOAT_EQ(state.position.x(), 0.0f);
    EXPECT_FLOAT_EQ(state.position.y(), 5.0f);
    EXPECT_FLOAT_EQ(state.position.z(), 0.0f);
}

TEST(ParticleCollision, CapsuleOutsideIsUnchanged)
{
    ParticleEmitterAsset emitter = CapsuleEmitter();
    ParticleCollisionState state =
        State(Eigen::Vector3f(40.0f, 0.0f, 0.0f),
            Eigen::Vector3f(-1.0f, 2.0f, -3.0f));
    const Eigen::Vector3f position = state.position;
    const Eigen::Vector3f velocity = state.velocity;

    EXPECT_TRUE(ResolveParticleCollision(state, emitter));
    EXPECT_TRUE(state.position.isApprox(position));
    EXPECT_TRUE(state.velocity.isApprox(velocity));
}

TEST(ParticleCollisionCcd, StepCountIsOneWhenDisabledAndCappedAtEight)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    emitter.collisionCcd = false;
    EXPECT_EQ(ParticleCollisionStepCount(
        Eigen::Vector3f(0.0f, -100.0f, 0.0f), emitter, 0.05f), 1);

    emitter.collisionCcd = true;
    EXPECT_EQ(ParticleCollisionStepCount(
        Eigen::Vector3f(0.0f, -1.0f, 0.0f), emitter, 0.05f), 1);
    EXPECT_EQ(ParticleCollisionStepCount(
        Eigen::Vector3f(0.0f, -100.0f, 0.0f), emitter, 0.05f), 8);
    EXPECT_EQ(ParticleCollisionStepCount(
        Eigen::Vector3f(0.0f, -1.0e9f, 0.0f), emitter, 1.0f), 8);

    emitter.collisionType = "none";
    EXPECT_EQ(ParticleCollisionStepCount(
        Eigen::Vector3f(0.0f, -100.0f, 0.0f), emitter, 0.05f), 1);
}

TEST(ParticleCollisionCcd, FastParticleTunnelsThroughThinBoxWithoutCcd)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    // 1cm 厚的盒体相对 5cm 的单帧扫过距离足够薄，单步只检查终点会穿透。
    emitter.collisionBoxMin = Eigen::Vector3f(-1.0f, -0.5f, -1.0f);
    emitter.collisionBoxMax = Eigen::Vector3f(1.0f, 0.5f, 1.0f);
    emitter.collisionBounce = 0.5f;
    emitter.collisionFriction = 0.0f;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, 2.0f, 0.0f),
            Eigen::Vector3f(0.0f, -100.0f, 0.0f));

    EXPECT_TRUE(ResolveParticleCollisionSteps(
        state, emitter, Eigen::Vector3f::Zero(), 0.0f, 0.05f));
    EXPECT_FLOAT_EQ(state.position.y(), -3.0f);
    EXPECT_FLOAT_EQ(state.velocity.y(), -100.0f);
}

TEST(ParticleCollisionCcd, FastParticleIsBlockedByThinBox)
{
    ParticleEmitterAsset emitter = BoxEmitter();
    emitter.collisionBoxMin = Eigen::Vector3f(-1.0f, -0.5f, -1.0f);
    emitter.collisionBoxMax = Eigen::Vector3f(1.0f, 0.5f, 1.0f);
    emitter.collisionBounce = 0.5f;
    emitter.collisionFriction = 0.0f;
    emitter.collisionCcd = true;
    ParticleCollisionState state =
        State(Eigen::Vector3f(0.0f, 2.0f, 0.0f),
            Eigen::Vector3f(0.0f, -100.0f, 0.0f));

    EXPECT_TRUE(emitter.collisionCcd);
    EXPECT_EQ(ParticleCollisionStepCount(
        state.velocity, emitter, 0.05f), 8);
    EXPECT_TRUE(ResolveParticleCollisionSteps(
        state, emitter, Eigen::Vector3f::Zero(), 0.0f, 0.05f));
    EXPECT_GT(state.position.y(), 0.5f);
    EXPECT_GT(state.velocity.y(), 0.0f);
}

TEST(ParticleCollisionCcd, FastParticleStopsAtPlaneWithAndWithoutCcd)
{
    for (const bool ccd : {false, true})
    {
        ParticleEmitterAsset emitter = PlaneEmitter();
        emitter.collisionCcd = ccd;
        ParticleCollisionState state =
            State(Eigen::Vector3f(0.0f, 2.0f, 0.0f),
                Eigen::Vector3f(0.0f, -100.0f, 0.0f));

        EXPECT_TRUE(ResolveParticleCollisionSteps(
            state, emitter, Eigen::Vector3f::Zero(), 0.0f, 0.05f));
        EXPECT_GE(state.position.y(), 0.0f) << "ccd=" << ccd;
        EXPECT_GT(state.velocity.y(), 0.0f) << "ccd=" << ccd;
        if (!ccd)
        {
            EXPECT_FLOAT_EQ(state.position.y(), 0.0f);
            EXPECT_FLOAT_EQ(state.velocity.y(), 50.0f);
        }
    }
}

TEST(ParticleCollisionCcd, SingleStepMatchesSemiImplicitGravityAndDrag)
{
    ParticleEmitterAsset emitter;
    emitter.collisionType = "none";
    ParticleCollisionState state =
        State(Eigen::Vector3f::Zero(), Eigen::Vector3f(0.0f, 10.0f, 0.0f));

    EXPECT_TRUE(ResolveParticleCollisionSteps(
        state, emitter, Eigen::Vector3f(0.0f, -10.0f, 0.0f), 0.5f, 0.1f));

    const float expectedVelocity = 9.0f * std::exp(-0.05f);
    EXPECT_NEAR(state.velocity.y(), expectedVelocity, 1.0e-4f);
    EXPECT_NEAR(state.position.y(), expectedVelocity * 0.1f, 1.0e-4f);
}
