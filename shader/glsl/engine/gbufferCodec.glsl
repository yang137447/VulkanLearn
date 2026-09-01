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
    // F: world tangent/anisotropy for ordinary lit models, or secondary SSS custom data for IDs 2 and 3.
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

float EncodeClothAnisotropy(
    float anisotropy,
    float anisotropyCross)
{
    // GBufferF.w 是 R16F，不能假设它能无损保存两个 8 bit 通道；v2 采用
    // 5+5 bit packed 语义，在半精度下仍能稳定 round-trip，误差约为 1/31。
    uint anisotropyCode = uint(round(
        (anisotropy * 0.5 + 0.5) * 31.0));
    uint crossCode = uint(round(anisotropyCross * 31.0));
    return float((anisotropyCode << 5u) | crossCode) / 1023.0;
}

vec2 DecodeClothAnisotropy(float encodedValue)
{
    uint packedValue = uint(round(encodedValue * 1023.0));
    uint anisotropyCode = (packedValue >> 5u) & 31u;
    uint crossCode = packedValue & 31u;
    return vec2(
        float(anisotropyCode) / 31.0 * 2.0 - 1.0,
        float(crossCode) / 31.0);
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


// Eye GBuffer V1 不复用普通 customData：所有字段都由本表独占并版本化。
const uint EYE_GBUFFER_ENCODING_VERSION = 1u;

float EncodeEyeProfilePair(
    float causticProfileId,
    float scleraProfileId,
    float validIrisHit)
{
    uint causticId = uint(causticProfileId + 0.5);
    uint scleraId = uint(scleraProfileId + 0.5);
    uint valid = validIrisHit > 0.5 ? 1u : 0u;
    return float(
        causticId +
        scleraId * 16u +
        valid * 256u +
        (EYE_GBUFFER_ENCODING_VERSION << 9u));
}

uint DecodeEyePackedProfile(float encodedValue)
{
    return uint(encodedValue + 0.5);
}

bool IsEyePackedProfileVersionValid(uint packedProfile)
{
    return ((packedProfile >> 9u) & 0x7fu) ==
        EYE_GBUFFER_ENCODING_VERSION;
}

GBufferData EncodeGBuffer(in MaterialSurface surface, in GBufferPixelData pixelData)
{
    GBufferData data;
    // Eye 的 A 通道保存 Base Pass 已采样的 iris color；其它模型保持既有 baseColor 语义。
    data.gbufferA = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(surface.modelInputs.eye.irisColor, surface.opacity)
        : surface.shadingModel == SHADING_MODEL_HAIR
            ? vec4(surface.hairAbsorption, surface.opacity)
            : vec4(surface.baseColor, surface.opacity);
    // ID 2 为了保持 opaque GBuffer 预算，把 transmissionWeight 放在 GBufferA.a。
    if (surface.shadingModel == SHADING_MODEL_SUBSURFACE)
    {
        data.gbufferA.a =
            surface.modelInputs.subsurface.transmissionWeight;
    }
    data.gbufferB = vec4(EncodeGBufferDirection(normalize(surface.worldNormal)), EncodeGBufferPacked(surface.shadingModel, surface.selectiveOutputMask));
    // Eye metallic/specular 不参与 evaluator；这两个通道改存 IOR 与 caustic strength。
    data.gbufferC = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(
            surface.modelInputs.eye.corneaIor,
            surface.modelInputs.eye.causticStrength,
            surface.roughness,
            surface.ambientOcclusion)
        : vec4(surface.metallic, 0.5, surface.roughness, surface.ambientOcclusion);
    data.gbufferD = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(
            surface.modelInputs.eye.irisUv,
            EncodeEyeProfilePair(
                surface.modelInputs.eye.causticProfileId,
                surface.modelInputs.eye.scleraProfileId,
                surface.modelInputs.eye.validIrisHit),
            surface.modelInputs.eye.irisMask)
        : surface.customData;
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
    // Eye E.rgb 保存 sclera color，E.a 保存 iris radius；Eye evaluator 自己计算 shadow。
    data.gbufferE = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(
            surface.modelInputs.eye.scleraColor,
            surface.modelInputs.eye.irisRadius)
        : surface.precomputedShadowFactors;
    data.gbufferVelocity = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(
            pixelData.velocity,
            surface.modelInputs.eye.pupilRadius /
                surface.modelInputs.eye.irisRadius,
            surface.modelInputs.eye.limbusWidth /
                surface.modelInputs.eye.irisRadius)
        : vec4(pixelData.velocity, 0.0, 0.0);
    data.gbufferF = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(
            EncodeGBufferDirection(
                normalize(surface.modelInputs.eye.irisNormal)),
            surface.modelInputs.eye.irisDistance)
        : surface.shadingModel == SHADING_MODEL_HAIR
        ? vec4(
            EncodeGBufferDirection(normalize(surface.worldTangent.xyz)),
            surface.worldTangent.w)
        : surface.shadingModel == SHADING_MODEL_CLOTH
        ? vec4(
            EncodeGBufferDirection(normalize(surface.worldTangent.xyz)),
            EncodeClothAnisotropy(
                surface.modelInputs.cloth.anisotropy,
                surface.modelInputs.cloth.anisotropyCross))
        : vec4(
            EncodeGBufferDirection(normalize(surface.worldTangent.xyz)),
            surface.anisotropy);
    // 三类 SSS 使用互不重叠的 customData 语义；decode 必须与 materialInputs 一一对应。
    if (surface.shadingModel == SHADING_MODEL_SUBSURFACE)
    {
        data.gbufferF = vec4(
            surface.modelInputs.subsurface.wrapWidth,
            surface.modelInputs.subsurface.backscatterPower,
            surface.modelInputs.subsurface.backscatterWeight,
            surface.modelInputs.subsurface.thickness);
    }
    else if (surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN)
    {
        data.gbufferF = vec4(
            surface.modelInputs.preintegratedSkin.curvature,
            surface.modelInputs.preintegratedSkin.transmissionWeight,
            0.0,
            0.0);
    }
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
    if (surface.shadingModel == SHADING_MODEL_EYE)
    {
        surface.metallic = 0.0;
        surface.specular = 0.5;
    }
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
    surface.worldTangent = vec4(
        DecodeGBufferDirection(data.gbufferF.rgb),
        surface.shadingModel == SHADING_MODEL_HAIR ? data.gbufferF.a : 1.0);
    surface.anisotropy = data.gbufferF.a;
    if (surface.shadingModel == SHADING_MODEL_CLOTH)
    {
        if ((surface.selectiveOutputMask & GBUFFER_HAS_ANISOTROPY_MASK) != 0u)
        {
            // Cloth 椭圆瓣对 T/B 的符号具有 180 度对称性，因此 Deferred
            // 只需恢复主方向与轴宽度，不必把 tangent.w 误当 handedness。
            vec2 clothAnisotropy = DecodeClothAnisotropy(data.gbufferF.a);
            surface.modelInputs.cloth.anisotropy = clothAnisotropy.x;
            surface.modelInputs.cloth.anisotropyCross = clothAnisotropy.y;
            surface.anisotropy = clothAnisotropy.x;
        }
        else
        {
            // 旧 Cloth v1 没有 v2 flag；它的 GBufferF 不携带可消费的
            // anisotropy，必须回退到各向同性 closure。
            surface.modelInputs.cloth.anisotropy = 0.0;
            surface.modelInputs.cloth.anisotropyCross = 0.0;
            surface.anisotropy = 0.0;
        }
    }
    surface.emissiveColor = data.sceneColorBase.rgb;
    if (surface.shadingModel == SHADING_MODEL_SUBSURFACE)
    {
        surface.opacity = 1.0;
        surface.modelInputs.subsurface.color = surface.customData.rgb;
        surface.modelInputs.subsurface.weight = surface.customData.a;
        surface.modelInputs.subsurface.wrapWidth = data.gbufferF.x;
        surface.modelInputs.subsurface.backscatterPower = data.gbufferF.y;
        surface.modelInputs.subsurface.backscatterWeight = data.gbufferF.z;
        surface.modelInputs.subsurface.thickness = data.gbufferF.w;
        surface.modelInputs.subsurface.transmissionWeight = data.gbufferA.a;
    }
    else if (surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN)
    {
        surface.modelInputs.preintegratedSkin.skinLutId = surface.customData.x;
        surface.modelInputs.preintegratedSkin.thickness = surface.customData.y;
        surface.modelInputs.preintegratedSkin.thicknessScale = surface.customData.z;
        surface.modelInputs.preintegratedSkin.weight = surface.customData.w;
        surface.modelInputs.preintegratedSkin.curvature = data.gbufferF.x;
        surface.modelInputs.preintegratedSkin.transmissionWeight = data.gbufferF.y;
    }
    else if (surface.shadingModel == SHADING_MODEL_SUBSURFACE_PROFILE)
    {
        surface.modelInputs.subsurfaceProfile.profileId = surface.customData.x;
        surface.modelInputs.subsurfaceProfile.weight = surface.customData.y;
        surface.modelInputs.subsurfaceProfile.thickness = surface.customData.z;
        surface.modelInputs.subsurfaceProfile.transmissionWeight = surface.customData.w;
    }
    else if (surface.shadingModel == SHADING_MODEL_EYE)
    {
        uint packedProfile = DecodeEyePackedProfile(data.gbufferD.z);
        bool validEncoding = IsEyePackedProfileVersionValid(packedProfile);
        uint validIrisHit = validEncoding &&
            (packedProfile & 256u) != 0u ? 1u : 0u;
        uint profilePair = validEncoding ? packedProfile & 255u : 0u;
        float irisRadius = data.gbufferE.a;
        float pupilRatio = data.gbufferVelocity.z;
        float limbusRatio = data.gbufferVelocity.w;
        float radial = length((data.gbufferD.xy - vec2(0.5)) * 2.0);
        float pupilRatioEdge = pupilRatio + 0.04;
        float limbusStart = 1.0 - limbusRatio;
        surface.baseColor = data.gbufferA.rgb;
        surface.modelInputs.eye.corneaNormal = surface.worldNormal;
        surface.modelInputs.eye.corneaIor = data.gbufferC.r;
        surface.modelInputs.eye.irisNormal =
            DecodeGBufferDirection(data.gbufferF.rgb);
        surface.modelInputs.eye.irisPlaneNormal = surface.worldNormal;
        surface.modelInputs.eye.irisDistance = data.gbufferF.a;
        surface.modelInputs.eye.irisRadius = irisRadius;
        surface.modelInputs.eye.pupilRadius = pupilRatio * irisRadius;
        surface.modelInputs.eye.limbusWidth = limbusRatio * irisRadius;
        surface.modelInputs.eye.irisColor = data.gbufferA.rgb;
        surface.modelInputs.eye.scleraColor = data.gbufferE.rgb;
        // Unknown packing versions must not leak stale UV/profile data into the
        // evaluator; neutral values force the invalid-hit path instead.
        surface.modelInputs.eye.irisUv = validEncoding
            ? data.gbufferD.xy
            : vec2(0.5);
        surface.modelInputs.eye.irisMask = validEncoding
            ? data.gbufferD.a
            : 0.0;
        surface.modelInputs.eye.causticProfileId = float(profilePair & 15u);
        surface.modelInputs.eye.scleraProfileId = float((profilePair >> 4u) & 15u);
        surface.modelInputs.eye.causticStrength = data.gbufferC.g;
        surface.modelInputs.eye.validIrisHit = float(validIrisHit);
        surface.modelInputs.eye.irisHitDistance = data.gbufferF.a;
        surface.modelInputs.eye.pupilMask = float(validIrisHit) *
            (1.0 - smoothstep(pupilRatio, pupilRatioEdge, radial)) *
            surface.modelInputs.eye.irisMask;
        surface.modelInputs.eye.limbusMask = float(validIrisHit) *
            smoothstep(limbusStart, 1.0, radial);
        // F 的 RGB 已属于 Eye iris normal，不再伪装成普通 tangent。
        surface.worldTangent = vec4(surface.worldNormal, 1.0);
    }
    else if (surface.shadingModel == SHADING_MODEL_HAIR)
    {
        // Deferred V1 只从 GBuffer 恢复已冻结的 Hair 子集；IOR/radius 使用
        // LUT 合同的固定默认值，不能把普通通道偷偷解释成新的资产字段。
        surface.hairAbsorption = data.gbufferA.rgb;
        surface.modelInputs.hair.absorption = data.gbufferA.rgb;
        surface.modelInputs.hair.scatter = surface.customData.r;
        surface.modelInputs.hair.backlit = surface.customData.g;
        surface.modelInputs.hair.cuticleTilt = surface.customData.b;
        surface.modelInputs.hair.multipleScatteringWeight = surface.customData.a;
        surface.modelInputs.hair.ior = 1.55;
        surface.modelInputs.hair.fiberRadius = 0.00005;
        surface.modelInputs.hair.coverage = 1.0;
        // common roughness 的 mapping version=1，同时提供两个 lobe 的宽度。
        surface.modelInputs.hair.longitudinalRoughness =
            max(0.04 + surface.roughness * 0.40, 0.02);
        surface.modelInputs.hair.azimuthalRoughness =
            max(0.08 + surface.roughness * 0.40, 0.02);
    }
    else if (surface.shadingModel == SHADING_MODEL_CLOTH)
    {
        // Cloth D 保存 v1/v2 共用的 sheen 参数；方向和 anisotropy 已在上面
        // 通过版本 flag 从 GBufferF 恢复，不能把 packed w 当 roughness。
        surface.modelInputs.cloth.sheenColor = surface.customData.rgb;
        surface.modelInputs.cloth.sheenRoughness = surface.customData.a;
        surface.metallic = 0.0;
    }
    return surface;
}

#endif
