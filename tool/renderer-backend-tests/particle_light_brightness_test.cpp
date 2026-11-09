#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "particles/particleLightGpu.h"

namespace
{

using namespace VL;

} // namespace

TEST(ParticleLightBrightness, EmptyCurveIsConstantOne)
{
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness({}, 0.0f), 1.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness({}, 0.5f), 1.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness({}, 1.0f), 1.0f);
}

TEST(ParticleLightBrightness, ClampsOutsideKeyRangeAndInterpolatesInside)
{
    // 先暗后亮再暗：NR_Thunder 一类"爆闪"灯的典型作者曲线。
    const std::vector<std::pair<float, float>> keys{
        {0.0f, 0.2f},
        {0.25f, 4.0f},
        {1.0f, 0.0f}};
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, 0.0f), 0.2f);
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, 0.25f), 4.0f);
    EXPECT_NEAR(EvaluateParticleLightBrightness(keys, 0.125f), 2.1f, 1.0e-5f);
    EXPECT_NEAR(EvaluateParticleLightBrightness(keys, 0.625f), 2.0f, 1.0e-5f);
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, 1.0f), 0.0f);
    // 越界输入按首尾外 clamp，不 extrapolate 出负功率。
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, -3.0f), 0.2f);
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, 9.0f), 0.0f);
}

TEST(ParticleLightBrightness, SingleKeyIsConstant)
{
    const std::vector<std::pair<float, float>> keys{{0.4f, 3.0f}};
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, 0.0f), 3.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, 0.4f), 3.0f);
    EXPECT_FLOAT_EQ(EvaluateParticleLightBrightness(keys, 1.0f), 3.0f);
}

TEST(ParticleLightBrightness, ColorCurveDefaultsToWhiteAndInterpolatesRgb)
{
    EXPECT_TRUE(EvaluateParticleLightColor({}, 0.5f).isApprox(
        Eigen::Vector3f::Ones()));

    const std::vector<std::pair<float, Eigen::Vector3f>> keys{
        {0.0f, Eigen::Vector3f(1.0f, 0.2f, 0.1f)},
        {0.5f, Eigen::Vector3f(0.1f, 0.4f, 1.0f)},
        {1.0f, Eigen::Vector3f(2.0f, 2.0f, 2.0f)}};
    EXPECT_TRUE(EvaluateParticleLightColor(keys, 0.0f).isApprox(
        keys[0].second));
    EXPECT_TRUE(EvaluateParticleLightColor(keys, 0.25f).isApprox(
        Eigen::Vector3f(0.55f, 0.3f, 0.55f)));
    EXPECT_TRUE(EvaluateParticleLightColor(keys, 0.75f).isApprox(
        Eigen::Vector3f(1.05f, 1.2f, 1.5f)));
    // 越界输入按首尾外 clamp，不 extrapolate 出负颜色。
    EXPECT_TRUE(EvaluateParticleLightColor(keys, -1.0f).isApprox(
        keys.front().second));
    EXPECT_TRUE(EvaluateParticleLightColor(keys, 2.0f).isApprox(
        keys.back().second));
}

TEST(ParticleLightBrightness, EmitShaderImplementsSameCurve)
{
    std::ifstream input(std::string(VL_TEST_SOURCE_ROOT) +
        "/shader/glsl/particle/particleLightEmit.comp");
    std::ostringstream stream;
    stream << input.rdbuf();
    const std::string source = stream.str();
    // GPU 端必须读取同一组 UBO key 并做同一段线性插值，否则 CPU 参考测试
    // 无法证明实际渲染行为。
    EXPECT_NE(source.find("frame.particleLightBrightnessKeys"), std::string::npos);
    EXPECT_NE(source.find("frame.particleLightBrightnessParams.x"), std::string::npos);
    EXPECT_NE(source.find("mix("), std::string::npos);
    EXPECT_NE(source.find("* brightness"), std::string::npos);
    // 颜色 key 必须和亮度共用同一条分段插值，并乘到最终 RGB 上。
    EXPECT_NE(source.find("frame.particleLightBrightnessColors"), std::string::npos);
    EXPECT_NE(source.find("brightnessColor = mix("), std::string::npos);
    EXPECT_NE(source.find("* brightnessColor"), std::string::npos);
}

TEST(ParticleLightBrightness, RendererUploadsPerKeyColorMultipliers)
{
    std::ifstream input(std::string(VL_TEST_SOURCE_ROOT) +
        "/source/particles/particleRenderer.cpp");
    std::ostringstream stream;
    stream << input.rdbuf();
    const std::string source = stream.str();
    EXPECT_NE(source.find("uniforms.particleLightBrightnessColors[keyIndex]"),
        std::string::npos);
    EXPECT_NE(source.find("key.color.x()"), std::string::npos);
    EXPECT_NE(source.find("key.color.y()"), std::string::npos);
    EXPECT_NE(source.find("key.color.z()"), std::string::npos);
}
