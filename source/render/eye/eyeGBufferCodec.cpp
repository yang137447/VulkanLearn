#include "render/eye/eyeGBufferCodec.h"

#include <cmath>
#include <stdexcept>

namespace VL
{
namespace
{

uint32_t DecodeInteger(float value)
{
    if (!std::isfinite(value) || value < 0.0f)
    {
        throw std::runtime_error("Eye GBuffer discrete field is invalid");
    }
    return static_cast<uint32_t>(value + 0.5f);
}

} // namespace

uint32_t PackEyeProfileAndValidity(
    uint32_t causticProfileId,
    uint32_t scleraProfileId,
    bool validIrisHit)
{
    if (causticProfileId > 15 || scleraProfileId > 15)
    {
        throw std::runtime_error("Eye GBuffer profile id exceeds V1 packing");
    }
    return causticProfileId |
        (scleraProfileId << 4u) |
        (validIrisHit ? (1u << 8u) : 0u) |
        (EyeGBufferEncodingVersion << 9u);
}

void UnpackEyeProfileAndValidity(
    uint32_t packed,
    uint32_t& causticProfileId,
    uint32_t& scleraProfileId,
    bool& validIrisHit)
{
    const uint32_t version = (packed >> 9u) & 0x7fu;
    if (version != EyeGBufferEncodingVersion)
    {
        throw std::runtime_error("Eye GBuffer encoding version mismatch");
    }
    causticProfileId = packed & 0x0fu;
    scleraProfileId = (packed >> 4u) & 0x0fu;
    validIrisHit = (packed & (1u << 8u)) != 0;
}

EyeGBufferPayload EncodeEyeGBuffer(
    const EyeMaterialGBufferInputs& inputs)
{
    EyeGBufferPayload payload;
    // Eye codec 只覆盖模型专用字段；A 的 world normal/per-object data 由通用 codec 写入。
    payload.gbufferA = {0.5f, 0.5f, 1.0f, 0.0f};
    payload.gbufferB = {
        inputs.corneaIor,
        inputs.causticStrength,
        inputs.roughness,
        0.0f};
    payload.gbufferC = {
        inputs.irisColor[0],
        inputs.irisColor[1],
        inputs.irisColor[2],
        inputs.ambientOcclusion};
    payload.gbufferD = {
        inputs.irisUv[0],
        inputs.irisUv[1],
        static_cast<float>(PackEyeProfileAndValidity(
            static_cast<uint32_t>(inputs.causticProfileId + 0.5f),
            static_cast<uint32_t>(inputs.scleraProfileId + 0.5f),
            inputs.validIrisHit > 0.5f)),
        inputs.irisMask};
    payload.gbufferE = {
        inputs.scleraColor[0],
        inputs.scleraColor[1],
        inputs.scleraColor[2],
        inputs.irisRadius};
    payload.gbufferVelocity = {
        0.0f,
        0.0f,
        inputs.irisRadius > 0.0f
            ? inputs.pupilRadius / inputs.irisRadius
            : 0.0f,
        inputs.irisRadius > 0.0f
            ? inputs.limbusWidth / inputs.irisRadius
            : 0.0f};
    payload.gbufferF = {
        inputs.irisNormal[0] * 0.5f + 0.5f,
        inputs.irisNormal[1] * 0.5f + 0.5f,
        inputs.irisNormal[2] * 0.5f + 0.5f,
        inputs.irisDistance};
    payload.sceneColorBase = {0.0f, 0.0f, 0.0f, inputs.opacity};
    return payload;
}

EyeMaterialGBufferInputs DecodeEyeGBuffer(
    const EyeGBufferPayload& payload)
{
    EyeMaterialGBufferInputs inputs;
    inputs.irisColor = {
        payload.gbufferC[0],
        payload.gbufferC[1],
        payload.gbufferC[2]};
    inputs.opacity = payload.sceneColorBase[3];
    inputs.corneaIor = payload.gbufferB[0];
    inputs.causticStrength = payload.gbufferB[1];
    inputs.roughness = payload.gbufferB[2];
    inputs.ambientOcclusion = payload.gbufferC[3];
    inputs.irisUv = {
        payload.gbufferD[0],
        payload.gbufferD[1]};
    uint32_t causticProfileId = 0;
    uint32_t scleraProfileId = 0;
    bool validIrisHit = false;
    UnpackEyeProfileAndValidity(
        DecodeInteger(payload.gbufferD[2]),
        causticProfileId,
        scleraProfileId,
        validIrisHit);
    inputs.causticProfileId = static_cast<float>(causticProfileId);
    inputs.scleraProfileId = static_cast<float>(scleraProfileId);
    inputs.validIrisHit = validIrisHit ? 1.0f : 0.0f;
    inputs.irisMask = payload.gbufferD[3];
    inputs.scleraColor = {
        payload.gbufferE[0],
        payload.gbufferE[1],
        payload.gbufferE[2]};
    inputs.irisRadius = payload.gbufferE[3];
    inputs.pupilRadius = payload.gbufferVelocity[2] * inputs.irisRadius;
    inputs.limbusWidth = payload.gbufferVelocity[3] * inputs.irisRadius;
    inputs.irisNormal = {
        payload.gbufferF[0] * 2.0f - 1.0f,
        payload.gbufferF[1] * 2.0f - 1.0f,
        payload.gbufferF[2] * 2.0f - 1.0f};
    inputs.irisDistance = payload.gbufferF[3];
    if (!validIrisHit)
    {
        // 无效交点不得把旧的 iris UV/mask 继续带入 Deferred evaluator。
        // profile pair 仍保留，便于同一像素的 sclera SSS rejection 使用。
        inputs.irisUv = {0.5f, 0.5f};
        inputs.irisMask = 0.0f;
    }
    return inputs;
}

} // namespace VL
