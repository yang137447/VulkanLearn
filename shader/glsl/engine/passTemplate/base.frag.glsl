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
#elif VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT
// ThinTranslucent 的输出路径随旧实现于 2026-09-12 清理：双源 Add/Mul 输出、混合状态与
// 对应的 build 函数都已删除。这里显式报错而不是留一条空分支，避免 macro 被重新打开后
// 静默产出错误画面；按论文重建该模型时，要连 renderMode 与混合状态一起重新设计。
#error "VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT was removed with the ThinTranslucent implementation (2026-09-12)"
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
    // clip 用的是作者的原始 opacity mask，不是 closure 的 coverage。若模型把 coverage
    // 再乘进几何裁剪，同一张卡会被稀释两次（Hair 的 Core/Fringe 分层就踩过这个坑）。
    ApplyAlphaClip(inputs.opacityMask, u_alphaClipThreshold);
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
#else
    outSceneColor = BuildMaterialForwardOutput(surface);
    outSelectionMask = vec4(uboM.selectionData.x * step(0.001, inputs.opacity));
#endif
}

#endif
