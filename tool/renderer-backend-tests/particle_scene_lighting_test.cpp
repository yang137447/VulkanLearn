#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <fstream>
#include <sstream>
#include <string>

#include <Eigen/Dense>

#include "render/shadow/localShadowMath.h"

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

// 与 shader/glsl/common/localShadowSampling.glsl 逐式对应的 CPU 参考实现。
// 这里不验证 GLSL 编译器，只把八面体映射的极点/轴向/接缝折叠性质钉住，
// 避免 shader 里的公式被悄悄改成另一种布局。
struct OctahedralVec2
{
    float x = 0.0f;
    float y = 0.0f;
};

struct OctahedralVec3
{
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

OctahedralVec2 FoldOctahedralHemisphere(OctahedralVec2 value)
{
    return OctahedralVec2{
        (1.0f - std::fabs(value.y)) * (value.x >= 0.0f ? 1.0f : -1.0f),
        (1.0f - std::fabs(value.x)) * (value.y >= 0.0f ? 1.0f : -1.0f)};
}

OctahedralVec2 EncodeOctahedralDirection(OctahedralVec3 direction)
{
    const float l1 =
        std::fabs(direction.x) + std::fabs(direction.y) + std::fabs(direction.z);
    const float scale = 1.0f / (l1 > 1.0e-8f ? l1 : 1.0e-8f);
    const OctahedralVec3 projected{
        direction.x * scale,
        direction.y * scale,
        direction.z * scale};
    if (projected.z >= 0.0f)
    {
        return OctahedralVec2{projected.x, projected.y};
    }
    return FoldOctahedralHemisphere(
        OctahedralVec2{projected.x, projected.y});
}

OctahedralVec3 DecodeOctahedralDirection(OctahedralVec2 encoded)
{
    OctahedralVec3 direction{
        encoded.x,
        encoded.y,
        1.0f - std::fabs(encoded.x) - std::fabs(encoded.y)};
    if (direction.z < 0.0f)
    {
        const OctahedralVec2 folded = FoldOctahedralHemisphere(encoded);
        direction.x = folded.x;
        direction.y = folded.y;
    }
    const float length = std::sqrt(
        direction.x * direction.x +
        direction.y * direction.y +
        direction.z * direction.z);
    direction.x /= length;
    direction.y /= length;
    direction.z /= length;
    return direction;
}

// shader BuildLocalSpotShadowWorldToNdc 的 CPU 镜像。它必须与
// source/render/shadow/localShadowMath.cpp 的 BuildSpotShadowMatrices
// 逐元素一致；这个测试专门防止两侧的 y 符号各自漂移。
Eigen::Matrix4f BuildSpotWorldToNdcReference(
    const Eigen::Vector3f& lightPosition,
    const Eigen::Vector3f& lightDirection,
    float coneOuterRadians,
    float nearCm,
    float farCm)
{
    Eigen::Vector3f direction = lightDirection;
    if (direction.squaredNorm() <= 1.0e-12f)
    {
        direction = Eigen::Vector3f(0.0f, 0.0f, -1.0f);
    }
    direction.normalize();

    const Eigen::Vector3f viewForward = -direction;
    const Eigen::Vector3f upHint =
        std::abs(viewForward.y()) > 0.99f
        ? Eigen::Vector3f::UnitX()
        : Eigen::Vector3f::UnitY();
    const Eigen::Vector3f right =
        upHint.cross(viewForward).normalized();
    const Eigen::Vector3f up =
        viewForward.cross(right).normalized();

    Eigen::Matrix4f view;
    view <<
        right.x(), right.y(), right.z(), -right.dot(lightPosition),
        up.x(), up.y(), up.z(), -up.dot(lightPosition),
        viewForward.x(), viewForward.y(), viewForward.z(),
            -viewForward.dot(lightPosition),
        0.0f, 0.0f, 0.0f, 1.0f;

    const float aspect = 1.0f;
    const float tangent = std::tan(std::clamp(
        coneOuterRadians, 0.001f, 1.5533431f));
    const float scale = -1.0f / tangent;
    const float nearValue = -nearCm;
    const float farValue = -farCm;
    const float depthA =
        -(nearValue + farValue) / (nearValue - farValue);
    const float depthB =
        2.0f * nearValue * farValue / (nearValue - farValue);

    Eigen::Matrix4f projection = Eigen::Matrix4f::Zero();
    projection(0, 0) = scale;
    projection(1, 1) = -aspect * scale;
    projection(2, 2) = -0.5f * depthA - 0.5f;
    projection(2, 3) = -0.5f * depthB;
    projection(3, 2) = -1.0f;
    return projection * view;
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

TEST(LocalShadowSampling, OctahedralEncodingPinsPolesAndAxes)
{
    const OctahedralVec2 positiveZ =
        EncodeOctahedralDirection(OctahedralVec3{0.0f, 0.0f, 1.0f});
    EXPECT_NEAR(positiveZ.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(positiveZ.y, 0.0f, 1.0e-6f);

    // -Z 极点在方形角落（四点重复）。这里同时钉住 "+0 视为正号" 的规则：
    // 若误用 sign(0)=0，-Z 会被折到中心并与 +Z 冲突。
    const OctahedralVec2 negativeZ =
        EncodeOctahedralDirection(OctahedralVec3{0.0f, 0.0f, -1.0f});
    EXPECT_NEAR(negativeZ.x, 1.0f, 1.0e-6f);
    EXPECT_NEAR(negativeZ.y, 1.0f, 1.0e-6f);

    const OctahedralVec2 positiveX =
        EncodeOctahedralDirection(OctahedralVec3{1.0f, 0.0f, 0.0f});
    EXPECT_NEAR(positiveX.x, 1.0f, 1.0e-6f);
    EXPECT_NEAR(positiveX.y, 0.0f, 1.0e-6f);

    const OctahedralVec2 positiveY =
        EncodeOctahedralDirection(OctahedralVec3{0.0f, 1.0f, 0.0f});
    EXPECT_NEAR(positiveY.x, 0.0f, 1.0e-6f);
    EXPECT_NEAR(positiveY.y, 1.0f, 1.0e-6f);
}

TEST(LocalShadowSampling, OctahedralEncodeDecodeRoundTrips)
{
    const OctahedralVec3 directions[] = {
        {0.0f, 0.0f, 1.0f},
        {0.0f, 0.0f, -1.0f},
        {1.0f, 0.0f, 0.0f},
        {0.3f, -0.4f, 0.8660254f},
        {-0.6f, 0.2f, -0.7745967f},
        {0.15f, 0.95f, 0.27f},
        {-0.7f, -0.5f, 0.5f}};
    for (const OctahedralVec3& direction : directions)
    {
        const float length = std::sqrt(
            direction.x * direction.x +
            direction.y * direction.y +
            direction.z * direction.z);
        const OctahedralVec3 normalized{
            direction.x / length,
            direction.y / length,
            direction.z / length};
        const OctahedralVec3 decoded = DecodeOctahedralDirection(
            EncodeOctahedralDirection(normalized));
        EXPECT_NEAR(decoded.x, normalized.x, 1.0e-5f);
        EXPECT_NEAR(decoded.y, normalized.y, 1.0e-5f);
        EXPECT_NEAR(decoded.z, normalized.z, 1.0e-5f);
    }
}

TEST(LocalShadowSampling, OctahedralTapFoldPreservesDirection)
{
    // 方形四边是展开切口。越界 tap 折叠后必须落在 [0,1]^2 内，并且
    // Decode(Fold(u)) == Decode(u)，即折叠只改 UV、不改它代表的方向。
    const OctahedralVec2 taps[] = {
        {1.02f, 0.25f},
        {1.05f, 0.95f},
        {0.25f, -0.03f},
        {-0.02f, 1.04f},
        {1.2f, -1.1f}};
    for (const OctahedralVec2& tap : taps)
    {
        const OctahedralVec3 decodedTap = DecodeOctahedralDirection(tap);
        const OctahedralVec2 folded = EncodeOctahedralDirection(decodedTap);
        EXPECT_GE(folded.x, -1.0f);
        EXPECT_LE(folded.x, 1.0f);
        EXPECT_GE(folded.y, -1.0f);
        EXPECT_LE(folded.y, 1.0f);
        const OctahedralVec3 decodedFolded =
            DecodeOctahedralDirection(folded);
        EXPECT_NEAR(decodedFolded.x, decodedTap.x, 1.0e-5f);
        EXPECT_NEAR(decodedFolded.y, decodedTap.y, 1.0e-5f);
        EXPECT_NEAR(decodedFolded.z, decodedTap.z, 1.0e-5f);
    }
}

TEST(LocalShadowSampling, ShaderImplementsOctahedralPcfAndFallback)
{
    const std::string source =
        ReadSource("shader/glsl/common/localShadowSampling.glsl");
    EXPECT_TRUE(Contains(source, "FoldOctahedralHemisphere"));
    EXPECT_TRUE(Contains(source, "1.0 - abs(value.yx)"));
    EXPECT_TRUE(Contains(source, "value.x >= 0.0 ? 1.0 : -1.0"));
    EXPECT_TRUE(Contains(source, "EncodeOctahedralDirection"));
    EXPECT_TRUE(Contains(source, "DecodeOctahedralDirection"));
    EXPECT_TRUE(Contains(source, "FoldOctahedralTap"));
    EXPECT_TRUE(Contains(source, "#define VL_LOCAL_SHADOW_PCF_RADIUS 1"));
    EXPECT_TRUE(Contains(source, "SampleOctahedralShadowPcf"));
    EXPECT_TRUE(Contains(source, "SampleSpotShadowPcf"));
    EXPECT_TRUE(Contains(source, "SampleLocalLightShadow"));
    // slice < 0 / type == 0 是“不投影”的显式回退。
    EXPECT_TRUE(Contains(source, "shadowParams.x < 0.0"));
    EXPECT_TRUE(Contains(source, "shadowParams.y < 0.5"));
    // 点光用线性距离；聚光用重建矩阵的 ndc.z。
    EXPECT_TRUE(Contains(source, "distanceCm / farCm - normalizedBias"));
    EXPECT_TRUE(Contains(source, "ndc.z - normalizedBias"));
}

TEST(LocalShadowSampling, SpotProjectionReconstructionMatchesRecorderConvention)
{
    const std::string source =
        ReadSource("shader/glsl/common/localShadowSampling.glsl");
    EXPECT_TRUE(Contains(source, "#define VL_LOCAL_SHADOW_SPOT_NEAR_CM 1.0"));
    // 灯方向是传播方向，相机前向取反；零方向回退到 (0,0,-1)（与录制端一致）。
    EXPECT_TRUE(Contains(source, "-lightDirection / directionLength"));
    EXPECT_TRUE(Contains(source, "vec3(0.0, 0.0, 1.0)"));
    // up 的选择必须与录制端一致，否则整张图绕光轴旋转。
    EXPECT_TRUE(Contains(source, "abs(viewForward.y) > 0.99"));
    // Camera::SetProjection + ndcMatrix 展开后的两个深度系数。
    EXPECT_TRUE(Contains(source, "-0.5 * depthA - 0.5"));
    EXPECT_TRUE(Contains(source, "-0.5 * depthB"));
    // BuildSpotShadowMatrices 的最终 y 比例是 +1/tan(cone)，不是 Camera 的
    // -1/tan；采样端必须跟随录制端，否则阴影图上下翻转。
    EXPECT_TRUE(Contains(source, "-aspect * scale"));
}

TEST(LocalShadowSampling, SpotSamplingMatrixMatchesRecorder)
{
    const Eigen::Vector3f lightPosition(120.0f, 40.0f, -30.0f);
    const Eigen::Vector3f lightDirection =
        Eigen::Vector3f(-0.3f, -0.2f, -1.0f).normalized();
    const float coneOuterRadians =
        32.0f * 3.14159265358979323846f / 180.0f;
    const float nearCm = 1.0f;
    const float farCm = 900.0f;

    const VL::LocalShadowSpotMatrices recorder =
        VL::BuildSpotShadowMatrices(
            lightPosition,
            lightDirection,
            coneOuterRadians,
            nearCm,
            farCm);
    const Eigen::Matrix4f reference = BuildSpotWorldToNdcReference(
        lightPosition,
        lightDirection,
        coneOuterRadians,
        nearCm,
        farCm);

    const Eigen::Vector3f points[] = {
        lightPosition + lightDirection * 100.0f,
        lightPosition + lightDirection * 500.0f +
            Eigen::Vector3f(80.0f, 0.0f, 0.0f),
        lightPosition + lightDirection * 700.0f +
            Eigen::Vector3f(0.0f, -120.0f, 0.0f),
        lightPosition + Eigen::Vector3f(50.0f, 60.0f, 200.0f)};
    for (const Eigen::Vector3f& point : points)
    {
        const Eigen::Vector4f recorderClip =
            recorder.projection * recorder.view *
            Eigen::Vector4f(point.x(), point.y(), point.z(), 1.0f);
        const Eigen::Vector4f referenceClip =
            reference *
            Eigen::Vector4f(point.x(), point.y(), point.z(), 1.0f);
        ASSERT_NEAR(recorderClip.w(), referenceClip.w(), 1.0e-3f);
        EXPECT_NEAR(
            recorderClip.x() / recorderClip.w(),
            referenceClip.x() / referenceClip.w(),
            1.0e-5f);
        EXPECT_NEAR(
            recorderClip.y() / recorderClip.w(),
            referenceClip.y() / referenceClip.w(),
            1.0e-5f);
        EXPECT_NEAR(
            recorderClip.z() / recorderClip.w(),
            referenceClip.z() / referenceClip.w(),
            1.0e-5f);
    }
}

TEST(LocalShadowSampling, OctahedralEncodingMatchesRecorderProjection)
{
    const Eigen::Vector3f lightPosition(10.0f, 20.0f, 30.0f);
    // 每个象限各取一个点，验证 CPU 象限投影的 clip.xy/w 与 shader 的
    // EncodeOctahedralDirection 输出同一个 [-1,1]^2 坐标。
    for (uint32_t octant = 0; octant < 8u; ++octant)
    {
        const float sx = (octant & 1u) != 0u ? 1.0f : -1.0f;
        const float sy = (octant & 2u) != 0u ? 1.0f : -1.0f;
        const float sz = (octant & 4u) != 0u ? 1.0f : -1.0f;
        const Eigen::Vector3f delta(
            sx * 13.0f,
            sy * 17.0f,
            sz * 23.0f);
        const Eigen::Vector3f point = lightPosition + delta;
        const Eigen::Vector4f clip =
            VL::BuildOctahedralProjectionMatrix(lightPosition, octant) *
            Eigen::Vector4f(point.x(), point.y(), point.z(), 1.0f);
        ASSERT_GT(std::abs(clip.w()), 1.0e-6f);
        const OctahedralVec2 encoded =
            EncodeOctahedralDirection(
                OctahedralVec3{delta.x(), delta.y(), delta.z()});
        EXPECT_NEAR(clip.x() / clip.w(), encoded.x, 1.0e-6f)
            << "octant " << octant;
        EXPECT_NEAR(clip.y() / clip.w(), encoded.y, 1.0e-6f)
            << "octant " << octant;
    }
}

TEST(LocalShadowSampling, PassesDeclareExplicitLocalShadowBindings)
{
    const std::string deferred =
        ReadSource("shader/glsl/engine/deferredLighting.glsl");
    EXPECT_TRUE(Contains(
        deferred,
        "layout(set = 3, binding = 15) "
        "uniform sampler2DArrayShadow localShadowMap;"));

    const std::string forward =
        ReadSource("shader/glsl/engine/forwardLighting.glsl");
    EXPECT_TRUE(Contains(
        forward,
        "layout (set = 3, binding = 6) "
        "uniform sampler2DArrayShadow localShadowMap;"));

    for (const char* path : {
        "shader/glsl/particle/particle.frag",
        "shader/glsl/particle/particleGlobal.frag",
        "shader/glsl/particle/particleMesh.frag",
        "shader/glsl/particle/particleRibbon.frag"})
    {
        SCOPED_TRACE(path);
        const std::string source = ReadSource(path);
        EXPECT_TRUE(Contains(
            source,
            "layout(set = 3, binding = 2) "
            "uniform sampler2DArrayShadow localShadowMap;"));
    }
}

TEST(LocalShadowSampling, ParticlePipelineLayoutsMatchPassSetInputs)
{
    const std::string renderer =
        ReadSource("source/particles/particleRenderer.cpp");
    // 全局池与共享图形管线都必须显式覆盖 set 3，否则未使用 CSM 的
    // beam/global 会因反射裁掉 binding 而与 pass descriptor set 不兼容。
    EXPECT_TRUE(Contains(renderer, "localShadowMap.binding = 2;"));
    EXPECT_TRUE(Contains(renderer, "localShadowMap.name = \"localShadowMap\";"));
    EXPECT_TRUE(Contains(renderer, "layout.overrideSets[PassSetIndex] = true;"));
    // pass set 3 的 binding 不能用固定 set 0 的 helper，否则会被归到 set 0。
    EXPECT_TRUE(Contains(
        renderer,
        "MakeParticlePassSetBinding(2, \"localShadowMap\")"));
    // distortion pass 只有 sceneDepth，必须使用删掉额外 binding 的副本。
    EXPECT_TRUE(Contains(
        renderer,
        "GraphicsPipelineLayoutDesc distortionLayout = sharedLayout;"));
}

TEST(LocalShadowSampling, LightingEntryPointsConsumeShadowSampler)
{
    const std::string lighting =
        ReadSource("shader/glsl/common/lighting.glsl");
    EXPECT_TRUE(Contains(
        lighting,
        "LightingLobes CalculatePointLightLobes(\n"
        "    in sampler2DArrayShadow localShadowMap,"));
    EXPECT_TRUE(Contains(
        lighting,
        "LightingLobes CalculateSpotLightLobes(\n"
        "    in sampler2DArrayShadow localShadowMap,"));
    EXPECT_TRUE(Contains(
        lighting,
        "LightingLobes CalculateDirectLightingLobes(\n"
        "    in sampler2DArrayShadow localShadowMap,"));
    EXPECT_TRUE(Contains(
        lighting,
        "vec3 CalculateClearCoatDirectLighting(\n"
        "    in sampler2DArrayShadow localShadowMap,"));
    // 每个局部光 lobe 都必须把可见性乘进 radiance，而不是乘到最终颜色上。
    EXPECT_TRUE(Contains(lighting, "pointLight.shadowParams"));
    EXPECT_TRUE(Contains(lighting, "spotLight.shadowParams"));
    EXPECT_TRUE(Contains(lighting, "radiance *= SampleLocalLightShadow("));
    // ClearCoat 的两个局部光入口同样要消费阴影。
    EXPECT_TRUE(Contains(lighting, "vec3 CalculateClearCoatPointLight(\n"
        "    in sampler2DArrayShadow localShadowMap,"));
    EXPECT_TRUE(Contains(lighting, "vec3 CalculateClearCoatSpotLight(\n"
        "    in sampler2DArrayShadow localShadowMap,"));

    const std::string particleLighting =
        ReadSource("shader/glsl/particle/particleSceneLighting.glsl");
    EXPECT_TRUE(Contains(particleLighting, "SampleLocalLightShadow("));
    EXPECT_TRUE(Contains(
        particleLighting,
        "ParticleLightingLobes CalculateParticleMeshSceneLighting(\n"
        "    sampler2DArrayShadow localShadowMap,"));
    EXPECT_TRUE(Contains(
        particleLighting,
        "vec3 CalculateParticleBillboardSceneLighting(\n"
        "    sampler2DArrayShadow localShadowMap,"));
    // GPU 粒子灯明确不投影：合成 Light 时写 slice = -1。
    EXPECT_TRUE(Contains(particleLighting, "vec4(-1.0),"));

    const std::string deferredPass =
        ReadSource("shader/glsl/pass/deferredLighting.frag");
    EXPECT_TRUE(Contains(deferredPass, "shadowMap,\n        localShadowMap);"));

    const std::string forward =
        ReadSource("shader/glsl/engine/forwardLighting.glsl");
    EXPECT_TRUE(Contains(forward, "CalculateDirectLighting(\n        localShadowMap,"));
}
