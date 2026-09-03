#ifndef VL_ENGINE_GBUFFER_CODEC_GLSL
#define VL_ENGINE_GBUFFER_CODEC_GLSL

#include "materialSurface.glsl"

struct GBufferData
{
    // A: encoded world normal + UE Legacy PerObjectGBufferData。
    // 当前 VulkanLearn 尚未接入 per-object data producer，因此 alpha 写 0。
    vec4 gbufferA;
    // B: metallic, specular, roughness + normalized packed ID/flags。
    // Alpha uses a UE-style packed integer value:
    //   low  4 bits: ShadingModelID, enough for 0..15 material shading models.
    //   high 4 bits: feature flags / SelectiveOutputMask, reserved for later phases.
    // The attachment stores this 8-bit semantic as 0..1, matching UE's UNORM target.
    vec4 gbufferB;
    // C: base color + ambient occlusion.
    vec4 gbufferC;
    // D: shading-model-specific custom data. Valid when GBUFFER_HAS_CUSTOM_DATA_MASK is set.
    // ID 6 的 D.a 保持 0，后续版本化扩展不得静默改写该保留槽。
    vec4 gbufferD;
    // E: precomputed shadow factors / model-specific visibility data.
    // Skin 像素改存角色光照倍率；Skin decode 时没有可用的预计算阴影输入，回退为 1。
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
    return float((shadingModel & 0x0fu) | (flags & 0xf0u)) / 255.0;
}

uint DecodeGBufferShadingModel(float packedValue)
{
    // The packed byte is normalized in the attachment; restore the integer before masking.
    return uint(packedValue * 255.0 + 0.5) & 0x0fu;
}

uint DecodeGBufferFlags(float packedValue)
{
    // High 4 bits are SelectiveOutputMask flags such as custom data, velocity and anisotropy.
    return uint(packedValue * 255.0 + 0.5) & 0xf0u;
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

vec3 DecodeHairAbsorptionFromBaseColor(vec3 baseColor)
{
    // Hair GBuffer C 必须保留作者 BaseColor；Deferred 使用固定参考光程重建 sigma_a，
    // 避免把米制吸收系数直接当颜色显示成白色，同时保持当前 Hair LUT 的默认合同。
    const float referencePathLength = 4.0 * 0.00005;
    vec3 referenceTransmittance = clamp(
        baseColor,
        vec3(1.0 / 255.0),
        vec3(1.0));
    return -log(referenceTransmittance) / referencePathLength;
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
    // 对齐 UE Legacy：A 只保存法线和 PerObjectGBufferData，不再承担 BaseColor/Opacity。
    data.gbufferA = vec4(
        EncodeGBufferDirection(normalize(surface.worldNormal)),
        0.0);
    // Eye metallic/specular 不参与 evaluator；沿用原有扩展合同，把 IOR 与 caustic
    // strength 放在 B.rg。Hair 的 B.r 继续保存角色环境光倍率。
    data.gbufferB = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(
            surface.modelInputs.eye.corneaIor,
            surface.modelInputs.eye.causticStrength,
            surface.roughness,
            EncodeGBufferPacked(
                surface.shadingModel,
                surface.selectiveOutputMask))
        : surface.shadingModel == SHADING_MODEL_HAIR
        ? vec4(
            surface.modelInputs.hair.characterLighting.x,
            surface.specular,
            surface.roughness,
            EncodeGBufferPacked(
                surface.shadingModel,
                surface.selectiveOutputMask))
        : surface.shadingModel == SHADING_MODEL_TWOSIDED_FOLIAGE
        ? vec4(
            surface.metallic,
            surface.specular,
            surface.roughness,
            EncodeGBufferPacked(
                surface.shadingModel,
                surface.selectiveOutputMask))
        : surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN
        ? vec4(
            surface.metallic,
            surface.specular,
            surface.roughness,
            EncodeGBufferPacked(
                surface.shadingModel,
                surface.selectiveOutputMask))
        : vec4(
            surface.metallic,
            // 其它旧模型继续保留 V1 固定 0.5 的介电高光合同。
            0.5,
            surface.roughness,
            EncodeGBufferPacked(
                surface.shadingModel,
                surface.selectiveOutputMask));
    // C 承担 UE Legacy 的 BaseColor/AO；Eye 的 irisColor 是当前 Eye BaseColor 扩展。
    data.gbufferC = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(surface.modelInputs.eye.irisColor, surface.ambientOcclusion)
        : vec4(surface.baseColor, surface.ambientOcclusion);
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
        : surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN
        ? surface.modelInputs.preintegratedSkin.characterLighting
        : surface.shadingModel == SHADING_MODEL_HAIR
        ? vec4(
            surface.precomputedShadowFactors.r,
            surface.modelInputs.hair.characterLighting.yzw)
        : surface.precomputedShadowFactors;
    // ID 6 在 Velocity.w 保存面向快照；Velocity.xy 仍保持运动矢量，z 仍由
    // Base 模板复用为 selection 标记，避免占用 GBufferD.a 保留槽。
    data.gbufferVelocity = surface.shadingModel == SHADING_MODEL_EYE
        ? vec4(
            pixelData.velocity,
            surface.modelInputs.eye.pupilRadius /
                surface.modelInputs.eye.irisRadius,
            surface.modelInputs.eye.limbusWidth /
                surface.modelInputs.eye.irisRadius)
        : surface.shadingModel == SHADING_MODEL_TWOSIDED_FOLIAGE
        ? vec4(pixelData.velocity, 0.0, surface.foliageFrontFacing)
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
        // Skin F.zw 专用保存 bottom normal 的八面体坐标；F.xy 仍保存曲率和透射权重。
        // 这两个通道不与 Clear Coat customData 或普通 tangent 语义重叠。
        vec2 encodedBottomNormal = UnitVectorToOctahedron(
            surface.preintegratedSkinBottomNormal);
        data.gbufferF = vec4(
            surface.modelInputs.preintegratedSkin.curvature,
            surface.modelInputs.preintegratedSkin.transmissionWeight,
            encodedBottomNormal);
    }
    data.sceneColorBase = vec4(surface.emissiveColor, surface.opacity);
    if (surface.shadingModel == SHADING_MODEL_SUBSURFACE)
    {
        // ID 2 当前为 opaque，复用非 GBuffer 的 sceneColorBase.a 保存透射权重；
        // 这样 A.a 可以恢复为 UE Legacy 的 PerObjectGBufferData。
        data.sceneColorBase.a =
            surface.modelInputs.subsurface.transmissionWeight;
    }
    return data;
}

