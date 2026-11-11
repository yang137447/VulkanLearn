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

TEST(ParticleRibbonE1, UboAndCurveTangentContract)
{
    const std::string header =
        ReadSource("source/particles/particleRenderer.h");
    EXPECT_TRUE(Contains(header, "sizeof(FrameUniforms) == 10224"));
    EXPECT_TRUE(Contains(
        header,
        "offsetof(FrameUniforms, ribbonColorTangentIn) == 9696"));
    EXPECT_TRUE(Contains(
        header,
        "offsetof(FrameUniforms, ribbonShapeParams) == 10208"));

    const std::string common =
        ReadSource("shader/glsl/particle/particleCommon.glsl");
    EXPECT_TRUE(Contains(common, "vec4 ribbonColorTangentIn[8];"));
    EXPECT_TRUE(Contains(common, "vec4 beamColorTangentIn[8];"));
    EXPECT_TRUE(Contains(common, "vec4 ribbonShapeParams;"));
    EXPECT_TRUE(Contains(
        common, "frame.ribbonWidthKeys[index].w"));
    EXPECT_TRUE(Contains(
        common, "frame.ribbonWidthKeys[index + 1].z"));
    EXPECT_TRUE(Contains(
        common, "frame.ribbonColorTangentOut[index]"));

    const std::string beam =
        ReadSource("shader/glsl/particle/particleBeam.vert");
    EXPECT_TRUE(Contains(beam, "frame.beamColorTangentOut[index]"));
    EXPECT_TRUE(Contains(beam, "frame.beamWidthKeys[index].w"));
}

TEST(ParticleRibbonE1, PivotAndTwistReachVisibleAndShadowGeometry)
{
    const std::string geometry =
        ReadSource("shader/glsl/particle/particleRibbonGeometry.glsl");
    EXPECT_TRUE(Contains(geometry, "RotateRibbonSide"));
    EXPECT_TRUE(Contains(geometry, "frame.ribbonShapeParams.y"));
    EXPECT_TRUE(Contains(
        geometry, "lateral + frame.ribbonShapeParams.x"));

    const std::string shadow =
        ReadSource("shader/glsl/particle/particleRibbonShadow.vert");
    EXPECT_TRUE(Contains(shadow, "BuildParticleRibbonVertex"));
    EXPECT_TRUE(Contains(shadow, "frame.shadowViewProj"));
}

TEST(ParticleRibbonE1, HistoryAdvancesInsideEachFixedStep)
{
    const std::string simulate =
        ReadSource("shader/glsl/particle/particleSimulate.comp");
    const size_t loop = simulate.find("for (uint frameStep = 0u;");
    const size_t shift =
        simulate.find("particle.history[historyIndex - 1]");
    ASSERT_NE(loop, std::string::npos);
    ASSERT_NE(shift, std::string::npos);
    // 历史搬移必须在 fixed-step 循环内部，不能整个 dispatch 只做一次。
    EXPECT_LT(loop, shift);
    EXPECT_TRUE(Contains(
        simulate,
        "particle.history[historyIndex] =\n"
        "                    particle.history[historyIndex - 1];"));
    EXPECT_TRUE(Contains(
        simulate,
        "particle.history[0] = vec4(\n"
        "                position, max(age - deltaSeconds, 0.0));"));
}
