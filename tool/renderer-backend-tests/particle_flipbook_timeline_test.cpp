#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadSource(const std::string& path)
{
    std::ifstream input(std::string(VL_TEST_SOURCE_ROOT) + "/" + path);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

bool Contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

} // namespace

TEST(ParticleFlipbookTimeline, UboAndShaderContract)
{
    const std::string header =
        ReadSource("source/particles/particleRenderer.h");
    EXPECT_TRUE(Contains(header, "sizeof(FrameUniforms) == 10224"));
    EXPECT_TRUE(Contains(
        header,
        "offsetof(FrameUniforms, flipbookPivotKeys) == 9152"));
    EXPECT_TRUE(Contains(
        header,
        "offsetof(FrameUniforms, flipbookTimelineParams) == 9664"));

    const std::string common =
        ReadSource("shader/glsl/particle/particleCommon.glsl");
    EXPECT_TRUE(Contains(common, "vec4 flipbookPivotKeys[8];"));
    EXPECT_TRUE(Contains(common, "vec4 flipbookRotationKeys[8];"));
    EXPECT_TRUE(Contains(common, "vec4 flipbookTimelineParams;"));
    EXPECT_TRUE(Contains(common, "vec2 EvaluateFlipbookPivotTimeline("));
    EXPECT_TRUE(Contains(common, "float EvaluateFlipbookRotationTimeline("));
    EXPECT_TRUE(Contains(common, "float timelineT;"));
    EXPECT_TRUE(Contains(
        common, "result.timelineT = clamp(timelineT, 0.0, 1.0);"));
    // 时间基必须与现有 flipbook 播放一致：perParticle 用粒子年龄，
    // 否则用 emitter 模拟时间。
    EXPECT_TRUE(Contains(
        common,
        "float timeBase = perParticle ? particleAge : frame.simulation.z;"));
}

TEST(ParticleFlipbookTimeline, BillboardAndGatherApplyTimelineAdditively)
{
    const std::string billboard =
        ReadSource("shader/glsl/particle/particleBillboardTransform.glsl");
    EXPECT_TRUE(Contains(
        billboard,
        "frameRotation +=\n            EvaluateFlipbookRotationTimeline(flipbook.timelineT);"));
    EXPECT_TRUE(Contains(
        billboard,
        "EvaluateFlipbookPivotTimeline(flipbook.timelineT) - vec2(0.5);"));
    EXPECT_TRUE(Contains(
        billboard,
        "vec2 pivotOffset = (vec2(0.5) - pivot) * 2.0;"));

    const std::string gather =
        ReadSource("shader/glsl/particle/particleGlobalGather.comp");
    EXPECT_TRUE(Contains(
        gather,
        "frameRotation +=\n            EvaluateFlipbookRotationTimeline(flipbook.timelineT);"));
    EXPECT_TRUE(Contains(
        gather,
        "EvaluateFlipbookPivotTimeline(flipbook.timelineT) - vec2(0.5);"));
    EXPECT_TRUE(Contains(
        gather,
        "record.frame = vec4(\n        frameIndex,\n        pivotOffset.x,"));

    const std::string distortion =
        ReadSource("shader/glsl/particle/particleDistortion.vert");
    EXPECT_TRUE(Contains(
        distortion, "EvaluateFlipbookRotationTimeline(timelineT)"));
    EXPECT_TRUE(Contains(
        distortion, "EvaluateFlipbookPivotTimeline(timelineT)"));
}

TEST(ParticleFlipbookTimeline, RendererUploadsTimelineTables)
{
    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_TRUE(Contains(
        renderer, "uniforms.flipbookPivotKeys"));
    EXPECT_TRUE(Contains(
        renderer, "uniforms.flipbookPivotTangentIn"));
    EXPECT_TRUE(Contains(
        renderer, "uniforms.flipbookPivotTangentOut"));
    EXPECT_TRUE(Contains(
        renderer, "uniforms.flipbookRotationKeys"));
    EXPECT_TRUE(Contains(
        renderer, "uniforms.flipbookTimelineParams"));
}
