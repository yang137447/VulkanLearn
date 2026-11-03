#pragma once

#include <cstdint>
#include <string_view>

#include "particles/particleBeam.h"
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
    uint32_t meshIndexCount,
    uint32_t beamSegments = 16,
    uint32_t beamCount = 1,
    uint32_t beamBranchCount = 0)
{
    if (renderMode == "mesh" && meshUsable && meshIndexCount > 0)
    {
        return ParticleIndirectDrawShape{
            true,
            meshIndexCount,
            0,
            maxParticles};
    }

    if (renderMode == "beam")
    {
        // beam 的实际绘制由 RecordGraphics 直接发起；这里保持与其它模式
        // 相同的描述形状，避免间接参数与管线选择出现两套事实来源。
        return ParticleIndirectDrawShape{
            false,
            0,
            ParticleBeamVerticesPerParticle(beamSegments),
            // 分支与主弧共用同一条间接形状；instanceCount 必须按
            // count * (1 + branchCount) 描述，否则 dump/统计会少算分支。
            static_cast<uint32_t>(
                ParticleBeamInstanceCount(
                    beamCount,
                    beamBranchCount))};
    }

    const uint32_t verticesPerParticle = renderMode == "ribbon"
        ? ParticleRibbonVerticesPerParticle(ribbonSegments)
        : 6u;
    return ParticleIndirectDrawShape{
        false,
        0,
        // 非索引路径改成“每实例固定顶点数 + 存活实例数”。GPU finalize
        // 只改写 instanceCount，billboard/ribbon vertex shader 因此可以用
        // gl_InstanceIndex 选择紧凑存活索引，而不是重放整池顶点。
        verticesPerParticle,
        maxParticles};
}

} // namespace VL
