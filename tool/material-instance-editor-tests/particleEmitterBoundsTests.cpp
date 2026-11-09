#include <gtest/gtest.h>

#include <cmath>

#include "particles/particleAsset.h"
#include "particles/particleEmitterBounds.h"

namespace
{

using namespace VL;

ParticleEmitterAsset BoundsTestEmitter()
{
    ParticleEmitterAsset emitter;
    emitter.shapeRadius = 0.0f;
    emitter.lifetimeMin = 1.0f;
    emitter.lifetimeMax = 1.0f;
    emitter.velocityMin = Eigen::Vector3f::Zero();
    emitter.velocityMax = Eigen::Vector3f::Zero();
    emitter.gravity = Eigen::Vector3f::Zero();
    emitter.sizeStart = 0.0f;
    emitter.sizeEnd = 0.0f;
    return emitter;
}

} // namespace

TEST(ParticleEmitterBounds, LargeStandingBillboardIsIncluded)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.sizeStart = 40.0f;
    emitter.sizeEnd = 25.0f;

    // 低速/零速度的发烟 billboard 也必须按最大半边长外扩。
    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter), 40.0f);
}

TEST(ParticleEmitterBounds, AsymmetricVelocityUsesLargestAbsoluteAxis)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.velocityMin = Eigen::Vector3f(-400.0f, 0.0f, 0.0f);
    emitter.velocityMax = Eigen::Vector3f(20.0f, 0.0f, 0.0f);

    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter), 400.0f);
}

TEST(ParticleEmitterBounds, GravityShellAndShapeRadiusAddUp)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.shapeRadius = 10.0f;
    emitter.lifetimeMax = 2.0f;
    emitter.gravity = Eigen::Vector3f(0.0f, -100.0f, 0.0f);

    // 10 + 2 * (0 + 0.5 * 100 * 2) = 210
    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter), 210.0f);
}

TEST(ParticleEmitterBounds, SizeKeysLargerThanEndpointsWin)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.sizeStart = 5.0f;
    emitter.sizeEnd = 7.0f;
    emitter.sizeKeys = {
        ParticleSizeKey{0.0f, 5.0f},
        ParticleSizeKey{0.5f, 30.0f},
        ParticleSizeKey{1.0f, 7.0f}};

    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter), 30.0f);
}

TEST(ParticleEmitterBounds, MeshSizeCurveScaleWidensBounds)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.renderMode = "mesh";
    emitter.meshUseSizeCurve = true;
    emitter.meshSizeScale = 2.0f;
    emitter.sizeStart = 10.0f;
    emitter.sizeEnd = 10.0f;
    emitter.sizeKeys = {
        ParticleSizeKey{0.0f, 10.0f},
        ParticleSizeKey{1.0f, 30.0f}};

    // max/base * sizeScale = 30/10 * 2 = 6；网格自身半径 10 -> 60。
    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter, 10.0f), 60.0f);
    emitter.meshUseSizeCurve = false;
    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter, 10.0f), 10.0f);
}

TEST(ParticleEmitterBounds, ModelScaleUsesLargestLinearColumn)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.sizeEnd = 12.0f;

    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model(0, 0) = 2.0f;
    model(1, 1) = 0.5f;
    model(2, 2) = 1.5f;

    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterBoundsRadius(emitter, model), 24.0f);
}

TEST(ParticleEmitterBounds, AutoBoundsCenterIsModelOrigin)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.sizeEnd = 10.0f;

    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 1>(0, 3) = Eigen::Vector3f(100.0f, 20.0f, -5.0f);

    Eigen::Vector3f center;
    float radius = 0.0f;
    ComputeParticleEmitterBounds(emitter, model, center, radius);

    EXPECT_FLOAT_EQ(center.x(), 100.0f);
    EXPECT_FLOAT_EQ(center.y(), 20.0f);
    EXPECT_FLOAT_EQ(center.z(), -5.0f);
    EXPECT_FLOAT_EQ(radius, 10.0f);
}

TEST(ParticleEmitterBounds, FixedBoundsUseAuthoredCenterAndExtent)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.useFixedBounds = true;
    emitter.fixedBoundsCenter =
        Eigen::Vector3f(10.0f, -20.0f, 30.0f);
    emitter.fixedBoundsExtent =
        Eigen::Vector3f(30.0f, 40.0f, 0.0f);

    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model(0, 0) = 2.0f;
    model(1, 1) = 0.5f;
    model(2, 2) = 1.5f;
    model.block<3, 1>(0, 3) =
        Eigen::Vector3f(100.0f, 200.0f, 300.0f);

    Eigen::Vector3f center;
    float radius = 0.0f;
    ComputeParticleEmitterBounds(emitter, model, center, radius);

    // center = model * (10,-20,30,1)
    EXPECT_FLOAT_EQ(center.x(), 120.0f);
    EXPECT_FLOAT_EQ(center.y(), 190.0f);
    EXPECT_FLOAT_EQ(center.z(), 345.0f);
    // radius = |(30,40,0)| * maxScale(2)
    EXPECT_FLOAT_EQ(radius, 100.0f);
}

