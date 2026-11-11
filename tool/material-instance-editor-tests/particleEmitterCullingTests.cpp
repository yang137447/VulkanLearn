#include <array>
#include <cmath>

#include <gtest/gtest.h>

#include "particles/particleEmitterCulling.h"

namespace
{

using namespace VL;

Eigen::Matrix4f PerspectiveMatrix(
    float fovYDegrees,
    float aspect,
    float nearPlane,
    float farPlane)
{
    const float focal =
        1.0f / std::tan(fovYDegrees * 3.14159265358979323846f / 360.0f);
    const float depthRange = farPlane - nearPlane;
    Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
    projection(0, 0) = focal / aspect;
    projection(1, 1) = focal;
    // Vulkan 右手系 0..1 深度：near 映射到 0，far 映射到 1。
    projection(2, 2) = -farPlane / depthRange;
    projection(2, 3) = -nearPlane * farPlane / depthRange;
    projection(3, 2) = -1.0f;
    return projection;
}

float ProjectedDepth(
    const Eigen::Matrix4f& viewProjection,
    float viewZ)
{
    const Eigen::Vector4f clip =
        viewProjection * Eigen::Vector4f(0.0f, 0.0f, viewZ, 1.0f);
    return clip.z() / clip.w();
}

ParticleEmitterCullingInput MakeEmitter(
    const Eigen::Vector3f& center,
    float radius)
{
    ParticleEmitterCullingInput emitter;
    emitter.worldCenter = center;
    emitter.worldRadius = radius;
    return emitter;
}

const std::array<Eigen::Vector4f, 6> kNoFrustum = {};

ParticleEmitterCullingResult CullWithoutFrustum(
    const ParticleEmitterCullingInput& emitter,
    const Eigen::Vector3f& cameraForward =
        Eigen::Vector3f(0.0f, 0.0f, -1.0f),
    float projectionScaleY = 1.0f,
    float viewportHeightPixels = 720.0f)
{
    return CullParticleEmitter(
        emitter,
        kNoFrustum,
        Eigen::Vector3f::Zero(),
        cameraForward,
        projectionScaleY,
        viewportHeightPixels);
}

} // namespace

TEST(ParticleEmitterCulling, PerspectiveFrustumRejectsOutsideSpheres)
{
    const Eigen::Matrix4f viewProjection =
        PerspectiveMatrix(60.0f, 1.0f, 1.0f, 100.0f);
    const auto planes = BuildParticleFrustumPlanes(viewProjection);
    const Eigen::Vector3f cameraPosition = Eigen::Vector3f::Zero();
    const Eigen::Vector3f cameraForward =
        Eigen::Vector3f(0.0f, 0.0f, -1.0f);

    EXPECT_NEAR(ProjectedDepth(viewProjection, -1.0f), 0.0f, 1.0e-6f);
    EXPECT_NEAR(
        ProjectedDepth(viewProjection, -100.0f), 1.0f, 1.0e-6f);

    EXPECT_TRUE(CullParticleEmitter(
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -5.0f), 0.5f),
        planes, cameraPosition, cameraForward, 1.0f, 720.0f).visible);
    EXPECT_TRUE(CullParticleEmitter(
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -1.01f), 0.01f),
        planes, cameraPosition, cameraForward, 1.0f, 720.0f).visible);
    EXPECT_FALSE(CullParticleEmitter(
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -0.5f), 0.05f),
        planes, cameraPosition, cameraForward, 1.0f, 720.0f).visible);
    EXPECT_FALSE(CullParticleEmitter(
        MakeEmitter(Eigen::Vector3f(-100.0f, 0.0f, -5.0f), 0.5f),
        planes, cameraPosition, cameraForward, 1.0f, 720.0f).visible);
    EXPECT_FALSE(CullParticleEmitter(
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, 5.0f), 0.5f),
        planes, cameraPosition, cameraForward, 1.0f, 720.0f).visible);
    EXPECT_FALSE(CullParticleEmitter(
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -101.0f), 0.5f),
        planes, cameraPosition, cameraForward, 1.0f, 720.0f).visible);
}