MaterialSurface DecodeGBufferSurface(in GBufferData data)
{
    MaterialSurface surface = CreateDefaultMaterialSurface();
    surface.worldNormal = DecodeGBufferDirection(data.gbufferA.rgb);
    surface.shadingModel = DecodeGBufferShadingModel(data.gbufferB.a);
    surface.selectiveOutputMask = DecodeGBufferFlags(data.gbufferB.a);
    surface.metallic = data.gbufferB.r;
    surface.specular = data.gbufferB.g;
    surface.roughness = data.gbufferB.b;
    surface.baseColor = data.gbufferC.rgb;
    surface.ambientOcclusion = data.gbufferC.a;
    // Eye metallic/specular 使用模型专用 B.rg；Surface evaluator 仍按原合同使用
    // 非金属默认高光。Skin/Hair 的 specular 则分别消费 B.g。
    if (surface.shadingModel == SHADING_MODEL_EYE)
    {
        surface.metallic = 0.0;
        surface.specular = 0.5;
    }
    surface.customData = data.gbufferD;
    if (surface.shadingModel == SHADING_MODEL_TWOSIDED_FOLIAGE)
    {
        // 缺失 custom-data flag 时禁止消费附件中的 stale D，回退为无背光增量。
        bool validFoliageData =
            (surface.selectiveOutputMask & GBUFFER_HAS_CUSTOM_DATA_MASK) != 0u;
        surface.modelInputs.twoSidedFoliage.subsurfaceColor =
            validFoliageData ? data.gbufferD.rgb : vec3(0.0);
        surface.foliageFrontFacing =
            validFoliageData ? data.gbufferVelocity.w : 1.0;
    }
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
    surface.precomputedShadowFactors =
        surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN
        ? vec4(1.0)
        : data.gbufferE;
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
    surface.opacity = data.sceneColorBase.a;
    surface.opacityMask = data.sceneColorBase.a;
    if (surface.shadingModel == SHADING_MODEL_SUBSURFACE)
    {
        surface.opacity = 1.0;
        surface.modelInputs.subsurface.color = surface.customData.rgb;
        surface.modelInputs.subsurface.weight = surface.customData.a;
        surface.modelInputs.subsurface.wrapWidth = data.gbufferF.x;
        surface.modelInputs.subsurface.backscatterPower = data.gbufferF.y;
        surface.modelInputs.subsurface.backscatterWeight = data.gbufferF.z;
        surface.modelInputs.subsurface.thickness = data.gbufferF.w;
        surface.modelInputs.subsurface.transmissionWeight =
            data.sceneColorBase.a;
    }
    else if (surface.shadingModel == SHADING_MODEL_PREINTEGRATED_SKIN)
    {
        surface.modelInputs.preintegratedSkin.skinLutId = surface.customData.x;
        surface.modelInputs.preintegratedSkin.thickness = surface.customData.y;
        surface.modelInputs.preintegratedSkin.thicknessScale = surface.customData.z;
        surface.modelInputs.preintegratedSkin.weight = surface.customData.w;
        surface.modelInputs.preintegratedSkin.curvature = data.gbufferF.x;
        surface.modelInputs.preintegratedSkin.transmissionWeight = data.gbufferF.y;
        surface.preintegratedSkinBottomNormal = OctahedronToUnitVector(
            data.gbufferF.zw);
        surface.modelInputs.preintegratedSkin.bottomNormal =
            surface.preintegratedSkinBottomNormal;
        surface.modelInputs.preintegratedSkin.characterLighting =
            data.gbufferE;
        // Skin 不消费 F.rgb 作为普通 tangent，避免把曲率/八面体坐标伪装成切线。
        surface.worldTangent = vec4(surface.worldNormal, 1.0);
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
        surface.baseColor = data.gbufferC.rgb;
        surface.modelInputs.eye.corneaNormal = surface.worldNormal;
        surface.modelInputs.eye.corneaIor = data.gbufferB.r;
        surface.modelInputs.eye.irisNormal =
            DecodeGBufferDirection(data.gbufferF.rgb);
        surface.modelInputs.eye.irisPlaneNormal = surface.worldNormal;
        surface.modelInputs.eye.irisDistance = data.gbufferF.a;
        surface.modelInputs.eye.irisRadius = irisRadius;
        surface.modelInputs.eye.pupilRadius = pupilRatio * irisRadius;
        surface.modelInputs.eye.limbusWidth = limbusRatio * irisRadius;
        surface.modelInputs.eye.irisColor = data.gbufferC.rgb;
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
        surface.modelInputs.eye.causticStrength = data.gbufferB.g;
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
        surface.hairAbsorption = DecodeHairAbsorptionFromBaseColor(data.gbufferC.rgb);
        surface.modelInputs.hair.absorption = surface.hairAbsorption;
        surface.modelInputs.hair.scatter = surface.customData.r;
        surface.modelInputs.hair.backlit = surface.customData.g;
        surface.modelInputs.hair.cuticleTilt = surface.customData.b;
        surface.modelInputs.hair.multipleScatteringWeight = surface.customData.a;
        surface.modelInputs.hair.ior = 1.55;
        surface.modelInputs.hair.fiberRadius = 0.00005;
        surface.modelInputs.hair.coverage = 1.0;
        // Hair 独占 B.r 与 E.gba，冻结环境/方向/局部/虚拟光四个标量；E.r
        // 继续保存预计算阴影，Forward 与 Deferred 因而消费同一角色光照合同。
        surface.modelInputs.hair.characterLighting = vec4(
            data.gbufferB.r,
            surface.precomputedShadowFactors.gba);
        surface.metallic = 0.0;
        // NeoX pbr_hair_transparent 的 roughness 直接进入 Hair lobe；Deferred
        // 不能把源默认 0.3 再压成 0.16，否则 Core Pass 会比透明探针更干、更尖。
        // RDI.B 的 strandId 只存在 Base Pass，Deferred 这里只恢复源基线。
        surface.modelInputs.hair.longitudinalRoughness =
            max(surface.roughness, 0.02);
        surface.modelInputs.hair.azimuthalRoughness = 0.25;
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
