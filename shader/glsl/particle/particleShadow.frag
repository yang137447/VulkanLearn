#version 450

#include "particleCommon.glsl"

layout(location = 0) in vec3 vShadowWorldPosition;

void main()
{
    // x = 0 透视（CSM / 聚光），直接写硬件插值深度；
    // x = 1 点光八面体，按象限丢弃并写方向长度归一化深度。
    if (frame.shadowCasterParams.x > 0.5)
    {
        int octant = int(frame.shadowCasterParams.y + 0.5);
        vec3 lightToPosition =
            vShadowWorldPosition - frame.shadowLightPosition.xyz;
        float sx = (octant & 1) != 0 ? 1.0 : -1.0;
        float sy = (octant & 2) != 0 ? 1.0 : -1.0;
        float sz = (octant & 4) != 0 ? 1.0 : -1.0;
        if (lightToPosition.x * sx < 0.0 ||
            lightToPosition.y * sy < 0.0 ||
            lightToPosition.z * sz < 0.0)
        {
            discard;
        }
        gl_FragDepth = clamp(
            length(lightToPosition) /
                max(frame.shadowCasterParams.z, 1.0e-4),
            0.0,
            1.0);
    }
    else
    {
        gl_FragDepth = gl_FragCoord.z;
    }
}
