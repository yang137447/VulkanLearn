#ifndef VL_STRAND_PROBE_SURFACE_GLSL
#define VL_STRAND_PROBE_SURFACE_GLSL
#include "../engine/materialInputs.glsl"

// 只验证几何/颜色的 Unlit 探针；不恢复 Hair ID，不冒充 Marschner。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    inputs.baseColor = u_tintColor.rgb;
    inputs.emissiveColor = u_tintColor.rgb;
    inputs.opacity = u_tintColor.a;
    // 工厂输出的 UV1.x 是 ribbon 横向坐标。显式保留 0 的硬边模式，
    // 避免 smoothstep 相同端点；参数范围由 M_/MI_ 校验，不在片元重复钳制。
    if (u_edgeFeather > 0.0)
    {
        float edgeDistance = min(context.texCoord1.x, 1.0 - context.texCoord1.x);
        inputs.opacity *= smoothstep(0.0, u_edgeFeather, edgeDistance);
    }
    inputs.normal = normalize(context.worldNormal);
    inputs.tangent = context.worldTangent;
    return inputs;
}
#endif
