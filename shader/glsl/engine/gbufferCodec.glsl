#ifndef VL_ENGINE_GBUFFER_CODEC_GLSL
#define VL_ENGINE_GBUFFER_CODEC_GLSL

#include "materialSurface.glsl"

struct GBufferData
{
    // A: base color + opacity.
    vec4 gbufferA;
    // B: world normal encoded to 0..1.
    // Alpha uses a UE-style packed integer value:
    //   low  4 bits: ShadingModelID, enough for 0..15 material shading models.
    //   high 4 bits: feature flags / SelectiveOutputMask, reserved for later phases.
    // The attachment is float today, but the value is intentionally treated like an 8-bit integer.
    vec4 gbufferB;
    // C: metallic, specular, roughness, ambient occlusion.
    vec4 gbufferC;
    // D: shading-model-specific custom data. Valid when GBUFFER_HAS_CUSTOM_DATA_MASK is set.
    vec4 gbufferD;
    // E: precomputed shadow factors / lightmap-style visibility data.
    // Phase 8 treats .r as a coarse precomputed direct-light visibility multiplier; default is 1.
    vec4 gbufferE;
    // Velocity: xy = current screen uv - previous screen uv, zw reserved.
    vec4 gbufferVelocity;
    // F: world tangent encoded to 0..1, alpha = anisotropy. Valid when GBUFFER_HAS_ANISOTROPY_MASK is set.
    vec4 gbufferF;
    // Not a GBuffer slot: base pass scene contribution such as emissive.
    vec4 sceneColorBase;
};

struct GBufferPixelData
{
    vec4 worldTangent;
    vec2 velocity;
};

GBufferPixelData CreateDefaultGBufferPixelData()
{
    GBufferPixelData data;
    data.worldTangent = vec4(1.0, 0.0, 0.0, 1.0);
    data.velocity = vec2(0.0);
    return data;
}

GBufferPixelData CreateGBufferPixelData(in vec4 worldTangent, in vec2 velocity)
{
    GBufferPixelData data;
    data.worldTangent = worldTangent;
    data.velocity = velocity;
    return data;
}

float EncodeGBufferPacked(uint shadingModel, uint flags)
{
    // Keep only the low nibble from shadingModel and the high nibble from flags, then merge them:
    //   shadingModel 0000ssss & 0x0f -> 0000ssss
    //   flags        ffff0000 & 0xf0 -> ffff0000
    //   packed                 -> ffffssss
    return float((shadingModel & 0x0fu) | (flags & 0xf0u));
}

uint DecodeGBufferShadingModel(float packedValue)
{
    // The packed value is stored in a float render target, so round it back to the intended
    // integer value before masking out the low 4-bit ShadingModelID.
    return uint(packedValue + 0.5) & 0x0fu;
}

uint DecodeGBufferFlags(float packedValue)
{
    // High 4 bits are SelectiveOutputMask flags such as custom data, velocity and anisotropy.
    return uint(packedValue + 0.5) & 0xf0u;
}

vec2 CalculateGBufferVelocity(vec4 clipPosition, vec4 previousClipPosition)
{
    // Velocity 使用屏幕 uv 空间差值，便于后续 TAA / motion blur 直接按屏幕采样偏移理解。
    // VulkanLearn 的 projection 已经输出 Vulkan NDC，因此这里直接从 clip.xy / clip.w 转到 0..1 uv。
    vec2 currentUv = (clipPosition.xy / clipPosition.w) * 0.5 + 0.5;
    vec2 previousUv = (previousClipPosition.xy / previousClipPosition.w) * 0.5 + 0.5;
    return currentUv - previousUv;
}

vec3 EncodeGBufferDirection(vec3 direction)
{
    return direction * 0.5 + 0.5;
}

vec3 DecodeGBufferDirection(vec3 encodedDirection)
{
    return normalize(encodedDirection * 2.0 - 1.0);
}

// UE Legacy Clear Coat 不直接存储完整底层法线，而是分别把顶层、底层法线映射到
// 八面体平面，再保存二者的相对偏移。这样只需占用 CustomData 剩余的两个通道。
vec2 UnitVectorToOctahedron(vec3 direction)
{
    vec3 normal = normalize(direction);
    normal.xy /= dot(vec3(1.0), abs(normal));
    if (normal.z <= 0.0)
    {
        vec2 signValue = mix(
            vec2(-1.0),
            vec2(1.0),
            greaterThanEqual(normal.xy, vec2(0.0)));
        normal.xy = (vec2(1.0) - abs(normal.yx)) * signValue;
    }
    return normal.xy;
}

