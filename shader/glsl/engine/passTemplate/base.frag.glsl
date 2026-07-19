#ifndef VL_PASS_TEMPLATE_BASE_FRAG_GLSL
#define VL_PASS_TEMPLATE_BASE_FRAG_GLSL

#include "../../common/commonUbo.glsl"
#include "../../materialFunction/mf_alphaClip.glsl"
#include "../materialPass.glsl"

#if VL_MATERIAL_OUTPUT_GBUFFER
    #include "../materialGBufferOutput.glsl"
#else
    #define VL_FORWARD_DECLARE_SHADOWMAP_INPUT
    #include "../materialForwardOutput.glsl"
#endif

layout(location = 0) in MaterialVaryings v2f;

#if VL_MATERIAL_OUTPUT_GBUFFER
layout(location = 0) out vec4 outGBufferA;
layout(location = 1) out vec4 outGBufferB;
layout(location = 2) out vec4 outGBufferC;
layout(location = 3) out vec4 outGBufferD;
layout(location = 4) out vec4 outGBufferE;
layout(location = 5) out vec4 outGBufferVelocity;
layout(location = 6) out vec4 outGBufferF;
layout(location = 7) out vec4 outSceneColorBase;
#else
layout(location = 0) out vec4 outSceneColor;
#endif

void main()
{
    MaterialPixelContext pixel = CreateMaterialPixelContext(v2f);
    MaterialSurface surface = EvaluateMaterialSurface(pixel);

    // Coverage 是 Pass 行為：所有需要 Alpha Clip 的 pass 都在消費 Surface 後統一執行。
#if MATERIAL_USES_OPACITY_MASK
    ApplyAlphaClip(surface.opacity, u_alphaClipThreshold);
#endif

#if VL_MATERIAL_OUTPUT_GBUFFER
    GBufferData gbuffer = BuildMaterialGBufferOutput(surface, pixel);
    outGBufferA = gbuffer.gbufferA;
    outGBufferB = gbuffer.gbufferB;
    outGBufferC = gbuffer.gbufferC;
    outGBufferD = gbuffer.gbufferD;
    outGBufferE = gbuffer.gbufferE;
    outGBufferVelocity = gbuffer.gbufferVelocity;
    outGBufferF = gbuffer.gbufferF;
    outSceneColorBase = gbuffer.sceneColorBase;
#else
    outSceneColor = BuildMaterialForwardOutput(surface);
#endif
}

#endif
