#include <gtest/gtest.h>

#include "particleSourceTestUtils.h"

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

} // namespace

// 骨骼三角面/骨段必须复用 mesh 的 dissolve 合同：同一个
// particleDissolve.glsl、同一组 frame.dissolveParams*、同一条边缘自发光
// 公式。Teleport 的骨骼表面 dissolve 依赖这里，不能各自复制一份噪声。
TEST(ParticleSkeletalDissolve, FragmentBodyUsesSharedDissolveContract)
{
    const std::string body = ReadSource(
        "shader/glsl/particle/particleSkeletalFragmentBody.glsl");
    EXPECT_NE(body.find("#include \"particleDissolve.glsl\""),
        std::string::npos);
    EXPECT_NE(body.find(
        "EvaluateParticleDissolve(\n"
        "        vWorldPosition,\n"
        "        vLifeT,\n"
        "        vSeed,\n"
        "        dissolveVisibility,\n"
        "        dissolveEdge);"),
        std::string::npos);
    EXPECT_NE(body.find("opacity *= dissolveVisibility"),
        std::string::npos);
    EXPECT_NE(body.find(
        "frame.dissolveEdgeColor.rgb *\n"
        "        frame.dissolveParams2.x *\n"
        "        dissolveEdge"),
        std::string::npos);
}

// vLifeT 驱动溶解阈值，vSeed 驱动逐粒子噪声偏移；两个骨骼顶点着色器
// 都必须把这两项写到 fragment body 读取的 location。
TEST(ParticleSkeletalDissolve, SkeletalVertexShadersExportLifeAndSeed)
{
    for (const char* path : {
             "shader/glsl/particle/particleSkeletalTriangle.vert",
             "shader/glsl/particle/particleSkeletalBone.vert"})
    {
        const std::string vertex = ReadSource(path);
        EXPECT_NE(vertex.find("layout(location = 9) out float vLifeT"),
            std::string::npos) << path;
        EXPECT_NE(vertex.find("layout(location = 10) out float vSeed"),
            std::string::npos) << path;
        EXPECT_NE(vertex.find("vLifeT = skeletalLifeT"),
            std::string::npos) << path;
        EXPECT_NE(vertex.find(
            "vSeed = fract(float(particleIndex) * 0.61803398875)"),
            std::string::npos) << path;
    }
}

// 姿态未变时只能跳过 palette 重写，不能跳过整段 emitter 更新。历史实现
// 在 pose key 相等时 continue，rate=0 的骨骼发射器因此冻结模拟；资源侧
// 只能把 rate 写成 1.0 绕过。palette 又是逐 image 缓冲，判定也必须逐 image。
TEST(ParticleSkeletalDissolve, PoseClockUsesPerImageKeysWithoutSkippingSimulation)
{
    const std::string header = ReadSource(
        "source/particles/particleRenderer.h");
    EXPECT_NE(header.find(
        "std::vector<ParticleSkeletalPoseKey> skeletalPoseKeys"),
        std::string::npos);
    const std::string renderer =
        ParticleSourceTest::ReadParticleRendererSources();
    EXPECT_NE(renderer.find("const bool poseUnchanged ="),
        std::string::npos);
    EXPECT_NE(renderer.find("emitter.skeletalPoseKeys[imageIndex] = poseKey"),
        std::string::npos);
    // 单 key 的旧字段已被逐 image 表替换。
    EXPECT_EQ(renderer.find("emitter.skeletalPoseKey =="),
        std::string::npos);
    EXPECT_EQ(renderer.find("emitter.skeletalPoseKey ="),
        std::string::npos);
}
