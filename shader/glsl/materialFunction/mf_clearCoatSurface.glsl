#ifndef VL_MATERIAL_FUNCTION_CLEAR_COAT_SURFACE_GLSL
#define VL_MATERIAL_FUNCTION_CLEAR_COAT_SURFACE_GLSL

// 通过专用 wrapper 打开 Clear Coat 命名输入，避免通用 PBR 材质 schema
// 被车漆专属参数和贴图槽污染。
#define VL_PBR_USE_CLEAR_COAT_INPUTS 1
#define VL_PBR_USE_CLEAR_COAT_BOTTOM_NORMAL_CONTROLS 1
#include "mf_pbrSurface.glsl"

#endif
