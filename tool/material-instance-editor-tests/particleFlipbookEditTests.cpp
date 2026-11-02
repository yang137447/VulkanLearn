#include <cmath>
#include <limits>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "editor/particles/particleFlipbookEdit.h"

namespace
{

using Json = nlohmann::json;
using namespace VL::ParticleFlipbookEdit;

Json EmitterWithRange(
    size_t startFrame,
    size_t endFrame,
    size_t columns = 4,
    size_t rows = 4)
{
    return Json{
        {"flipbook", Json{
            {"columns", columns},
            {"rows", rows},
            {"startFrame", startFrame},
            {"endFrame", endFrame}}}};
}

} // namespace

TEST(ParticleFlipbookEdit, FrameCountUsesExplicitRangeAndAtlasDefault)
{
    EXPECT_EQ(FlipbookFrameCount(EmitterWithRange(2, 5)), 4u);
    EXPECT_EQ(FlipbookFrameCount(Json{
        {"flipbook", Json{{"columns", 4}, {"rows", 4},
            {"startFrame", 3}}}}), 13u);
    EXPECT_EQ(FlipbookFrameCount(EmitterWithRange(5, 2)), 0u);
    EXPECT_EQ(FlipbookFrameCount(Json{
        {"flipbook", Json{{"startFrame", 0.5}, {"endFrame", 2}}}}), 3u);
}

TEST(ParticleFlipbookEdit, MissingTablesProduceCompleteDefaults)
{
    const Json emitter = EmitterWithRange(0, 2);
    EXPECT_FALSE(HasFlipbookDurations(emitter));
    EXPECT_FALSE(HasFlipbookPivots(emitter));

    const Json durations = EnsureFlipbookDurations(emitter);
    const Json pivots = EnsureFlipbookPivots(emitter);
    ASSERT_TRUE(durations.is_array());
    ASSERT_TRUE(pivots.is_array());
    ASSERT_EQ(durations.size(), 3u);
    ASSERT_EQ(pivots.size(), 3u);
    for (const Json& value : durations)
        EXPECT_FLOAT_EQ(value.get<float>(), 1.0f);
    for (const Json& value : pivots)
    {
        EXPECT_FLOAT_EQ(value.at(0).get<float>(), 0.5f);
        EXPECT_FLOAT_EQ(value.at(1).get<float>(), 0.5f);
    }
}

