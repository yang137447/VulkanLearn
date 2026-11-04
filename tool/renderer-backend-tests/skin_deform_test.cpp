#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <Eigen/Dense>

#include "VertexDataStruct.h"
#include "render/skinning/skinDeformLayout.h"

namespace
{

using VL::Skinning::SkinDeformBufferSizeBytes;
using VL::Skinning::SkinDeformGroupCount;
using VL::Skinning::SkinDeformVertexLayout;
using VL::Skinning::SkinDeformWorkgroupSize;

struct DeformedVertex
{
    Eigen::Vector4f position = Eigen::Vector4f::Zero();
    Eigen::Vector4f previousPosition = Eigen::Vector4f::Zero();
    Eigen::Vector4f normal = Eigen::Vector4f::Zero();
    Eigen::Vector4f tangent = Eigen::Vector4f::Zero();
};

static_assert(sizeof(DeformedVertex) == 64);
static_assert(offsetof(DeformedVertex, position) == SkinDeformVertexLayout::PositionOffsetBytes);
static_assert(offsetof(DeformedVertex, previousPosition) ==
    SkinDeformVertexLayout::PreviousPositionOffsetBytes);
static_assert(offsetof(DeformedVertex, normal) == SkinDeformVertexLayout::NormalOffsetBytes);
static_assert(offsetof(DeformedVertex, tangent) == SkinDeformVertexLayout::TangentOffsetBytes);

float SignOfDeterminant(const Eigen::Matrix3f& linear)
{
    const float determinant = linear.determinant();
    return determinant > 0.0f ? 1.0f : (determinant < 0.0f ? -1.0f : 0.0f);
}

// 与 shader/glsl/engine/skinnedVertexMath.glsl 的 SkinVertex 同式；只用于把
// identity/rotation/translation/multi-influence 的期望值钉在 CPU 侧。
DeformedVertex EvaluateSkinVertex(
    const Eigen::Vector3f& localPosition,
    const Eigen::Vector3f& localNormal,
    const Eigen::Vector4f& localTangent,
    const std::vector<Eigen::Matrix4f>& currentMatrices,
    const std::vector<Eigen::Matrix4f>& previousMatrices,
    const std::vector<uint32_t>& joints,
    const std::vector<float>& weights)
{
    Eigen::Matrix4f current = Eigen::Matrix4f::Zero();
    Eigen::Matrix4f previous = Eigen::Matrix4f::Zero();
    for (size_t influence = 0; influence < joints.size(); ++influence)
    {
        current += weights[influence] * currentMatrices[joints[influence]];
        previous += weights[influence] * previousMatrices[joints[influence]];
    }

    DeformedVertex result;
    result.position = current * localPosition.homogeneous();
    result.previousPosition = previous * localPosition.homogeneous();
    const Eigen::Matrix3f linear = current.block<3, 3>(0, 0);
    result.normal = (linear.inverse().transpose() * localNormal).normalized().homogeneous();
    result.normal.w() = 0.0f;
    Eigen::Vector3f tangent = linear * localTangent.head<3>();
    const Eigen::Vector3f normal = result.normal.head<3>();
    tangent = (tangent - normal * normal.dot(tangent)).normalized();
    result.tangent = Eigen::Vector4f(tangent.x(), tangent.y(), tangent.z(),
        localTangent.w() * SignOfDeterminant(linear));
    return result;
}

void ExpectVectorNear(const Eigen::Vector4f& actual, const Eigen::Vector4f& expected,
    float tolerance = 1e-5f)
{
    EXPECT_NEAR(actual.x(), expected.x(), tolerance);
    EXPECT_NEAR(actual.y(), expected.y(), tolerance);
    EXPECT_NEAR(actual.z(), expected.z(), tolerance);
    EXPECT_NEAR(actual.w(), expected.w(), tolerance);
}

} // namespace

TEST(SkinDeformLayout, StrideAndOffsetsMatchTheShaderContract)
{
    EXPECT_EQ(SkinDeformVertexLayout::PositionOffsetBytes, 0u);
    EXPECT_EQ(SkinDeformVertexLayout::PreviousPositionOffsetBytes, 16u);
    EXPECT_EQ(SkinDeformVertexLayout::NormalOffsetBytes, 32u);
    EXPECT_EQ(SkinDeformVertexLayout::TangentOffsetBytes, 48u);
    EXPECT_EQ(SkinDeformVertexLayout::StrideBytes, 64u);
    EXPECT_EQ(SkinDeformWorkgroupSize, 64u);
}

