#ifndef PARTICLE_SKELETAL_SKIN_GLSL
#define PARTICLE_SKELETAL_SKIN_GLSL

#if defined(VL_SKINNED_MESH) && VL_SKINNED_MESH
struct ParticleSkinJoint { mat4 current; mat4 previous; };
layout(std430, set = 2, binding = 1) readonly buffer SkinPalette
{
    ParticleSkinJoint joints[];
} skinPalette;
layout(std430, set = 2, binding = 2) readonly buffer SkinInfluences
{
    uint words[];
} skinInfluences;

void ParticleSkinVertex(
    vec3 localPosition,
    vec3 localNormal,
    vec4 localTangent,
    out vec3 skinnedPosition,
    out vec3 skinnedNormal,
    out vec4 skinnedTangent)
{
    mat4 current = mat4(0.0);
    uint begin = skinInfluences.words[gl_VertexIndex];
    uint end = skinInfluences.words[gl_VertexIndex + 1u];
    for (uint index = begin; index < end; index += 2u)
    {
        uint joint = skinInfluences.words[index];
        float weight = uintBitsToFloat(skinInfluences.words[index + 1u]);
        current += weight * skinPalette.joints[joint].current;
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
