#pragma once

#include "render/resource/terrainCompositedQualityData.h"
#include "terrain/terrainRvtBudget.h"

#include <memory>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

class Texture;
class PipelineFactory;
class PipelineBase;
struct PassPipelineContractKey;
struct ShaderVariantKey;
struct ShaderBinding;

namespace VL
{
class RendererBackendVulkan;

struct TerrainCompositedQualitySurface
{
    std::string surfaceId;
    TerrainQualityAbi::Surface data{};
    std::shared_ptr<Texture> pageTable;
};

struct TerrainCompositedQualityDepthBinding
{
    vk::DescriptorImageInfo descriptor;
    std::shared_ptr<const void> graphDepthOwner;
    vk::Image image;
    vk::Extent2D extent;
    vk::Format format = vk::Format::eUndefined;
    vk::ImageUsageFlags usage;
    vk::SampleCountFlagBits samples = vk::SampleCountFlagBits::e1;
};

inline bool TerrainQualityDepthReadable(const TerrainCompositedQualityDepthBinding& depth)
{
    return depth.graphDepthOwner && depth.image && depth.descriptor.imageView &&
        depth.descriptor.sampler && depth.extent.width == TerrainQualityAbi::Extent &&
        depth.extent.height == TerrainQualityAbi::Extent && depth.format == vk::Format::eD32Sfloat &&
        depth.samples == vk::SampleCountFlagBits::e1 &&
        bool(depth.usage & vk::ImageUsageFlagBits::eSampled) &&
        (depth.descriptor.imageLayout == vk::ImageLayout::eDepthStencilReadOnlyOptimal ||
         depth.descriptor.imageLayout == vk::ImageLayout::eShaderReadOnlyOptimal);
}

struct TerrainCompositedQualitySnapshot
{
    uint64_t frame = 0;
    uint64_t poseId = 0;
    uint64_t worldGeneration = 0;
    uint64_t drawSubmissionEpoch = 0;
    uint32_t renderWidth = 0;
    uint32_t renderHeight = 0;
    uint32_t uninitializedDraws = 0;
    std::array<double, 3> cameraPosition{};
    std::array<double, 3> cameraLookAt{};
    std::array<TerrainQualityAbi::Owner, TerrainQualityAbi::Slots> owners{};
    std::vector<TerrainCompositedQualitySurface> surfaces;
    TerrainCompositedQualityDepthBinding finalFeedbackDepth;
    std::shared_ptr<const void> drawPageTableOwner;
};

struct TerrainCompositedQualityLimits
{
    uint32_t maxCaptures = 256;
    uint64_t maxReadbackBytes = uint64_t(TerrainQualityAbi::Bytes) * 256;
    TerrainRvtCost metadataOverhead;
    uint64_t importedWinnerGpuBytes = 0;
    std::vector<std::shared_ptr<const void>> importedWinnerOwners;
};

struct TerrainCompositedQualityAllocationQuote
{
    uint64_t outputBytes = 0;
    uint64_t hostBytes = 0;
    uint64_t selectorBytes = 0;
    TerrainRvtCost total;
};

struct TerrainCompositedQualityReadback
{
    const uint32_t* words = nullptr;
    size_t bytes = 0;
    uint64_t submissionEpoch = 0;
    uint64_t completedEpoch = 0;
    double capturedMonotonicMs = 0;
    double completedMonotonicMs = 0;
    double gpuMilliseconds = 0;
    uint64_t gpuAllocationBytes = 0;
    uint64_t hostAllocationBytes = 0;
    uint64_t workspaceObservedPeakBytes = 0;
    TerrainRvtBudgetStats budget;
    TerrainQualityAbi::Counts counts;
    std::shared_ptr<const TerrainCompositedQualitySnapshot> snapshot;
    std::shared_ptr<const void> storage;
    std::shared_ptr<const void> captureGate;
};

class TerrainCompositedQualityGpu
{
public:
    using Handle = uint64_t;
    static TerrainCompositedQualityAllocationQuote QueryAllocation(
        RendererBackendVulkan& backend, const TerrainCompositedQualityLimits& limits);
    TerrainCompositedQualityGpu(RendererBackendVulkan& backend, PipelineFactory& factory,
        std::shared_ptr<TerrainRvtBudgetOwner> budget, TerrainCompositedQualityLimits limits);
    ~TerrainCompositedQualityGpu();
    TerrainCompositedQualityGpu(const TerrainCompositedQualityGpu&) = delete;
    TerrainCompositedQualityGpu& operator=(const TerrainCompositedQualityGpu&) = delete;

    std::optional<Handle> Record(vk::CommandBuffer command,
        std::shared_ptr<const TerrainCompositedQualitySnapshot> snapshot,
        double monotonicMs,
        const std::array<TerrainCompositedQualityDepthBinding, 2>& winnerAttachments);
    void RecordResolveAndCopy(vk::CommandBuffer command, Handle handle);
    [[nodiscard]] std::shared_ptr<const void> Submitted(Handle handle, uint64_t epoch);
    void Abort(Handle handle);
    std::shared_ptr<const TerrainCompositedQualityReadback> TryConsume(
        uint64_t completedEpoch, double completedMonotonicMs);
    void WriteReadback(const TerrainCompositedQualityReadback& readback,
        std::ostream& binary, std::ostream& metadata);
    bool Pending() const;
    uint64_t Requested() const;
    uint64_t Deferred() const;
    uint64_t Completed() const;
    bool ExportAvailable() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};
}
