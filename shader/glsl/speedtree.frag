#version 450

#include "common/commonUbo.glsl"
#include "generate/M_speedtreeParamter.glsl"
#include "engine/materialContext.glsl"
#include "engine/materialSurface.glsl"
#include "materialFunction/mf_pbrSurface.glsl"
#include "engine/materialPass.glsl"

#if VL_MATERIAL_OUTPUT_GBUFFER
    #include "engine/materialGBufferOutput.glsl"
#else
    // Forward transparent pass 的 Set 3 由当前 pass 自己声明。
    // 约定 binding 0 = shadowMap；它不影响 opaque/GBuffer 变体，也不占用 deferredLighting 的 Set 3。
    #define VL_FORWARD_DECLARE_SHADOWMAP_INPUT
    #include "engine/materialForwardOutput.glsl"
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

// MaterialPixel 是片元阶段的公开材质入口：一般材质作者改这里。
// SpeedTree 第一版复用 PBR surface，后续在这里逐步接入 foliage shading。
void MaterialPixel(in MaterialPixelContext pixel, inout MaterialSurface surface)
{
    surface = EvaluatePbrSurface(pixel);
}

void main()
{
    // main 保留为引擎包装层：准备 context，调用材质入口，再按编译期 pass 类型输出。
    MaterialPixelContext pixel = CreateMaterialPixelContext(v2f);
    MaterialSurface surface = CreateDefaultMaterialSurface();
    MaterialPixel(pixel, surface);

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
