#ifndef VL_SKINNED_VERTEX_FACTORY_GLSL
#define VL_SKINNED_VERTEX_FACTORY_GLSL

#if defined(VL_SKINNED_MESH) && VL_SKINNED_MESH
struct SkinJoint { mat4 current; mat4 previous; };
layout(std430, set = 2, binding = 1) readonly buffer SkinPalette { SkinJoint joints[]; } skinPalette;
// CSR 头保存绝对 word offsets，尾部为 joint/floatBits(weight)；保留全部影响，不限四/八骨。
layout(std430, set = 2, binding = 2) readonly buffer SkinInfluences { uint words[]; } skinInfluences;

vec3 ApplySkinnedVertexFactory(inout MaterialVertexInput inputVertex)
{
    mat4 current = mat4(0.0);
    mat4 previous = mat4(0.0);
    uint end = skinInfluences.words[gl_VertexIndex + 1];
    for (uint i = skinInfluences.words[gl_VertexIndex]; i < end; i += 2)
    {
        uint joint = skinInfluences.words[i];
        float weight = uintBitsToFloat(skinInfluences.words[i + 1]);
        current += weight * skinPalette.joints[joint].current;
        previous += weight * skinPalette.joints[joint].previous;
    }
    vec3 previousPosition = (previous * vec4(inputVertex.localPosition, 1.0)).xyz;
    inputVertex.localPosition = (current * vec4(inputVertex.localPosition, 1.0)).xyz;
    // 法线取混合仿射的逆转置，切线取线性变换后正交化；镜像改变 Mikk handedness。
    mat3 linear = mat3(current);
    vec3 normal = normalize(transpose(inverse(linear)) * inputVertex.localNormal);
    vec3 tangent = linear * inputVertex.localTangent.xyz;
    inputVertex.localNormal = normal;
    inputVertex.localTangent = vec4(normalize(tangent - normal * dot(normal, tangent)),
        inputVertex.localTangent.w * sign(determinant(linear)));
    return previousPosition;
}
#endif
#endif
