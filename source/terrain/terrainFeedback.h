#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace VL
{

// P2 feedback 格式版本 1：128×128 RGBA8，单颜色附件。
// R: bit0..4 = virtualPageX，bit5 = miss，bit6 = valid，bit7 保留
// G: bit0..4 = virtualPageY，其余保留
// B: priority（0..255，越大越优先驻留）
// A: frameTag（(submittedEpoch % 255) + 1；0 表示未写入/无效）
// 32×32 page table 只需要 5+5 bit；扩展到 64×64 时必须同时升级本格式。
constexpr uint32_t TerrainFeedbackResolution = 128;
constexpr uint32_t TerrainFeedbackTexelBytes = 4;
constexpr uint32_t TerrainFeedbackPageBitCount = 5;
constexpr uint32_t TerrainFeedbackPageMask =
    (1u << TerrainFeedbackPageBitCount) - 1u;
constexpr uint8_t TerrainFeedbackMissBit = 0x20u;
constexpr uint8_t TerrainFeedbackValidBit = 0x40u;

struct TerrainFeedbackTexel
{
    uint32_t virtualPageX = 0;
    uint32_t virtualPageY = 0;
    uint8_t priority = 0;
    uint8_t frameTag = 0;
    bool miss = false;
    bool valid = false;
};

TerrainFeedbackTexel DecodeTerrainFeedbackTexel(const uint8_t* texel);
void EncodeTerrainFeedbackTexel(
    const TerrainFeedbackTexel& texel,
    uint8_t* outTexel);

// frameTag 0 保留为“未写入”；回绕周期 255 远大于当前 2 帧 ring 延迟。
uint8_t TerrainFeedbackFrameTagForEpoch(uint64_t epoch) noexcept;

struct TerrainFeedbackPageRecord
{
    uint32_t virtualPageX = 0;
    uint32_t virtualPageY = 0;
    uint8_t priority = 0;
    bool miss = false;
    uint32_t sampleCount = 0;
};

struct TerrainFeedbackFrameSummary
{
    uint64_t copiedEpoch = 0;
    uint8_t frameTag = 0;
    uint32_t validTexelCount = 0;
    uint32_t invalidTexelCount = 0;
    uint32_t staleTexelCount = 0;
    uint32_t distinctPageCount = 0;
    uint32_t missPageCount = 0;
    uint8_t maxPriority = 0;
    // 按 priority 降序、再按页坐标排序，便于控制台稳定输出与测试断言。
    std::vector<TerrainFeedbackPageRecord> pages;
};

// byteCount 必须是 4 的倍数；函数只按 texel 顺序解码，不依赖分辨率。
// 无效 texel（valid=0，例如 clear 值）在检查 frameTag 前就被丢弃。
TerrainFeedbackFrameSummary SummarizeTerrainFeedback(
    const uint8_t* texels,
    size_t byteCount,
    uint8_t expectedFrameTag);

// 纯 CPU 的 ring 槽状态机。GPU buffer 由 renderer 持有；本类只表达
// “该槽已录制、对应 epoch 是否已完成、是否允许 CPU 解析一次”。
class TerrainFeedbackRingState
{
public:
    explicit TerrainFeedbackRingState(uint32_t slotCount = 0);

    void Reset(uint32_t slotCount);
    uint32_t SlotCount() const noexcept;

    // Record 只登记新回读请求；调用方必须保证同一 frameIndex 的 fence 已等待过，
    // 否则 GPU 可能仍在写旧槽。CPU 解析由 TryConsume 的 epoch 比较单独把关。
    void Record(uint32_t slot, uint64_t copiedEpoch, uint8_t frameTag);

    struct CompletedFrame
    {
        uint64_t copiedEpoch = 0;
        uint8_t frameTag = 0;
    };

    // copiedEpoch <= completedEpoch 才返回；成功后清除 pending，因此只消费一次。
    std::optional<CompletedFrame> TryConsume(
        uint32_t slot,
        uint64_t completedEpoch);

    bool IsPending(uint32_t slot) const noexcept;
    uint64_t CopiedEpoch(uint32_t slot) const noexcept;
    uint8_t FrameTag(uint32_t slot) const noexcept;
    // pending 槽中的原始 tag 范围；无 pending 时返回 false。
    bool PendingTagRange(uint8_t& oldest, uint8_t& newest) const noexcept;

private:
    struct Slot
    {
        bool pending = false;
        uint64_t copiedEpoch = 0;
        uint8_t frameTag = 0;
    };

    std::vector<Slot> slots;
};

// 一条待处理的 miss 请求。pendingQueue 由调用方（TerrainStreamingState）持有，
// 跨帧保留；deferredRequestCount 用于同 priority 下的轮转，避免饥饿。
struct TerrainPageRequest
{
    std::string placementId;
    uint32_t tileX = 0;
    uint32_t tileZ = 0;
    // TerrainPageManager 内的稳定页下标；pages 只追加不删除，下标在 manager
    // 生命周期内有效。pending 去重与可见性检查走本字段，避免每帧对
    // placementId 字符串做线性扫描。
    uint32_t pageIndex = 0xFFFFFFFFu;
    uint32_t priority = 0;
    uint32_t deferredRequestCount = 0;
};

struct TerrainFeedbackApplyStats
{
    uint32_t markedPages = 0;
    uint32_t missPagesSeen = 0;
    uint32_t queuedRequests = 0;
    uint32_t attemptedRequests = 0;
    uint32_t succeededRequests = 0;
    uint32_t deferredRequests = 0;
    uint32_t unregisteredPages = 0;
    uint32_t pendingRequests = 0;
};

} // namespace VL
