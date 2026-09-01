#pragma once

#include <cstdint>

namespace VL
{

inline constexpr uint32_t ClothModelVersion = 2;
inline constexpr uint32_t ClothSheenRoughnessMappingVersion = 1;
inline constexpr uint32_t ClothAnisotropyMappingVersion = 1;
inline constexpr uint32_t ClothCharlieDistributionVersion = 2;
inline constexpr uint32_t ClothVisibilityVersion = 2;
inline constexpr uint32_t ClothDirectionalAlbedoLutVersion = 1;
inline constexpr uint32_t ClothAnisotropicDirectionalAlbedoLutVersion = 1;
inline constexpr uint32_t ClothGBufferEncodingVersion = 2;
inline constexpr uint32_t ClothSheenIblVersion = 0;
inline constexpr uint32_t ClothDirectionalAlbedoLutWidth = 256;
inline constexpr uint32_t ClothDirectionalAlbedoLutHeight = 256;
inline constexpr uint32_t ClothAnisotropicDirectionalAlbedoLutWidth = 128;
inline constexpr uint32_t ClothAnisotropicDirectionalAlbedoLutHeight = 64;
inline constexpr uint32_t ClothAnisotropicDirectionalAlbedoLutLayers = 33;

} // namespace VL
