#include "environmentSHGenerator.h"

#include "../commonFunction.h"
#include "../resource/image/textureIO.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <stdexcept>

namespace
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;

    // 这里返回的是 9 项实数 SH 基底函数 Ylm(dir) 的工程化写法，
    // 与 shader 侧 EvaluateIrradianceSH() 中使用的基底顺序保持一致。
    // 注意：这一步只是在做方向投影，不包含任何 diffuse 卷积常数。
    std::array<float, 9> EvaluateSH9(const Eigen::Vector3f& direction)
    {
        const float x = direction.x();
        const float y = direction.y();
        const float z = direction.z();

        return {
            0.282095f,
            0.488603f * y,
            0.488603f * z,
            0.488603f * x,
            1.092548f * x * y,
            1.092548f * y * z,
            0.315392f * (3.0f * z * z - 1.0f),
            1.092548f * x * z,
            0.546274f * (x * x - y * y)
        };
    }

    std::array<Eigen::Vector4f, 9> ZeroCoefficients()
    {
        std::array<Eigen::Vector4f, 9> coefficients{};
        for (auto& coefficient : coefficients)
        {
            coefficient = Eigen::Vector4f::Zero();
        }
        return coefficients;
    }
}

std::array<Eigen::Vector4f, 9> EnvironmentSHGenerator::Generate(const std::string& hdrPath)
{
    if (hdrPath.empty())
    {
        return ZeroCoefficients();
    }

    TextureIO::LoadOptions loadOptions;
    loadOptions.semantic = HostImage::TextureSemantic::EnvHdr;
    loadOptions.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
    loadOptions.transfer = TextureIO::LoadOptions::Transfer::Linear;
    loadOptions.forceChannels = 4;

    const std::filesystem::path hdrFullPath = CommonFunction::Path(hdrPath);
    auto cpuHdrImage = TextureIO::Load(hdrFullPath, loadOptions);
    if (!cpuHdrImage.has_value())
    {
        throw std::runtime_error("Failed to load environment image for SH generation: " + hdrFullPath.string());
    }

    const HostImage& image = *cpuHdrImage;
    if (image.width == 0 || image.height == 0)
    {
        return ZeroCoefficients();
    }
    if (image.format != HostImage::PixelFormat::RGBA32_FLOAT)
    {
        throw std::runtime_error("Environment SH generator expects RGBA32_FLOAT host image.");
    }

    const float* pixels = reinterpret_cast<const float*>(image.data.data());
    const uint32_t width = image.width;
    const uint32_t height = image.height;

    std::array<Eigen::Vector3f, 9> accumulators{};
    for (auto& accumulator : accumulators)
    {
        accumulator = Eigen::Vector3f::Zero();
    }

    // 这里累加的是环境 radiance 在 SH 基底上的投影：
    //   c_i = ∫ L(dir) * Y_i(dir) dω
    // 也就是说，CPU 当前产出的是 radiance SH，而不是已经对 Lambert 核做过卷积的 diffuse SH。
    // 因此 shader 侧在做 diffuse IBL 时，还需要再乘一遍各阶的 band 系数
    // (L0=PI, L1=2PI/3, L2=PI/4)，才能得到更接近学习资料里
    //   L_diffuse = rho / PI * sum(C_i * T_i(n))
    // 这种形式中的工程化 C_i。
    float weightSum = 0.0f;
    for (uint32_t y = 0; y < height; ++y)
    {
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        const float theta = v * kPi;
        const float sinTheta = std::sin(theta);
        if (sinTheta <= 0.0f)
        {
            continue;
        }

        for (uint32_t x = 0; x < width; ++x)
        {
            const float u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            const float phi = u * kTwoPi - kPi;

            Eigen::Vector3f direction;
            direction.x() = std::cos(phi) * sinTheta;
            direction.y() = std::cos(theta);
            direction.z() = std::sin(phi) * sinTheta;

            const size_t pixelIndex = (static_cast<size_t>(y) * width + x) * 4;
            const Eigen::Vector3f radiance(
                pixels[pixelIndex + 0],
                pixels[pixelIndex + 1],
                pixels[pixelIndex + 2]);

            const float solidAngleWeight =
                (kTwoPi / static_cast<float>(width)) *
                (kPi / static_cast<float>(height)) *
                sinTheta;
            const auto shBasis = EvaluateSH9(direction);
            for (size_t coefficientIndex = 0; coefficientIndex < shBasis.size(); ++coefficientIndex)
            {
                accumulators[coefficientIndex] += radiance * (shBasis[coefficientIndex] * solidAngleWeight);
            }
            weightSum += solidAngleWeight;
        }
    }

    if (weightSum <= 0.0f)
    {
        return ZeroCoefficients();
    }

    std::array<Eigen::Vector4f, 9> coefficients{};
    for (size_t coefficientIndex = 0; coefficientIndex < accumulators.size(); ++coefficientIndex)
    {
        coefficients[coefficientIndex] =
            Eigen::Vector4f(accumulators[coefficientIndex].x(), accumulators[coefficientIndex].y(), accumulators[coefficientIndex].z(), 0.0f);
    }
    return coefficients;
}
