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

TEST(ParticleLodRender, UboAndRendererContract)
{
    const std::string header =
        ReadSource("source/particles/particleRenderer.h");
    EXPECT_TRUE(Contains(header, "sizeof(FrameUniforms) == 10224"));
    EXPECT_TRUE(Contains(
        header, "offsetof(FrameUniforms, lodParams) == 9680"));

    const std::string common =
        ReadSource("shader/glsl/particle/particleCommon.glsl");
    EXPECT_TRUE(Contains(common, "vec4 lodParams;"));

    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_TRUE(Contains(renderer, "EffectiveParticleLod("));
    EXPECT_TRUE(Contains(renderer, "uniforms.lodParams"));
    EXPECT_TRUE(Contains(renderer, "lod.renderScale"));
    EXPECT_TRUE(Contains(renderer, "lod.spawnRateScale"));

    const std::string decal =
        ReadSource("source/particles/particleDecalRuntime.cpp");
    EXPECT_TRUE(Contains(decal, "EffectiveParticleLod("));
    EXPECT_TRUE(Contains(decal, "settings.size * 0.5f * lod.renderScale"));
}

TEST(ParticleLodRender, RenderScaleReachesEveryRenderPath)
{
    const std::string billboard =
        ReadSource("shader/glsl/particle/particleBillboardTransform.glsl");
    EXPECT_TRUE(Contains(
        billboard, "EvaluateSizeCurve(lifeT) * frame.lodParams.x"));

    const std::string gather =
        ReadSource("shader/glsl/particle/particleGlobalGather.comp");
    EXPECT_TRUE(Contains(
        gather, "EvaluateSizeCurve(lifeT) * frame.lodParams.x"));

    const std::string distortion =
        ReadSource("shader/glsl/particle/particleDistortion.vert");
    EXPECT_TRUE(Contains(
        distortion, "EvaluateSizeCurve(lifeT) * frame.lodParams.x"));

    const std::string ribbon =
        ReadSource("shader/glsl/particle/particleRibbonGeometry.glsl");
    EXPECT_TRUE(Contains(ribbon, "frame.lodParams.x"));

    const std::string mesh =
        ReadSource("shader/glsl/particle/particleMesh.vert");
    EXPECT_TRUE(Contains(mesh, "frame.meshScale.xyz * frame.lodParams.x"));

    const std::string shadow =
        ReadSource("shader/glsl/particle/particleShadow.vert");
    EXPECT_TRUE(Contains(shadow, "frame.lodParams.x"));

    const std::string skeletalBone =
        ReadSource("shader/glsl/particle/particleSkeletalBone.vert");
    EXPECT_TRUE(Contains(skeletalBone, "frame.lodParams.x"));

    const std::string skeletalTriangle =
        ReadSource("shader/glsl/particle/particleSkeletalTriangle.vert");
    EXPECT_TRUE(Contains(skeletalTriangle, "frame.lodParams.x"));

    const std::string beam =
        ReadSource("shader/glsl/particle/particleBeam.vert");
    EXPECT_TRUE(Contains(beam, "frame.beamEnd.w * frame.lodParams.x"));
}

TEST(ParticleLodRender, AlphaStillOwnedByDistanceFade)
{
    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    // LOD 只把 renderScale/blend 写入 lodParams；alpha 仍只乘
    // culling.opacityScale，不重复叠 LOD 或距离 fade。
    EXPECT_TRUE(Contains(
        renderer,
        "emitter.asset.opacity * culling.opacityScale"));
    EXPECT_TRUE(Contains(
        renderer,
        "lod.renderScale,\n            lod.blend,"));
}
