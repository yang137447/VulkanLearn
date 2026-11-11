#ifndef PARTICLE_DISSOLVE_GLSL
#define PARTICLE_DISSOLVE_GLSL

float ParticleDissolveHash(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.x + p.y) * p.z);
}

float ParticleDissolveNoise(vec3 p)
{
    vec3 cell = floor(p);
    vec3 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float n000 = ParticleDissolveHash(cell);
    float n100 = ParticleDissolveHash(cell + vec3(1.0, 0.0, 0.0));
    float n010 = ParticleDissolveHash(cell + vec3(0.0, 1.0, 0.0));
    float n110 = ParticleDissolveHash(cell + vec3(1.0, 1.0, 0.0));
    float n001 = ParticleDissolveHash(cell + vec3(0.0, 0.0, 1.0));
    float n101 = ParticleDissolveHash(cell + vec3(1.0, 0.0, 1.0));
    float n011 = ParticleDissolveHash(cell + vec3(0.0, 1.0, 1.0));
    float n111 = ParticleDissolveHash(cell + vec3(1.0, 1.0, 1.0));
    return mix(
        mix(mix(n000, n100, f.x), mix(n010, n110, f.x), f.y),
        mix(mix(n001, n101, f.x), mix(n011, n111, f.x), f.y),
        f.z);
}

float ParticleDissolveFbm(vec3 p)
{
    float value = 0.0;
    float amplitude = 0.55;
    for (int octave = 0; octave < 4; ++octave)
    {
        value += ParticleDissolveNoise(p) * amplitude;
        p = p * 2.03 + vec3(17.1, 9.2, 3.7);
        amplitude *= 0.5;
    }
    return value;
}

// 返回 visibility 与自发光边缘强度；作者关闭 dissolve 时直接返回 1/0。
void EvaluateParticleDissolve(
    vec3 worldPosition,
    float lifeT,
    float seed,
    out float visibility,
    out float edge)
{
    visibility = 1.0;
    edge = 0.0;
    if (frame.dissolveParams2.z < 0.5)
    {
        return;
    }
    vec3 samplePosition =
        worldPosition * max(frame.dissolveParams.w, 0.001) +
        vec3(seed * 13.0, frame.simulation.z *
            frame.dissolveParams2.y, seed * 7.0);
    float noise = clamp(ParticleDissolveFbm(samplePosition), 0.0, 1.0);
    float threshold = mix(
        min(frame.dissolveParams.x, frame.dissolveParams.y),
        max(frame.dissolveParams.x, frame.dissolveParams.y),
        clamp(lifeT, 0.0, 1.0));
    float width = max(frame.dissolveParams.z, 1.0e-4);
    visibility = smoothstep(
        threshold - width,
        threshold,
        noise);
    edge = 1.0 - smoothstep(
        threshold,
        threshold + width,
        noise);
    edge *= step(threshold - width, noise);
}

#endif
