#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "terrain/terrainCompositedFeedback.h"
#include "terrain/terrainFeedback.h"

namespace
{

std::vector<uint8_t> CompositedBuffer()
{
    return std::vector<uint8_t>(VL::TerrainCompositedFeedbackBufferBytes, 0);
}

void WriteComposited(
    std::vector<uint8_t>& buffer, uint32_t pair, uint32_t pixel,
    VL::TerrainCompositedFeedbackTexel texel)
{
    VL::EncodeTerrainCompositedFeedbackTexel(
        texel, buffer.data() + pair * 2 * VL::TerrainCompositedFeedbackImageBytes + pixel * 4,
        buffer.data() + (pair * 2 + 1) * VL::TerrainCompositedFeedbackImageBytes + pixel * 4);
}

} // namespace

TEST(TerrainCompositedFeedbackEncoding, ExhaustsPageCoordinatesAcrossAllMips)
{
    for (const uint32_t surface : {0u, 1u, 127u, 255u})
    {
        for (uint32_t mip = 0; mip <= 13; ++mip)
        {
            for (uint32_t pageX = 0; pageX < 64; ++pageX)
            {
                for (uint32_t pageY = 0; pageY < 64; ++pageY)
                {
                    std::array<uint8_t, 4> bytes{};
                    std::array<uint8_t, 4> metadata{};
                    VL::EncodeTerrainCompositedFeedbackTexel(
                        {surface, mip, pageX, pageY, true, 255}, bytes.data(), metadata.data());
                    const auto decoded = VL::DecodeTerrainCompositedFeedbackTexel(
                        bytes.data(), metadata.data());
                    ASSERT_EQ(decoded.surfaceIndex, surface);
                    ASSERT_EQ(decoded.mip, mip);
                    ASSERT_EQ(decoded.x, pageX);
                    ASSERT_EQ(decoded.y, pageY);
                    ASSERT_TRUE(decoded.valid);
                    ASSERT_EQ(decoded.frameTag, 255);
                }
            }
        }
    }
}

TEST(TerrainCompositedFeedbackEncoding, PinsFullMipAddressAndMetadataBytes)
{
    std::array<uint8_t, 4> bytes{};
    std::array<uint8_t, 4> metadata{};
    VL::EncodeTerrainCompositedFeedbackTexel(
        {255, 13, 63, 63, true, 17}, bytes.data(), metadata.data());
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{255, 63, 63, 141}));
    EXPECT_EQ(metadata, (std::array<uint8_t, 4>{17, 1, 0, 0}));
    VL::EncodeTerrainCompositedFeedbackTexel(
        {0, 7, 63, 0, true, 1}, bytes.data(), metadata.data());
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{0, 63, 0, 135}));
    EXPECT_EQ(metadata, (std::array<uint8_t, 4>{1, 1, 0, 0}));
}

TEST(TerrainCompositedFeedbackEncoding, InvalidWritesCanonicalClear)
{
    std::array<uint8_t, 4> bytes{255, 255, 255, 255};
    std::array<uint8_t, 4> metadata{255, 255, 255, 255};
    VL::EncodeTerrainCompositedFeedbackTexel(
        {42, 13, 63, 63, false, 9}, bytes.data(), metadata.data());
    EXPECT_EQ(bytes, (std::array<uint8_t, 4>{0, 0, 0, 0}));
    EXPECT_EQ(metadata, (std::array<uint8_t, 4>{0, 0, 0, 0}));
    EXPECT_FALSE(VL::DecodeTerrainCompositedFeedbackTexel(bytes.data(), metadata.data()).valid);
    EXPECT_FALSE(VL::DecodeTerrainCompositedFeedbackTexel(nullptr, metadata.data()).valid);
    EXPECT_FALSE(VL::DecodeTerrainCompositedFeedbackTexel(bytes.data(), nullptr).valid);
}

