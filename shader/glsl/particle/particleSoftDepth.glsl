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

// 半分辨率软深度资源的双边 upsample；1x1/未提供的占位资源回退 5 tap。
float SampleParticleSoftViewDepth(
    sampler2D softDepth,
    sampler2D sceneDepth,
    vec2 fragCoord,
    float tapRadiusPixels,
    mat4 invProjection,
    float thicknessCm)
{
    ivec2 softSize = textureSize(softDepth, 0);
    if (softSize.x <= 1 || softSize.y <= 1)
    {
        return SampleParticleSceneViewDepth(
            sceneDepth, fragCoord, tapRadiusPixels, invProjection);
    }

    vec2 fullSize = vec2(textureSize(sceneDepth, 0));
    vec2 uv = clamp(
        fragCoord / max(fullSize, vec2(1.0)),
        vec2(0.0),
        vec2(1.0));
    float centerDepth = ParticleViewDepthFromDeviceDepth(
        texture(sceneDepth, uv).r,
        invProjection);
    float tolerance = thicknessCm > 0.0
        ? max(thicknessCm, 1.0)
        : 50.0;

    vec2 halfCoord = uv * vec2(softSize) - 0.5;
    ivec2 base = ivec2(floor(halfCoord));
    vec2 fraction = fract(halfCoord);
    float weightedDepth = 0.0;
    float totalWeight = 0.0;
    for (int y = 0; y < 2; ++y)
    {
        for (int x = 0; x < 2; ++x)
        {
            ivec2 coord = clamp(
                base + ivec2(x, y),
                ivec2(0),
                softSize - ivec2(1));
            float bilinear = (x == 0 ? 1.0 - fraction.x : fraction.x) *
                (y == 0 ? 1.0 - fraction.y : fraction.y);
            float depth = texelFetch(softDepth, coord, 0).r;
            float depthWeight = exp(
                -abs(depth - centerDepth) / tolerance);
            float weight = max(bilinear, 1.0e-4) * depthWeight;
            weightedDepth += depth * weight;
            totalWeight += weight;
        }
    }
    return totalWeight > 1.0e-6
        ? weightedDepth / totalWeight
        : centerDepth;
}

// thicknessCm=0 保持旧行为；非零时限制 fade 的增长速度，避免薄几何
// 在深度过滤边缘出现硬切。
float ParticleSoftFade(
    float depthDelta,
    float fadeDistance,
    float thicknessCm)
{
    float fade = clamp(
        depthDelta / max(fadeDistance, 0.001),
        0.0,
        1.0);
    if (thicknessCm > 0.0)
    {
        fade = min(
            fade,
            clamp(depthDelta / max(thicknessCm, 0.001), 0.0, 1.0));
    }
    return fade;
}

#endif