TEST(ParticleFlipbookEdit, ExistingTablesArePaddedAndTruncated)
{
    Json emitter = EmitterWithRange(0, 3);
    emitter["flipbook"]["frameDurations"] =
        Json::array({2.0f, 0.0f, 4.0f, 8.0f, 16.0f});
    emitter["flipbook"]["framePivots"] = Json::array({
        Json::array({0.2f, 0.8f}),
        Json::array({-1.0f, 2.0f})});

    const Json durations = EnsureFlipbookDurations(emitter);
    const Json pivots = EnsureFlipbookPivots(emitter);
    ASSERT_EQ(durations.size(), 4u);
    EXPECT_FLOAT_EQ(durations.at(0).get<float>(), 2.0f);
    EXPECT_FLOAT_EQ(durations.at(1).get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(durations.at(2).get<float>(), 4.0f);
    EXPECT_FLOAT_EQ(durations.at(3).get<float>(), 8.0f);
    ASSERT_EQ(pivots.size(), 4u);
    EXPECT_FLOAT_EQ(pivots.at(0).at(0).get<float>(), 0.2f);
    EXPECT_FLOAT_EQ(pivots.at(0).at(1).get<float>(), 0.8f);
    EXPECT_FLOAT_EQ(pivots.at(1).at(0).get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(pivots.at(1).at(1).get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(pivots.at(2).at(0).get<float>(), 0.5f);
}

TEST(ParticleFlipbookEdit, DurationSetterRejectsNonPositiveAndNonFinite)
{
    Json durations = UniformFlipbookDurations(2);
    EXPECT_TRUE(SetFlipbookDuration(durations, 0, 3.5f));
    EXPECT_FLOAT_EQ(durations.at(0).get<float>(), 3.5f);
    EXPECT_FALSE(SetFlipbookDuration(durations, 0, 0.0f));
    EXPECT_FALSE(SetFlipbookDuration(durations, 0, -1.0f));
    EXPECT_FALSE(SetFlipbookDuration(
        durations, 0, std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(SetFlipbookDuration(durations, 2, 1.0f));
    EXPECT_FLOAT_EQ(durations.at(0).get<float>(), 3.5f);
}

TEST(ParticleFlipbookEdit, PivotSetterClampsAndSelectsComponent)
{
    Json pivots = UniformFlipbookPivots(1);
    EXPECT_TRUE(SetFlipbookPivot(pivots, 0, 'u', -0.5f));
    EXPECT_TRUE(SetFlipbookPivot(pivots, 0, 'V', 2.0f));
    EXPECT_FLOAT_EQ(pivots.at(0).at(0).get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(pivots.at(0).at(1).get<float>(), 1.0f);
    EXPECT_FALSE(SetFlipbookPivot(
        pivots, 0, 'x', 0.5f));
    EXPECT_FALSE(SetFlipbookPivot(
        pivots, 0, 'u', std::numeric_limits<float>::quiet_NaN()));
}

TEST(ParticleFlipbookEdit, UniformArraysUseRequestedFrameCount)
{
    const Json durations = UniformFlipbookDurations(4, 2.5f);
    const Json pivots = UniformFlipbookPivots(4, 0.25f, 0.75f);
    ASSERT_EQ(durations.size(), 4u);
    ASSERT_EQ(pivots.size(), 4u);
    EXPECT_FLOAT_EQ(durations.at(3).get<float>(), 2.5f);
    EXPECT_FLOAT_EQ(pivots.at(3).at(0).get<float>(), 0.25f);
    EXPECT_FLOAT_EQ(pivots.at(3).at(1).get<float>(), 0.75f);
    EXPECT_TRUE(UniformFlipbookDurations(0).is_null());
    EXPECT_TRUE(UniformFlipbookPivots(kMaxFrames + 1).is_null());
}

TEST(ParticleFlipbookEdit, InvalidFrameSpanDoesNotAllocateTable)
{
    const Json oversized = EmitterWithRange(0, kMaxFrames, 16, 16);
    EXPECT_EQ(FlipbookFrameCount(oversized), kMaxFrames + 1);
    EXPECT_TRUE(EnsureFlipbookDurations(oversized).is_null());
    EXPECT_TRUE(EnsureFlipbookPivots(oversized).is_null());
}

TEST(ParticleFlipbookEdit, MissingUvTableDefaultsToFullSpriteRect)
{
    const Json emitter = EmitterWithRange(2, 4);
    EXPECT_FALSE(HasFlipbookUVs(emitter));

    const Json uvs = EnsureFlipbookUVs(emitter);
    ASSERT_TRUE(uvs.is_array());
    ASSERT_EQ(uvs.size(), 3u);
    for (const Json& rect : uvs)
    {
        ASSERT_EQ(rect.size(), 4u);
        EXPECT_FLOAT_EQ(rect.at(0).get<float>(), 0.0f);
        EXPECT_FLOAT_EQ(rect.at(1).get<float>(), 0.0f);
        EXPECT_FLOAT_EQ(rect.at(2).get<float>(), 1.0f);
        EXPECT_FLOAT_EQ(rect.at(3).get<float>(), 1.0f);
    }
}

TEST(ParticleFlipbookEdit, UvTablePreservesExistingRectsAndRepairsInvalidOnes)
{
    Json emitter = EmitterWithRange(0, 2);
    emitter["flipbook"]["frameUVs"] = Json::array({
        Json::array({0.0, 0.0, 0.5, 1.0}),
        Json::array({0.5, 0.5, 0.25, 0.75}),
        Json::array({0.5, 0.0})});

    EXPECT_TRUE(HasFlipbookUVs(emitter));
    const Json uvs = EnsureFlipbookUVs(emitter);
    ASSERT_EQ(uvs.size(), 3u);
    EXPECT_FLOAT_EQ(uvs.at(0).at(2).get<float>(), 0.5f);
    // 反向/短 rect 回退到完整 sprite 矩形，避免把非法数据带进编辑器。
    EXPECT_FLOAT_EQ(uvs.at(1).at(0).get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(uvs.at(1).at(2).get<float>(), 1.0f);
    EXPECT_FLOAT_EQ(uvs.at(2).at(3).get<float>(), 1.0f);
}

TEST(ParticleFlipbookEdit, UvSetterClampsAndRejectsReversedRect)
{
    Json uvs = UniformFlipbookUVs(1);
    EXPECT_TRUE(SetFlipbookUV(uvs, 0, "u1", 0.6f));
    EXPECT_FLOAT_EQ(uvs.at(0).at(2).get<float>(), 0.6f);
    EXPECT_TRUE(SetFlipbookUV(uvs, 0, "u0", -0.5f));
    EXPECT_FLOAT_EQ(uvs.at(0).at(0).get<float>(), 0.0f);
    EXPECT_TRUE(SetFlipbookUV(uvs, 0, "V0", 0.2f));
    EXPECT_FLOAT_EQ(uvs.at(0).at(1).get<float>(), 0.2f);

    // v1 <= v0 或 u1 <= u0 时必须拒绝，而不是提交非法 rect。
    EXPECT_FALSE(SetFlipbookUV(uvs, 0, "v1", 0.1f));
    EXPECT_FALSE(SetFlipbookUV(uvs, 0, "u1", 0.0f));
    EXPECT_FALSE(SetFlipbookUV(
        uvs, 0, "u0", std::numeric_limits<float>::quiet_NaN()));
    EXPECT_FALSE(SetFlipbookUV(uvs, 1, "u0", 0.5f));
    EXPECT_FALSE(SetFlipbookUV(uvs, 0, "x", 0.5f));
}

TEST(ParticleFlipbookEdit, UniformUvRectsRespectFrameLimit)
{
    const Json uvs = UniformFlipbookUVs(4, 0.25f, 0.1f, 0.75f, 0.9f);
    ASSERT_EQ(uvs.size(), 4u);
    EXPECT_FLOAT_EQ(uvs.at(3).at(0).get<float>(), 0.25f);
    EXPECT_FLOAT_EQ(uvs.at(3).at(3).get<float>(), 0.9f);
    EXPECT_TRUE(UniformFlipbookUVs(0).is_null());
    EXPECT_TRUE(UniformFlipbookUVs(kMaxUvFrames + 1).is_null());
    EXPECT_TRUE(UniformFlipbookUVs(2, 0.5f, 0.0f, 0.5f, 1.0f).is_null());
}

TEST(ParticleFlipbookEdit, MissingRotationTableDefaultsToZeroDegrees)
{
    const Json emitter = EmitterWithRange(2, 4);
    EXPECT_FALSE(HasFlipbookRotations(emitter));
    const Json rotations = EnsureFlipbookRotations(emitter);
    ASSERT_TRUE(rotations.is_array());
    ASSERT_EQ(rotations.size(), 3u);
    for (const Json& value : rotations)
        EXPECT_FLOAT_EQ(value.get<float>(), 0.0f);
}

TEST(ParticleFlipbookEdit, RotationTablePreservesAndRepairsEntries)
{
    Json emitter = EmitterWithRange(0, 2);
    emitter["flipbook"]["frameRotations"] =
        Json::array({15.0f, "spin", 90.0f, 180.0f});

    EXPECT_TRUE(HasFlipbookRotations(emitter));
    const Json rotations = EnsureFlipbookRotations(emitter);
    ASSERT_EQ(rotations.size(), 3u);
    EXPECT_FLOAT_EQ(rotations.at(0).get<float>(), 15.0f);
    EXPECT_FLOAT_EQ(rotations.at(1).get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(rotations.at(2).get<float>(), 90.0f);
}

TEST(ParticleFlipbookEdit, RotationSetterKeepsSignedAngles)
{
    Json rotations = UniformFlipbookRotations(2, 0.0f);
    EXPECT_TRUE(SetFlipbookRotation(rotations, 0, -45.5f));
    EXPECT_FLOAT_EQ(rotations.at(0).get<float>(), -45.5f);
    EXPECT_FALSE(SetFlipbookRotation(
        rotations, 0, std::numeric_limits<float>::infinity()));
    EXPECT_FALSE(SetFlipbookRotation(rotations, 2, 10.0f));
    EXPECT_FLOAT_EQ(rotations.at(0).get<float>(), -45.5f);
}

TEST(ParticleFlipbookEdit, UniformRotationsRespectFrameLimit)
{
    const Json rotations = UniformFlipbookRotations(4, 12.5f);
    ASSERT_EQ(rotations.size(), 4u);
    EXPECT_FLOAT_EQ(rotations.at(3).get<float>(), 12.5f);
    EXPECT_TRUE(UniformFlipbookRotations(0).is_null());
    EXPECT_TRUE(
        UniformFlipbookRotations(kMaxRotationFrames + 1).is_null());
}
