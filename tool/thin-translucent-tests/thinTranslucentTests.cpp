#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "thinTranslucentLegacyReference.h"
#include "shaderVariant.h"

namespace
{

void Require(bool condition, const std::string& message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void RequireNear(
    double actual,
    double expected,
    double tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(
            message + ": actual=" + std::to_string(actual) +
            ", expected=" + std::to_string(expected));
    }
}

void TestUeLegacyNormalIncidenceReference()
{
    // 锁定 UE Legacy 单次界面透射的 0.9216 基准，防止误加内部多次反射。
    const ThinTranslucentReference::Color baseColor{0.0, 0.0, 0.0};
    const ThinTranslucentReference::Color specularColor =
        ThinTranslucentReference::ComputeSpecularColor(
            baseColor,
            0.0,
            0.5);
    const ThinTranslucentReference::Color transmission =
        ThinTranslucentReference::ComputeLegacyTransmission(
            {1.0, 1.0, 1.0},
            specularColor,
            1.0,
            0.0);

    for (double channel : transmission)
    {
        RequireNear(
            channel,
            0.9216,
            1e-12,
            "UE 5.8 Legacy normal-incidence transmission mismatch");
    }

    const double incoherentSlabTransmission = 12.0 / 13.0;
    Require(
        std::abs(transmission[0] - incoherentSlabTransmission) > 1e-3,
        "Legacy transmission unexpectedly included internal round trips");
}

void TestCoverageAndFallbackReduction()
{
    // 同时验证 Coverage 插值和不支持双源混合时的标量 alpha 降级。
    const ThinTranslucentReference::Color covered =
        ThinTranslucentReference::ApplySurfaceCoverage(
            {0.2, 0.5, 0.8},
            0.25);
    RequireNear(covered[0], 0.8, 1e-12, "Red coverage multiplier mismatch");
    RequireNear(covered[1], 0.875, 1e-12, "Green coverage multiplier mismatch");
    RequireNear(covered[2], 0.95, 1e-12, "Blue coverage multiplier mismatch");
    RequireNear(
        ThinTranslucentReference::ComputeFallbackOpacity(covered),
        0.125,
        1e-12,
        "Scalar fallback opacity mismatch");
}

void TestMaterialMacroLookup()
{
    // 宏解析应忽略等号两侧空格，并且必须区分能力值 0 与 1。
    Require(
        HasMaterialMacroValue(
            {"VL_THIN_TRANSLUCENT_DUAL_SOURCE = 1"},
            kThinTranslucentDualSourceMacro,
            "1"),
        "Engine-owned dual-source macro lookup failed");
    Require(
        !HasMaterialMacroValue(
            {"VL_THIN_TRANSLUCENT_DUAL_SOURCE=0"},
            kThinTranslucentDualSourceMacro,
            "1"),
        "Engine-owned dual-source macro lookup accepted fallback value");
}

void TestRenderModeContract()
{
    // RenderMode 身份会参与 pass 路由、排序和 shader variant key。
    Require(
        IsTransparentRenderMode(RenderMode::ThinTranslucent),
        "ThinTranslucent was not routed as transparent");
    Require(
        RenderModeToString(RenderMode::ThinTranslucent) ==
            "ThinTranslucent",
        "ThinTranslucent render-mode identity mismatch");
    Require(
        IsTransparentRenderMode(
            RenderMode::TransparentAlphaBlendWriteDepth),
        "Depth-writing alpha blend was not routed as transparent");
    Require(
        RequiresTransparentDepthWrite(
            RenderMode::TransparentAlphaBlendWriteDepth),
        "Depth-writing alpha blend lost its pipeline-state requirement");
    Require(
        RenderModeToString(
            RenderMode::TransparentAlphaBlendWriteDepth) ==
            "TransparentAlphaBlendWriteDepth",
        "Depth-writing alpha blend render-mode identity mismatch");
}

} // namespace

int main()
{
    try
    {
        TestUeLegacyNormalIncidenceReference();
        TestCoverageAndFallbackReduction();
        TestRenderModeContract();
        TestMaterialMacroLookup();
        std::cout << "Thin Translucent tests passed." << std::endl;
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << "Thin Translucent tests failed: "
                  << exception.what() << std::endl;
        return 1;
    }
}
