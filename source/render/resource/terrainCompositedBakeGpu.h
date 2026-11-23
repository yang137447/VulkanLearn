#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.hpp>
#include "terrain/terrainCompositedAdjacency.h"

class ComputePipeline;
class PipelineFactory;

namespace VL
{

class RendererBackendVulkan;
class TerrainRvtBudgetLease;
class TerrainCompositedProducerSnapshot;
class TerrainCompositedProducerReloadState;
class TerrainCompositedComputeReloadParticipants;
class ShaderReloadCoordinator;

// 消费 Cook/Editor 冻结的 GPU 作者快照，生产合成材质页；不管理驻留、页表或发布。
// 外部必须保活所有输入纹理与输出 buffer 至提交 fence 完成；同一实例串行调用，
// Record 的 slot 只在上次提交完成后复用，析构前等待全部使用该实例的提交。
class TerrainCompositedBakeGpu
{
public:
    static constexpr uint32_t InteriorExtent = 128;
    static constexpr uint32_t Border = 4;
    static constexpr uint32_t StoredExtent = InteriorExtent + Border * 2;
    static constexpr uint32_t ChannelBytes = StoredExtent * StoredExtent * 4;
    static constexpr uint32_t PackedBytes = ChannelBytes * 3;
    static constexpr uint32_t StatusOffset = PackedBytes;
    static constexpr uint32_t OutputBytes = PackedBytes + 4;
    static constexpr uint32_t InvalidSlice = 0xffffffffu;
    static constexpr uint32_t MaxSurfaces = 16;
    static constexpr uint32_t WorldParameterBytes = 32 + MaxSurfaces * 2240;
    static constexpr uint32_t ScratchBytes = 0;

    enum class Encoding
    {
        Linear,
        SrgbView
    };

    enum class WeightSource
    {
        CoverageAtlas,
        LocalLayerArray
    };

    struct SampledTexture
    {
        // 元数据描述实际 image view。层源为 repeat、linear mipmap sampler；
        // coverage IDs 用 texelFetch，权重为 linear，catalog 为无 mip LUT。
        vk::DescriptorImageInfo descriptor{};
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t layers = 1;
        uint32_t mipLevels = 1;
        Encoding encoding = Encoding::Linear;
    };

    struct RetainedFragmentParameters
    {
        float heightBlendStrength = 0.0f;
        float detailUvScale = 1.0f;
        float detailStrength = 0.0f;
        float detailFadeStartM = 0.0f;
        float detailFadeEndM = 1.0f;
        float macroUvScale = 1.0f;
        float macroStrength = 0.0f;
        float globalColourWeightNear = 0.0f;
        float globalColourWeight = 0.0f;
        float globalFadeStartM = 0.0f;
        float globalFadeEndM = 1.0f;
        float globalAoStrength = 0.0f;
        float globalWetnessStrength = 0.0f;
        std::array<float, 2> globalOriginCm{};
        float globalSizeCm = 1.0f;
    };

    struct AdjacencySnapshot;

