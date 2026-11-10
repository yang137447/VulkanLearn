#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VL
{

// L4b 层页 feedback：每帧 4 张 128×128 RGBA8 颜色附件，一个 top-4 slot 一张，
// 与覆盖页 feedback（terrainFeedback）并排存在。编码：
//   R = layerId（0..255）
//   G = pageIndex（pageY * side + pageX，0..255；2K mip0 正好 256 页）
//   B = bit0..2 mip（0..4），bit7 = valid（1 表示本帧写入）
//   A = frameTag（(submittedEpoch % 255) + 1；0 表示未写入/无效）
//
// 与覆盖页 feedback 共用 frameTag/ring/epoch 规则，但语义不同：
// 覆盖页编码“虚拟覆盖页坐标”，这里编码“层页身份”。mip 报告的是 shader
// 按导数选出的目标 mip，而不是缺页回退后的粗 mip——否则更细的 mip 永远
// 不会被请求。
constexpr uint32_t TerrainLayerFeedbackResolution = 128;
constexpr uint32_t TerrainLayerFeedbackTexelBytes = 4;
constexpr uint32_t TerrainLayerFeedbackSlotCount = 4;
constexpr uint32_t TerrainLayerFeedbackTexelsPerImage =
    TerrainLayerFeedbackResolution * TerrainLayerFeedbackResolution;
constexpr uint32_t TerrainLayerFeedbackImageBytes =
    TerrainLayerFeedbackTexelsPerImage * TerrainLayerFeedbackTexelBytes;
constexpr uint32_t TerrainLayerFeedbackBufferBytes =
    TerrainLayerFeedbackImageBytes * TerrainLayerFeedbackSlotCount;
constexpr uint32_t TerrainLayerFeedbackMipMask = 0x07u;
constexpr uint32_t TerrainLayerFeedbackMaxMip = 4;
constexpr uint32_t TerrainLayerFeedbackMaxPageIndex = 255;
constexpr uint8_t TerrainLayerFeedbackValidBit = 0x80u;

struct TerrainLayerFeedbackTexel
{
    uint32_t layerId = 0;
    uint32_t pageIndex = 0;
    uint32_t mip = 0;
    bool valid = false;
    uint8_t frameTag = 0;
};

TerrainLayerFeedbackTexel DecodeTerrainLayerFeedbackTexel(
    const uint8_t* texel);
void EncodeTerrainLayerFeedbackTexel(
    const TerrainLayerFeedbackTexel& texel,
    uint8_t* outTexel);

// 汇总后的一条层页记录。pageIndex 保持 G 通道原值；pageX/pageY 的还原依赖
// 该层在该 mip 的 pagesPerSide，因此由调用方（TerrainLayerVisibilityResolver）
// 用 TerrainLayerPageManager 解码，避免这个纯 CPU 格式反向依赖页管理器。
struct TerrainLayerFeedbackRecord
{
    uint32_t layerId = 0;
    uint32_t mip = 0;
    uint32_t pageIndex = 0;
    uint32_t sampleCount = 0;
    // 由帧内最大 sampleCount 归一化到 1..255，供请求排序与页表 A 通道复用。
    uint32_t priority = 0;

    bool operator<(const TerrainLayerFeedbackRecord& other) const noexcept
    {
        if (layerId != other.layerId) return layerId < other.layerId;
        if (mip != other.mip) return mip < other.mip;
        return pageIndex < other.pageIndex;
    }
};

struct TerrainLayerFeedbackFrameSummary
{
    uint64_t copiedEpoch = 0;
    uint8_t frameTag = 0;
    uint32_t validTexelCount = 0;
    uint32_t invalidTexelCount = 0;
    // 有效但 frameTag 不匹配：GPU/CPU 错位或旧内容，必须丢弃。
    uint32_t staleTexelCount = 0;
    uint32_t distinctPageCount = 0;
    // 每张 slot attachment 的 valid texel 数，用于确认 4 路 MRT 都对上。
    uint32_t slotSampleCount[TerrainLayerFeedbackSlotCount] = {};
    // 按 priority 降序、再按页身份排序，控制台输出与测试断言可复现。
    std::vector<TerrainLayerFeedbackRecord> pages;
};

// byteCount 可以是 1..4 张图像的连续数据；不足一张完整图像时抛
// std::invalid_argument。函数只按 texel 顺序解码，不依赖调用方布局以外的假设。
TerrainLayerFeedbackFrameSummary SummarizeTerrainLayerFeedback(
    const uint8_t* texels,
    size_t byteCount,
    uint8_t expectedFrameTag);

} // namespace VL
