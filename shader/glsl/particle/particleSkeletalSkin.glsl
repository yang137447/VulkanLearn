#ifndef PARTICLE_SKELETAL_SKIN_GLSL
#define PARTICLE_SKELETAL_SKIN_GLSL

#if defined(VL_SKINNED_MESH) && VL_SKINNED_MESH
// 骨骼网格粒子的 GPU skinning。
//
// 数据合同（与 source/particles/particleRenderer.cpp 的
// EmitterResources::SkeletalSection 一一对应）：
//   set 2 binding 0 = 关节 palette（std430，每关节 mat4，CPU 每帧写当前姿态的
//                     model_j · inverseBind_j）
//   set 2 binding 1 = CSR 影响流（uint 流：顶点 i 的区间是
//                     [words[i], words[i+1])，区间内每两项为 (关节, bitcast float 权重)）
//
// 顶点缓冲保存的是绑定姿态，形变只在这里发生；粒子实例化复用同一份顶点，
// 不复制每实例蒙皮结果。
struct ParticleSkinJoint
{
    mat4 current;
    mat4 previous;
};
layout(std430, set = 2, binding = 0) readonly buffer ParticleSkinPalette
{
    ParticleSkinJoint joints[];
} particleSkinPalette;
layout(std430, set = 2, binding = 1) readonly buffer ParticleSkinInfluences
{
    uint words[];
} particleSkinInfluences;

// 顶点权重区间；palette 为空时返回空区间（等价于单位变换）。
void ParticleSkinInfluenceRange(
    out uint begin,
    out uint end)
{
    begin = particleSkinInfluences.words[gl_VertexIndex];
    end = particleSkinInfluences.words[gl_VertexIndex + 1u];
}

void ParticleSkinVertex(
    vec3 localPosition,
    vec3 localNormal,
    vec4 localTangent,
    out vec3 skinnedPosition,
    out vec3 skinnedNormal,
    out vec4 skinnedTangent)
{
    mat4 current = mat4(0.0);
    uint begin = 0u;
    uint end = 0u;
    ParticleSkinInfluenceRange(begin, end);
    for (uint index = begin; index < end; index += 2u)
    {
        uint joint = particleSkinInfluences.words[index];
        float weight = uintBitsToFloat(
            particleSkinInfluences.words[index + 1u]);
        current += weight * particleSkinPalette.joints[joint].current;
    }
    skinnedPosition = (current * vec4(localPosition, 1.0)).xyz;
    mat3 linear = mat3(current);
    skinnedNormal = normalize(transpose(inverse(linear)) * localNormal);
    vec3 tangent = linear * localTangent.xyz;
    skinnedTangent = vec4(
        normalize(tangent - skinnedNormal * dot(skinnedNormal, tangent)),
        localTangent.w * sign(determinant(linear)));
}
#endif

#endif
