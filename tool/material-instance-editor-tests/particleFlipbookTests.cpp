#include <vector>

#include <gtest/gtest.h>

#include "particles/particleAsset.h"
#include "particles/particleFlipbook.h"

namespace
{

using namespace VL;

ParticleEmitterAsset EmitterWithFrames(
    uint32_t startFrame,
    uint32_t endFrame)
{
    ParticleEmitterAsset emitter;
    emitter.flipbookColumns = 4;
    emitter.flipbookRows = 4;
    emitter.flipbookStartFrame = startFrame;
    emitter.flipbookEndFrame = endFrame;
    return emitter;
}

} // namespace

TEST(ParticleFlipbook, PacksDurationsAndPivotsIntoVec4Slots)
{
    ParticleEmitterAsset emitter = EmitterWithFrames(0, 2);
    emitter.flipbookFrameDurations = {0.1f, 0.2f, 0.3f};
    emitter.flipbookFramePivots = {
        Eigen::Vector2f(0.25f, 0.75f),
        Eigen::Vector2f(0.5f, 0.5f)};

    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);

    EXPECT_FLOAT_EQ(table.flags.x(), 1.0f);
    EXPECT_FLOAT_EQ(table.flags.y(), 1.0f);
    EXPECT_FLOAT_EQ(table.flags.z(), 3.0f);
    EXPECT_FLOAT_EQ(table.durations[0].x(), 0.1f);
    EXPECT_FLOAT_EQ(table.durations[0].y(), 0.2f);
    EXPECT_FLOAT_EQ(table.durations[0].z(), 0.3f);
    EXPECT_FLOAT_EQ(table.durations[0].w(), 0.0f);
    EXPECT_FLOAT_EQ(table.durations[1].x(), 0.0f);
    EXPECT_FLOAT_EQ(table.pivots[0].x(), 0.25f);
    EXPECT_FLOAT_EQ(table.pivots[0].y(), 0.75f);
    EXPECT_FLOAT_EQ(table.pivots[0].z(), 0.5f);
    EXPECT_FLOAT_EQ(table.pivots[0].w(), 0.5f);
}

TEST(ParticleFlipbook, EmptyTablesLeaveFlagsCleared)
{
    const ParticleEmitterAsset emitter = EmitterWithFrames(0, 15);

    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);

    EXPECT_FLOAT_EQ(table.flags.x(), 0.0f);
    EXPECT_FLOAT_EQ(table.flags.y(), 0.0f);
    EXPECT_FLOAT_EQ(table.flags.z(), 16.0f);
    for (const Eigen::Vector4f& packed : table.durations)
        EXPECT_FLOAT_EQ(packed.squaredNorm(), 0.0f);
    for (const Eigen::Vector4f& packed : table.pivots)
        EXPECT_FLOAT_EQ(packed.squaredNorm(), 0.0f);
}

TEST(ParticleFlipbook, PacksSubRangeAtLocalIndices)
{
    ParticleEmitterAsset emitter = EmitterWithFrames(4, 5);
    emitter.flipbookFrameDurations = {0.4f, 0.6f};

    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);

    EXPECT_FLOAT_EQ(table.flags.z(), 2.0f);
    EXPECT_FLOAT_EQ(table.durations[0].x(), 0.4f);
    EXPECT_FLOAT_EQ(table.durations[0].y(), 0.6f);
}

TEST(ParticleFlipbook, FullTableStaysInBounds)
{
    ParticleEmitterAsset emitter = EmitterWithFrames(0, 255);
    emitter.flipbookColumns = 16;
    emitter.flipbookRows = 16;
    emitter.flipbookFrameDurations.assign(kMaxParticleFlipbookFrames, 0.05f);
    emitter.flipbookFrameDurations.back() = 0.5f;
    emitter.flipbookFramePivots.assign(
        kMaxParticleFlipbookFrames, Eigen::Vector2f(0.5f, 0.35f));

    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);

    EXPECT_FLOAT_EQ(table.flags.z(), 256.0f);
    EXPECT_FLOAT_EQ(
        table.durations[kParticleFlipbookDurationVectors - 1].w(), 0.5f);
    EXPECT_FLOAT_EQ(
        table.pivots[kParticleFlipbookPivotVectors - 1].z(), 0.5f);
    EXPECT_FLOAT_EQ(
        table.pivots[kParticleFlipbookPivotVectors - 1].w(), 0.35f);
}

