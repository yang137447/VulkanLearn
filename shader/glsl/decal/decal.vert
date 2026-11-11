#version 450

#include "decalCommon.glsl"

layout(location = 0) in vec3 inPosition;

layout(location = 0) flat out int vInstanceIndex;

void main()
{
    DecalInstance instance =
        decalInstances.instances[gl_InstanceIndex];
    vec4 worldPosition =
        instance.decalToWorld * vec4(inPosition, 1.0);
    gl_Position =
        decalFrame.viewProjection * worldPosition;
    vInstanceIndex = int(gl_InstanceIndex);
}
