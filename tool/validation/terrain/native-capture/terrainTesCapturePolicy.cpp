#include "terrainTesCapture.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace VL
{
namespace
{
void Require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}
uint64_t Add(uint64_t first, uint64_t second)
{
    Require(first <= UINT64_MAX-second, "TES capture allocation quote overflow");
    return first+second;
}
}

void TerrainTesCapture::ValidateActualDraw(const TerrainTesDrawWitness& witness)
{
    Require(witness.submissionEpoch && witness.worldGeneration && witness.poseId,
        "TES capture requires actual nonzero epoch/World/pose");
    Require(witness.actualLod == 0 && witness.topology == vk::PrimitiveTopology::ePatchList,
        "TES capture requires actual LOD0 PATCH_LIST");
    Require(witness.primitiveCount >= 1 && witness.primitiveCount <= 4 &&
        uint64_t(witness.firstPrimitive)+witness.primitiveCount <= witness.actualIndexCount/4,
        "TES primitive interval must select 1..4 actual patches");
    Require(witness.actualIndexCount%4 == 0 && witness.actualInstanceCount == 1 &&
        witness.vertexBuffer && witness.patchIndexBuffer, "TES actual VB/patch IB/instance witness missing");
    Require(std::all_of(witness.immutableDrawOwners.begin(), witness.immutableDrawOwners.end(),
        [](const auto& owner) { return bool(owner); }), "TES immutable draw/descriptor/outer owners missing");
    Require((witness.actualDescriptorSetMask & 7u) == 7u && witness.actualDescriptorSetMask <= 15,
        "TES actual global/material/object descriptor mask missing");
    for (uint32_t set = 0; set < 4; ++set)
        Require(bool(witness.actualDescriptorSets[set]) == bool(witness.actualDescriptorSetMask & (1u << set)),
            "TES actual descriptor-set presence differs from native mask");
    Require(!witness.surfaceId.empty() && witness.surfaceId.size() <= 256 &&
        witness.publicationSha256.size() == 64 && witness.capturedShaderSha256.size() == 64 &&
        witness.inputClosureSha256.size() == 64, "TES bounded source identity witness missing");
    Require(std::isfinite(witness.displacementScale) && witness.displacementScale != 0 &&
        std::isfinite(witness.tessRadiusCm) && witness.tessRadiusCm > 0 &&
        witness.baseFineResidentPages == 0, "TES active parameters/fine-residency witness invalid");
    Require(std::all_of(witness.model.begin(), witness.model.end(), [](float value) { return std::isfinite(value); }) &&
        std::all_of(witness.viewProjection.begin(), witness.viewProjection.end(), [](float value) { return std::isfinite(value); }),
        "TES actual model/viewProjection nonfinite");
}

void TerrainTesCapture::ValidateHeader(const std::array<uint32_t, 16>& actual,
    const std::array<uint32_t, 16>& expected, uint32_t capacity)
{
    Require(actual[0] > 0 && actual[0] <= capacity && actual[1] == 0,
        "TES readback empty/overflow; missing evidence");
    for (uint32_t word = 2; word < 16; ++word)
        Require(actual[word] == expected[word], "TES actual readback draw/epoch header mismatch");
}

TerrainTesCaptureQuote TerrainTesCapture::AllocationQuote(uint64_t deviceRequirementBytes,
    uint64_t hostRequirementBytes, const TerrainTesCaptureLimits& limits)
{
    Require(limits.capacity >= 1 && limits.capacity <= 4096, "TES capture capacity outside 1..4096");
    Require(limits.explicitMetadataCost.gpuBytes >= 4096 && limits.explicitMetadataCost.cpuBytes >= 65536 &&
        !limits.explicitMetadataCost.physicalSlots && !limits.explicitMetadataCost.productionTasks,
        "TES capture requires explicit descriptor/metadata accounting");
    TerrainTesCaptureQuote quote;
    quote.payloadBytes = HeaderBytes+uint64_t(limits.capacity)*RecordBytes;
    Require(deviceRequirementBytes >= quote.payloadBytes && hostRequirementBytes >= quote.payloadBytes,
        "TES capture must quote actual sufficient Vulkan requirements");
    quote.deviceRequirementBytes = deviceRequirementBytes;
    quote.hostRequirementBytes = hostRequirementBytes;
    quote.total = limits.explicitMetadataCost;
    quote.total.gpuBytes = Add(Add(quote.total.gpuBytes, deviceRequirementBytes), hostRequirementBytes);
    quote.total.cpuBytes = Add(quote.total.cpuBytes, hostRequirementBytes);
    return quote;
}
}
