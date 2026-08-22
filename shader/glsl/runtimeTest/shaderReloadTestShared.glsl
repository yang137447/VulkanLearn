#ifndef VL_SHADER_RELOAD_TEST_SHARED_GLSL
#define VL_SHADER_RELOAD_TEST_SHARED_GLSL

vec3 ShaderReloadTestColor(in MaterialFunctionContext pixel)
{
    // The runtime validation mutates only this expression and restores the
    // complete file bytes before it reports success.
    return u_reloadTestColor.rgb;
}

#endif
