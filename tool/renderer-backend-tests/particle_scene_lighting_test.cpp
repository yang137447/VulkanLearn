#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{

std::string ReadSource(const std::string& relativePath)
{
    std::ifstream input(
        std::string(VL_TEST_SOURCE_ROOT) + "/" + relativePath);
    std::ostringstream stream;
    stream << input.rdbuf();
    return stream.str();
}

bool Contains(const std::string& source, const std::string& needle)
{
    return source.find(needle) != std::string::npos;
}

} // namespace

TEST(ParticleSceneLighting, DeclaresParticleOwnedBindings)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleSceneLighting.glsl");
    EXPECT_TRUE(Contains(
        source,
        "layout(std430, set = 0, binding = 6) "
        "readonly buffer ParticleSceneLightSSBO"));
    EXPECT_TRUE(Contains(
        source,
        "layout(std430, set = 0, binding = 7) "
        "readonly buffer ParticleLightSSBO"));
    EXPECT_TRUE(Contains(
        source,
        "layout(set = 0, binding = 8) "
        "uniform samplerCube prefilteredEnvironmentCube"));
    EXPECT_TRUE(Contains(
        source,
        "layout(set = 0, binding = 9) uniform sampler2D brdfLut"));
    // 场景灯 header 必须是 6 个有效 int + 2 个 padding；Light 数组从 32
    // 字节开始，才能与 CPU 的 LightSSBOHeader 对齐。
    EXPECT_TRUE(Contains(source, "int reservedHeader0;"));
    EXPECT_TRUE(Contains(source, "int reservedHeader1;"));
}

TEST(ParticleSceneLighting, RadiusWindowMatchesSharedFormula)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleSceneLighting.glsl");
    EXPECT_TRUE(Contains(source, "if (radius <= 0.0)"));
    EXPECT_TRUE(Contains(
        source,
        "1.0 - normalizedSquared * normalizedSquared"));
    EXPECT_TRUE(Contains(source, "return window * window;"));
    EXPECT_TRUE(Contains(
        source,
        "ParticleLightRadiusWindow(distanceSquared, radius) /"));
}

TEST(ParticleSceneLighting, ParticleFragmentShadersIncludeSceneLighting)
{
    const std::string particle =
        ReadSource("shader/glsl/particle/particle.frag");
    const std::string global =
        ReadSource("shader/glsl/particle/particleGlobal.frag");
    const std::string mesh =
        ReadSource("shader/glsl/particle/particleMesh.frag");
    const std::string ribbon =
        ReadSource("shader/glsl/particle/particleRibbon.frag");

    EXPECT_TRUE(Contains(particle, "#include \"particleSceneLighting.glsl\""));
    EXPECT_TRUE(Contains(global, "#include \"particleSceneLighting.glsl\""));
    EXPECT_TRUE(Contains(mesh, "#include \"particleSceneLighting.glsl\""));
    EXPECT_TRUE(Contains(ribbon, "#include \"particleSceneLighting.glsl\""));
}

TEST(ParticleSceneLighting, UnlitPathRemainsGuardedByAuthorSwitch)
{
    const std::string particle =
        ReadSource("shader/glsl/particle/particle.frag");
    EXPECT_TRUE(Contains(particle, "if (frame.billboardLightParams.x > 0.5)"));
    EXPECT_TRUE(Contains(
        particle,
        "CalculateParticleBillboardSceneLighting"));
    EXPECT_TRUE(Contains(
        particle,
        "else if (frame.shadowParams.w > 0.5)"));

    const std::string mesh =
        ReadSource("shader/glsl/particle/particleMesh.frag");
    EXPECT_TRUE(Contains(mesh, "if (frame.meshLightParams.x > 0.5)"));
    EXPECT_TRUE(Contains(mesh, "CalculateParticleMeshSceneLighting"));
}

