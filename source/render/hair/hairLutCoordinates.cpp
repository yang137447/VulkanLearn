#include "render/hair/hairLutCoordinates.h"

#include <cmath>
#include <stdexcept>
#include <string>

#include "render/hair/hairAssets.h"
#include "render/hair/hairConventions.h"

namespace VL::Hair
{
namespace
{

bool IsFinite(float value) noexcept
{
    return std::isfinite(value) != 0;
}

void RequireFinite(float value, const char* field)
{
    if (!IsFinite(value))
    {
        throw std::runtime_error(
            std::string("Hair LUT coordinate is not finite: ") + field);
    }
}

void RequireRange(float value, float lower, float upper, const char* field)
{
    RequireFinite(value, field);
    if (value < lower || value > upper)
    {
        throw std::runtime_error(
            std::string("Hair LUT coordinate is outside the contract: ") + field);
    }
}

} // namespace

HairLutUv EncodeHairAzimuthalLutUv(
    float deltaPhi,
    float thetaD,
    float roughness)
{
    RequireFinite(deltaPhi, "deltaPhi");
    RequireRange(thetaD, -HairHalfPi, HairHalfPi, "thetaD");
    RequireRange(roughness, 0.0f, 1.0f, "roughness");

    const float wrappedDeltaPhi = WrapAngle(deltaPhi);
    const float thetaDCoordinate =
        thetaD / HairPi + 0.5f;
    const float u =
        std::fmod(wrappedDeltaPhi / (2.0f * HairPi) + 0.5f + 1.0f, 1.0f);
    // roughness 与 thetaD 共用 atlas Y；每个 roughness slice 连续占用一段 thetaD 行，
    // +0.5 把编码落在 texel center，避免把物理采样点误读成边界坐标。
    const float v =
        (roughness *
             static_cast<float>(HairAzimuthalLutRoughnessSliceCount - 1) *
             static_cast<float>(HairAzimuthalLutThetaDSampleCount) +
         thetaDCoordinate *
             static_cast<float>(HairAzimuthalLutThetaDSampleCount - 1) +
         0.5f) /
        static_cast<float>(HairAzimuthalLutThetaDSampleCount *
                           HairAzimuthalLutRoughnessSliceCount);
    return {u, v};
}

HairLutDecodedUv DecodeHairAzimuthalLutUv(HairLutUv uv)
{
    RequireRange(uv.u, 0.0f, 1.0f, "u");
    RequireRange(uv.v, 0.0f, 1.0f, "v");

    const float deltaPhi = WrapAngle((uv.u - 0.5f) * 2.0f * HairPi);
    // Decode 先恢复带 texel-center 偏移的 atlas row，再拆出 roughness slice 与 thetaD 样本；
    // roughnessSlice 是 atlas 行身份，不表示可以独立改变 thetaD 的第二个坐标轴。
    const float atlasY =
        uv.v * static_cast<float>(HairAzimuthalLutHeight) - 0.5f;
    const float roughnessSlice = std::floor(
        atlasY / static_cast<float>(HairAzimuthalLutThetaDSampleCount));
    const float thetaDSample =
        atlasY -
        roughnessSlice * static_cast<float>(HairAzimuthalLutThetaDSampleCount);
    const float thetaDCoordinate =
        thetaDSample /
        static_cast<float>(HairAzimuthalLutThetaDSampleCount - 1);

    HairLutDecodedUv result;
    result.deltaPhi = deltaPhi;
    result.thetaD = (thetaDCoordinate - 0.5f) * HairPi;
    result.roughnessSlice = roughnessSlice /
        static_cast<float>(HairAzimuthalLutRoughnessSliceCount - 1);
    result.thetaDSample = thetaDSample /
        static_cast<float>(HairAzimuthalLutThetaDSampleCount - 1);
    return result;
}

} // namespace VL::Hair
