#include "terrain/terrainCompositedFeedback.h"

#include <algorithm>
#include <stdexcept>
#include <tuple>


namespace VL
{

TerrainCompositedFeedbackTexel DecodeTerrainCompositedFeedbackTexel(
    const uint8_t* address,
    const uint8_t* metadata)
{
    TerrainCompositedFeedbackTexel result;
    if (address == nullptr || metadata == nullptr)
        return result;
    result.surfaceIndex = address[0];
    result.x = address[1];
    result.y = address[2];
    result.mip = address[3] & 127u;
    result.frameTag = metadata[0];
    result.valid = (address[3] & 128u) != 0 &&
        result.x <= 63 && result.y <= 63 &&
        result.mip <= TerrainCompositedFeedbackMaxMip &&
        result.frameTag != 0 &&
        metadata[1] == TerrainCompositedFeedbackProtocolVersion &&
        metadata[2] == 0 && metadata[3] == 0;
    return result;
}

void EncodeTerrainCompositedFeedbackTexel(
    const TerrainCompositedFeedbackTexel& texel,
    uint8_t* outAddress,
    uint8_t* outMetadata)
{
    if (outAddress == nullptr || outMetadata == nullptr)
        throw std::invalid_argument("Composited feedback encode requires output data");
    if (texel.surfaceIndex > 255 || texel.mip > TerrainCompositedFeedbackMaxMip ||
        texel.x > 63 || texel.y > 63)
        throw std::out_of_range("Composited feedback exceeds surface8/mip0..13/x6/y6 encoding");
    if (texel.valid && texel.frameTag == 0)
        throw std::invalid_argument("Composited feedback valid texel requires nonzero frameTag");
    if (!texel.valid)
    {
        std::fill_n(outAddress, 4, uint8_t{0});
        std::fill_n(outMetadata, 4, uint8_t{0});
        return;
    }
    outAddress[0] = static_cast<uint8_t>(texel.surfaceIndex);
    outAddress[1] = static_cast<uint8_t>(texel.x);
    outAddress[2] = static_cast<uint8_t>(texel.y);
    outAddress[3] = static_cast<uint8_t>(texel.mip | 128u);
    outMetadata[0] = texel.frameTag;
    outMetadata[1] = TerrainCompositedFeedbackProtocolVersion;
    outMetadata[2] = 0;
    outMetadata[3] = 0;
}

std::vector<TerrainCompositedFeedbackRecord> DecodeTerrainCompositedFeedback(
    const uint8_t* texels,
    size_t byteCount,
    uint8_t expectedFrameTag,
    uint64_t copiedEpoch)
{
    if (texels == nullptr || byteCount != TerrainCompositedFeedbackBufferBytes ||
        expectedFrameTag == 0)
        throw std::invalid_argument("Composited feedback requires four full attachments and nonzero tag");
    std::vector<TerrainCompositedFeedbackRecord> pages;
    for (uint32_t pair = 0; pair < 2; ++pair)
    {
        const size_t addressBase = pair * 2 * TerrainCompositedFeedbackImageBytes;
        const size_t metadataBase = addressBase + TerrainCompositedFeedbackImageBytes;
        for (size_t offset = 0; offset < TerrainCompositedFeedbackImageBytes; offset += 4)
        {
            const auto texel = DecodeTerrainCompositedFeedbackTexel(
                texels + addressBase + offset, texels + metadataBase + offset);
            if (!texel.valid || texel.frameTag != expectedFrameTag)
                continue;
            pages.push_back({texel.surfaceIndex, texel.mip, texel.x, texel.y, copiedEpoch});
        }
    }
    const auto key = [](const TerrainCompositedFeedbackRecord& page)
    {
        return std::make_tuple(page.surfaceIndex, page.mip, page.x, page.y);
    };
    std::sort(pages.begin(), pages.end(), [&](const auto& first, const auto& second)
    {
        return key(first) < key(second);
    });
    pages.erase(std::unique(pages.begin(), pages.end(), [&](const auto& first, const auto& second)
    {
        return key(first) == key(second);
    }), pages.end());
    return pages;
}

} // namespace VL
