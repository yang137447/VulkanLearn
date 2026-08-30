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
#elif VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT && VL_THIN_TRANSLUCENT_DUAL_SOURCE
// 双源输出共享 location=0，通过 index=0/1 分别表示 Add 和 Multiplier。
layout(location = 0, index = 0) out vec4 outThinTranslucentAdd;
layout(location = 0, index = 1) out vec4 outThinTranslucentMultiplier;
#else
layout(location = 0) out vec4 outSceneColor;
layout(location = 1) out vec4 outSelectionMask;
#endif

void main()
{
    MaterialFunctionContext context = CreateMaterialFunctionContext(v2f);
    MaterialInputs inputs = EvaluateMaterialInputs(context);
    MaterialSurface surface = ResolveMaterialSurface(inputs, context);

    // Coverage 是 Pass 行為：所有需要 Alpha Clip 的 pass 都在消費 Surface 後統一執行。
#if MATERIAL_USES_OPACITY_MASK
    float resolvedOpacityMask = inputs.opacityMask;
    if (surface.shadingModel == SHADING_MODEL_HAIR)
    {
        // Hair coverage 与 opacityMask 在主 Pass 只合并一次；它不参与 absorption。
        resolvedOpacityMask *= surface.modelInputs.hair.coverage;
    }
    ApplyAlphaClip(resolvedOpacityMask, u_alphaClipThreshold);
#endif

#if VL_MATERIAL_OUTPUT_GBUFFER
    GBufferData gbuffer = BuildMaterialGBufferOutput(surface, context);
    outGBufferA = gbuffer.gbufferA;
    outGBufferB = gbuffer.gbufferB;
    outGBufferC = gbuffer.gbufferC;
    outGBufferD = gbuffer.gbufferD;
    outGBufferE = gbuffer.gbufferE;
    outGBufferVelocity = gbuffer.gbufferVelocity;
    // Geometry 已占满设备允许的 8 个颜色附件，选中标记复用 Velocity 的保留 z 通道；
    // Eye 的 z/w 有既定语义，因此只对普通 GBuffer 表面写入标记。
    if (surface.shadingModel != SHADING_MODEL_EYE)
        outGBufferVelocity.z = uboM.selectionData.x;
    outGBufferF = gbuffer.gbufferF;
    outSceneColorBase = gbuffer.sceneColorBase;
#elif VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT
    // 先构造统一的 Add/Mul 结果，再按平台能力选择双源输出或标量 alpha 降级。
    ThinTranslucentOutput thinOutput =
        BuildThinTranslucentForwardOutput(surface);
    #if VL_THIN_TRANSLUCENT_DUAL_SOURCE
        outThinTranslucentAdd = thinOutput.add;
        outThinTranslucentMultiplier = thinOutput.multiplier;
        // Vulkan 双源混合要求所有 fragment output 都位于 dual-source location 0；
        // 因此该变体不能额外写 selectionMask，透明双源材质暂不参与轮廓描边。
    #else
        outSceneColor = BuildThinTranslucentFallbackOutput(thinOutput);
        outSelectionMask = vec4(uboM.selectionData.x * step(0.001, inputs.opacity));
    #endif
#else
    outSceneColor = BuildMaterialForwardOutput(surface);
    outSelectionMask = vec4(uboM.selectionData.x * step(0.001, inputs.opacity));
#endif
}

#endif