TEST(ParticleEmitterCulling, SphereCrossingPlaneIsKept)
{
    const Eigen::Matrix4f viewProjection =
        PerspectiveMatrix(60.0f, 1.0f, 1.0f, 100.0f);
    const auto planes = BuildParticleFrustumPlanes(viewProjection);

    EXPECT_TRUE(CullParticleEmitter(
        MakeEmitter(Eigen::Vector3f(-3.1f, 0.0f, -5.0f), 0.5f),
        planes,
        Eigen::Vector3f::Zero(),
        Eigen::Vector3f(0.0f, 0.0f, -1.0f),
        1.0f,
        720.0f).visible);
}

TEST(ParticleEmitterCulling, MaxDrawDistanceUsesNearestBoundsPoint)
{
    ParticleEmitterCullingInput emitter =
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -1000.0f), 900.0f);
    emitter.maxDrawDistance = 200.0f;
    EXPECT_TRUE(CullWithoutFrustum(emitter).visible);

    emitter.worldRadius = 799.0f;
    const ParticleEmitterCullingResult rejected =
        CullWithoutFrustum(emitter);
    EXPECT_FALSE(rejected.visible);
    EXPECT_FLOAT_EQ(rejected.opacityScale, 0.0f);
}

TEST(ParticleEmitterCulling, DistanceFadeRampsAndClamps)
{
    ParticleEmitterCullingInput emitter =
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -1200.0f), 200.0f);
    emitter.maxDrawDistance = 1000.0f;
    emitter.fadeDistance = 200.0f;

    EXPECT_NEAR(CullWithoutFrustum(emitter).opacityScale, 0.0f, 1.0e-6f);

    emitter.worldCenter.z() = -1100.0f;
    EXPECT_NEAR(CullWithoutFrustum(emitter).opacityScale, 0.5f, 1.0e-6f);

    emitter.worldCenter.z() = -1000.0f;
    EXPECT_NEAR(CullWithoutFrustum(emitter).opacityScale, 1.0f, 1.0e-6f);

    emitter.worldCenter.z() = -900.0f;
    EXPECT_NEAR(CullWithoutFrustum(emitter).opacityScale, 1.0f, 1.0e-6f);

    emitter.worldCenter.z() = -1200.0f;
    const ParticleEmitterCullingResult atThreshold =
        CullWithoutFrustum(emitter);
    EXPECT_TRUE(atThreshold.visible);
    EXPECT_NEAR(atThreshold.opacityScale, 0.0f, 1.0e-6f);
}

TEST(ParticleEmitterCulling, MinScreenSizeUsesProjectedDiameter)
{
    ParticleEmitterCullingInput emitter =
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -1000.0f), 10.0f);
    emitter.minScreenSize = 7.1f;
    EXPECT_TRUE(CullWithoutFrustum(emitter).visible);

    emitter.minScreenSize = 7.3f;
    EXPECT_FALSE(CullWithoutFrustum(emitter).visible);

    emitter.minScreenSize = 0.0f;
    EXPECT_TRUE(CullWithoutFrustum(emitter).visible);
}

TEST(ParticleEmitterCulling, InvalidNegativeValuesAreSafe)
{
    ParticleEmitterCullingInput emitter =
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, -10.0f), -5.0f);
    emitter.maxDrawDistance = -20.0f;
    emitter.fadeDistance = -5.0f;
    emitter.minScreenSize = -1.0f;

    const ParticleEmitterCullingResult result =
        CullWithoutFrustum(emitter);
    EXPECT_TRUE(result.visible);
    EXPECT_FLOAT_EQ(result.opacityScale, 1.0f);
}

TEST(ParticleEmitterCulling, ZeroForwardSkipsMinScreenTest)
{
    ParticleEmitterCullingInput emitter =
        MakeEmitter(Eigen::Vector3f(0.0f, 0.0f, 10.0f), 1.0f);
    emitter.minScreenSize = 1000.0f;

    EXPECT_TRUE(CullWithoutFrustum(
        emitter,
        Eigen::Vector3f::Zero()).visible);
}
