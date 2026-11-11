#pragma once

#include <memory>

#include <vulkan/vulkan.hpp>

#include "terrain/terrainPageManager.h"
#include "render/resource/terrain/terrainAtlasGpu.h"
#include "render/resource/terrain/terrainUploadRings.h"

namespace VL
{
class RendererBackendVulkan;
struct TerrainStreamingState;
// 一次流式上传事务：Build 成功后 Begin；SubmitFrame 成功才 Commit。
// 析构时仍未 Commit（录制/提交流程抛异常）则 Abort，保留 dirty 与 pin 语义。
class TerrainUploadTransaction
{
public:
    void Begin(
        std::shared_ptr<TerrainStreamingState> streamingState,
        TerrainUploadBatch batch);
    void Commit();
    bool Active() const noexcept { return active; }
    ~TerrainUploadTransaction();

private:
    std::shared_ptr<TerrainStreamingState> streaming;
    TerrainUploadBatch batch;
    bool active = false;
};
// 消费 page manager 的 dirty 页，用 RHI 子区域 copy 增量写入 page table/atlas。
// 当前用于加载期首次上传；内部走 BuildUploadBatch -> 单 command buffer 录制 ->
// 一次 submit + wait -> Commit，失败 Abort 保留 dirty。
// 返回本批成功提交的 atlas 页数；加载期也计入 uploadedPagesTotal。
size_t ApplyTerrainPageUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainPageManager& pageManager,
    TerrainPageGpuResources& pageResources);

// 只录制，不 submit：把本批页表/ID/权重 copy 写进调用方的主帧 command buffer。
// 调用方负责 image 当前 layout 是 ShaderReadOnly、staging 内容已填充且存活到提交后。
// 三张 image 各自做一次 ShaderReadOnly -> TransferDst -> ShaderReadOnly 往返。
void RecordTerrainPageUploads(
    vk::CommandBuffer commandBuffer,
    TerrainPageGpuResources& pageResources,
    const TerrainUploadBatch& batch,
    vk::Buffer stagingBuffer,
    const TerrainUploadStagingOffsets& stagingOffsets);

// 加载期同步上传入口：上传成功后把 streaming state 的 uploadedRevision
// 推进到本次内容版本；失败时保持 pending，便于下次整批重试。
void ApplyTerrainStreamingUploads(
    RendererBackendVulkan& rendererBackend,
    TerrainStreamingState& streamingState);

} // namespace VL