#pragma once

#include <cstdint>
#include <memory>

#include <vulkan/vulkan.hpp>

#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainLayerSources.h"
#include "render/resource/terrain/terrainAtlasGpu.h"

namespace VL
{
class RendererBackendVulkan;
struct TerrainStreamingState;
// L4a：层页上传事务，语义与 TerrainUploadTransaction 相同，但提交到
// TerrainLayerPageManager，并累计层页上传统计。
class TerrainLayerUploadTransaction
{
public:
    void Begin(
        std::shared_ptr<TerrainStreamingState> streamingState,
        TerrainLayerUploadBatch batch);
    void Commit();
    bool Active() const noexcept { return active; }
    ~TerrainLayerUploadTransaction();

private:
    std::shared_ptr<TerrainStreamingState> streaming;
    TerrainLayerUploadBatch batch;
    uint32_t uploadedPageCount = 0;
    bool active = false;
};
// 加载期初始测试集合：每层取源 mip 链的尾页（page 0,0），受物理页预算约束。
// 这是 L2 的映射验证集合，不是 L4 的 feedback 驻留策略。
void RequestInitialTerrainLayerPages(
    TerrainStreamingState& streamingState,
    uint64_t completedEpoch,
    uint32_t framesInFlight);

// L3 的 VT 验证路径：每层每个 mip 的全部页一次性请求。没有 feedback 驱动，
// 因此加载期必须建立完整映射。L4a 起只保留 debug console 的对照入口。
void RequestAllTerrainLayerPages(
    TerrainStreamingState& streamingState,
    uint64_t completedEpoch,
    uint32_t framesInFlight);

// L4a：debug console `terrainlayer preload` 的对照入口。请求不受每帧预算
// 限制；像素仍由逐帧上传 ring 分批写进 atlas，不回到加载期一次 submit。
void RequestTerrainLayerPreload(TerrainStreamingState& streamingState);

// 同步版：BuildUploadBatch -> 页表 staging -> 页表 copy + 源->atlas copy
// 全部录制进同一个 command buffer，一次 submit + wait 后 Commit。
// 返回本批成功上传的 atlas 页数；失败 Abort 保留 dirty。
size_t ApplyTerrainLayerPageUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceSets& sources);

// 加载期入口：成功后推进 streaming state 的 uploadedRevision。
void ApplyTerrainLayerStreamingUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState);

// L4a：只录制层页上传（页表 + 三张 atlas 的 GPU→GPU copy），不 submit。
// 调用方负责 staging 内容已填充并存活到提交后；本函数会在页表写入前后
// 各做一次 layout 往返（ShaderReadOnly → TransferDst → ShaderReadOnly）。
void RecordTerrainLayerPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainLayerPageManager& layerPageManager,
    TerrainLayerGpuResources& layerResources,
    const TerrainLayerSourceSets& sources,
    const TerrainLayerUploadBatch& batch,
    vk::Buffer stagingBuffer,
    vk::DeviceSize pageTableStagingOffset);

} // namespace VL