TEST(ParticleSceneLighting, MeshPathUsesIblAndAllLightTypes)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleSceneLighting.glsl");
    EXPECT_TRUE(Contains(source, "particleSceneLights.directionalLightOffset"));
    EXPECT_TRUE(Contains(source, "particleSceneLights.pointLightOffset"));
    EXPECT_TRUE(Contains(source, "particleSceneLights.spotLightOffset"));
    EXPECT_TRUE(Contains(source, "particleLightSSBO.header.x"));
    EXPECT_TRUE(Contains(source, "ParticleSampleDiffuseIbl"));
    EXPECT_TRUE(Contains(source, "ParticleEvaluateSpecularIbl"));
    EXPECT_TRUE(Contains(source, "texture(brdfLut"));
}

TEST(ParticleSceneLighting, ColorCurveStaysLinear)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleCommon.glsl");
    EXPECT_TRUE(Contains(source, "vec4 EvaluateColorCurve(float t)"));
    EXPECT_TRUE(Contains(source, "return mix("));
    EXPECT_TRUE(Contains(source, "ColorKeyTime(index)"));
    EXPECT_TRUE(Contains(source, "int count = int(frame.curveParams.x);"));
}

TEST(ParticleSceneLighting, LightingStrengthBlendsAuthoredColor)
{
    const std::string source =
        ReadSource("shader/glsl/particle/particleSceneLighting.glsl");
    // 强度是“作者颜色 → 场景光照”的插值比例，不能退化成纯乘法：
    // 纯乘法会让 authored 颜色在暗场景掉亮度、在强光附近过曝。
    EXPECT_TRUE(Contains(source, "return mix("));
    EXPECT_TRUE(Contains(source, "baseColor,"));
    EXPECT_TRUE(Contains(source, "clamp(lightingStrength, 0.0, 1.0)"));

    const std::string particle =
        ReadSource("shader/glsl/particle/particle.frag");
    const std::string global =
        ReadSource("shader/glsl/particle/particleGlobal.frag");
    const std::string ribbon =
        ReadSource("shader/glsl/particle/particleRibbon.frag");
    EXPECT_TRUE(Contains(
        particle,
        "clamp(frame.billboardLightParams.z, 0.0, 1.0)"));
    EXPECT_TRUE(Contains(
        global,
        "clamp(vLightParams.z, 0.0, 1.0)"));
    EXPECT_TRUE(Contains(
        ribbon,
        "clamp(frame.billboardLightParams.z, 0.0, 1.0)"));

    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_TRUE(Contains(
        renderer,
        "emitter.asset.billboardLightingStrength"));
}

TEST(ParticleSceneLighting, RendererWritesDescriptorBindingsSixToNine)
{
    const std::string source =
        ReadSource("source/particles/particleRenderer.cpp");
    EXPECT_TRUE(Contains(source, "void ParticleRenderer::SetSceneLightingResources"));
    EXPECT_TRUE(Contains(source, ".setDstBinding(6)"));
    EXPECT_TRUE(Contains(source, ".setDstBinding(7)"));
    EXPECT_TRUE(Contains(source, ".setDstBinding(8)"));
    EXPECT_TRUE(Contains(source, ".setDstBinding(9)"));
    EXPECT_TRUE(Contains(source, "SceneLightInfoForImage(imageIndex)"));
    EXPECT_TRUE(Contains(source, "ParticleLightInfoForImage(imageIndex)"));
    // 图形池每 image 至少要有 2 个额外 storage 与 2 个额外 sampler。
    EXPECT_TRUE(Contains(source, "imageCount * 19"));
    EXPECT_TRUE(Contains(source, "imageCount * 3"));
}

TEST(ParticleSceneLighting, RenderSystemSuppliesIblAndLightBuffers)
{
    const std::string source = ReadSource("source/renderSystem.cpp");
    EXPECT_TRUE(Contains(source, "void RenderSystem::ConfigureParticleSceneLighting()"));
    EXPECT_TRUE(Contains(source, "\"prefilteredEnvironmentCube\""));
    EXPECT_TRUE(Contains(source, "\"brdfLut\""));
    EXPECT_TRUE(Contains(
        source,
        "particleRenderer.SetSceneLightingResources("));
}
