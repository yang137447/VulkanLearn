#include <gtest/gtest.h>

#include "particleSourceTestUtils.h"

#include <fstream>
#include <sstream>
#include <string>

#include "particles/particleMaterial.h"

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

} // namespace

TEST(ParticleMeshMaterial, DescriptorContractUsesSetOneBindingsZeroToFour)
{
    static_assert(VL::kParticleMaterialSetIndex == 1);
    static_assert(VL::kParticleMaterialUniformBinding == 0);
    EXPECT_EQ(VL::kParticleMaterialAlbedoBinding, 1u);
    EXPECT_EQ(VL::kParticleMaterialNormalBinding, 2u);
    EXPECT_EQ(VL::kParticleMaterialOrmBinding, 3u);
    EXPECT_EQ(VL::kParticleMaterialEmissiveBinding, 4u);
    EXPECT_EQ(VL::kParticleMaterialTextureCount, 4u);
}

TEST(ParticleMeshMaterial, VertexPassesUvAndTangentBasis)
{
    const std::string vertex = ReadSource(
        "shader/glsl/particle/particleMesh.vert");
    EXPECT_NE(vertex.find("layout(location = 4) in vec4 inTangent"),
        std::string::npos);
    EXPECT_NE(vertex.find("layout(location = 6) out vec3 vTangent"),
        std::string::npos);
    EXPECT_NE(vertex.find("layout(location = 7) out float vTangentSign"),
        std::string::npos);
    EXPECT_NE(vertex.find("layout(location = 8) out vec2 vUv"),
        std::string::npos);
    EXPECT_NE(vertex.find("ParticleMeshWorldTangent"), std::string::npos);
    EXPECT_NE(vertex.find("material.uvScaleOffset"), std::string::npos);
}

TEST(ParticleMeshMaterial, FragmentDeclaresFourTextureSlots)
{
    const std::string fragment = ReadSource(
        "shader/glsl/particle/particleMesh.frag");
    EXPECT_NE(fragment.find("set = 1, binding = 0"),
        std::string::npos);
    EXPECT_NE(fragment.find("set = 1, binding = 1"),
        std::string::npos);
    EXPECT_NE(fragment.find("set = 1, binding = 2"),
        std::string::npos);
    EXPECT_NE(fragment.find("set = 1, binding = 3"),
        std::string::npos);
    EXPECT_NE(fragment.find("set = 1, binding = 4"),
        std::string::npos);
    EXPECT_NE(fragment.find("texture(albedoMap, vUv)"),
        std::string::npos);
    EXPECT_NE(fragment.find("texture(normalMap, vUv)"),
        std::string::npos);
    EXPECT_NE(fragment.find("texture(ormMap, vUv)"),
        std::string::npos);
    EXPECT_NE(fragment.find("texture(emissiveMap, vUv)"),
        std::string::npos);
    // 无 tangent 的网格必须能用屏幕导数重建切线基。
    EXPECT_NE(fragment.find("dFdx(worldPosition)"),
        std::string::npos);
    EXPECT_NE(fragment.find("ReconstructParticleTangent"),
        std::string::npos);
    // 导数重建的 TBN 必须把 UV 行列式符号传给 bitangent，否则镜像 UV
    // 的法线贴图会左右翻转。
    EXPECT_NE(fragment.find("determinant < 0.0 ? -1.0 : 1.0"),
        std::string::npos);
}

TEST(ParticleMeshMaterial, MissingMapsKeepNumericFallback)
{
    const std::string fragment = ReadSource(
        "shader/glsl/particle/particleMesh.frag");
    // 贴图分支全部由 textureFlags 控制；未绑定 MI 时继续走原数值材质。
    EXPECT_NE(fragment.find("textureFlags.x > 0.5"), std::string::npos);
    EXPECT_NE(fragment.find("textureFlags.y > 0.5"), std::string::npos);
    EXPECT_NE(fragment.find("textureFlags.z > 0.5"), std::string::npos);
    EXPECT_NE(fragment.find("textureFlags.w > 0.5"), std::string::npos);
    EXPECT_NE(fragment.find("frame.materialBaseColor.rgb"),
        std::string::npos);
    EXPECT_NE(fragment.find("frame.materialPbr.z > 0.5"),
        std::string::npos);
}

TEST(ParticleMeshMaterial, RendererOwnsMeshMaterialSet)
{
    const std::string renderer =
        ParticleSourceTest::ReadParticleRendererSources();
    EXPECT_NE(renderer.find("BuildParticleMeshLayout"), std::string::npos);
    EXPECT_NE(renderer.find("\"albedoMap\""), std::string::npos);
    EXPECT_NE(renderer.find("\"normalMap\""), std::string::npos);
    EXPECT_NE(renderer.find("\"ormMap\""), std::string::npos);
    EXPECT_NE(renderer.find("\"emissiveMap\""), std::string::npos);
    EXPECT_NE(renderer.find("emitter.materialSets"), std::string::npos);
    EXPECT_NE(renderer.find("LoadEmitterMaterialTextures"),
        std::string::npos);
    EXPECT_NE(renderer.find("DestroyBuffer(rendererBackend, emitter.materialUniform)"),
        std::string::npos);
}

TEST(ParticleMeshMaterial, StaticSectionsKeepPerSectionGeometryAndMaterial)
{
    const std::string renderer =
        ParticleSourceTest::ReadParticleRendererSources();
    EXPECT_NE(renderer.find("useSectionPlans"), std::string::npos);
    EXPECT_NE(renderer.find("emitter.staticSections"), std::string::npos);
    EXPECT_NE(renderer.find("imported.sectionPlans[sectionIndex]"),
        std::string::npos);
    EXPECT_NE(renderer.find("section.materialSets"), std::string::npos);
    EXPECT_NE(renderer.find("section.materialUniform"), std::string::npos);
    // 多 section 时逐条 DrawIndexedIndirectCommand；instanceCount 由
    // finalize compute 按命令步长写入。
    EXPECT_NE(renderer.find("indirectStride"), std::string::npos);
    EXPECT_NE(renderer.find("sectionIndex) *\n                            indirectStride"),
        std::string::npos);
    EXPECT_NE(renderer.find("emitter.meshIndexCount += section.indexCount"),
        std::string::npos);
    EXPECT_NE(renderer.find("!emitter.staticSections.empty()"),
        std::string::npos);

    const std::string finalize = ReadSource(
        "shader/glsl/particle/particleFinalizeCount.comp");
    EXPECT_NE(finalize.find("command * 5 + 1"), std::string::npos);
    EXPECT_NE(finalize.find("command < 64u"), std::string::npos);
}
