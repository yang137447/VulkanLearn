#ifndef VL_ENGINE_DECAL_COMPOSITE_GLSL
#define VL_ENGINE_DECAL_COMPOSITE_GLSL

#include "materialSurface.glsl"

// DBufferB.rgb 是覆盖率先乘后的 world normal 之和 P = Σ N_i·w_i，
// DBufferB.a 是 over 覆盖率 c。这里不恢复"平均编码法线"，而是直接
// 用 P 的模长做相干强度（单层时 |P| = c），再与接收表面法线合成。
//
// 同半球：whiteout（坡度叠加）。把 decal 法线表达在接收表面的切平面
// 基里，base 自身的坡度为 0，whiteout 的坡度形式为
//   normalize(base + T · slopeX + B · slopeY)
// coverage 缩放 detail 的坡度；单层时 0 精确回到 base、1 精确回到 decal。
// 这正是 UE 5.8 legacy DBuffer 的线性
//   normalize(base * opacity + premulNormal)
// 在相反法线、coverage=0.5 时会相消到 0 的替代方案。
//
// 跨半球：gnomonic 坡度在 dz <= 0 处无定义，改用单位球上的测地线
// （great-circle）插值；对跖点的 50/50 会得到与 base 垂直的单位向量，
// 绝不返回 0/NaN。该分支的选择理由与残余近似见
// documents/contracts/decal-runtime.md。
vec3 DecalBlendWorldNormal(
    vec3 baseNormal,
    vec3 premultipliedDecalNormal,
    float coverage)
{
    const float epsilon = 1.0e-6;
    // base 来自 GBuffer decode，正常情况下已是单位向量；零向量时
    // normalize 会产生 NaN，与 CPU 镜像一致地回退到 +Z。
    float baseLengthSq = dot(baseNormal, baseNormal);
    baseNormal = baseLengthSq > epsilon * epsilon
        ? baseNormal * inversesqrt(baseLengthSq)
        : vec3(0.0, 0.0, 1.0);

    float weight = clamp(coverage, 0.0, 1.0);
    float decalLengthSq =
        dot(premultipliedDecalNormal, premultipliedDecalNormal);
    if (weight <= 0.0 || decalLengthSq <= epsilon * epsilon)
        return baseNormal;

    float decalLength = sqrt(decalLengthSq);
    vec3 decalNormal = premultipliedDecalNormal / decalLength;
    // 数学上 |P| <= c；取最小值只是防御 fp16 累积误差。
    float fade = min(weight, decalLength);

    // 以接收表面法线为 +Z 建正交基；helper 与 base 至少约 25.8°，
    // 叉积不会退化。
    vec3 helper = abs(baseNormal.x) < 0.9
        ? vec3(1.0, 0.0, 0.0)
        : vec3(0.0, 0.0, 1.0);
    vec3 tangent = normalize(cross(helper, baseNormal));
    vec3 bitangent = cross(baseNormal, tangent);

    float dz = dot(decalNormal, baseNormal);
    float dx = dot(decalNormal, tangent);
    float dy = dot(decalNormal, bitangent);

    if (dz > 0.0)
    {
        float slopeX = (dx / dz) * fade;
        float slopeY = (dy / dz) * fade;
        return normalize(
            baseNormal +
            tangent * slopeX +
            bitangent * slopeY);
    }

    float sinAngle = sqrt(dx * dx + dy * dy);
    vec3 axis = tangent;
    float angle = 3.141592653589793 * fade;
    if (sinAngle > epsilon)
    {
        axis = (tangent * dx + bitangent * dy) / sinAngle;
        angle = acos(clamp(dz, -1.0, 1.0)) * fade;
    }
    return baseNormal * cos(angle) + axis * sin(angle);
}

// DBuffer A: baseColor.rgb + color coverage
// DBuffer B: premultiplied world normal + normal coverage
// DBuffer C: roughness/ambientOcclusion + roughness coverage
// DBuffer D: metallic + metallic coverage
// DBufferMask: r = color, g = normal, b = roughness, a = metallic
// DBufferEmissive: rgb = emissive * strength * coverage（预乘），a = coverage
// DBuffer 的 RGB 已由 AlphaBlend（SrcAlpha / OneMinusSrcAlpha）预乘覆盖率，
// 所以这里必须用 over 形式累加，不能再乘一次 alpha。
void ApplyDecalDBuffer(
    inout MaterialSurface material,
    vec4 colorBuffer,
    vec4 normalBuffer,
    vec4 surfaceBuffer,
    vec4 metallicBuffer,
    vec4 responseMask)
{
    // 掩码用 One/One 累积，重叠 decal 可能让同一通道大于 1；
    // saturate 后只保留“该通道是否允许覆盖”的 0/1 语义。
    vec4 response = clamp(responseMask, 0.0, 1.0);

    float colorCoverage =
        clamp(colorBuffer.a, 0.0, 1.0) * response.r;
    if (colorCoverage > 0.0)
    {
        material.baseColor =
            colorBuffer.rgb +
            material.baseColor * (1.0 - colorCoverage);
    }

    float normalCoverage =
        clamp(normalBuffer.a, 0.0, 1.0) * response.g;
    if (normalCoverage > 0.0)
    {
        // 不再除以 normalCoverage：P 本身就是预乘和，除法会在低覆盖率
        // 时放大 fp16 噪声，也无法表达多层法线的相消程度。
        material.worldNormal = DecalBlendWorldNormal(
            material.worldNormal,
            normalBuffer.rgb,
            normalCoverage);
    }

    float surfaceCoverage = clamp(surfaceBuffer.a, 0.0, 1.0);
    // roughness 和 metallic 分开控位；DecalResponse 没有独立 AO 位，
    // 因此 AO 沿用 roughness 位。
    float roughnessCoverage = surfaceCoverage * response.b;
    if (roughnessCoverage > 0.0)
    {
        material.roughness =
            surfaceBuffer.r +
            material.roughness * (1.0 - roughnessCoverage);
        material.ambientOcclusion =
            surfaceBuffer.g +
            material.ambientOcclusion * (1.0 - roughnessCoverage);
    }

    float metallicCoverage =
        clamp(metallicBuffer.a, 0.0, 1.0) * response.a;
    if (metallicCoverage > 0.0)
    {
        material.metallic =
            metallicBuffer.r +
            material.metallic * (1.0 - metallicCoverage);
    }
}

// Emissive 在直接/间接光之后作为独立附加项合成，不参与任何光照计算，
// 也不读取 DBufferMask：它没有 color/normal/roughness/metallic 响应位。
// RGB 已在写端乘过 coverage 并由 One/OneMinusSrcAlpha 完成 over 合成，
// 因此这里只做加法；覆盖率为 0 时 RGB 本来就是 0。
vec3 ResolveDecalEmissive(vec4 emissiveBuffer)
{
    return max(emissiveBuffer.rgb, vec3(0.0));
}

#endif
