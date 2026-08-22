#ifndef VL_ENGINE_MATERIAL_CONTEXT_GLSL
#define VL_ENGINE_MATERIAL_CONTEXT_GLSL

// 母材质 Vertex/Surface 入口与 Mesh Pass 模板之间的稳定数据合同。
// 母材质只修改材质语义；投影、GBuffer、Forward 输出和 ShadowDepth 由模板负责。
struct MaterialVertexInput
{
    vec3 localPosition;
    vec3 localNormal;
    vec4 vertexColor;
    vec2 texCoord;
    vec4 localTangent;
    vec4 speedTreeWindBranch1;
    vec4 speedTreeWindBranch2;
};

struct MaterialVertex
{
    vec3 localPosition;
    vec3 localNormal;
    vec4 vertexColor;
    vec2 texCoord;
    vec4 localTangent;
};

MaterialVertex CreateDefaultMaterialVertex(in MaterialVertexInput vertexInput)
{
    MaterialVertex vertex;
    vertex.localPosition = vertexInput.localPosition;
    vertex.localNormal = vertexInput.localNormal;
    vertex.vertexColor = vertexInput.vertexColor;
    vertex.texCoord = vertexInput.texCoord;
    vertex.localTangent = vertexInput.localTangent;
    return vertex;
}

struct MaterialVertexOutput
{
    // clipPosition 最终写入 gl_Position；world* 字段传给片元阶段继续做材质采样和光照。
    vec4 clipPosition;
    vec4 previousClipPosition;
    vec3 worldPosition;
    vec3 worldNormal;
    vec4 vertexColor;
    vec2 texCoord;
    vec4 worldTangent;
};

struct MaterialVaryings
{
    // Vertex -> Fragment 的稳定插值边界。它只承载片元材质求值需要的数据。
    vec4 clipPosition;
    vec4 previousClipPosition;
    vec3 worldPosition;
    vec3 worldNormal;
    vec4 vertexColor;
    vec2 texCoord;
    vec4 worldTangent;
};

// MaterialFunctionContext 是片元 Material Function 共享的阶段数据。
// 它不暴露灯光列表、Shadow Map 或最终 Pass 输出。
struct MaterialFunctionContext
{
    // 片元入口只保存材质求值需要的插值数据；Lighting / GBuffer 决策不放在这里。
    vec4 clipPosition;
    vec4 previousClipPosition;
    vec3 worldPosition;
    vec3 worldNormal;
    vec4 vertexColor;
    vec2 texCoord;
    vec4 worldTangent;
};

MaterialVaryings CreateMaterialVaryings(in MaterialVertexOutput vertexOutput)
{
    MaterialVaryings varyings;
    varyings.clipPosition = vertexOutput.clipPosition;
    varyings.previousClipPosition = vertexOutput.previousClipPosition;
    varyings.worldPosition = vertexOutput.worldPosition;
    varyings.worldNormal = vertexOutput.worldNormal;
    varyings.vertexColor = vertexOutput.vertexColor;
    varyings.texCoord = vertexOutput.texCoord;
    varyings.worldTangent = vertexOutput.worldTangent;
    return varyings;
}

MaterialFunctionContext CreateMaterialFunctionContext(in MaterialVaryings varyings)
{
    MaterialFunctionContext context;
    context.clipPosition = varyings.clipPosition;
    context.previousClipPosition = varyings.previousClipPosition;
    context.worldPosition = varyings.worldPosition;
    context.worldNormal = varyings.worldNormal;
    context.vertexColor = varyings.vertexColor;
    context.texCoord = varyings.texCoord;
    context.worldTangent = varyings.worldTangent;
    return context;
}

#endif