    struct InputSnapshot
    {
        // page table 为 RGBA8 UNORM，XY 为物理页坐标/255、A 为有效位。
        // IDs 为 RGBA8 UNORM global ID；weight array slice 必须经 localToGlobal。
        SampledTexture coveragePageTable;
        SampledTexture coverageIds;
        SampledTexture coverageWeights;
        SampledTexture localLayerWeights;
        SampledTexture layerCatalog;
        SampledTexture layerAlbedo;
        SampledTexture layerNormal;
        SampledTexture layerOrm;
        // RGBA32F 原始有符号局部 XYZ 与 tangent.xyzw（w 为 handedness）。
        // 保留 mesh 的未正交化插值基；这里不是 [0,1] 编码的 normal map。
        SampledTexture surfaceLocalNormal;
        SampledTexture surfaceLocalTangent;
        SampledTexture globalColour;
        SampledTexture globalMasks;
        SampledTexture layerHeight;
        WeightSource weightSource = WeightSource::CoverageAtlas;
        uint32_t coveragePageResolution = 128;
        uint32_t localLayerCount = 0;
        uint32_t sourceSetCount = 1;
        std::array<uint32_t, 256> globalToSourceSlice = [] {
            std::array<uint32_t, 256> mapping{};
            mapping.fill(InvalidSlice);
            return mapping;
        }();
        std::array<uint32_t, 256> localToGlobalLayer = [] {
            std::array<uint32_t, 256> mapping{};
            mapping.fill(InvalidSlice);
            return mapping;
        }();
        std::array<float, 2> surfaceUvOffset{};
        std::array<float, 2> surfaceUvScale{1.0f, 1.0f};
        std::array<float, 2> coverageUvOffset{};
        std::array<float, 2> coverageUvScale{1.0f, 1.0f};
        std::array<float, 2> frameUvOffset{};
        std::array<float, 2> frameUvScale{1.0f, 1.0f};
        // column-major；S1 只接受正 determinant 的均匀缩放 affine placement。
        std::array<float, 16> localToWorld{
            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        float uvScale = 1.0f;
        // 非零 heightBlendStrength 必须显式指定 1 并纳入内容 hash；
        float frozenHeightBlendFade = -1.0f;
        // detail/macro/global/位移均不写入三通道页，消费端继续绑定原始源数组、
        // global maps、height atlas/source 及这些参数。localToWorld/global maps
        // 在基础层评价中不读取，但属于作者快照/依赖身份，不能从 hash 排除。
        RetainedFragmentParameters retainedFragment;
        bool retainFragmentPostprocess = false;
        bool hasUnsupportedViewTimeOrTransparentTerms = false;
        std::string contentIdentity;
        std::string surfaceId;
        std::shared_ptr<const AdjacencySnapshot> adjacency;
        vk::DescriptorBufferInfo adjacencyScratch{};
        std::shared_ptr<const TerrainRvtBudgetLease> cookWorkspaceLease;
        vk::QueryPool diagnosticQueries{};
        uint32_t diagnosticFirstQuery = 0;
    };

    struct PageRequest
    {
        uint32_t virtualWidth = 0;
        uint32_t virtualHeight = 0;
        uint32_t virtualMip = 0;
        uint32_t pageX = 0;
        uint32_t pageY = 0;
        bool repeatSurface = false;
        // 每输出 texel 分层 box 积分，层源按子样本 footprint 显式选 mip。
        // normal 归一化均值、roughness RMS，其余通道算术平均；这是有限采样近似。
        uint32_t areaSamplesPerAxis = 4;
    };

    struct PageBytes
    {
        std::vector<uint8_t> albedoSrgb;
        std::vector<uint8_t> normalLocalUnorm;
        std::vector<uint8_t> ormLinear;
        std::string contentIdentity;
        PageRequest page;
    };

    struct SurfaceInput
    {
        std::string surfaceId;
        InputSnapshot input;
    };

    struct AdjacencySnapshot
    {
        std::string sourceSurfaceId;
        TerrainCompositedAdjacencyGraph graph;
        std::vector<SurfaceInput> surfaces;
        std::shared_ptr<const TerrainRvtBudgetLease> cookWorkspaceLease;
    };

    TerrainCompositedBakeGpu(
        RendererBackendVulkan& backend,
        PipelineFactory& pipelineFactory,
        uint32_t slotCount = 1,
        uint32_t adjacencySurfaceCapacity = 16);
    TerrainCompositedBakeGpu(
        RendererBackendVulkan& backend,
        PipelineFactory& pipelineFactory,
        uint32_t slotCount,
        uint32_t adjacencySurfaceCapacity,
        const vk::PhysicalDeviceLimits* selectedDeviceLimits);
    ~TerrainCompositedBakeGpu();
    TerrainCompositedBakeGpu(const TerrainCompositedBakeGpu&) = delete;
    TerrainCompositedBakeGpu& operator=(const TerrainCompositedBakeGpu&) = delete;

    bool Available() const noexcept { return available; }
    const std::string& UnavailableReason() const noexcept { return unavailableReason; }
    using ProducerSnapshot = std::shared_ptr<const TerrainCompositedProducerSnapshot>;
    ProducerSnapshot CaptureProducer() const noexcept;
    ProducerSnapshot PendingProducer() const noexcept;
    bool PublishProducer(const ProducerSnapshot& selected) noexcept;
    void DiscardPendingProducer(const ProducerSnapshot& selected) noexcept;
    void SetShaderReloadCoordinator(ShaderReloadCoordinator* coordinator);
    bool Record(
        vk::CommandBuffer commandBuffer, uint32_t slot,
        const InputSnapshot& input, const PageRequest& page,
        const vk::DescriptorBufferInfo& output,
        const ProducerSnapshot& selectedProducer, std::string& error);
    bool Record(
        vk::CommandBuffer commandBuffer, uint32_t slot,
        const AdjacencySnapshot& input, const PageRequest& page,
        const vk::DescriptorBufferInfo& output,
        const vk::DescriptorBufferInfo& scratch,
        const ProducerSnapshot& selectedProducer, std::string& error);

