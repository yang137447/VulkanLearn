#ifndef VL_STRAND_PBR_SURFACE_GLSL
#define VL_STRAND_PBR_SURFACE_GLSL
#include "../engine/materialInputs.glsl"

// 简单非金属 DefaultLit 预览；光照沿用引擎 forward 路径，不恢复 Hair ID。
MaterialInputs EvaluateMaterialInputs(in MaterialFunctionContext context)
{
    MaterialInputs inputs = CreateDefaultMaterialInputs();
    inputs.baseColor = u_tintColor.rgb;
    inputs.roughness = u_roughness;
    inputs.specular = u_specular;
    inputs.metallic = 0.0;
    inputs.ambientOcclusion = 1.0;
    inputs.emissiveColor = vec3(0.0);
    inputs.opacity = u_tintColor.a;
    // 与诊断探针保持相同覆盖语义；随机裁除和 alpha=1 写入由 Base Pass 负责。
    if (u_edgeFeather > 0.0)
    {
        float edgeDistance = min(context.texCoord1.x, 1.0 - context.texCoord1.x);
        inputs.opacity *= smoothstep(0.0, u_edgeFeather, edgeDistance);
    }
    // ribbon 是双面薄片，背面翻转法线；切线仍表示根→梢，不随面朝向反转。
    inputs.normal = normalize(context.worldNormal) * (gl_FrontFacing ? 1.0 : -1.0);
    inputs.tangent = context.worldTangent;
    return inputs;
}
#endif
