#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VL
{

inline constexpr uint32_t TerrainCompositedFeedbackMaxMip = 13;
inline constexpr uint8_t TerrainCompositedFeedbackProtocolVersion = 1;
inline constexpr uint32_t TerrainCompositedFeedbackResolution = 128;
inline constexpr uint32_t TerrainCompositedFeedbackSlotCount = 4;
inline constexpr uint32_t TerrainCompositedFeedbackImageBytes =
    TerrainCompositedFeedbackResolution * TerrainCompositedFeedbackResolution * 4;
inline constexpr uint32_t TerrainCompositedFeedbackBufferBytes =
    TerrainCompositedFeedbackImageBytes * TerrainCompositedFeedbackSlotCount;

// 四张 RGBA8 组成两组：slot0/1 为 floor 地址/元数据，slot2/3 为 ceil 地址/元数据。
// 地址=(surfaceIndex,x,y,mip|0x80)，元数据=(frameTag,version=1,0,0)。
// 256 个 surface、每边最多 64 页、完整 mip0..13（8192 到 1）；独立 valid、
// version 和保留零字节可拒绝旧层输出与默认 RT clear，World 仍显式选择消费模式。
struct TerrainCompositedFeedbackTexel
{
    uint32_t surfaceIndex = 0;
    uint32_t mip = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    bool valid = false;
    uint8_t frameTag = 0;
};

struct TerrainCompositedFeedbackRecord
{
    uint32_t surfaceIndex = 0;
    uint32_t mip = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint64_t copiedEpoch = 0;
};

TerrainCompositedFeedbackTexel DecodeTerrainCompositedFeedbackTexel(
    const uint8_t* address,
    const uint8_t* metadata);
void EncodeTerrainCompositedFeedbackTexel(
    const TerrainCompositedFeedbackTexel& texel,
    uint8_t* outAddress,
    uint8_t* outMetadata);

// 输入必须是完整四附件回读；忽略 invalid/旧 tag，按页身份去重排序并保留 epoch。
// 非协议数据（包括未覆盖像素的默认 RT clear）作为 invalid 丢弃；World 禁止混模式。
std::vector<TerrainCompositedFeedbackRecord> DecodeTerrainCompositedFeedback(
    const uint8_t* texels,
    size_t byteCount,
    uint8_t expectedFrameTag,
    uint64_t copiedEpoch);

} // namespace VL
