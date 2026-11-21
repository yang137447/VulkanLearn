#pragma once

#include "terrain/terrainRvtBudget.h"
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

class PipelineBase;
struct ShaderBinding;
namespace VL
{
class RendererBackendVulkan;
class RendererObjectResourceEntry;
struct TerrainCaptureDrawInputs;

struct TerrainReferencePage
{
    uint32_t actualPhysicalSlot = 0;
    std::filesystem::path originalGpuOutput;
    std::string originalOutputBlake3;
    std::string surfaceId;
    uint32_t mip = 0, x = 0, y = 0;
};

class TerrainReferenceAtlas
{
public:
    TerrainReferenceAtlas(RendererBackendVulkan& backend,
        std::shared_ptr<TerrainRvtBudgetOwner> admittedReferencePoolOwner,
        std::shared_ptr<TerrainRvtBudgetOwner> actualWorkspaceOwner,
        const std::array<vk::Format, 3>& actualFormats, uint32_t actualSlotCapacity,
        const std::vector<TerrainReferencePage>& exactReferencedPages);
    ~TerrainReferenceAtlas();
    TerrainReferenceAtlas(const TerrainReferenceAtlas&) = delete;
    TerrainReferenceAtlas& operator=(const TerrainReferenceAtlas&) = delete;
    std::array<vk::DescriptorImageInfo,3> DescriptorsWithActualSamplers(
        const std::array<vk::Sampler,3>& actualSamplers) const;
    bool ContainsActualSlot(uint32_t slot) const;
    uint64_t ActualImageAllocationBytes() const;
    std::shared_ptr<const void> LifetimeOwner() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

class TerrainReferenceDescriptorOverride
{
public:
    TerrainReferenceDescriptorOverride(RendererBackendVulkan& backend,
        std::shared_ptr<TerrainRvtBudgetOwner> actualWorkspaceOwner,
        std::shared_ptr<const PipelineBase> actualPipeline,
        std::shared_ptr<RendererObjectResourceEntry> actualObjectOwner,
        std::shared_ptr<TerrainReferenceAtlas> admittedAtlas, uint32_t swapchainIndex);
    ~TerrainReferenceDescriptorOverride();
    void BindOnlySet1(vk::CommandBuffer command) const;
    std::shared_ptr<const void> LifetimeOwner() const;
private:
    struct Impl;
    std::shared_ptr<Impl> impl;
};

struct TerrainReferenceReplayStamp
{
    uint64_t frame = 0, submissionEpoch = 0, worldGeneration = 0, poseId = 0;
    std::string inputClosureSha256;
    std::string originalEvaluatorSha256;
    std::string referencedPageSetSha256;
    std::vector<uint32_t> allRoiSampledPhysicalSlots;
    std::shared_ptr<const void> immutableFrameOwner;
    std::shared_ptr<const void> referenceTargetOwner;
};

void RecordFrozenTerrainReferenceDraw(vk::CommandBuffer command,
    const TerrainCaptureDrawInputs& actualFrozenDraw,
    const TerrainReferenceReplayStamp& candidateStamp, const TerrainReferenceReplayStamp& referenceStamp,
    const TerrainReferenceAtlas& admittedReferenceAtlas,
    const TerrainReferenceDescriptorOverride& referenceMaterial);
}
