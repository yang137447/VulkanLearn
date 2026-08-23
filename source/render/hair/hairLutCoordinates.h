#pragma once

namespace VL::Hair
{

struct HairLutUv
{
    float u = 0.0f;
    float v = 0.0f;
};

struct HairLutDecodedUv
{
    float deltaPhi = 0.0f;
    float thetaD = 0.0f;
    float roughnessSlice = 0.0f;
    float thetaDSample = 0.0f;
};

// 坐标 helper 只描述 Hair LUT 的 atlas 合同；它不访问 Vulkan，也不创建生产纹素。
// atlas Y 将 roughness slice 与 thetaD 采样连续排布；Decode 返回的 texel/slice 中心
// 坐标具有物理含义，任意 roughness 坐标只能解释为 atlas row，而不是独立二维轴。
HairLutUv EncodeHairAzimuthalLutUv(
    float deltaPhi,
    float thetaD,
    float roughness);

HairLutDecodedUv DecodeHairAzimuthalLutUv(HairLutUv uv);

} // namespace VL::Hair
