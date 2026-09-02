#pragma once

#include <array>

namespace VL::Hair
{

// CPU GBuffer codec 只用于验证 shader/gbufferCodec.glsl 的 Hair 槽位合同，
// 不参与生产渲染；非 Hair 模型的 GBufferF.a 仍由现有 anisotropy 合同解释。
struct HairGBufferInputs
{
    float scatter = 0.0f;
    float backlit = 0.0f;
    float cuticleTilt = 0.0f;
    float multipleScatteringWeight = 0.0f;
    std::array<float, 3> baseColor = {1.0f, 1.0f, 1.0f};
    std::array<float, 3> absorption = {1.0f, 1.0f, 1.0f};
    float opacity = 1.0f;
    std::array<float, 3> tangent = {1.0f, 0.0f, 0.0f};
    float tangentHandedness = 1.0f;
    float specular = 0.5f;
    float roughness = 0.5f;
    float ambientOcclusion = 1.0f;
    std::array<float, 4> characterLighting = {1.0f, 1.0f, 1.0f, 0.0f};
    float precomputedShadowFactor = 1.0f;
};

struct HairGBufferPayload
{
    std::array<float, 4> gbufferA = {0.5f, 0.5f, 1.0f, 0.0f};
    std::array<float, 4> gbufferB = {1.0f, 0.5f, 0.5f, 0.0f};
    std::array<float, 4> gbufferC = {1.0f, 1.0f, 1.0f, 1.0f};
    std::array<float, 4> gbufferD = {0.0f, 0.0f, 0.0f, 0.0f};
    std::array<float, 4> gbufferE = {1.0f, 1.0f, 1.0f, 0.0f};
    std::array<float, 4> gbufferF = {0.5f, 0.5f, 0.5f, 1.0f};
    std::array<float, 4> sceneColorBase = {0.0f, 0.0f, 0.0f, 1.0f};
};

HairGBufferPayload EncodeHairGBuffer(const HairGBufferInputs& inputs);
HairGBufferInputs DecodeHairGBuffer(const HairGBufferPayload& payload);

} // namespace VL::Hair
