#ifndef PARTICLE_STORAGE_RW_GLSL
#define PARTICLE_STORAGE_RW_GLSL

layout(std430, set = 0, binding = 1) buffer ParticleBuffer
{
    Particle particles[];
} particleBuffer;

#endif
