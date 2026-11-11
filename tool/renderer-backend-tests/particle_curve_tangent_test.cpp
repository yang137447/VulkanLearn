#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "particles/particleCurve.h"

namespace
{

using namespace VL;

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

TEST(ParticleCurveTangent, HermiteMatchesCpuReference)
{
    // p0=0, p1=1, m0=m1=0 的中点仍是 0.5；正切线把中段推高。
    EXPECT_NEAR(
        ParticleHermiteScalar(0.0f, 1.0f, 0.0f, 0.0f, 0.5f, 1.0f),
        0.5f,
        1.0e-6f);
    EXPECT_NEAR(
        ParticleHermiteScalar(0.0f, 1.0f, 3.0f, 0.0f, 0.5f, 1.0f),
        0.875f,
        1.0e-6f);
    EXPECT_FLOAT_EQ(
        ParticleCurveTangentScale(
            ParticleCurveInterpolation::Bezier),
        3.0f);
    EXPECT_FLOAT_EQ(
        ParticleCurveTangentScale(
            ParticleCurveInterpolation::Hermite),
        1.0f);
    EXPECT_FLOAT_EQ(
        ParticleCurveTangentScale(
            ParticleCurveInterpolation::Linear),
        0.0f);
}

TEST(ParticleCurveTangent, ShaderAndUboContract)
{
    const std::string common =
        ReadSource("shader/glsl/particle/particleCommon.glsl");
    EXPECT_TRUE(Contains(common, "vec4 colorTangentIn[8];"));
    EXPECT_TRUE(Contains(common, "vec4 colorTangentOut[8];"));
    EXPECT_TRUE(Contains(common, "vec4 curveInterpolationParams;"));
    EXPECT_TRUE(Contains(common, "int ParticleCurveMode()"));
    EXPECT_TRUE(Contains(common, "ParticleHermiteVector4("));
    EXPECT_TRUE(Contains(common, "ParticleHermiteScalar("));
    EXPECT_TRUE(Contains(
        common, "frame.colorTangentIn[index + 1]"));
    EXPECT_TRUE(Contains(
        common, "frame.colorTangentOut[index]"));
    EXPECT_TRUE(Contains(common, "frame.sizeKeys[index].w"));
    EXPECT_TRUE(Contains(common, "frame.sizeKeys[index + 1].z"));

    const std::string header =
        ReadSource("source/particles/particleRenderer.h");
    EXPECT_TRUE(Contains(header, "sizeof(FrameUniforms) == 10224"));
    EXPECT_TRUE(Contains(
        header, "offsetof(FrameUniforms, colorTangentIn) == 8880"));
    EXPECT_TRUE(Contains(
        header,
        "offsetof(FrameUniforms, curveInterpolationParams) == 9136"));

    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_TRUE(Contains(
        renderer, "ParticleCurveTangentScale("));
    EXPECT_TRUE(Contains(
        renderer, "uniforms.curveInterpolationParams"));
    EXPECT_TRUE(Contains(
        renderer, "uniforms.colorTangentIn[index]"));
    EXPECT_TRUE(Contains(
        renderer, "emitter.asset.sizeKeys[index].tangentIn"));
}
