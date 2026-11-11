#ifndef PARTICLE_SORTED_GLSL
#define PARTICLE_SORTED_GLSL

// graphics 只读最终 pair 表；xy = (viewDepth, originalParticleIndex)。
// 排序关闭时 binding 仍绑定 pairA，但 vertex 走直接索引分支，不读取内容。
layout(std430, set = 0, binding = 3) readonly buffer SortedPairs
{
    vec4 pairs[];
} sortedPairs;

#endif