TEST(TerrainCompositedFeedbackEncoding, RejectsOverflowAndReservedTag)
{
    std::array<uint8_t, 4> bytes{};
    std::array<uint8_t, 4> metadata{};
    for (const auto& texel : std::vector<VL::TerrainCompositedFeedbackTexel>{
         {256, 0, 0, 0, true, 1}, {0, 14, 0, 0, true, 1},
         {0, 0, 64, 0, true, 1}, {0, 0, 0, 64, true, 1}})
        EXPECT_THROW(VL::EncodeTerrainCompositedFeedbackTexel(texel, bytes.data(), metadata.data()),
                     std::out_of_range);
    EXPECT_THROW(VL::EncodeTerrainCompositedFeedbackTexel(
        {0, 0, 0, 0, true, 0}, bytes.data(), metadata.data()), std::invalid_argument);
    EXPECT_THROW(VL::EncodeTerrainCompositedFeedbackTexel({}, nullptr, metadata.data()),
                 std::invalid_argument);
    EXPECT_THROW(VL::EncodeTerrainCompositedFeedbackTexel({}, bytes.data(), nullptr),
                 std::invalid_argument);
}

TEST(TerrainCompositedFeedbackDecode, DeduplicatesBothMipSlotsAndKeepsCopiedEpoch)
{
    auto buffer = CompositedBuffer();
    WriteComposited(buffer, 0, 0, {255, 12, 0, 0, true, 8});
    WriteComposited(buffer, 0, 1, {1, 0, 63, 63, true, 8});
    WriteComposited(buffer, 1, 0, {255, 13, 0, 0, true, 8});
    WriteComposited(buffer, 1, 1, {1, 0, 63, 63, true, 8});
    const auto pages = VL::DecodeTerrainCompositedFeedback(
        buffer.data(), buffer.size(), 8, 1234567890123ull);
    ASSERT_EQ(pages.size(), 3u);
    EXPECT_EQ(pages[0].surfaceIndex, 1u);
    EXPECT_EQ(pages[0].x, 63u);
    EXPECT_EQ(pages[0].y, 63u);
    EXPECT_EQ(pages[1].mip, 12u);
    EXPECT_EQ(pages[2].mip, 13u);
    for (const auto& page : pages)
        EXPECT_EQ(page.copiedEpoch, 1234567890123ull);
}

TEST(TerrainCompositedFeedbackDecode, DiscardsStaleTagsAndClearTexels)
{
    auto buffer = CompositedBuffer();
    WriteComposited(buffer, 0, 0, {0, 0, 0, 0, true, 255});
    WriteComposited(buffer, 1, 0, {255, 13, 0, 0, true, 1});
    const auto pages = VL::DecodeTerrainCompositedFeedback(
        buffer.data(), buffer.size(), 1, 256);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].surfaceIndex, 255u);
    EXPECT_EQ(pages[0].copiedEpoch, 256u);
    std::fill(buffer.begin(), buffer.end(), 0);
    EXPECT_TRUE(VL::DecodeTerrainCompositedFeedback(buffer.data(), buffer.size(), 1, 256).empty());
}

TEST(TerrainCompositedFeedbackDecode, IgnoresDefaultClearEvenWhenExpectedTagIs255)
{
    auto buffer = CompositedBuffer();
    for (size_t offset = 0; offset < buffer.size(); offset += 4)
    {
        buffer[offset] = 51;
        buffer[offset + 1] = 51;
        buffer[offset + 2] = 51;
        buffer[offset + 3] = 255;
    }
    for (uint32_t tag = 1; tag <= 255; ++tag)
        EXPECT_TRUE(VL::DecodeTerrainCompositedFeedback(
            buffer.data(), buffer.size(), static_cast<uint8_t>(tag), 254).empty());
    WriteComposited(buffer, 1, 1, {255, 13, 0, 0, true, 255});
    const auto pages = VL::DecodeTerrainCompositedFeedback(buffer.data(), buffer.size(), 255, 254);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].mip, 13u);
}