TEST(ParticleFlipbook, RebuildPicksUpChangedDurationsAndPivots)
{
    ParticleEmitterAsset emitter = EmitterWithFrames(0, 1);
    emitter.flipbookFrameDurations = {0.1f, 0.2f};
    emitter.flipbookFramePivots = {
        Eigen::Vector2f(0.5f, 0.5f),
        Eigen::Vector2f(0.5f, 0.5f)};
    const ParticleFlipbookGpuTable first =
        BuildParticleFlipbookGpuTable(emitter);
    EXPECT_FLOAT_EQ(first.durations[0].x(), 0.1f);
    EXPECT_FLOAT_EQ(first.pivots[0].x(), 0.5f);

    // Synchronize 的“同 key、参数变化”分支依赖重新构建派生表，
    // 这里直接覆盖该重建路径的数据语义。
    emitter.flipbookFrameDurations = {0.7f, 0.9f};
    emitter.flipbookFramePivots = {
        Eigen::Vector2f(0.25f, 0.75f),
        Eigen::Vector2f(0.5f, 0.25f)};
    const ParticleFlipbookGpuTable second =
        BuildParticleFlipbookGpuTable(emitter);

    EXPECT_FLOAT_EQ(first.durations[0].x(), 0.1f);
    EXPECT_FLOAT_EQ(second.durations[0].x(), 0.7f);
    EXPECT_FLOAT_EQ(second.durations[0].y(), 0.9f);
    EXPECT_FLOAT_EQ(second.pivots[0].x(), 0.25f);
    EXPECT_FLOAT_EQ(second.pivots[0].y(), 0.75f);
    EXPECT_FLOAT_EQ(second.pivots[0].z(), 0.5f);
    EXPECT_FLOAT_EQ(second.pivots[0].w(), 0.25f);

    emitter.flipbookFrameDurations.clear();
    emitter.flipbookFramePivots.clear();
    const ParticleFlipbookGpuTable cleared =
        BuildParticleFlipbookGpuTable(emitter);
    EXPECT_FLOAT_EQ(cleared.flags.x(), 0.0f);
    EXPECT_FLOAT_EQ(cleared.flags.y(), 0.0f);
    EXPECT_FLOAT_EQ(cleared.durations[0].squaredNorm(), 0.0f);
    EXPECT_FLOAT_EQ(cleared.pivots[0].squaredNorm(), 0.0f);
}

TEST(ParticleFlipbook, DurationsAreWeightsWithFpsDefiningCycleLength)
{
    const float weights[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ParticleFlipbookPhaseInput input;
    input.weights = weights;
    input.frameCount = 4;
    input.framesPerSecond = 2.0f;
    input.loop = true;
    input.elapsedSeconds = 1.0f;

    // fps=2、4 帧 => 一轮 2 秒；1 秒走到权重 2.0，命中第 3 帧。
    const float phase =
        ComputeParticleFlipbookPhaseWeight(input);
    EXPECT_FLOAT_EQ(phase, 2.0f);
    EXPECT_EQ(SelectParticleFlipbookFrame(weights, 4, phase), 2u);

    // fps 数值仍有效：fps=4 时 1 秒刚好走完整轮并回绕到第 1 帧。
    input.framesPerSecond = 4.0f;
    const float wrapped =
        ComputeParticleFlipbookPhaseWeight(input);
    EXPECT_FLOAT_EQ(wrapped, 0.0f);
    EXPECT_EQ(SelectParticleFlipbookFrame(weights, 4, wrapped), 0u);
}

TEST(ParticleFlipbook, LoopFalseClampsToLastFrame)
{
    const float weights[] = {1.0f, 1.0f, 1.0f};
    ParticleFlipbookPhaseInput input;
    input.weights = weights;
    input.frameCount = 3;
    input.framesPerSecond = 3.0f;
    input.loop = false;
    input.elapsedSeconds = 5.0f;

    const float phase =
        ComputeParticleFlipbookPhaseWeight(input);
    EXPECT_FLOAT_EQ(phase, 3.0f);
    EXPECT_EQ(SelectParticleFlipbookFrame(weights, 3, phase), 2u);
}

TEST(ParticleFlipbook, RandomStartUsesFractionalSeed)
{
    const float weights[] = {1.0f, 1.0f, 1.0f, 1.0f};
    ParticleFlipbookPhaseInput input;
    input.weights = weights;
    input.frameCount = 4;
    input.framesPerSecond = 1.0f;
    input.loop = true;
    input.randomStartFrame = true;
    input.randomSeed01 = 0.6f;

    const float phase =
        ComputeParticleFlipbookPhaseWeight(input);
    EXPECT_FLOAT_EQ(phase, 2.4f);
    EXPECT_EQ(SelectParticleFlipbookFrame(weights, 4, phase), 2u);

    input.randomSeed01 = 0.1f;
    EXPECT_EQ(
        SelectParticleFlipbookFrame(
            weights, 4,
            ComputeParticleFlipbookPhaseWeight(input)),
        0u);
}

TEST(ParticleFlipbook, LifeDrivenWeightsCoverOneCycle)
{
    const float weights[] = {1.0f, 3.0f};
    ParticleFlipbookPhaseInput input;
    input.weights = weights;
    input.frameCount = 2;
    input.framesPerSecond = 0.0f;
    input.lifeT = 0.5f;

    const float phase =
        ComputeParticleFlipbookPhaseWeight(input);
    EXPECT_FLOAT_EQ(phase, 2.0f);
    EXPECT_EQ(SelectParticleFlipbookFrame(weights, 2, phase), 1u);
}

TEST(ParticleFlipbook, PacksPerFrameUvRectsAndSetsFlag)
{
    ParticleEmitterAsset emitter = EmitterWithFrames(0, 2);
    emitter.flipbookFrameUVs = {
        Eigen::Vector4f(0.0f, 0.0f, 0.5f, 1.0f),
        Eigen::Vector4f(0.5f, 0.0f, 1.0f, 1.0f),
        Eigen::Vector4f(0.25f, 0.25f, 0.75f, 0.75f)};

    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);

    EXPECT_FLOAT_EQ(table.flags.w(), 1.0f);
    EXPECT_FLOAT_EQ(table.uvs[0].x(), 0.0f);
    EXPECT_FLOAT_EQ(table.uvs[0].z(), 0.5f);
    EXPECT_FLOAT_EQ(table.uvs[1].x(), 0.5f);
    EXPECT_FLOAT_EQ(table.uvs[2].y(), 0.25f);
    EXPECT_FLOAT_EQ(table.uvs[2].w(), 0.75f);
    EXPECT_FLOAT_EQ(table.uvs[3].squaredNorm(), 0.0f);
}

