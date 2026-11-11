#include <gtest/gtest.h>

#include "particleSourceTestUtils.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>

#include "particles/particleGlobalSort.h"

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

TEST(ParticleRibbonGlobalSort, RecordHasFourCornersKindAndEndColor)
{
    static_assert(VL::kParticleGlobalRecordVec4Count == 17u);
    for (const char* path : {
        "shader/glsl/particle/particleGlobalGather.comp",
        "shader/glsl/particle/particleGlobalSortInit.comp",
        "shader/glsl/particle/particleGlobal.vert"})
    {
        const std::string source = ReadSource(path);
        EXPECT_TRUE(Contains(source, "vec4 corner3Kind;")) << path;
        EXPECT_TRUE(Contains(source, "vec4 color2;")) << path;
    }

    const std::string gather =
        ReadSource("shader/glsl/particle/particleGlobalGather.comp");
    EXPECT_TRUE(Contains(gather, "frame.ribbonParams.w > 0.5"));
    EXPECT_TRUE(Contains(gather, "BuildParticleRibbonQuad("));
    EXPECT_TRUE(Contains(
        gather, "record.corner3Kind = vec4(corner3, 1.0);"));
    EXPECT_TRUE(Contains(gather, "record.color2 = color1 * frame.colorTint;"));

    const std::string sortInit =
        ReadSource("shader/glsl/particle/particleGlobalSortInit.comp");
    EXPECT_TRUE(Contains(sortInit, "vec4 flags2;"));
    EXPECT_TRUE(Contains(sortInit, "vec4 flags3;"));
    EXPECT_TRUE(Contains(sortInit, "vec4 uvTransform;"));
    EXPECT_TRUE(Contains(sortInit, "record.corner3Kind.w > 0.5"));
    EXPECT_TRUE(Contains(sortInit, "* 0.25;"));
}

TEST(ParticleRibbonGlobalSort, GlobalVertexAndFragmentBranchByKind)
{
    const std::string vertex =
        ReadSource("shader/glsl/particle/particleGlobal.vert");
    EXPECT_TRUE(Contains(vertex, "layout(location = 24) out flat float vKind;"));
    EXPECT_TRUE(Contains(vertex, "record.corner3Kind.w > 0.5"));
    EXPECT_TRUE(Contains(vertex, "vColor = mix(record.color, record.color2, u);"));
    EXPECT_TRUE(Contains(vertex, "vKind = 1.0;"));
    EXPECT_TRUE(Contains(vertex, "vKind = 0.0;"));

    const std::string fragment =
        ReadSource("shader/glsl/particle/particleGlobal.frag");
    EXPECT_TRUE(Contains(fragment, "layout(location = 24) in flat float vKind;"));
    EXPECT_TRUE(Contains(fragment, "if (vKind > 0.5)"));
    EXPECT_TRUE(Contains(fragment, "vUV.y * 2.0"));
    EXPECT_TRUE(Contains(fragment, "vLightingNormal"));
}

TEST(ParticleRibbonGlobalSort, GeometryHeaderHasNoSamplerOrStorageBinding)
{
    const std::string geometry =
        ReadSource("shader/glsl/particle/particleRibbonGeometry.glsl");
    EXPECT_EQ(geometry.find("sampler2D"), std::string::npos);
    EXPECT_EQ(geometry.find("layout(std430"), std::string::npos);
    EXPECT_TRUE(Contains(geometry, "BuildParticleRibbonPath("));
    EXPECT_TRUE(Contains(geometry, "BuildParticleRibbonCorner("));
    EXPECT_TRUE(Contains(geometry, "BuildParticleRibbonQuad("));
    EXPECT_TRUE(Contains(geometry, "uv0 = vec2(startUV.x, 0.0);"));
    EXPECT_TRUE(Contains(geometry, "uv1 = vec2(endUV.x, 1.0);"));
}

TEST(ParticleRibbonGlobalSort, SynchronizeAllowsAlphaRibbonAndCountsQuads)
{
    const std::string renderer =
        ParticleSourceTest::ReadParticleRendererSources();
    EXPECT_TRUE(Contains(
        renderer, "const bool ribbon =\n            emitter.asset.renderMode == \"ribbon\";"));
    EXPECT_TRUE(Contains(
        renderer, "static_cast<uint64_t>(emitter.asset.maxParticles) *\n            segments"));
    EXPECT_TRUE(Contains(
        renderer, "#\" + (candidate.ribbon ? \"ribbon\" : \"billboard\")"));
    EXPECT_TRUE(Contains(renderer, "ClampParticleRibbonSegments("));
    // ribbon shadow 仍逐 emitter：全局池只跳过可见 draw，shadow pass 不变。
    const std::string shadow =
        ReadSource("shader/glsl/particle/particleRibbonShadow.vert");
    EXPECT_TRUE(Contains(shadow, "BuildParticleRibbonVertex"));
}

TEST(ParticleRibbonGlobalSort, CapacityUsesQuadCount)
{
    // 2048 粒子 × 4 段 = 8192 quads，刚好达到上限。
    EXPECT_TRUE(VL::CanUseParticleGlobalSort(2048ull * 4ull, 1));
    EXPECT_FALSE(VL::CanUseParticleGlobalSort(2048ull * 5ull, 1));
}
