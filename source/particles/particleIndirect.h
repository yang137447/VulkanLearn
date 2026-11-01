#pragma once

#include <cstdint>
#include <string_view>

#include "particles/particleRibbon.h"

namespace VL
{

// Indirect commands are authored from the same render-mode decision as the
// graphics recording path. Keeping this calculation pure makes a mode switch
// visible to Synchronize instead of leaving stale fixed counts in the buffer.
struct ParticleIndirectDrawShape
{
    bool indexed = false;
    uint32_t indexCount = 0;
    uint32_t vertexCount = 0;
    uint32_t instanceCount = 0;

    constexpr bool operator==(const ParticleIndirectDrawShape& other) const noexcept
    {
        return indexed == other.indexed &&
            indexCount == other.indexCount &&
            vertexCount == other.vertexCount &&
            instanceCount == other.instanceCount;
    }

    constexpr bool operator!=(const ParticleIndirectDrawShape& other) const noexcept
    {
        return !(*this == other);
    }
};

inline ParticleIndirectDrawShape BuildParticleIndirectDrawShape(
    std::string_view renderMode,
    uint32_t maxParticles,
    uint32_t ribbonSegments,
    bool meshUsable,
    uint32_t meshIndexCount)
{
    if (renderMode == "mesh" && meshUsable && meshIndexCount > 0)
    {
        return ParticleIndirectDrawShape{
            true,
            meshIndexCount,
            0,
            maxParticles};
    }

    const uint32_t verticesPerParticle = renderMode == "ribbon"
        ? ParticleRibbonVerticesPerParticle(ribbonSegments)
        : 6u;
    return ParticleIndirectDrawShape{
        false,
        0,
        maxParticles * verticesPerParticle,
        1};
}

} // namespace VL
