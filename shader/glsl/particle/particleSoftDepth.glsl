#ifndef PARTICLE_SOFT_DEPTH_GLSL
#define PARTICLE_SOFT_DEPTH_GLSL

// 软粒子深度：中心 + 四向十字共 5 tap，取最近的不透明表面视深度。
// 单采样在轮廓/深度不连续处会产生硬边或错误淡出；多 tap 用最近表面
// 同时承担“与场景深度碰撞”的保守判定。
float ParticleViewDepthFromDeviceDepth(
    float deviceDepth,
    mat4 invProjection)
{
    vec4 sceneView =
        invProjection * vec4(0.0, 0.0, deviceDepth, 1.0);
    return -sceneView.z / max(sceneView.w, 1.0e-6);
}

float SampleParticleSceneViewDepth(
    sampler2D sceneDepth,
    vec2 fragCoord,
    float tapRadiusPixels,
    mat4 invProjection)
{
    vec2 size = vec2(textureSize(sceneDepth, 0));
    vec2 pixel = 1.0 / max(size, vec2(1.0));
    vec2 uv = fragCoord / max(size, vec2(1.0));
    float radius = clamp(tapRadiusPixels, 0.0, 32.0);
    if (radius <= 0.5)
    {
        return ParticleViewDepthFromDeviceDepth(
            texture(sceneDepth, uv).r,
            invProjection);
    }
    vec2 offset = pixel * radius;
    float depth = ParticleViewDepthFromDeviceDepth(
        texture(sceneDepth, uv).r,
        invProjection);
    depth = min(depth, ParticleViewDepthFromDeviceDepth(
        texture(sceneDepth, uv + vec2(offset.x, 0.0)).r,
        invProjection));
    depth = min(depth, ParticleViewDepthFromDeviceDepth(
        texture(sceneDepth, uv - vec2(offset.x, 0.0)).r,
        invProjection));
    depth = min(depth, ParticleViewDepthFromDeviceDepth(
        texture(sceneDepth, uv + vec2(0.0, offset.y)).r,
        invProjection));
    depth = min(depth, ParticleViewDepthFromDeviceDepth(
        texture(sceneDepth, uv - vec2(0.0, offset.y)).r,
        invProjection));
    return depth;
}

#endif
