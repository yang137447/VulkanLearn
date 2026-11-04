#include <gtest/gtest.h>

#include <fstream>
#include <limits>
#include <sstream>
#include <string>

#include <Eigen/Dense>

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

TEST(ParticleMeshShadow, CastShadowsOnlyAppliesToMesh)
{
    EXPECT_TRUE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("mesh", true)));
    EXPECT_FALSE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("mesh", false)));
    // 作者把开关写在其它渲染模式上时必须忽略，而不是让 billboard 进入
    // mesh caster 路径。
    EXPECT_FALSE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("billboard", true)));
    EXPECT_FALSE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("ribbon", true)));
    EXPECT_FALSE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("beam", true)));
    // 网格导入失败会回退 billboard；此时几何形状与 mesh 不符，不可以投影。
    EXPECT_FALSE(VL::ParticleEmitterCastsMeshShadow(
        MakeEmitter("mesh", true), true));
}

TEST(ParticleMeshShadow, CasterVolumeCullingIsConservative)
{
    const Eigen::Vector3f light(0.0f, 0.0f, 0.0f);
    // farCm <= 0 表示 CSM 没有单灯范围，必须保留全部 caster。
    EXPECT_TRUE(VL::ParticleMeshCasterIntersectsShadowVolume(
        Eigen::Vector3f(10000.0f, 0.0f, 0.0f), 50.0f, light, 0.0f));
    EXPECT_TRUE(VL::ParticleMeshCasterIntersectsShadowVolume(
        Eigen::Vector3f(900.0f, 0.0f, 0.0f), 50.0f, light, 1000.0f));
    EXPECT_TRUE(VL::ParticleMeshCasterIntersectsShadowVolume(
        Eigen::Vector3f(1040.0f, 0.0f, 0.0f), 50.0f, light, 1000.0f));
    EXPECT_FALSE(VL::ParticleMeshCasterIntersectsShadowVolume(
        Eigen::Vector3f(1051.0f, 0.0f, 0.0f), 50.0f, light, 1000.0f));
    // 非有限输入退回“不剔除”，避免 NaN 包围球静默丢掉阴影。
    EXPECT_TRUE(VL::ParticleMeshCasterIntersectsShadowVolume(
        Eigen::Vector3f(
            std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f),
        50.0f,
        light,
        1000.0f));
}

TEST(ParticleMeshShadow, ShadersReuseMeshTransformAndWriteDepth)
{
    const std::string transform = ReadSource(
        "shader/glsl/particle/particleMeshTransform.glsl");
    const std::string mesh = ReadSource(
        "shader/glsl/particle/particleMesh.vert");
    const std::string vertex = ReadSource(
        "shader/glsl/particle/particleShadow.vert");
    const std::string fragment = ReadSource(
        "shader/glsl/particle/particleShadow.frag");

    EXPECT_NE(transform.find("ParticleMeshWorldPosition"), std::string::npos);
    EXPECT_NE(mesh.find("particleMeshTransform.glsl"), std::string::npos);
    EXPECT_NE(vertex.find("particleMeshTransform.glsl"), std::string::npos);
    // 两套顶点路径必须共享实例变换，不能各自复制一份导致阴影错位。
    EXPECT_EQ(mesh.find("ParticleMeshRotateAroundAxis"), std::string::npos);
    EXPECT_NE(vertex.find("frame.shadowViewProj"), std::string::npos);
    EXPECT_NE(fragment.find("frame.shadowCasterParams"), std::string::npos);
    EXPECT_NE(fragment.find("frame.shadowLightPosition"), std::string::npos);
    EXPECT_NE(fragment.find("gl_FragDepth"), std::string::npos);
    EXPECT_NE(fragment.find("discard"), std::string::npos);
    // caster 不接收 sceneDepth/shadowMap/localShadowMap，避免多绑一套 set 3。
    EXPECT_EQ(vertex.find("set = 3"), std::string::npos);
    EXPECT_EQ(fragment.find("set = 3"), std::string::npos);
}
