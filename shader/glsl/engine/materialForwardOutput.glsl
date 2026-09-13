#ifndef VL_ENGINE_MATERIAL_FORWARD_OUTPUT_GLSL
#define VL_ENGINE_MATERIAL_FORWARD_OUTPUT_GLSL

#include "materialSurface.glsl"
#include "forwardLighting.glsl"
#include "materialDebugView.glsl"

struct ThinTranslucentOutput
{
    // 双源路径分别写入 Add 和目标颜色乘数；降级路径会把 multiplier 压成 alpha。
    // 旧 Thin Translucent 实现已于 2026-09-12 清理，这里只保留输出路径的**接口形状**，
    // 按论文重建该模型时再决定是否沿用双源混合。
    vec4 add;
    vec4 multiplier;
};

// 前向输出：把 ShadingModel 求值结果写成 framebuffer 颜色。
//
// 2026-09-12 的旧实现清理删掉了这里的分支：Thin Translucent 双源输出、Eye dual-shell
// 合成、以及 Hair / Eye / Cloth / TwoSidedFoliage 的调试数据注入。重建这些模型时，
// 输出路径要连 renderMode 与 pass 结构一起重新设计，不要照搬旧分支。
vec4 BuildMaterialForwardOutput(in MaterialSurface surface)
{
    ForwardLightingResult lighting = ShadeForwardSurfaceDetailed(surface);
    vec4 color = vec4(lighting.finalColor, surface.opacity);
#if defined(ENABLE_DEBUG_VIEW)
    // Skin 专项 Debug View 只观察 Deferred Skin；前向材质不能覆盖那份快照，
    // 否则会把 Deferred 的调试结果染成前向材质的值。
    if (uboVP.debugViewMode >= 74 && uboVP.debugViewMode <= 79)
    {
        return vec4(0.0);
    }
#endif
    MaterialDebugLightingData debugLighting = CreateMaterialDebugLightingData(
        lighting.shadow,
        lighting.shadowCascadeIndex,
        lighting.directLighting,
        lighting.indirectDiffuse,
        lighting.indirectSpecular);
    return ResolveMaterialDebugView(surface, debugLighting, color);
}

#endif