TEST(SkinDeformLayout, SourceVertexFloatOffsetsMatchSkinnedDeformComp)
{
    // skinnedDeform.comp 的 PositionOffsetFloats/NormalOffsetFloats/TangentOffsetFloats
    // 与 VertexStrideFloats 必须与这里一致，改顶点 ABI 时两处同改。
    EXPECT_EQ(offsetof(Vertex, position) / sizeof(float), 0u);
    EXPECT_EQ(offsetof(Vertex, normal) / sizeof(float), 3u);
    EXPECT_EQ(offsetof(Vertex, tangent) / sizeof(float), 16u);
    EXPECT_EQ(sizeof(Vertex) / sizeof(float), 28u);
}

TEST(SkinDeformLayout, DispatchCountRoundsUpToWorkgroups)
{
    EXPECT_EQ(SkinDeformGroupCount(0), 0u);
    EXPECT_EQ(SkinDeformGroupCount(1), 1u);
    EXPECT_EQ(SkinDeformGroupCount(63), 1u);
    EXPECT_EQ(SkinDeformGroupCount(64), 1u);
    EXPECT_EQ(SkinDeformGroupCount(65), 2u);
    EXPECT_EQ(SkinDeformGroupCount(235000), (235000u + 63u) / 64u);
    for (uint32_t vertexCount = 0; vertexCount < 512; ++vertexCount)
    {
        EXPECT_EQ(SkinDeformGroupCount(vertexCount) * SkinDeformWorkgroupSize >= vertexCount,
            vertexCount != 0 || SkinDeformGroupCount(vertexCount) == 0);
    }
}

TEST(SkinDeformLayout, BufferSizeIsSixtyFourBytesPerVertex)
{
    EXPECT_EQ(SkinDeformBufferSizeBytes(0), 0u);
    EXPECT_EQ(SkinDeformBufferSizeBytes(1), 64u);
    EXPECT_EQ(SkinDeformBufferSizeBytes(235000), 15'040'000u);
    EXPECT_EQ(SkinDeformBufferSizeBytes(1377429), 88'155'456u);
}

TEST(SkinDeformMath, IdentityPosePreservesVertexAndPreviousPosition)
{
    const Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();
    const DeformedVertex result = EvaluateSkinVertex(
        Eigen::Vector3f(1.0f, 2.0f, 3.0f),
        Eigen::Vector3f(0.0f, 0.0f, 1.0f),
        Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f),
        {identity}, {identity}, {0}, {1.0f});

    ExpectVectorNear(result.position, Eigen::Vector4f(1.0f, 2.0f, 3.0f, 1.0f));
    ExpectVectorNear(result.previousPosition, Eigen::Vector4f(1.0f, 2.0f, 3.0f, 1.0f));
    ExpectVectorNear(result.normal, Eigen::Vector4f(0.0f, 0.0f, 1.0f, 0.0f));
    ExpectVectorNear(result.tangent, Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f));
}

TEST(SkinDeformMath, TranslationMovesPositionAndPreviousIndependently)
{
    Eigen::Matrix4f current = Eigen::Matrix4f::Identity();
    current.block<3, 1>(0, 3) = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    Eigen::Matrix4f previous = Eigen::Matrix4f::Identity();
    previous.block<3, 1>(0, 3) = Eigen::Vector3f(5.0f, 0.0f, 0.0f);

    const DeformedVertex result = EvaluateSkinVertex(
        Eigen::Vector3f(1.0f, 0.0f, 0.0f),
        Eigen::Vector3f(0.0f, 0.0f, 1.0f),
        Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f),
        {current}, {previous}, {0}, {1.0f});

    ExpectVectorNear(result.position, Eigen::Vector4f(11.0f, 0.0f, 0.0f, 1.0f));
    ExpectVectorNear(result.previousPosition, Eigen::Vector4f(6.0f, 0.0f, 0.0f, 1.0f));
    ExpectVectorNear(result.normal, Eigen::Vector4f(0.0f, 0.0f, 1.0f, 0.0f));
    ExpectVectorNear(result.tangent, Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f));
}

