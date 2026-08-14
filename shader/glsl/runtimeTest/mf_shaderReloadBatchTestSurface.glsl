#ifndef VL_MATERIAL_FUNCTION_SHADER_RELOAD_BATCH_TEST_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_SHADER_RELOAD_BATCH_TEST_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"

MaterialSurface EvaluateMaterialSurface(in MaterialPixelContext pixel)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.shadingModel = SHADING_MODEL_UNLIT;
    surface.baseColor =
        u_reloadBatchColor.rgb *
        (0.95 + 0.05 * pixel.vertexColor.rgb);
    surface.opacity = u_reloadBatchColor.a;
    return surface;
}

#endif
