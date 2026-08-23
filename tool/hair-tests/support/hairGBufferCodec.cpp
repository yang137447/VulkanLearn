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
    payload.gbufferA = {
        inputs.absorption[0],
        inputs.absorption[1],
        inputs.absorption[2],
        inputs.opacity};
    payload.gbufferD = {
        inputs.scatter,
        inputs.backlit,
        inputs.cuticleTilt,
        inputs.multipleScatteringWeight};
    payload.gbufferF = {
        EncodeDirection(inputs.tangent[0]),
        EncodeDirection(inputs.tangent[1]),
        EncodeDirection(inputs.tangent[2]),
        inputs.tangentHandedness < 0.0f ? -1.0f : 1.0f};
    payload.roughness = inputs.roughness;
    return payload;
}

HairGBufferInputs DecodeHairGBuffer(const HairGBufferPayload& payload)
{
    HairGBufferInputs inputs;
    inputs.absorption = {
        payload.gbufferA[0],
        payload.gbufferA[1],
        payload.gbufferA[2]};
    inputs.opacity = payload.gbufferA[3];
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
    inputs.roughness = payload.roughness;
    return inputs;
}

} // namespace VL::Hair
