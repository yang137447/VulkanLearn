#ifndef VL_ENGINE_MATERIAL_GBUFFER_OUTPUT_GLSL
#define VL_ENGINE_MATERIAL_GBUFFER_OUTPUT_GLSL

#include "materialContext.glsl"
#include "materialSurface.glsl"
#include "gbufferCodec.glsl"

GBufferData BuildMaterialGBufferOutput(inout MaterialSurface surface, in MaterialFunctionContext context)
{
    // Velocity 属于 GBuffer/base pass 包装层职责，不暴露给普通 MaterialPixel 作者。
    // 作者只描述材质表面；底层根据 current / previous clip position 写 motion vector。
    vec2 velocity = CalculateGBufferVelocity(
        context.clipPosition,
        context.previousClipPosition);
    surface.selectiveOutputMask = surface.selectiveOutputMask | GBUFFER_HAS_VELOCITY_MASK;

    return EncodeGBuffer(
        surface,
        CreateGBufferPixelData(context.worldTangent, velocity));
}

#endif