    // output 需要 StorageBuffer|TransferDst|TransferSrc、offset=0、至少 OutputBytes。
    // 三段 ChannelBytes 后为 uint status；fence 后读取并要求其为 0 才能发布。
    // 输入 image 必须已转 readable layout 并完成写→compute read 屏障；
    // 返回时只保证 compute write→transfer read，不替消费者录制 atlas 上传。
    bool Record(
        vk::CommandBuffer commandBuffer,
        uint32_t slot,
        const InputSnapshot& input,
        const PageRequest& page,
        const vk::DescriptorBufferInfo& output,
        std::string& error);
    bool Record(
        vk::CommandBuffer commandBuffer,
        uint32_t slot,
        const AdjacencySnapshot& input,
        const PageRequest& page,
        const vk::DescriptorBufferInfo& output,
        const vk::DescriptorBufferInfo& scratch,
        std::string& error);
    // 使用专用内部 slot；不覆盖外部 Record 槽。成功后才替换 result，失败保留旧值。
    bool BakePageSync(
        const InputSnapshot& input,
        const PageRequest& page,
        PageBytes& result,
        std::string& error);
    bool BakePageSync(
        const AdjacencySnapshot& input,
        const PageRequest& page,
        PageBytes& result,
        std::string& error);
    static bool Validate(
        const InputSnapshot& input,
        const PageRequest& page,
        std::string& error);
    static std::string DescribeStatus(uint32_t status);
    static uint64_t ParameterBytes(uint32_t slotCount, uint32_t surfaceCapacity);
    static uint32_t RequiredScratchBytes(uint32_t areaSamplesPerAxis);
    static uint32_t DiagnosticQueryCount(uint32_t) { return 3; }
    static bool ValidateDeviceLimits(const vk::PhysicalDeviceLimits& limits, std::string& error);

private:
    struct Slot
    {
        vk::Buffer params{};
        vk::DeviceMemory memory{};
        void* mapped = nullptr;
        vk::DescriptorSet descriptors{};
        vk::Buffer charts{};
        vk::DeviceMemory chartsMemory{};
        void* chartsMapped = nullptr;
        vk::DescriptorSet paddingDescriptors{};
        ProducerSnapshot producer;
    };
    struct GpuParams;
    struct GpuWorldParams;
    static GpuParams MakeParams(const InputSnapshot& input, const PageRequest& page);
    bool RecordAdjacency(
        vk::CommandBuffer commandBuffer, uint32_t slot,
        const AdjacencySnapshot& input, const std::string& sourceSurfaceId,
        const PageRequest& page, const vk::DescriptorBufferInfo& output,
        const vk::DescriptorBufferInfo& scratch, const ProducerSnapshot& selectedProducer,
        std::string& error);
    void WriteDescriptors(
        vk::DescriptorSet descriptors, const std::vector<const InputSnapshot*>& inputs,
        const vk::DescriptorBufferInfo& params, const vk::DescriptorBufferInfo& output,
        const vk::DescriptorBufferInfo& charts);
    bool RecordPage(
        vk::CommandBuffer commandBuffer,
        uint32_t slot,
        const InputSnapshot& input,
        const PageRequest& page,
        const vk::DescriptorBufferInfo& output,
        const ProducerSnapshot& selectedProducer, std::string& error);
    void Release();

    RendererBackendVulkan* backend = nullptr;
    std::shared_ptr<ComputePipeline> pipeline;
    std::shared_ptr<ComputePipeline> paddingPipeline;
    std::unique_ptr<TerrainCompositedProducerReloadState> producerState;
    std::unique_ptr<TerrainCompositedComputeReloadParticipants> reloadParticipants;
    vk::DescriptorPool descriptorPool{};
    std::vector<Slot> slots;
    uint32_t surfaceCapacity = 1;
    bool available = false;
    std::string unavailableReason;
};

}
