#include "render/subsurface/subsurfaceGBufferCodec.h"

namespace VL
{

uint32_t DecodeSubsurfaceDiscreteId(float encodedValue)
{
    return static_cast<uint32_t>(encodedValue + 0.5f);
}

SubsurfaceGBufferPayload EncodeSubsurfaceGBuffer(
    const SubsurfaceMaterialGBufferInputs& inputs)
{
    // CPU codec 与 gbufferCodec.glsl 保持同一通道顺序，只用于合同 round-trip 测试。
    SubsurfaceGBufferPayload payload;
    payload.gbufferA[3] = inputs.transmissionWeight;
    payload.gbufferD = {
        inputs.subsurfaceColor[0],
        inputs.subsurfaceColor[1],
        inputs.subsurfaceColor[2],
        inputs.subsurfaceWeight,
    };
    payload.gbufferF = {
        inputs.wrapWidth,
        inputs.backscatterPower,
        inputs.backscatterWeight,
        inputs.thickness,
    };
    return payload;
}

SubsurfaceMaterialGBufferInputs DecodeSubsurfaceGBuffer(
    const SubsurfaceGBufferPayload& payload)
{
    SubsurfaceMaterialGBufferInputs inputs;
    inputs.subsurfaceColor = {
        payload.gbufferD[0],
        payload.gbufferD[1],
        payload.gbufferD[2],
    };
    inputs.subsurfaceWeight = payload.gbufferD[3];
    inputs.wrapWidth = payload.gbufferF[0];
    inputs.backscatterPower = payload.gbufferF[1];
    inputs.backscatterWeight = payload.gbufferF[2];
    inputs.thickness = payload.gbufferF[3];
    inputs.transmissionWeight = payload.gbufferA[3];
    return inputs;
}

SubsurfaceGBufferPayload EncodePreintegratedSkinGBuffer(
    const PreintegratedSkinMaterialGBufferInputs& inputs)
{
    // 离散 skinLutId 以 float 存入 GBuffer，decode 端统一执行 +0.5 的确定性还原。
    SubsurfaceGBufferPayload payload;
    payload.gbufferD = {
        static_cast<float>(inputs.skinLutId),
        inputs.thickness,
        inputs.thicknessScale,
        inputs.subsurfaceWeight,
    };
    payload.gbufferF = {
        inputs.curvature,
        inputs.transmissionWeight,
        0.0f,
        0.0f,
    };
    payload.gbufferE = inputs.characterLighting;
    return payload;
}

PreintegratedSkinMaterialGBufferInputs DecodePreintegratedSkinGBuffer(
    const SubsurfaceGBufferPayload& payload)
{
    PreintegratedSkinMaterialGBufferInputs inputs;
    inputs.skinLutId = DecodeSubsurfaceDiscreteId(payload.gbufferD[0]);
    inputs.thickness = payload.gbufferD[1];
    inputs.thicknessScale = payload.gbufferD[2];
    inputs.subsurfaceWeight = payload.gbufferD[3];
    inputs.curvature = payload.gbufferF[0];
    inputs.transmissionWeight = payload.gbufferF[1];
    inputs.characterLighting = payload.gbufferE;
    return inputs;
}

SubsurfaceGBufferPayload EncodeSubsurfaceProfileGBuffer(
    const SubsurfaceProfileMaterialGBufferInputs& inputs)
{
    // profileId 与 weight/thickness/transmissionWeight 共用 GBufferD，避免新增 attachment。
    SubsurfaceGBufferPayload payload;
    payload.gbufferD = {
        static_cast<float>(inputs.profileId),
        inputs.subsurfaceWeight,
        inputs.thickness,
        inputs.transmissionWeight,
    };
    return payload;
}

SubsurfaceProfileMaterialGBufferInputs DecodeSubsurfaceProfileGBuffer(
    const SubsurfaceGBufferPayload& payload)
{
    SubsurfaceProfileMaterialGBufferInputs inputs;
    inputs.profileId = DecodeSubsurfaceDiscreteId(payload.gbufferD[0]);
    inputs.subsurfaceWeight = payload.gbufferD[1];
    inputs.thickness = payload.gbufferD[2];
    inputs.transmissionWeight = payload.gbufferD[3];
    return inputs;
}

} // namespace VL
