#ifndef PARTICLE_STORAGE_RO_GLSL
#define PARTICLE_STORAGE_RO_GLSL

// graphics 阶段只读；设备未开启 vertexPipelineStoresAndAtomics，
// vertex 读取必须带 NonWritable/readonly 装饰。
layout(std430, set = 0, binding = 1) readonly buffer ParticleBuffer
{
    Particle particles[];
} particleBuffer;

#endif
