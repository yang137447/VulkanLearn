#pragma once

#include <array>
#include <cstdint>

namespace VL
{

// 该 CPU codec 只用于锁定 GLSL GBuffer packing 的 round-trip 合同，
// 不参与运行时渲染，也不得承载 profile/LUT 预计算。
inline constexpr uint32_t SubsurfaceGBufferEncodingVersion = 1;

// ID 2 Surface Evaluation 产生的输入快照，供 CPU round-trip 合同测试消费；
// 不参与运行时 GBuffer 写入，也不执行材质范围校验。
struct SubsurfaceMaterialGBufferInputs
{
    std::array<float, 3> subsurfaceColor{};
    float wrapWidth = 0.0f;
    float backscatterPower = 0.0f;
    float backscatterWeight = 0.0f;
    float subsurfaceWeight = 0.0f;
    float thickness = 0.0f;
    float transmissionWeight = 0.0f;
};

// ID 3 Surface Evaluation 产生的输入快照，供 CPU round-trip 合同测试消费；
// 不生成 skin LUT，也不拥有 GPU descriptor。
struct PreintegratedSkinMaterialGBufferInputs
{
    uint32_t skinLutId = 0;
    float thickness = 0.0f;
    float thicknessScale = 1.0f;
    float subsurfaceWeight = 0.0f;
    float curvature = 0.0f;
    float transmissionWeight = 0.0f;
};

// ID 5 Surface Evaluation 产生的输入快照，供 CPU round-trip 合同测试消费；
// 不执行 profile filter 或资源解析。
struct SubsurfaceProfileMaterialGBufferInputs
{
    uint32_t profileId = 0;
    float subsurfaceWeight = 0.0f;
    float thickness = 0.0f;
    float transmissionWeight = 0.0f;
};

// 三类 SSS codec 的 CPU 表示，模拟 GLSL GBufferA/D/F 通道；不属于运行时资源包。
struct SubsurfaceGBufferPayload
{
    std::array<float, 4> gbufferA{};
    std::array<float, 4> gbufferD{};
    std::array<float, 4> gbufferF{};
};

uint32_t DecodeSubsurfaceDiscreteId(float encodedValue);

SubsurfaceGBufferPayload EncodeSubsurfaceGBuffer(
    const SubsurfaceMaterialGBufferInputs& inputs);
SubsurfaceMaterialGBufferInputs DecodeSubsurfaceGBuffer(
    const SubsurfaceGBufferPayload& payload);

SubsurfaceGBufferPayload EncodePreintegratedSkinGBuffer(
    const PreintegratedSkinMaterialGBufferInputs& inputs);
PreintegratedSkinMaterialGBufferInputs DecodePreintegratedSkinGBuffer(
    const SubsurfaceGBufferPayload& payload);

SubsurfaceGBufferPayload EncodeSubsurfaceProfileGBuffer(
    const SubsurfaceProfileMaterialGBufferInputs& inputs);
SubsurfaceProfileMaterialGBufferInputs DecodeSubsurfaceProfileGBuffer(
    const SubsurfaceGBufferPayload& payload);

} // namespace VL
