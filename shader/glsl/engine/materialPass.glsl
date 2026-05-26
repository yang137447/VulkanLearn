#ifndef VL_ENGINE_MATERIAL_PASS_GLSL
#define VL_ENGINE_MATERIAL_PASS_GLSL

// Material shader 的输出壳由 render mode 在编译期决定：
// - Opaque / OpaqueClip：写 GBuffer，后续 deferredLighting 统一算光。
// - Transparent*：直接 forward shading 并输出 sceneColor，由 forwardTransparent pass 做 blend。
// 这里是 preprocessor 分流，不是运行时动态分支。
#if defined(RENDER_MODE_TRANSPARENT_ALPHA_BLEND) || defined(RENDER_MODE_TRANSPARENT_ADDITIVE)
    #define VL_MATERIAL_OUTPUT_FORWARD 1
    #define VL_MATERIAL_OUTPUT_GBUFFER 0
#else
    #define VL_MATERIAL_OUTPUT_FORWARD 0
    #define VL_MATERIAL_OUTPUT_GBUFFER 1
#endif

#endif
