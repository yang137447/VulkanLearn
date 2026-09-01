#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

#include "render/cloth/clothAssets.h"

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
    float actual,
    float expected,
    float tolerance,
    const std::string& message)
{
    if (std::abs(actual - expected) > tolerance)
    {
        throw std::runtime_error(
            message + ": actual=" + std::to_string(actual) +
            ", expected=" + std::to_string(expected));
    }
}

uint32_t EncodeClothAnisotropy(float anisotropy, float anisotropyCross)
{
    const uint32_t anisotropyCode = static_cast<uint32_t>(std::round(
        (anisotropy * 0.5f + 0.5f) * 31.0f));
    const uint32_t crossCode = static_cast<uint32_t>(std::round(
        anisotropyCross * 31.0f));
    return (anisotropyCode << 5u) | crossCode;
}

std::array<float, 2> DecodeClothAnisotropy(uint32_t packedValue)
{
    const uint32_t anisotropyCode = (packedValue >> 5u) & 31u;
    const uint32_t crossCode = packedValue & 31u;
    return {
        static_cast<float>(anisotropyCode) / 31.0f * 2.0f - 1.0f,
        static_cast<float>(crossCode) / 31.0f};
}

void TestVersionContract()
{
    Require(VL::ClothModelVersion == 2, "Cloth model version must be v2");
    Require(VL::ClothAnisotropyMappingVersion == 1, "Unexpected anisotropy mapping version");
    Require(VL::ClothCharlieDistributionVersion == 2, "Unexpected Charlie distribution version");
    Require(VL::ClothVisibilityVersion == 2, "Unexpected visibility version");
    Require(VL::ClothGBufferEncodingVersion == 2, "Unexpected GBuffer encoding version");
    Require(VL::ClothAnisotropicDirectionalAlbedoLutLayers == 33, "Unexpected anisotropic LUT layer count");
}

void TestPackedAnisotropyRoundTrip()
{
    constexpr std::array<float, 5> anisotropies = {-1.0f, -0.5f, 0.0f, 0.35f, 1.0f};
    constexpr std::array<float, 5> crossValues = {0.0f, 0.17f, 0.5f, 0.83f, 1.0f};
    for (float anisotropy : anisotropies)
    {
        for (float anisotropyCross : crossValues)
        {
            const std::array<float, 2> decoded = DecodeClothAnisotropy(
                EncodeClothAnisotropy(anisotropy, anisotropyCross));
            Require(
                std::abs(decoded[0] - anisotropy) <= 1.0f / 31.0f + 1.0e-6f,
                "Packed anisotropy exceeded the 5-bit error budget");
            Require(
                std::abs(decoded[1] - anisotropyCross) <= 0.5f / 31.0f + 1.0e-6f,
                "Packed anisotropy cross exceeded the quantization error budget");
        }
    }
}

void TestAnisotropyAxisExchange()
{
    constexpr float alpha = 0.36f;
    constexpr float anisotropy = 0.75f;
    const float positiveAspect = std::exp2(anisotropy);
    const float negativeAspect = std::exp2(-anisotropy);
    const std::array<float, 2> positiveAxes = {
        alpha * positiveAspect,
        alpha / positiveAspect};
    const std::array<float, 2> negativeAxes = {
        alpha * negativeAspect,
        alpha / negativeAspect};
    RequireNear(
        positiveAxes[0],
        negativeAxes[1],
        1.0e-6f,
        "Negative anisotropy must exchange the T/B roughness axes");
    RequireNear(
        positiveAxes[1],
        negativeAxes[0],
        1.0e-6f,
        "Negative anisotropy must exchange the T/B roughness axes");
}

void TestDirectionalInterpolationContinuity()
{
    // cos(2phi) 是 v2 LUT 的轴向插值坐标；相邻方位角不应发生二值跳变。
    const auto axisWeight = [](float phi)
    {
        return 0.5f + 0.5f * std::cos(2.0f * phi);
    };
    const float previous = axisWeight(0.49f);
    const float next = axisWeight(0.51f);
    Require(
        std::abs(next - previous) < 0.05f,
        "Directional albedo axis interpolation is not continuous");
    RequireNear(axisWeight(0.0f), 1.0f, 1.0e-6f, "T-axis interpolation mismatch");
    RequireNear(axisWeight(0.5f * 3.14159265358979323846f), 0.0f, 1.0e-6f, "B-axis interpolation mismatch");
}

void TestZeroAnisotropyContract()
{
    // 零值是兼容选择器，不是“很小的 v2”近似；它必须保留 v1 LUT/closure 语义。
    Require(
        VL::ClothAnisotropyMappingVersion == 1 &&
            VL::ClothDirectionalAlbedoLutVersion == 1,
        "Cloth v1 compatibility mapping is not versioned");
    RequireNear(std::exp2(0.0f), 1.0f, 1.0e-6f, "Zero anisotropy aspect mismatch");
}

} // namespace

int main()
{
    try
    {
        TestVersionContract();
        TestPackedAnisotropyRoundTrip();
        TestAnisotropyAxisExchange();
        TestDirectionalInterpolationContinuity();
        TestZeroAnisotropyContract();
        std::cout << "Cloth contract tests passed\n";
        return 0;
    }
    catch (const std::exception& exception)
    {
        std::cerr << exception.what() << '\n';
        return 1;
    }
}
