#ifndef VL_ENGINE_MATERIAL_CONTEXT_GLSL
#define VL_ENGINE_MATERIAL_CONTEXT_GLSL

// 材质作者入口的公开数据层。
// 一般使用者优先通过 MaterialVertex / MaterialPixel 读写这些 context；
// main、descriptor、lighting、GBuffer 和 debug view 属于底层封装，默认不在普通材质里改。
struct MaterialVertexInput
{
    vec3 localPosition;
    vec3 localNormal;
    vec3 vertexColor;
    vec2 texCoord;
    vec4 localTangent;
};

struct MaterialVertexOutput
{
    // clipPosition 最终写入 gl_Position；world* 字段传给片元阶段继续做材质采样和光照。
    vec4 clipPosition;
    vec3 worldPosition;
    vec3 worldNormal;
    vec3 vertexColor;
    vec2 texCoord;
    vec4 worldTangent;
};

struct MaterialVertexContext
{
    // 当前先只暴露 modelMatrix。view/projection 仍来自全局 UBO，避免把 pass 级状态塞进材质入口。
    mat4 modelMatrix;
    MaterialVertexInput vertexInput;
    MaterialVertexOutput vertexOutput;
};

struct MaterialVaryings
{
    // Vertex -> Fragment 的稳定插值边界。它只承载片元材质求值需要的数据。
    vec3 worldPosition;
    vec3 worldNormal;
    vec3 vertexColor;
    vec2 texCoord;
    vec4 worldTangent;
};

struct MaterialPixelContext
{
    // 片元入口只保存材质求值需要的插值数据；lighting / GBuffer 决策不放在这里。
    vec3 worldPosition;
    vec3 worldNormal;
    vec3 vertexColor;
    vec2 texCoord;
    vec4 worldTangent;
};

MaterialVaryings CreateMaterialVaryings(in MaterialVertexOutput vertexOutput)
{
    MaterialVaryings varyings;
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
    pixel.worldPosition = varyings.worldPosition;
    pixel.worldNormal = varyings.worldNormal;
    pixel.vertexColor = varyings.vertexColor;
    pixel.texCoord = varyings.texCoord;
    pixel.worldTangent = varyings.worldTangent;
    return pixel;
}

#endif