TEST(SkinDeformMath, RotationRotatesPositionNormalAndTangent)
{
    const float halfSqrt = std::sqrt(0.5f);
    Eigen::Matrix4f rotation = Eigen::Matrix4f::Identity();
    rotation.block<3, 3>(0, 0) << 0.0f, -1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f;

    const DeformedVertex result = EvaluateSkinVertex(
        Eigen::Vector3f(1.0f, 0.0f, 0.0f),
        Eigen::Vector3f(halfSqrt, halfSqrt, 0.0f),
        // 输入 TBN 必须正交：T=(a,-a,0) 与 N=(a,a,0) 垂直，旋转后成为 (a,a,0)。
        Eigen::Vector4f(halfSqrt, -halfSqrt, 0.0f, 1.0f),
        {rotation}, {rotation}, {0}, {1.0f});

    ExpectVectorNear(result.position, Eigen::Vector4f(0.0f, 1.0f, 0.0f, 1.0f));
    ExpectVectorNear(result.previousPosition, Eigen::Vector4f(0.0f, 1.0f, 0.0f, 1.0f));
    ExpectVectorNear(result.normal, Eigen::Vector4f(-halfSqrt, halfSqrt, 0.0f, 0.0f));
    ExpectVectorNear(result.tangent, Eigen::Vector4f(halfSqrt, halfSqrt, 0.0f, 1.0f));
}

TEST(SkinDeformMath, MultiInfluenceBlendsMatricesBeforeTransforming)
{
    Eigen::Matrix4f translation = Eigen::Matrix4f::Identity();
    translation.block<3, 1>(0, 3) = Eigen::Vector3f(10.0f, 0.0f, 0.0f);
    Eigen::Matrix4f identity = Eigen::Matrix4f::Identity();

    // 0.5*T(10) + 0.5*I = x 方向 0.5 缩放 + 平移 5；位置 (1,0,0) -> (6,0,0)。
    const DeformedVertex result = EvaluateSkinVertex(
        Eigen::Vector3f(1.0f, 0.0f, 0.0f),
        Eigen::Vector3f(0.0f, 0.0f, 1.0f),
        Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f),
        {translation, identity}, {translation, identity}, {0, 1}, {0.5f, 0.5f});

    ExpectVectorNear(result.position, Eigen::Vector4f(6.0f, 0.0f, 0.0f, 1.0f));
    ExpectVectorNear(result.previousPosition, Eigen::Vector4f(6.0f, 0.0f, 0.0f, 1.0f));
    ExpectVectorNear(result.normal, Eigen::Vector4f(0.0f, 0.0f, 1.0f, 0.0f));
    // 混合线性部分把切线 x 分量压到 0.5，正交化/归一化后仍为 +X。
    ExpectVectorNear(result.tangent, Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f));
}

TEST(SkinDeformMath, NormalUsesInverseTransposeNotTheLinearPart)
{
    Eigen::Matrix3f rotation;
    rotation << 0.0f, -1.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f;
    const Eigen::Matrix3f scale = Eigen::Vector3f(2.0f, 1.0f, 1.0f).asDiagonal();
    Eigen::Matrix4f linearTransform = Eigen::Matrix4f::Identity();
    linearTransform.block<3, 3>(0, 0) = rotation * scale;

    const float halfSqrt = std::sqrt(0.5f);
    const DeformedVertex result = EvaluateSkinVertex(
        Eigen::Vector3f(0.0f, 0.0f, 0.0f),
        Eigen::Vector3f(halfSqrt, halfSqrt, 0.0f),
        Eigen::Vector4f(1.0f, 0.0f, 0.0f, 1.0f),
        {linearTransform}, {linearTransform}, {0}, {1.0f});

    // 逆转置：(Rz90 * S(2,1,1))^-T * n = Rz90 * S^-1 * n。
    ExpectVectorNear(result.normal,
        Eigen::Vector4f(-0.89442718f, 0.44721359f, 0.0f, 0.0f), 1e-4f);
}

TEST(SkinDeformMath, MirroredAffineFlipsMikkHandedness)
{
    Eigen::Matrix4f mirror = Eigen::Matrix4f::Identity();
    mirror(0, 0) = -1.0f;

    const DeformedVertex result = EvaluateSkinVertex(
        Eigen::Vector3f(1.0f, 0.0f, 0.0f),
        Eigen::Vector3f(1.0f, 0.0f, 0.0f),
        Eigen::Vector4f(0.0f, 1.0f, 0.0f, 1.0f),
        {mirror}, {mirror}, {0}, {1.0f});

    ExpectVectorNear(result.position, Eigen::Vector4f(-1.0f, 0.0f, 0.0f, 1.0f));
    ExpectVectorNear(result.normal, Eigen::Vector4f(-1.0f, 0.0f, 0.0f, 0.0f));
    ExpectVectorNear(result.tangent, Eigen::Vector4f(0.0f, 1.0f, 0.0f, -1.0f));
}
