#ifndef VL_MATERIAL_FUNCTION_ALPHA_CLIP_GLSL
#define VL_MATERIAL_FUNCTION_ALPHA_CLIP_GLSL

void ApplyAlphaClip(float opacity, float threshold)
{
    if (opacity < threshold)
    {
        discard;
    }
}

#endif

