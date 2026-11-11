#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "particles/particleShadowCaster.h"

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

VL::ParticleEmitterAsset MakeEmitter(
    const std::string& renderMode,
    bool castShadows)
{
    VL::ParticleEmitterAsset asset;
    asset.renderMode = renderMode;
    asset.castShadows = castShadows;
    return asset;
}

} // namespace

TEST(ParticleSpriteShadow, CastShadowsSupportsBillboardAndRibbon)
{
    using VL::ParticleShadowCasterMode;

    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("billboard", true)),
        ParticleShadowCasterMode::Billboard);
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("ribbon", true)),
        ParticleShadowCasterMode::Ribbon);
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("mesh", true)),
        ParticleShadowCasterMode::Mesh);

    // 作者开关关闭时所有模式都不投影。
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("billboard", false)),
        ParticleShadowCasterMode::None);
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("ribbon", false)),
        ParticleShadowCasterMode::None);

    // beam/light/decal 仍不参与阴影；mesh 导入失败回退 billboard 时
    // 几何形状与作者资产不符，也必须不投影。
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("beam", true)),
        ParticleShadowCasterMode::None);
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("distortion", true)),
        ParticleShadowCasterMode::None);
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("light", true)),
        ParticleShadowCasterMode::None);
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(MakeEmitter("decal", true)),
        ParticleShadowCasterMode::None);
    EXPECT_EQ(
        VL::ParticleShadowCasterModeFor(
            MakeEmitter("mesh", true), true),
        ParticleShadowCasterMode::None);

    // 旧入口仍然只回答 mesh，避免既有调用点语义被悄悄放宽。
    EXPECT_TRUE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("mesh", true)));
    EXPECT_FALSE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("billboard", true)));
    EXPECT_FALSE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("ribbon", true)));
}

TEST(ParticleSpriteShadow, BillboardCasterSharesVisibleGeometry)
{
    const std::string transform = ReadSource(
        "shader/glsl/particle/particleBillboardTransform.glsl");
    const std::string visible = ReadSource(
        "shader/glsl/particle/particle.vert");
    const std::string vertex = ReadSource(
        "shader/glsl/particle/particleBillboardShadow.vert");
    const std::string fragment = ReadSource(
        "shader/glsl/particle/particleBillboardShadow.frag");

    // 可见与 caster 必须共享同一几何函数；公式只留在 include 里。
    EXPECT_NE(transform.find("BuildParticleBillboardVertex"), std::string::npos);
    EXPECT_NE(visible.find("particleBillboardTransform.glsl"), std::string::npos);
    EXPECT_NE(vertex.find("particleBillboardTransform.glsl"), std::string::npos);
    EXPECT_EQ(visible.find("kParticleCorners"), std::string::npos);
    EXPECT_EQ(visible.find("EvaluateSizeCurve"), std::string::npos);

    // caster 不关心绘制顺序，固定 aliveIndices，避免全局排序池的
    // per-emitter pair 表未初始化。
    EXPECT_NE(transform.find("aliveIndices.indices"), std::string::npos);
    EXPECT_NE(vertex.find("false,"), std::string::npos);
    EXPECT_NE(vertex.find("frame.shadowViewProj"), std::string::npos);

    // 阴影端必须做 alpha test，并复用与 mesh 相同的深度约定。
    EXPECT_NE(fragment.find("particleTexture"), std::string::npos);
    EXPECT_NE(fragment.find("kParticleShadowAlphaThreshold"), std::string::npos);
    EXPECT_NE(fragment.find("discard"), std::string::npos);
    EXPECT_NE(fragment.find("gl_FragDepth"), std::string::npos);
    EXPECT_NE(fragment.find("particleShadowCasterCommon.glsl"), std::string::npos);
    // caster 不接收 sceneDepth/shadowMap/localShadowMap。
    EXPECT_EQ(vertex.find("set = 3"), std::string::npos);
    EXPECT_EQ(fragment.find("set = 3"), std::string::npos);
}

TEST(ParticleSpriteShadow, RibbonCasterSharesVisibleGeometry)
{
    const std::string transform = ReadSource(
        "shader/glsl/particle/particleRibbonTransform.glsl");
    const std::string visible = ReadSource(
        "shader/glsl/particle/particleRibbon.vert");
    const std::string vertex = ReadSource(
        "shader/glsl/particle/particleRibbonShadow.vert");
    const std::string fragment = ReadSource(
        "shader/glsl/particle/particleRibbonShadow.frag");

    EXPECT_NE(transform.find("BuildParticleRibbonVertex"), std::string::npos);
    EXPECT_NE(transform.find("particle.history[historyIndex]"), std::string::npos);
    EXPECT_NE(transform.find("vec3 points[17]"), std::string::npos);
    EXPECT_NE(visible.find("particleRibbonTransform.glsl"), std::string::npos);
    EXPECT_NE(vertex.find("particleRibbonTransform.glsl"), std::string::npos);
    EXPECT_EQ(visible.find("kParticleCorners"), std::string::npos);

    EXPECT_NE(vertex.find("frame.shadowViewProj"), std::string::npos);
    EXPECT_NE(fragment.find("particleTexture"), std::string::npos);
    EXPECT_NE(fragment.find("vShadowUV.y"), std::string::npos);
    EXPECT_NE(fragment.find("kParticleShadowAlphaThreshold"), std::string::npos);
    EXPECT_NE(fragment.find("gl_FragDepth"), std::string::npos);
    EXPECT_NE(fragment.find("particleShadowCasterCommon.glsl"), std::string::npos);
    EXPECT_EQ(vertex.find("set = 3"), std::string::npos);
    EXPECT_EQ(fragment.find("set = 3"), std::string::npos);
}

TEST(ParticleSpriteShadow, AlphaThresholdStaysInMaskedShadowRange)
{
    const std::string common = ReadSource(
        "shader/glsl/particle/particleShadowCasterCommon.glsl");
    const std::string marker = "kParticleShadowAlphaThreshold = ";
    const size_t position = common.find(marker);
    ASSERT_NE(position, std::string::npos);
    const size_t valueBegin = position + marker.size();
    const size_t valueEnd = common.find(';', valueBegin);
    ASSERT_NE(valueEnd, std::string::npos);
    const float threshold = std::stof(
        common.substr(valueBegin, valueEnd - valueBegin));
    // 任务约定固定阈值落在 0.1~0.25，避免极淡尾迹写实心阴影。
    EXPECT_GE(threshold, 0.1f);
    EXPECT_LE(threshold, 0.25f);
    // 八面体/透视两种深度模式都在同一函数里，供三种 caster 共用。
    EXPECT_NE(common.find("frame.shadowCasterParams"), std::string::npos);
    EXPECT_NE(common.find("frame.shadowLightPosition"), std::string::npos);
}
