#ifndef VL_ENGINE_VIRTUAL_LIGHT_GLSL
#define VL_ENGINE_VIRTUAL_LIGHT_GLSL

#include "../common/commonUbo.glsl"
#include "materialSurface.glsl"

// Virtual Light 是与摄像机绑定的角色补光，不属于 uboLight 中的真实点光、
// 聚光或方向光。它没有距离衰减和独立阴影，方向由当前像素指向摄像机决定。
struct VirtualLight
{
    vec3 direction;
    vec3 radiance;
};

VirtualLight CreateVirtualLight(
    in vec3 lightDirection,
    in vec3 radiance)
{
    VirtualLight light;
    light.direction = normalize(lightDirection);
    light.radiance = radiance;
    return light;
}

VirtualLight CreateCameraVirtualLight(
    in MaterialSurface surface,
    in vec3 radiance)
{
    // 源 shader 的 camera_vector 是像素指向摄像机的方向；它看起来像摄像机处的
    // 点光，但 Virtual Light 不读取光源位置，也不执行点光的平方反比衰减。
    return CreateVirtualLight(
        uboVP.cameraPosition - surface.worldPosition,
        radiance);
}

float EvaluateVirtualLightVisibility(
    in vec3 visibilityNormal,
    in VirtualLight light)
{
    // 可见性只负责抑制几何背面漏光；具体材质的 NoL、BRDF 或 SSS response
    // 仍由 Hair/Skin evaluator 自己计算，避免公共模块重复乘余弦项。
    return max(
        dot(normalize(visibilityNormal), light.direction),
        0.0);
}

#endif
