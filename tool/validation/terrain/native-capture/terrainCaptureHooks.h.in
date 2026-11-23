#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vulkan.hpp>

class Material;
class MaterialInstance;
class PipelineBase;
class RenderableObject;
struct Renderpass;

namespace VL
{
class RendererBackendVulkan;
class RendererObjectResourceEntry;
class TerrainRvtBudgetOwner;
struct RenderDrawPacket;
struct PassRuntimeContext;

struct TerrainMeshDrawBindings
{
    std::array<vk::Buffer, 2> vertexBuffers{};
    std::array<vk::DeviceSize, 2> vertexOffsets{};
    vk::Buffer indexBuffer;
    vk::DeviceSize indexOffset = 0;
    vk::IndexType indexType = vk::IndexType::eUint32;
    uint32_t indexCount = 0;
    uint32_t instanceCount = 0;
    uint32_t firstIndex = 0;
    int32_t vertexOffset = 0;
    uint32_t firstInstance = 0;
    bool patches = false;
};

struct TerrainCaptureDrawInputs
{
    const RenderDrawPacket* packet = nullptr;
    const PipelineBase* regularPipeline = nullptr;
    uint32_t drawPacketIndex = 0;
    uint32_t frameSlot = 0;
    TerrainMeshDrawBindings mesh;
    std::array<vk::DescriptorSet, 4> descriptorSets{};
    uint32_t descriptorSetMask = 0;
    std::shared_ptr<RenderableObject> renderableOwner;
    std::shared_ptr<RendererObjectResourceEntry> objectResourceOwner;
    std::shared_ptr<Material> materialOwner;
    std::shared_ptr<MaterialInstance> materialInstanceOwner;
};

struct TerrainCaptureFrameInfo
{
    RendererBackendVulkan* backend = nullptr;
    std::shared_ptr<TerrainRvtBudgetOwner> actualWorldBudget;
    std::shared_ptr<const void> immutableFrameOwner;
    std::shared_ptr<const void> immutableGraphOwner;
    uint64_t frame = 0;
    uint64_t submissionEpoch = 0;
    uint64_t worldGeneration = 0;
    uint64_t poseId = 0;
    uint32_t actualBaseFineResidentPages = UINT32_MAX;
};

class ITerrainCaptureHooks
{
public:
    virtual ~ITerrainCaptureHooks() = default;
    virtual bool RequiresTesVertexStores() const noexcept = 0;
    virtual void BeforeGeometry(PassRuntimeContext& context, const TerrainCaptureFrameInfo& frame) = 0;
    virtual void AfterGeometry(vk::CommandBuffer command) = 0;
    virtual std::shared_ptr<PipelineBase> SelectActualDraw(const TerrainCaptureDrawInputs& inputs) = 0;
    virtual void BindActualDraw(vk::CommandBuffer command, const TerrainCaptureDrawInputs& inputs,
        const PipelineBase& actuallyBoundPipeline) = 0;
    virtual void ActualDrawRecorded(const TerrainCaptureDrawInputs& inputs) = 0;
    virtual std::shared_ptr<const void> Submitted(uint64_t actualEpoch) = 0;
    virtual void AbortUnsubmitted() = 0;
    virtual void ConsumeCompleted(uint64_t actualCompletedEpoch) = 0;
    virtual void CloseAfterDeviceIdle() = 0;
};

std::shared_ptr<ITerrainCaptureHooks> AcquireTerrainCaptureHooks();
void RegisterTerrainCaptureHooks(std::shared_ptr<ITerrainCaptureHooks> hooks);
void PublishTerrainCaptureDeviceFeature(bool enabled);
bool TerrainCaptureDeviceFeatureEnabled();
TerrainCaptureDrawInputs BuildTerrainCaptureDrawInputs(const RenderDrawPacket& packet,
    uint32_t drawPacketIndex, uint32_t frameSlot, uint32_t swapchainIndex,
    const PipelineBase& regularPipeline, const Renderpass& pass,
    std::shared_ptr<RenderableObject> renderable,
    std::shared_ptr<RendererObjectResourceEntry> objectResource,
    std::shared_ptr<Material> material, std::shared_ptr<MaterialInstance> instance);
}
