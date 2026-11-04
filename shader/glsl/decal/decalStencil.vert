#version 450

#include "decalCommon.glsl"

layout(location = 0) in vec3 inPosition;

void main()
{
    // 实例数据与 decal pass 完全共用：stencil mask 的像素覆盖率必须
    // 与真正写 DBuffer 的投影体一致。
    DecalInstance instance =
        decalInstances.instances[gl_InstanceIndex];
    vec4 worldPosition =
        instance.decalToWorld * vec4(inPosition, 1.0);
    gl_Position =
        decalFrame.viewProjection * worldPosition;
}