TEST(ParticleEmitterBounds, FixedBoundsIgnoreAutoRadius)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.sizeEnd = 500.0f;
    emitter.lifetimeMax = 4.0f;
    emitter.gravity = Eigen::Vector3f(0.0f, -100.0f, 0.0f);
    emitter.useFixedBounds = true;
    emitter.fixedBoundsExtent =
        Eigen::Vector3f(5.0f, 0.0f, 0.0f);

    Eigen::Vector3f center;
    float radius = 0.0f;
    ComputeParticleEmitterBounds(
        emitter,
        Eigen::Matrix4f::Identity(),
        center,
        radius);

    EXPECT_FLOAT_EQ(radius, 5.0f);
}

TEST(ParticleEmitterBounds, MeshUsesImportedVertexExtentInsteadOfSizeCurve)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.renderMode = "mesh";
    emitter.sizeStart = 500.0f;
    emitter.sizeEnd = 500.0f;

    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model(0, 0) = 2.0f;
    model(1, 1) = 2.0f;
    model(2, 2) = 2.0f;

    Eigen::Vector3f center;
    float radius = 0.0f;
    ComputeParticleEmitterBounds(
        emitter,
        model,
        center,
        radius,
        25.0f);

    EXPECT_FLOAT_EQ(radius, 50.0f);
}

TEST(ParticleEmitterBounds, MeshFallbackKeepsBillboardRadius)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.renderMode = "mesh";
    emitter.sizeStart = 40.0f;
    emitter.sizeEnd = 25.0f;

    Eigen::Vector3f center;
    float radius = 0.0f;
    ComputeParticleEmitterBounds(
        emitter,
        Eigen::Matrix4f::Identity(),
        center,
        radius,
        0.0f);

    EXPECT_FLOAT_EQ(radius, 40.0f);
}

TEST(ParticleEmitterBounds, BeamCoversMidpointNoiseAndWidth)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.renderMode = "beam";
    emitter.beam.start = Eigen::Vector3f(-200.0f, 0.0f, 0.0f);
    emitter.beam.end = Eigen::Vector3f(200.0f, 0.0f, 0.0f);
    emitter.beam.width = 3.0f;
    emitter.beam.noiseAmplitude = 18.0f;
    emitter.beam.endpointJitter = 4.0f;

    // 200（半长）+ 18*sqrt(2)（两轴噪声）+ 3（半宽）+
    // 4*sqrt(3)（三轴端点抖动）
    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter),
        200.0f + 18.0f * std::sqrt(2.0f) +
            3.0f + 4.0f * std::sqrt(3.0f));

    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model(0, 0) = 2.0f;
    model(1, 1) = 0.5f;
    model(2, 2) = 1.5f;
    model.block<3, 1>(0, 3) =
        Eigen::Vector3f(100.0f, 20.0f, -5.0f);

    Eigen::Vector3f center;
    float radius = 0.0f;
    ComputeParticleEmitterBounds(emitter, model, center, radius);

    // 自动中心取变换后的线段中点，而不是发射器原点。
    EXPECT_FLOAT_EQ(center.x(), 100.0f);
    EXPECT_FLOAT_EQ(center.y(), 20.0f);
    EXPECT_FLOAT_EQ(center.z(), -5.0f);
    EXPECT_FLOAT_EQ(
        radius,
        2.0f * (200.0f + 18.0f * std::sqrt(2.0f) +
            3.0f + 4.0f * std::sqrt(3.0f)));
}

TEST(ParticleEmitterBounds, BeamCenterFollowsOffsetEndpoints)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.renderMode = "beam";
    emitter.beam.start = Eigen::Vector3f(100.0f, 0.0f, 0.0f);
    emitter.beam.end = Eigen::Vector3f(300.0f, 0.0f, 0.0f);
    emitter.beam.width = 2.0f;
    emitter.beam.noiseAmplitude = 10.0f;
    emitter.beam.endpointJitter = 0.0f;

    Eigen::Matrix4f model = Eigen::Matrix4f::Identity();
    model.block<3, 1>(0, 3) = Eigen::Vector3f(10.0f, 0.0f, 0.0f);

    Eigen::Vector3f center;
    float radius = 0.0f;
    ComputeParticleEmitterBounds(emitter, model, center, radius);

    EXPECT_FLOAT_EQ(center.x(), 210.0f);
    EXPECT_FLOAT_EQ(
        radius,
        100.0f + 10.0f * std::sqrt(2.0f) + 2.0f);
}

TEST(ParticleEmitterBounds, BeamBranchesExtendRadius)
{
    ParticleEmitterAsset emitter = BoundsTestEmitter();
    emitter.renderMode = "beam";
    emitter.beam.start = Eigen::Vector3f(-200.0f, 0.0f, 0.0f);
    emitter.beam.end = Eigen::Vector3f(200.0f, 0.0f, 0.0f);
    emitter.beam.width = 3.0f;
    emitter.beam.noiseAmplitude = 0.0f;
    emitter.beam.endpointJitter = 2.0f;
    emitter.beam.branchCount = 2;
    emitter.beam.branchStart = 0.45f;
    emitter.beam.branchLength = 0.35f;
    emitter.beam.branchSpread = 35.0f;

    // 200（半长）+ 0.35 * 400（分支长度）+ 35（横向 spread）+ 3（半宽）
    // + (1 + 2*0.35)*sqrt(3)*2（主弧与分支两端抖动）。
    EXPECT_FLOAT_EQ(
        ComputeParticleEmitterLocalRadius(emitter),
        200.0f + 0.35f * 400.0f + 35.0f + 3.0f +
            (1.0f + 2.0f * 0.35f) * std::sqrt(3.0f) * 2.0f);
}
