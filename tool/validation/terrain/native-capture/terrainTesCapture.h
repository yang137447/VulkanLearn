#pragma once

#include "terrain/terrainRvtBudget.h"
#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vulkan/vulkan.hpp>

class PipelineBase;
namespace VL
{
class RendererBackendVulkan;

struct TerrainTesDrawWitness
{
    uint64_t frame = 0;
    uint64_t submissionEpoch = 0;
    uint64_t worldGeneration = 0;
    uint64_t poseId = 0;
    uint32_t drawOrdinal = 0;
    uint32_t surfaceIndex = 0;
    uint32_t actualLod = UINT32_MAX;
    vk::PrimitiveTopology topology = vk::PrimitiveTopology::eTriangleList;
    uint32_t firstPrimitive = 0;
    uint32_t primitiveCount = 0;
    uint32_t actualIndexCount = 0;
    uint32_t actualFirstIndex = 0;
    int32_t actualVertexOffset = 0;
    uint32_t actualInstanceCount = 0;
    uint32_t actualFirstInstance = 0;
    vk::Buffer vertexBuffer;
    vk::Buffer patchIndexBuffer;
    std::array<vk::DescriptorSet, 4> actualDescriptorSets{};
    uint32_t actualDescriptorSetMask = 0;
    std::array<std::shared_ptr<const void>, 8> immutableDrawOwners{};
    std::string surfaceId;
    std::string publicationSha256;
    std::string capturedShaderSha256;
    std::string inputClosureSha256;
    std::array<float, 16> model{};
    std::array<float, 16> viewProjection{};
    float displacementScale = 0;
    float tessRadiusCm = 0;
    uint32_t baseFineResidentPages = UINT32_MAX;
};

struct TerrainTesCaptureLimits
{
    uint32_t capacity = 512;
    TerrainRvtCost explicitMetadataCost;
};

struct TerrainTesCaptureQuote
{
    uint64_t payloadBytes = 0;
    uint64_t deviceRequirementBytes = 0;
    uint64_t hostRequirementBytes = 0;
    TerrainRvtCost total;
};

struct TerrainTesCaptureReadback
{
    const void* data = nullptr;
    uint64_t bytes = 0;
    uint64_t completedEpoch = 0;
    uint32_t recordCount = 0;
    std::shared_ptr<const TerrainTesDrawWitness> witness;
    std::shared_ptr<const void> storageOwner;
    TerrainTesCaptureQuote quote;
};

class TerrainTesCapture
{
public:
    static constexpr uint32_t HeaderBytes = 64;
    static constexpr uint32_t RecordBytes = 192;
    static void ValidateActualDraw(const TerrainTesDrawWitness& actualDraw);
    static void ValidateHeader(const std::array<uint32_t, 16>& actual,
        const std::array<uint32_t, 16>& expected, uint32_t capacity);
    static TerrainTesCaptureQuote AllocationQuote(uint64_t deviceRequirementBytes,
        uint64_t hostRequirementBytes, const TerrainTesCaptureLimits& limits);
    static TerrainTesCaptureQuote QueryAllocation(RendererBackendVulkan& backend,
        const TerrainTesCaptureLimits& limits);
    TerrainTesCapture(RendererBackendVulkan& backend,
        std::shared_ptr<TerrainRvtBudgetOwner> actualWorldBudget,
        std::shared_ptr<PipelineBase> diagnosticPipeline, TerrainTesCaptureLimits limits);
    ~TerrainTesCapture();
    void BeginOutsideRenderPass(vk::CommandBuffer command,
        std::shared_ptr<const TerrainTesDrawWitness> actualDraw);
    void BindInsideActualDraw(vk::CommandBuffer command, const PipelineBase& actuallyBoundPipeline);
    void CopyOutsideRenderPass(vk::CommandBuffer command);
    std::shared_ptr<const void> Submitted(uint64_t actualSubmissionEpoch);
    void AbortUnsubmitted();
    std::shared_ptr<const TerrainTesCaptureReadback> TryConsume(uint64_t actualCompletedEpoch);
    static void WriteReadback(const TerrainTesCaptureReadback& readback,
        std::ostream& binary, std::ostream& metadata);

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
