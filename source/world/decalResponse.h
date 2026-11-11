#pragma once

#include <cstdint>
#include <string>

namespace VL
{
// UE Deferred Decal 的 DBuffer 响应位。每一位对应一个可被 decal 覆盖的
// GBuffer 通道；decal pass 把它写入 DBufferMask，deferredLighting 在合成
// 时按位决定 color/normal/roughness/metallic 是否允许覆盖。
inline constexpr uint32_t DecalResponseColor = 1u << 0;
inline constexpr uint32_t DecalResponseNormal = 1u << 1;
inline constexpr uint32_t DecalResponseRoughness = 1u << 2;
inline constexpr uint32_t DecalResponseMetallic = 1u << 3;

// 与 UE 默认 ColorNormalRoughness 一致：写颜色、法线和粗糙度，不改金属度。
constexpr uint32_t DefaultDecalResponse()
{
    return DecalResponseColor | DecalResponseNormal | DecalResponseRoughness;
}

// 只接受 UE DecalResponse 的枚举名（大小写敏感）；未知值不修改 mask，
// 由调用方决定是抛错还是回退默认值。
inline bool TryParseDecalResponse(const std::string& value, uint32_t& mask)
{
    if (value == "colorNormalRoughness")
    {
        mask = DecalResponseColor | DecalResponseNormal |
            DecalResponseRoughness;
    }
    else if (value == "colorNormal")
    {
        mask = DecalResponseColor | DecalResponseNormal;
    }
    else if (value == "color")
    {
        mask = DecalResponseColor;
    }
    else if (value == "normal")
    {
        mask = DecalResponseNormal;
    }
    else if (value == "roughness")
    {
        mask = DecalResponseRoughness;
    }
    else if (value == "roughnessNormal")
    {
        mask = DecalResponseRoughness | DecalResponseNormal;
    }
    else if (value == "metallic")
    {
        mask = DecalResponseMetallic;
    }
    else if (value == "metallicNormal")
    {
        mask = DecalResponseMetallic | DecalResponseNormal;
    }
    else if (value == "metallicRoughness")
    {
        mask = DecalResponseMetallic | DecalResponseRoughness;
    }
    else if (value == "metallicRoughnessNormal")
    {
        mask = DecalResponseMetallic | DecalResponseRoughness |
            DecalResponseNormal;
    }
    else
    {
        return false;
    }
    return true;
}
} // namespace VL
