#ifndef VL_COMMON_LOCAL_SHADOW_DEPTH_GLSL
#define VL_COMMON_LOCAL_SHADOW_DEPTH_GLSL

#include "commonUbo.glsl"

// LocalShadow 的深度写入规则必须由 CPU 选灯/矩阵和两个 shader 变体共用：
// - mode 0：聚光透视，直接使用硬件插值后的 gl_FragCoord.z；
// - mode 1：点光八面体，按象限方向长度归一化到 far，并丢弃不属于该象限的片元。
void WriteLocalShadowDepth(vec3 worldPosition)
{
    vec3 lightToPosition =
        worldPosition - uboVP.localShadowLightPos.xyz;
    if (uboVP.localShadowParams.x > 0.5)
    {
        int octant = int(uboVP.localShadowParams.y + 0.5);
        float sx = (octant & 1) != 0 ? 1.0 : -1.0;
        float sy = (octant & 2) != 0 ? 1.0 : -1.0;
        float sz = (octant & 4) != 0 ? 1.0 : -1.0;
        // 边界使用 >=/<=，避免方向恰好落在轴上时两侧象限都丢失片元。
        if (lightToPosition.x * sx < 0.0 ||
            lightToPosition.y * sy < 0.0 ||
            lightToPosition.z * sz < 0.0)
        {
            discard;
        }
        gl_FragDepth = clamp(
            length(lightToPosition) /
                max(uboVP.localShadowLightPos.w, 1.0e-4),
            0.0,
            1.0);
    }
    else
    {
        gl_FragDepth = gl_FragCoord.z;
    }
}

#endif
