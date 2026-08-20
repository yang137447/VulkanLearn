#ifndef VL_ENGINE_MATERIAL_PASS_GLSL
#define VL_ENGINE_MATERIAL_PASS_GLSL

// 材质 shader 的输出壳由 RenderMode 在编译期决定：
// - Opaque / OpaqueClip：写 GBuffer，后续 deferredLighting 统一算光。
// - Transparent*：直接 forward shading 并输出 sceneColor，由 forwardTransparent pass 做 blend。
// - ThinTranslucent：同样走 forward，但输出 UE Legacy 所需的 Add / Mul 双源结果。
// 这里是预处理器分流，不是运行时动态分支。
#if defined(RENDER_MODE_THIN_TRANSLUCENT)
    #define VL_MATERIAL_OUTPUT_FORWARD 1
    #define VL_MATERIAL_OUTPUT_GBUFFER 0
    #define VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT 1
#elif defined(RENDER_MODE_TRANSPARENT_ALPHA_BLEND) || defined(RENDER_MODE_TRANSPARENT_ADDITIVE)
    #define VL_MATERIAL_OUTPUT_FORWARD 1
    #define VL_MATERIAL_OUTPUT_GBUFFER 0
    #define VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT 0
#else
    #define VL_MATERIAL_OUTPUT_FORWARD 0
    #define VL_MATERIAL_OUTPUT_GBUFFER 1
    #define VL_MATERIAL_OUTPUT_THIN_TRANSLUCENT 0
#endif

#ifndef VL_THIN_TRANSLUCENT_DUAL_SOURCE
    // 能力宏由引擎注入；缺省值 0 保证不支持双源混合的平台仍能编译降级 shader。
    #define VL_THIN_TRANSLUCENT_DUAL_SOURCE 0
#endif

#endif
