#pragma once

#include <array>
#include <cstdint>

namespace VL
{

// Eye Deferred GBuffer V1 独占 B/D/E/F/Velocity 的 Eye 语义；版本变化必须同步
// GLSL codec、shader ABI 和 round-trip tests，不能静默复用旧 packing。
inline constexpr uint32_t EyeGBufferEncodingVersion = 1;

struct EyeMaterialGBufferInputs
{
    std::array<float, 3> irisColor{};
    std::array<float, 2> irisUv{};
    std::array<float, 3> scleraColor{};
    std::array<float, 3> irisNormal{};
    float irisMask = 0.0f;
    float validIrisHit = 0.0f;
    float irisRadius = 0.0f;
    float irisDistance = 0.0f;
    float pupilRadius = 0.0f;
    float limbusWidth = 0.0f;
    float corneaIor = 1.376f;
    float causticStrength = 0.0f;
    float causticProfileId = 0.0f;
    float scleraProfileId = 0.0f;
    float roughness = 1.0f;
    float ambientOcclusion = 1.0f;
    float opacity = 1.0f;
};

struct EyeGBufferPayload
{
    std::array<float, 4> gbufferA{};
    std::array<float, 4> gbufferB{};
    std::array<float, 4> gbufferC{};
    std::array<float, 4> gbufferD{};
    std::array<float, 4> gbufferE{};
    std::array<float, 4> gbufferVelocity{};
    std::array<float, 4> gbufferF{};
    std::array<float, 4> sceneColorBase{};
};

EyeGBufferPayload EncodeEyeGBuffer(
    const EyeMaterialGBufferInputs& inputs);
EyeMaterialGBufferInputs DecodeEyeGBuffer(
    const EyeGBufferPayload& payload);

uint32_t PackEyeProfileAndValidity(
    uint32_t causticProfileId,
    uint32_t scleraProfileId,
    bool validIrisHit);
void UnpackEyeProfileAndValidity(
    uint32_t packed,
    uint32_t& causticProfileId,
    uint32_t& scleraProfileId,
    bool& validIrisHit);

} // namespace VL
