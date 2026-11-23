#pragma once

#include <cstdint>

namespace VL
{
inline constexpr uint32_t TerrainCompositedGeneratorVersion = 3;
inline constexpr const char* TerrainCompositedFilterRule =
    "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3";
inline constexpr const char* TerrainCompositedEncoding =
    "gpu-rvt-adjacency-area4-rgba8-v3";
}
