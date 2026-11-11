#ifndef PARTICLE_ALIVE_GLSL
#define PARTICLE_ALIVE_GLSL

// 未排序路径用紧凑存活索引绘制：instance 编号 -> aliveIndices[instance]
// -> 粒子池索引。排序路径仍直接使用 sortedPairs[instance].y。
layout(std430, set = 0, binding = 4) readonly buffer ParticleAliveIndices
{
    uint indices[];
} aliveIndices;

#endif
