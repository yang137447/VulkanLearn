#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>

class ComputePipeline;
class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;
class TerrainPageManager;
struct TerrainPageBakeJob;
struct TerrainPageGpuResources;

// 覆盖页 ID/权重的 GPU 烘焙器。输入（高度图/层参数/绘制）按 placement
// 常驻 device-local SSBO；每帧输出写到 frame slot 对应的 storage buffer，
// compute dispatch 后 barrier，再用 buffer→image copy 写进现有 atlas。
class TerrainPageBakeGpu
{
public:
    // 每帧输出槽数。请求预算为 8，相机预取另有少量名额；32 足够覆盖
    // 单帧全部 job，同时保持输出 buffer 约 4 MiB/帧。
    static constexpr uint32_t MaxJobsPerFrame = 32;

    TerrainPageBakeGpu(
        RendererBackendVulkan& rendererBackend,
        PipelineFactory& pipelineFactory,
        TerrainPageManager& pageManager);
    ~TerrainPageBakeGpu();

    TerrainPageBakeGpu(const TerrainPageBakeGpu&) = delete;
    TerrainPageBakeGpu& operator=(const TerrainPageBakeGpu&) = delete;

    bool Available() const noexcept { return available; }
    const std::string& UnavailableReason() const noexcept
    {
        return unavailableReason;
    }

    // 编辑器提交 heightmap/paint 后，在 render thread idle 边界重建该
    // placement 的输入 SSBO。
    void RefreshPlacement(const std::string& placementId);

    // 把本批 job 录制进主帧 command buffer：dispatch → 全局 barrier →
    // storage buffer → layerIds/layerWeights image copy。调用方保证
    // frameIndex 槽的 fence 已完成（BeginFrame 已等待）。
    void Record(
        vk::CommandBuffer commandBuffer,
        uint32_t frameIndex,
        const std::vector<TerrainPageBakeJob>& jobs,
        TerrainPageGpuResources& pageResources);

    // 调试 parity：同步烘焙同一页并回读原始 ID/权重 buffer，不做任何
    // 驻留/atlas 状态修改。GPU 不可用时返回 false 并给出原因。
    bool BakePageSync(
        const TerrainPageManager& pageManager,
        uint32_t registrationIndex,
        uint32_t tileX,
        uint32_t tileZ,
        std::vector<uint8_t>& ids,
        std::vector<uint8_t>& weights,
        std::string& error);

private:
    struct BufferResource
    {
        vk::Buffer buffer{};
        vk::DeviceMemory memory{};
    };

    struct PlacementInput
    {
        BufferResource heights;
        BufferResource layers;
        BufferResource paintWeights;
        BufferResource paintOpacityPacked;
        uint32_t heightWidth = 0;
        uint32_t heightHeight = 0;
        uint32_t layerCount = 0;
        uint32_t paintWidth = 0;
        uint32_t paintHeight = 0;
        bool hasPaint = false;
        const void* heightmapIdentity = nullptr;
        const void* paintIdentity = nullptr;
    };

    struct JobParams;

    void Initialize();
    PlacementInput CreatePlacementInput(
        const TerrainPageManager& pageManager,
        size_t registrationIndex);
    BufferResource CreateDeviceLocalBuffer(
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        const void* data,
        const std::string& debugName);
    BufferResource CreateHostBuffer(
        vk::DeviceSize size,
        vk::BufferUsageFlags usage,
        const std::string& debugName,
        void** mapped);
    void DestroyBuffer(BufferResource& resource);
    void DestroyPlacementInput(PlacementInput& input);
    void FillParams(
        JobParams& params,
        const TerrainPageBakeJob& job,
        uint32_t frameIndex) const;
    void WriteJobDescriptors(
        uint32_t frameIndex,
        uint32_t jobIndex,
        const TerrainPageBakeJob& job,
        vk::Buffer outputIds,
        vk::DeviceSize outputIdsOffset,
        vk::Buffer outputWeights,
        vk::DeviceSize outputWeightsOffset);
    void DispatchJob(
        vk::CommandBuffer commandBuffer,
        uint32_t frameIndex,
        uint32_t jobIndex,
        const TerrainPageBakeJob& job);
    void RecordAtlasCopies(
        vk::CommandBuffer commandBuffer,
        const std::vector<TerrainPageBakeJob>& jobs,
        TerrainPageGpuResources& pageResources,
        vk::DeviceSize idsSectionSize,
        uint32_t frameIndex);

    RendererBackendVulkan* rendererBackend = nullptr;
    TerrainPageManager* pageManager = nullptr;
    std::shared_ptr<ComputePipeline> pipeline;
    std::vector<PlacementInput> placements;
    BufferResource dummyBuffer;
    std::vector<BufferResource> paramsBuffers;
    std::vector<void*> paramsMapped;
    std::vector<BufferResource> outputBuffers;
    vk::DescriptorPool descriptorPool{};
    // [frameIndex][jobIndex]
    std::vector<std::vector<vk::DescriptorSet>> descriptorSets;
    uint32_t frameSlotCount = 0;
    vk::DeviceSize paramsStride = 0;
    bool available = false;
    std::string unavailableReason;
};

} // namespace VL
