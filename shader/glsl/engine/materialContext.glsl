#ifndef VL_ENGINE_MATERIAL_CONTEXT_GLSL
#define VL_ENGINE_MATERIAL_CONTEXT_GLSL

// 材質求值與 Mesh Pass 模板之間的穩定數據合同。
// 材質只修改局部頂點語義和 Surface；投影、GBuffer 與 ShadowDepth 由模板負責。
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

struct MaterialPixelContext
{
    // 片元入口只保存材质求值需要的插值数据；lighting / GBuffer 决策不放在这里。
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

MaterialPixelContext CreateMaterialPixelContext(in MaterialVaryings varyings)
{
    MaterialPixelContext pixel;
    pixel.clipPosition = varyings.clipPosition;
    pixel.previousClipPosition = varyings.previousClipPosition;
    pixel.worldPosition = varyings.worldPosition;
    pixel.worldNormal = varyings.worldNormal;
    pixel.vertexColor = varyings.vertexColor;
    pixel.texCoord = varyings.texCoord;
    pixel.worldTangent = varyings.worldTangent;
    return pixel;
}

#endif
