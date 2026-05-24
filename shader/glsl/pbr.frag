#version 450

#include "common/commonUbo.glsl"
#include "generate/M_pbrParamter.glsl"
#include "engine/materialContext.glsl"
#include "engine/materialSurface.glsl"
#include "materialFunction/mf_pbrSurface.glsl"
#include "engine/forwardLighting.glsl"
#include "engine/materialDebugView.glsl"

layout(location = 0) in MaterialVaryings v2f;

layout(location = 0) out vec4 outColor;

// MaterialPixel 是片元阶段的公开材质入口：一般材质作者改这里。
// 材质采样、模块组合和参数解释写入 MaterialSurface；forward/deferred 分流留给底层封装。
void MaterialPixel(in MaterialPixelContext pixel, inout MaterialSurface surface)
{
    surface = EvaluatePbrSurface(pixel);
}

void main()
{
    // main 先保留为引擎包装层：准备 context，调用材质入口，再选择 forward lighting / debug view。
    MaterialPixelContext pixel = CreateMaterialPixelContext(v2f);
    MaterialSurface surface = CreateDefaultMaterialSurface();
    MaterialPixel(pixel, surface);

    ForwardLightingResult lighting = ShadeForwardSurfaceDetailed(surface);
    vec4 finalColor = vec4(lighting.finalColor, surface.opacity);
    outColor = ResolveMaterialDebugView(surface, lighting, finalColor);
}
