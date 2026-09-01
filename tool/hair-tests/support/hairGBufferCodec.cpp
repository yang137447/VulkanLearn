#include "hairGBufferCodec.h"

#include <cmath>

namespace VL::Hair
{
namespace
{

float EncodeDirection(float value) noexcept
{
    return value * 0.5f + 0.5f;
}

float DecodeDirection(float value) noexcept
{
    return value * 2.0f - 1.0f;
}

} // namespace

HairGBufferPayload EncodeHairGBuffer(const HairGBufferInputs& inputs)
{
    HairGBufferPayload payload;
    payload.gbufferA = {0.5f, 0.5f, 1.0f, 0.0f};
    payload.gbufferB = {
        inputs.characterLighting[0],
        inputs.specular,
        inputs.roughness,
        0.0f};
    payload.gbufferC = {
        inputs.absorption[0],
        inputs.absorption[1],
        inputs.absorption[2],
        inputs.ambientOcclusion};
    payload.gbufferD = {
        inputs.scatter,
        inputs.backlit,
        inputs.cuticleTilt,
        inputs.multipleScatteringWeight};
    payload.gbufferE = {
        inputs.precomputedShadowFactor,
        inputs.characterLighting[1],
        inputs.characterLighting[2],
        inputs.characterLighting[3]};
    payload.gbufferF = {
        EncodeDirection(inputs.tangent[0]),
        EncodeDirection(inputs.tangent[1]),
        EncodeDirection(inputs.tangent[2]),
        inputs.tangentHandedness < 0.0f ? -1.0f : 1.0f};
    payload.sceneColorBase = {0.0f, 0.0f, 0.0f, inputs.opacity};
    return payload;
}

HairGBufferInputs DecodeHairGBuffer(const HairGBufferPayload& payload)
{
    HairGBufferInputs inputs;
    inputs.absorption = {
        payload.gbufferC[0],
        payload.gbufferC[1],
        payload.gbufferC[2]};
    inputs.opacity = payload.sceneColorBase[3];
    inputs.characterLighting = {
        payload.gbufferB[0],
        payload.gbufferE[1],
        payload.gbufferE[2],
        payload.gbufferE[3]};
    inputs.specular = payload.gbufferB[1];
    inputs.roughness = payload.gbufferB[2];
    inputs.ambientOcclusion = payload.gbufferC[3];
    inputs.scatter = payload.gbufferD[0];
    inputs.backlit = payload.gbufferD[1];
    inputs.cuticleTilt = payload.gbufferD[2];
    inputs.multipleScatteringWeight = payload.gbufferD[3];
    inputs.tangent = {
        DecodeDirection(payload.gbufferF[0]),
        DecodeDirection(payload.gbufferF[1]),
        DecodeDirection(payload.gbufferF[2])};
    const float tangentLength = std::sqrt(
        inputs.tangent[0] * inputs.tangent[0] +
        inputs.tangent[1] * inputs.tangent[1] +
        inputs.tangent[2] * inputs.tangent[2]);
    if (tangentLength > 1.0e-6f)
    {
        for (float& component : inputs.tangent)
        {
            component /= tangentLength;
        }
    }
    inputs.tangentHandedness = payload.gbufferF[3] < 0.0f ? -1.0f : 1.0f;
    inputs.precomputedShadowFactor = payload.gbufferE[0];
    return inputs;
}

} // namespace VL::Hair
