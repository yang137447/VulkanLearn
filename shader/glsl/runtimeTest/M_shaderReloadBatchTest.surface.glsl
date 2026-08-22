#ifndef VL_MATERIAL_FUNCTION_SHADER_RELOAD_BATCH_TEST_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_SHADER_RELOAD_BATCH_TEST_SURFACE_GLSL

#include "../engine/materialContext.glsl"
#include "../engine/materialSurface.glsl"

MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext pixel)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    
    inputs.baseColor =
        u_reloadBatchColor.rgb *
        (0.95 + 0.05 * pixel.vertexColor.rgb);
    inputs.opacity = u_reloadBatchColor.a;
    return inputs;
}

#endif
