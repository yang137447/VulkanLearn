#ifndef VL_SKINNED_VERTEX_MATH_GLSL
#define VL_SKINNED_VERTEX_MATH_GLSL

// 蒙皮数学的唯一实现：Base/ShadowDepth 顶点工厂与 skinnedDeform.comp 都 include
// 本文件。palette/influences 的 Set 索引由包含方在 include 前用
// VL_SKIN_SET_INDEX 指定（图形路径 Set 2，compute 路径 Set 0），布局本身完全一致。
#ifndef VL_SKIN_SET_INDEX
#define VL_SKIN_SET_INDEX 2
#endif

struct SkinJoint { mat4 current; mat4 previous; };
// CSR 头保存绝对 word offsets，尾部为 joint/floatBits(weight)；保留全部影响，不限四/八骨。
layout(std430, set = VL_SKIN_SET_INDEX, binding = 1) readonly buffer SkinPalette { SkinJoint joints[]; } skinPalette;
layout(std430, set = VL_SKIN_SET_INDEX, binding = 2) readonly buffer SkinInfluences { uint words[]; } skinInfluences;

// 逐顶点混合 current/previous 两个仿射矩阵；权重由同一份不可变 CSR 流驱动。
void SkinBlendMatrices(uint vertexIndex, out mat4 current, out mat4 previous)
{
    current = mat4(0.0);
    previous = mat4(0.0);
    uint end = skinInfluences.words[vertexIndex + 1];
    for (uint i = skinInfluences.words[vertexIndex]; i < end; i += 2)
    {
        uint joint = skinInfluences.words[i];
        float weight = uintBitsToFloat(skinInfluences.words[i + 1]);
        current += weight * skinPalette.joints[joint].current;
        previous += weight * skinPalette.joints[joint].previous;
    }
}

// 位置/法线/切线/上一帧位置的全部变换。法线取混合仿射的逆转置，切线取线性变换后
// 正交化；镜像改变 Mikk handedness。compute 直接把结果写入 pre-skin buffer，
// 顶点工厂在 VL_SKINNED_PRERENDERED 路径原样读取，因此两条路径必须逐位同式。
void SkinVertex(uint vertexIndex, vec3 localPosition, vec3 localNormal, vec4 localTangent,
    out vec3 position, out vec3 normal, out vec4 tangent, out vec3 previousPosition)
{
    mat4 current;
    mat4 previous;
    SkinBlendMatrices(vertexIndex, current, previous);

    previousPosition = (previous * vec4(localPosition, 1.0)).xyz;
    position = (current * vec4(localPosition, 1.0)).xyz;

    mat3 linear = mat3(current);
    normal = normalize(transpose(inverse(linear)) * localNormal);
    vec3 transformedTangent = linear * localTangent.xyz;
    tangent = vec4(normalize(transformedTangent - normal * dot(normal, transformedTangent)),
        localTangent.w * sign(determinant(linear)));
}

#endif