vec3 OctahedronToUnitVector(vec2 octahedron)
{
    vec3 normal = vec3(
        octahedron,
        1.0 - dot(vec2(1.0), abs(octahedron)));
    float fold = max(-normal.z, 0.0);
    vec2 foldOffset = mix(
        vec2(fold),
        vec2(-fold),
        greaterThanEqual(normal.xy, vec2(0.0)));
    normal.xy += foldOffset;
    return normalize(normal);
}

vec2 EncodeClearCoatBottomNormal(
    vec3 topNormal,
    vec3 bottomNormal)
{
    vec2 topOctahedron = UnitVectorToOctahedron(topNormal);
    vec2 bottomOctahedron = UnitVectorToOctahedron(bottomNormal);
    // 0.5 对八面体平面上的相对偏移做统一 half-scale 压缩；它不代表球面角度
    // 被均匀压缩，不同方向的角度分布仍由八面体映射决定。
    // 使用 128/255 而不是 0.5 是为了保留 UE 以 8 bit 中心值定义的编码语义；
    // 当前 GBufferD 虽为浮点格式，仍沿用相同常量保证编解码合同一致。
    return
        (bottomOctahedron - topOctahedron) * 0.5 +
        vec2(128.0 / 255.0);
}

vec3 DecodeClearCoatBottomNormal(
    vec3 topNormal,
    vec2 encodedBottomNormal)
{
    // 编码阶段的逆过程：乘 2.0 恢复 half-scale 压缩前的相对偏移；
    // 256/255 等于 2 * 128/255，用于抵消编码中心偏置。
    vec2 bottomOctahedron =
        encodedBottomNormal * 2.0 -
        vec2(256.0 / 255.0) +
        UnitVectorToOctahedron(topNormal);
    return OctahedronToUnitVector(bottomOctahedron);
}

GBufferData EncodeGBuffer(in MaterialSurface surface, in GBufferPixelData pixelData)
{
    GBufferData data;
    data.gbufferA = vec4(surface.baseColor, surface.opacity);
    data.gbufferB = vec4(EncodeGBufferDirection(normalize(surface.worldNormal)), EncodeGBufferPacked(surface.shadingModel, surface.selectiveOutputMask));
    data.gbufferC = vec4(surface.metallic, 0.5, surface.roughness, surface.ambientOcclusion);
    data.gbufferD = surface.customData;
    if (surface.shadingModel == SHADING_MODEL_CLEAR_COAT)
    {
        vec2 encodedBottomNormal = EncodeClearCoatBottomNormal(
            surface.worldNormal,
            surface.clearCoatBottomNormal);
        // 对齐 UE Legacy Clear Coat 的通道顺序：编码 x 写入 CustomData.a，
        // 编码 y 写入 CustomData.z；CustomData.xy 已由清漆权重和粗糙度占用。
        data.gbufferD.w = encodedBottomNormal.x;
        data.gbufferD.z = encodedBottomNormal.y;
    }
    data.gbufferE = surface.precomputedShadowFactors;
    data.gbufferVelocity = vec4(pixelData.velocity, 0.0, 0.0);
    data.gbufferF = vec4(EncodeGBufferDirection(normalize(surface.worldTangent.xyz)), surface.anisotropy);
    data.sceneColorBase = vec4(surface.emissiveColor, surface.opacity);
    return data;
}

MaterialSurface DecodeGBufferSurface(in GBufferData data)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.baseColor = data.gbufferA.rgb;
    surface.opacity = data.gbufferA.a;
    surface.worldNormal = DecodeGBufferDirection(data.gbufferB.rgb);
    surface.shadingModel = DecodeGBufferShadingModel(data.gbufferB.a);
    surface.selectiveOutputMask = DecodeGBufferFlags(data.gbufferB.a);
    surface.metallic = data.gbufferC.r;
    surface.roughness = data.gbufferC.b;
    surface.ambientOcclusion = data.gbufferC.a;
    surface.customData = data.gbufferD;
    if (surface.shadingModel == SHADING_MODEL_CLEAR_COAT)
    {
        // 先恢复相对八面体坐标，再以顶层 worldNormal 为基准重建底层法线。
        surface.clearCoatBottomNormal = DecodeClearCoatBottomNormal(
            surface.worldNormal,
            vec2(surface.customData.w, surface.customData.z));
    }
    else
    {
        // 非 Clear Coat 像素不消费 GBufferD.zw，统一回退到表面法线。
        surface.clearCoatBottomNormal = surface.worldNormal;
    }
    surface.precomputedShadowFactors = data.gbufferE;
    surface.worldTangent = vec4(DecodeGBufferDirection(data.gbufferF.rgb), 1.0);
    surface.anisotropy = data.gbufferF.a;
    surface.emissiveColor = data.sceneColorBase.rgb;
    return surface;
}

#endif