TEST(TerrainCompositedFeedbackDecode, RequiresValidVersionAndReservedMetadataBytes)
{
    std::array<uint8_t, 4> address{255, 63, 63, 141};
    std::array<uint8_t, 4> metadata{255, 1, 0, 0};
    EXPECT_TRUE(VL::DecodeTerrainCompositedFeedbackTexel(address.data(), metadata.data()).valid);
    for (const auto& invalid : std::vector<std::array<uint8_t, 4>>{
         {0, 1, 0, 0}, {255, 0, 0, 0}, {255, 2, 0, 0},
         {255, 1, 1, 0}, {255, 1, 0, 1}, {51, 51, 51, 255}})
        EXPECT_FALSE(VL::DecodeTerrainCompositedFeedbackTexel(address.data(), invalid.data()).valid);
    for (const auto& invalid : std::vector<std::array<uint8_t, 4>>{
         {255, 63, 63, 13}, {255, 64, 63, 141}, {255, 63, 64, 141},
         {255, 63, 63, 142}, {255, 63, 63, 255}})
        EXPECT_FALSE(VL::DecodeTerrainCompositedFeedbackTexel(invalid.data(), metadata.data()).valid);
}

TEST(TerrainCompositedFeedbackDecode, DoesNotPairAddressWithDifferentPixelOrRequest)
{
    auto buffer = CompositedBuffer();
    std::array<uint8_t, 4> address{};
    std::array<uint8_t, 4> metadata{};
    VL::EncodeTerrainCompositedFeedbackTexel(
        {1, 13, 0, 0, true, 7}, address.data(), metadata.data());
    std::copy(address.begin(), address.end(), buffer.begin());
    std::copy(metadata.begin(), metadata.end(),
              buffer.begin() + VL::TerrainCompositedFeedbackImageBytes + 4);
    std::copy(metadata.begin(), metadata.end(),
              buffer.begin() + 3 * VL::TerrainCompositedFeedbackImageBytes);
    EXPECT_TRUE(VL::DecodeTerrainCompositedFeedback(buffer.data(), buffer.size(), 7, 6).empty());
}

TEST(TerrainCompositedFeedbackDecode, RejectsIncompleteAndOversizedBuffers)
{
    auto buffer = CompositedBuffer();
    EXPECT_THROW(VL::DecodeTerrainCompositedFeedback(nullptr, buffer.size(), 1, 1),
                 std::invalid_argument);
    for (const size_t size : {size_t{0}, size_t{3},
         size_t{VL::TerrainCompositedFeedbackImageBytes}, buffer.size() - 1, buffer.size() + 4})
        EXPECT_THROW(VL::DecodeTerrainCompositedFeedback(buffer.data(), size, 1, 1),
                     std::invalid_argument);
    EXPECT_THROW(VL::DecodeTerrainCompositedFeedback(buffer.data(), buffer.size(), 0, 1),
                 std::invalid_argument);
}

TEST(TerrainCompositedFeedbackDecode, UsesFenceCompletedEpochAndConsumesOnlyOnce)
{
    VL::TerrainFeedbackRingState ring;
    ring.Reset(2);
    ring.Record(1, 510, 1);
    EXPECT_FALSE(ring.TryConsume(1, 509).has_value());
    EXPECT_TRUE(ring.IsPending(1));
    const auto completed = ring.TryConsume(1, 510);
    ASSERT_TRUE(completed.has_value());
    auto buffer = CompositedBuffer();
    WriteComposited(buffer, 0, 0, {0, 0, 0, 0, true, completed->frameTag});
    const auto pages = VL::DecodeTerrainCompositedFeedback(
        buffer.data(), buffer.size(), completed->frameTag, completed->copiedEpoch);
    ASSERT_EQ(pages.size(), 1u);
    EXPECT_EQ(pages[0].copiedEpoch, 510u);
    EXPECT_FALSE(ring.TryConsume(1, 511).has_value());
}