TEST(ParticleFlipbook, EmptyUvTableLeavesFlagCleared)
{
    const ParticleEmitterAsset emitter = EmitterWithFrames(0, 1);
    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);
    EXPECT_FLOAT_EQ(table.flags.w(), 0.0f);
    for (const Eigen::Vector4f& packed : table.uvs)
        EXPECT_FLOAT_EQ(packed.squaredNorm(), 0.0f);
}

TEST(ParticleFlipbook, SpriteUvMappingMatchesShaderMix)
{
    const Eigen::Vector4f rect(0.25f, 0.1f, 0.75f, 0.9f);

    // 左下/右上角映射到 rect 的两个角，中心映射到 rect 中心。
    const Eigen::Vector2f bottomLeft =
        MapParticleFlipbookSpriteUV(rect, Eigen::Vector2f(0.0f, 0.0f));
    const Eigen::Vector2f topRight =
        MapParticleFlipbookSpriteUV(rect, Eigen::Vector2f(1.0f, 1.0f));
    const Eigen::Vector2f center =
        MapParticleFlipbookSpriteUV(rect, Eigen::Vector2f(0.5f, 0.5f));
    EXPECT_FLOAT_EQ(bottomLeft.x(), 0.25f);
    EXPECT_FLOAT_EQ(bottomLeft.y(), 0.1f);
    EXPECT_FLOAT_EQ(topRight.x(), 0.75f);
    EXPECT_FLOAT_EQ(topRight.y(), 0.9f);
    EXPECT_FLOAT_EQ(center.x(), 0.5f);
    EXPECT_FLOAT_EQ(center.y(), 0.5f);

    // 无 rect 时整张 sprite 就是原 UV。
    const Eigen::Vector2f full =
        MapParticleFlipbookSpriteUV(
            Eigen::Vector4f(0.0f, 0.0f, 1.0f, 1.0f),
            Eigen::Vector2f(0.3f, 0.8f));
    EXPECT_FLOAT_EQ(full.x(), 0.3f);
    EXPECT_FLOAT_EQ(full.y(), 0.8f);
}

TEST(ParticleFlipbook, PacksFrameRotationsFourPerVec4AndSetsFlag)
{
    ParticleEmitterAsset emitter = EmitterWithFrames(0, 5);
    emitter.flipbookFrameRotations = {
        0.0f, 15.0f, -30.0f, 45.0f, 90.0f, -90.0f};

    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);

    EXPECT_FLOAT_EQ(table.rotationFlags.x(), 1.0f);
    EXPECT_FLOAT_EQ(table.rotations[0].x(), 0.0f);
    EXPECT_FLOAT_EQ(table.rotations[0].y(), 15.0f);
    EXPECT_FLOAT_EQ(table.rotations[0].z(), -30.0f);
    EXPECT_FLOAT_EQ(table.rotations[0].w(), 45.0f);
    EXPECT_FLOAT_EQ(table.rotations[1].x(), 90.0f);
    EXPECT_FLOAT_EQ(table.rotations[1].y(), -90.0f);
    EXPECT_FLOAT_EQ(table.rotations[1].z(), 0.0f);
}

TEST(ParticleFlipbook, EmptyRotationTableClearsFlag)
{
    const ParticleEmitterAsset emitter = EmitterWithFrames(0, 15);
    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);
    EXPECT_FLOAT_EQ(table.rotationFlags.x(), 0.0f);
    for (const Eigen::Vector4f& packed : table.rotations)
        EXPECT_FLOAT_EQ(packed.squaredNorm(), 0.0f);
}

TEST(ParticleFlipbook, RotationTableUsesLocalFrameIndices)
{
    ParticleEmitterAsset emitter = EmitterWithFrames(4, 5);
    emitter.flipbookFrameRotations = {10.0f, 20.0f};
    const ParticleFlipbookGpuTable table =
        BuildParticleFlipbookGpuTable(emitter);
    // 表按区间内 0..1 写入，而不是 atlas 绝对帧 4..5。
    EXPECT_FLOAT_EQ(table.rotations[0].x(), 10.0f);
    EXPECT_FLOAT_EQ(table.rotations[0].y(), 20.0f);
    EXPECT_FLOAT_EQ(table.rotations[0].z(), 0.0f);
}
