#ifndef VL_COMMON_SHADING_MODEL_GLSL
#define VL_COMMON_SHADING_MODEL_GLSL

// UE 5.8 Legacy Material Shading Model ID；未实现模型仍保留原始槽位。
const uint SHADING_MODEL_UNLIT = 0u;
const uint SHADING_MODEL_DEFAULT_LIT = 1u;
const uint SHADING_MODEL_SUBSURFACE = 2u;
const uint SHADING_MODEL_PREINTEGRATED_SKIN = 3u;
const uint SHADING_MODEL_CLEAR_COAT = 4u;
const uint SHADING_MODEL_SUBSURFACE_PROFILE = 5u;
const uint SHADING_MODEL_TWOSIDED_FOLIAGE = 6u;
const uint SHADING_MODEL_HAIR = 7u;
const uint SHADING_MODEL_CLOTH = 8u;
const uint SHADING_MODEL_EYE = 9u;
const uint SHADING_MODEL_SINGLE_LAYER_WATER = 10u;
const uint SHADING_MODEL_THIN_TRANSLUCENT = 11u;
const uint SHADING_MODEL_STRATA = 12u;

// SHADING_MODEL_CLEAR_COAT 使用 GBufferD/customData 传递清漆层参数：
//   x = 清漆层权重
//   y = 清漆层粗糙度
//   w,z = 底层法线相对顶层法线的 UE 八面体偏移编码
// 车漆材质把底漆保留为 MaterialSurface 的 baseColor/roughness/metallic，
// 顶层使用固定 IOR 1.5 / F0 0.04；普通 worldNormal 表示顶层法线。

// GBufferB.a 使用 UE-style packed byte 语义：
//   low  4 bits = ShadingModelID
//   high 4 bits = SelectiveOutputMask / optional data flags
// 这些 mask 只声明“该像素写入的数据是否有效”，不改变 attachment 的存在与否。
const uint GBUFFER_HAS_CUSTOM_DATA_MASK = 0x10u;
const uint GBUFFER_HAS_PRECOMPUTED_SHADOW_MASK = 0x20u;
const uint GBUFFER_HAS_VELOCITY_MASK = 0x40u;
const uint GBUFFER_HAS_ANISOTROPY_MASK = 0x80u;

// GBufferF.a 按 ShadingModel 解释：普通 Lit 模型保存 anisotropy，Hair 保存
// tangent.w 的 bitangent handedness。两者不能在 Deferred decode 中互换消费。

float ShadingModelMask(uint actualShadingModel, uint expectedShadingModel)
{
    // 返回 1 表示匹配，0 表示不匹配；给 lighting dispatch / debug 这类选择逻辑使用。
    // 先用 mask + mix 保持 shader 结构线性，后续 shading model 多了再升级成更完整的分发表。
    return 1.0 - min(abs(float(actualShadingModel) - float(expectedShadingModel)), 1.0);
}

#endif
