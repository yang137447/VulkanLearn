#include <cstdint>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainLayerFeedback.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainLayerVisibility.h"

namespace
{

VL::TerrainLayerPageKey Key(
    uint32_t layerId,
    uint32_t mip,
    uint32_t pageX,
    uint32_t pageY)
{
    VL::TerrainLayerPageKey key;
    key.layerId = layerId;
    key.mip = mip;
    key.pageX = pageX;
    key.pageY = pageY;
    return key;
}

std::vector<VL::TerrainLayerDesc> MakeLayers(
    uint32_t count,
    uint32_t sourceResolution = 512)
{
    std::vector<VL::TerrainLayerDesc> layers;
    for (uint32_t index = 0; index < count; ++index)
    {
        VL::TerrainLayerDesc layer;
        layer.name = "layer" + std::to_string(index);
        layer.sourceResolution = sourceResolution;
        layers.push_back(layer);
    }
    return layers;
}

// 4 张 128×128 RGBA8 的连续缓冲区，默认全 0（invalid）。
std::vector<uint8_t> MakeFeedbackBuffer()
{
    return std::vector<uint8_t>(VL::TerrainLayerFeedbackBufferBytes, 0);
}

void WriteSlotTexel(
    std::vector<uint8_t>& buffer,
    uint32_t slot,
    size_t texelIndex,
    const VL::TerrainLayerFeedbackTexel& texel)
{
    const size_t offset =
        static_cast<size_t>(slot) * VL::TerrainLayerFeedbackImageBytes +
        texelIndex * VL::TerrainLayerFeedbackTexelBytes;
    ASSERT_LE(offset + VL::TerrainLayerFeedbackTexelBytes, buffer.size());
    VL::EncodeTerrainLayerFeedbackTexel(
        texel, buffer.data() + offset);
}

VL::TerrainLayerVisibilityConfig MakeConfig(uint32_t layerCount)
{
    VL::TerrainLayerVisibilityConfig config;
    config.layerCount = layerCount;
    config.sourceMipLevels = 3;
    config.maxRequestsPerFrame = 64;
    config.exactFeedbackNeighborhoodRadius = 0;
    config.idleEvictionEpochs = 4;
    return config;
}

VL::TerrainLayerFeedbackFrameSummary SummaryFrom(
    const std::vector<uint8_t>& buffer,
    uint8_t frameTag,
    uint64_t copiedEpoch)
{
    VL::TerrainLayerFeedbackFrameSummary summary =
        VL::SummarizeTerrainLayerFeedback(
            buffer.data(), buffer.size(), frameTag);
    summary.copiedEpoch = copiedEpoch;
    return summary;
}

} // namespace

TEST(TerrainLayerFeedbackEncoding, RoundTripsSlotFields)
{
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 42;
    texel.pageIndex = 200;
    texel.mip = 3;
    texel.valid = true;
    texel.frameTag = 17;

    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    WriteSlotTexel(buffer, 2, 5, texel);

    const VL::TerrainLayerFeedbackTexel decoded =
        VL::DecodeTerrainLayerFeedbackTexel(
            buffer.data() + 2 * VL::TerrainLayerFeedbackImageBytes +
            5 * VL::TerrainLayerFeedbackTexelBytes);
    EXPECT_EQ(decoded.layerId, 42u);
    EXPECT_EQ(decoded.pageIndex, 200u);
    EXPECT_EQ(decoded.mip, 3u);
    EXPECT_TRUE(decoded.valid);
    EXPECT_EQ(decoded.frameTag, 17u);
}

TEST(TerrainLayerFeedbackEncoding, RejectsOutOfRangeFields)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 256;
    EXPECT_THROW(
        VL::EncodeTerrainLayerFeedbackTexel(texel, buffer.data()),
        std::out_of_range);

    texel.layerId = 0;
    texel.pageIndex = 256;
    EXPECT_THROW(
        VL::EncodeTerrainLayerFeedbackTexel(texel, buffer.data()),
        std::out_of_range);

    texel.pageIndex = 0;
    texel.mip = 5;
    EXPECT_THROW(
        VL::EncodeTerrainLayerFeedbackTexel(texel, buffer.data()),
        std::out_of_range);
}

TEST(TerrainLayerFeedbackSummary, DedupesAcrossSlotsAndIgnoresInvalid)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 7;
    texel.pageIndex = 3;
    texel.mip = 1;
    texel.valid = true;
    texel.frameTag = 9;
    // 同一 (layer,mip,page) 出现在不同屏幕位置/不同 slot，必须合并成一条。
    WriteSlotTexel(buffer, 0, 0, texel);
    WriteSlotTexel(buffer, 1, 10, texel);
    WriteSlotTexel(buffer, 3, 99, texel);

    VL::TerrainLayerFeedbackFrameSummary summary =
        SummaryFrom(buffer, 9, 5);
    EXPECT_EQ(summary.validTexelCount, 3u);
    EXPECT_EQ(summary.distinctPageCount, 1u);
    ASSERT_EQ(summary.pages.size(), 1u);
    EXPECT_EQ(summary.pages[0].sampleCount, 3u);
    EXPECT_EQ(summary.slotSampleCount[0], 1u);
    EXPECT_EQ(summary.slotSampleCount[1], 1u);
    EXPECT_EQ(summary.slotSampleCount[2], 0u);
    EXPECT_EQ(summary.slotSampleCount[3], 1u);
    // 除写入的 texel 外都是 clear 值（invalid）。
    EXPECT_EQ(
        summary.invalidTexelCount,
        VL::TerrainLayerFeedbackTexelsPerImage *
                VL::TerrainLayerFeedbackSlotCount -
            3u);
}

TEST(TerrainLayerFeedbackSummary, DropsStaleTags)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel current;
    current.layerId = 2;
    current.mip = 0;
    current.valid = true;
    current.frameTag = 40;
    WriteSlotTexel(buffer, 0, 1, current);

    VL::TerrainLayerFeedbackTexel stale = current;
    stale.frameTag = 39;
    WriteSlotTexel(buffer, 1, 1, stale);

    VL::TerrainLayerFeedbackFrameSummary summary =
        SummaryFrom(buffer, 40, 100);
    EXPECT_EQ(summary.validTexelCount, 1u);
    EXPECT_EQ(summary.staleTexelCount, 1u);
    EXPECT_EQ(summary.pages.size(), 1u);
    EXPECT_EQ(summary.pages[0].sampleCount, 1u);
}

TEST(TerrainLayerFeedbackSummary, OrdersByCoveragePriority)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel background;
    background.layerId = 1;
    background.mip = 2;
    background.valid = true;
    background.frameTag = 5;
    VL::TerrainLayerFeedbackTexel foreground = background;
    foreground.layerId = 3;
    foreground.mip = 0;

    WriteSlotTexel(buffer, 0, 0, background);
    // 前景层占 4 个 texel，必须排在只有 1 个 texel 的背景层之前。
    for (size_t texel = 1; texel <= 4; ++texel)
        WriteSlotTexel(buffer, 0, texel, foreground);

    VL::TerrainLayerFeedbackFrameSummary summary =
        SummaryFrom(buffer, 5, 6);
    ASSERT_EQ(summary.pages.size(), 2u);
    EXPECT_EQ(summary.pages[0].layerId, 3u);
    EXPECT_EQ(summary.pages[0].mip, 0u);
    EXPECT_EQ(summary.pages[0].priority, 255u);
    EXPECT_EQ(summary.pages[1].layerId, 1u);
    // 覆盖率 1/4 -> sqrt(0.25) * 255 ≈ 128。
    EXPECT_EQ(summary.pages[1].priority, 128u);
}

TEST(TerrainLayerFeedbackSummary, SummarizesFullFourSlotBufferAndResetsScratch)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    for (uint32_t slot = 0;
         slot < VL::TerrainLayerFeedbackSlotCount;
         ++slot)
    {
        for (uint32_t texelIndex = 0;
             texelIndex < VL::TerrainLayerFeedbackTexelsPerImage;
             ++texelIndex)
        {
            VL::TerrainLayerFeedbackTexel texel;
            texel.layerId = slot;
            texel.mip = slot;
            texel.pageIndex = texelIndex % 256u;
            texel.valid = true;
            texel.frameTag = 23;
            WriteSlotTexel(buffer, slot, texelIndex, texel);
        }
    }

    const VL::TerrainLayerFeedbackFrameSummary first =
        SummaryFrom(buffer, 23, 10);
    EXPECT_EQ(first.validTexelCount,
        VL::TerrainLayerFeedbackTexelsPerImage *
            VL::TerrainLayerFeedbackSlotCount);
    EXPECT_EQ(first.invalidTexelCount, 0u);
    EXPECT_EQ(first.staleTexelCount, 0u);
    EXPECT_EQ(first.distinctPageCount, 1024u);
    EXPECT_EQ(first.pages.size(), 1024u);
    for (uint32_t slot = 0;
         slot < VL::TerrainLayerFeedbackSlotCount;
         ++slot)
    {
        EXPECT_EQ(first.slotSampleCount[slot],
            VL::TerrainLayerFeedbackTexelsPerImage);
    }

    // 同一个 scratch 第二次调用必须从 0 开始，不能沿用上一帧计数。
    buffer.assign(buffer.size(), 0);
    WriteSlotTexel(buffer, 0, 0, VL::TerrainLayerFeedbackTexel{
        9, 1, 2, true, 24});
    const VL::TerrainLayerFeedbackFrameSummary second =
        SummaryFrom(buffer, 24, 11);
    EXPECT_EQ(second.validTexelCount, 1u);
    EXPECT_EQ(second.distinctPageCount, 1u);
    EXPECT_EQ(second.pages[0].sampleCount, 1u);
}

TEST(TerrainLayerFeedbackSummary, RejectsMipOutsideEncodedRange)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    // 手工写入 valid + mip=7；编码器本身拒绝该值，解码端必须也能防御。
    buffer[0] = 0;       // layerId
    buffer[1] = 0;       // pageIndex
    buffer[2] = 0x87;    // valid | 7
    buffer[3] = 12;      // frameTag
    const VL::TerrainLayerFeedbackFrameSummary summary =
        SummaryFrom(buffer, 12, 1);
    EXPECT_EQ(summary.validTexelCount, 0u);
    EXPECT_GE(summary.invalidTexelCount, 1u);
    EXPECT_EQ(summary.distinctPageCount, 0u);
    EXPECT_TRUE(summary.pages.empty());
}

TEST(TerrainLayerExactFeedback, RequestsExactPageAndMip)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 1;
    // 512² 层 mip1 是 2×2 页，pageIndex 3 == (pageX=1, pageY=1)。
    texel.mip = 1;
    texel.pageIndex = 3;
    texel.valid = true;
    texel.frameTag = 12;
    WriteSlotTexel(buffer, 2, 0, texel);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayers(2));
    layerPages.SetGpuEpochContext(20, 0, 2);
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityResolver resolver;

    const VL::TerrainLayerExactFeedbackStats stats = resolver.ApplyExact(
        SummaryFrom(buffer, 12, 20),
        layerPages,
        pending,
        20,
        20,
        MakeConfig(2));

    EXPECT_EQ(stats.validSlots, 1u);
    EXPECT_EQ(stats.distinctPages, 1u);
    // L4c：目标页缺页时同时预取更粗 mip（此处 mip1 -> mip2 一页；
    // 512² 的 maxMip 是 2，所以没有 +2 级）。
    EXPECT_EQ(stats.candidatePages, 2u);
    EXPECT_EQ(stats.acceptedRequests, 2u);
    EXPECT_EQ(stats.mipRequestCounts[1], 1u);
    EXPECT_EQ(stats.mipRequestCounts[2], 1u);
    EXPECT_EQ(stats.fallbackTargetPages, 1u);
    EXPECT_EQ(stats.fallbackLevelPages[0], 1u);
    EXPECT_TRUE(pending.empty());
    EXPECT_TRUE(layerPages.IsResident(Key(1, 1, 1, 1)));
    EXPECT_TRUE(layerPages.IsResident(Key(1, 2, 0, 0)));
    // 不能把 pageIndex 误当成 (pageX=pageY=pageIndex) 或请求别的 mip。
    EXPECT_FALSE(layerPages.IsResident(Key(1, 1, 3, 0)));
    EXPECT_FALSE(layerPages.IsResident(Key(1, 0, 1, 1)));
}

TEST(TerrainLayerExactFeedback, ExpandsNeighborhoodAsPrefetch)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 0;
    texel.mip = 1; // 512² mip1 = 2×2 页。
    texel.pageIndex = 0;
    texel.valid = true;
    texel.frameTag = 3;
    WriteSlotTexel(buffer, 0, 0, texel);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayers(1));
    layerPages.SetGpuEpochContext(4, 0, 2);
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityResolver resolver;

    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.exactFeedbackNeighborhoodRadius = 1;
    const VL::TerrainLayerExactFeedbackStats stats = resolver.ApplyExact(
        SummaryFrom(buffer, 3, 4),
        layerPages,
        pending,
        4,
        4,
        config);

    // 邻域扩展到 2×2 全部页，但中心页保留更高 priority。L4c 再为这 4 个
    // 缺页各加一张更粗 mip 页（mip2 每层只有 1 页，去重后 1 页）。
    EXPECT_EQ(stats.candidatePages, 5u);
    EXPECT_EQ(stats.acceptedRequests, 5u);
    EXPECT_EQ(stats.fallbackTargetPages, 4u);
    EXPECT_EQ(stats.fallbackLevelPages[0], 1u);
    const VL::TerrainLayerPageManager::PageState* center =
        layerPages.FindPage(Key(0, 1, 0, 0));
    const VL::TerrainLayerPageManager::PageState* neighbor =
        layerPages.FindPage(Key(0, 1, 1, 1));
    const VL::TerrainLayerPageManager::PageState* fallback =
        layerPages.FindPage(Key(0, 2, 0, 0));
    ASSERT_NE(center, nullptr);
    ASSERT_NE(neighbor, nullptr);
    ASSERT_NE(fallback, nullptr);
    EXPECT_GT(center->priority, neighbor->priority);
    // 粗 mip 页只保证“回退有内容可采”，优先级必须低于目标 mip 页。
    EXPECT_LT(fallback->priority, center->priority);
}

TEST(TerrainLayerExactFeedback, DropsOutOfRangePageIndex)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 0;
    // 512² mip2 只有 1 页；pageIndex 1 越界。
    texel.mip = 2;
    texel.pageIndex = 1;
    texel.valid = true;
    texel.frameTag = 1;
    WriteSlotTexel(buffer, 0, 0, texel);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayers(1));
    layerPages.SetGpuEpochContext(1, 0, 2);
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityResolver resolver;

    const VL::TerrainLayerExactFeedbackStats stats = resolver.ApplyExact(
        SummaryFrom(buffer, 1, 1),
        layerPages,
        pending,
        1,
        1,
        MakeConfig(1));

    EXPECT_EQ(stats.outOfRangePages, 1u);
    EXPECT_EQ(stats.candidatePages, 0u);
    EXPECT_TRUE(pending.empty());
}

TEST(TerrainLayerExactFeedback, MarksResidentPagesWithoutReRequesting)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 0;
    texel.mip = 0;
    texel.pageIndex = 5;
    texel.valid = true;
    texel.frameTag = 7;
    WriteSlotTexel(buffer, 0, 0, texel);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayers(1));
    layerPages.SetGpuEpochContext(30, 0, 2);
    // 512² mip0 是 4×4；pageIndex 5 == (pageX=1, pageY=1)。
    ASSERT_TRUE(layerPages.RequestPageAtEpoch(
        Key(0, 0, 1, 1), 10, 10, 30));
    layerPages.SetGpuEpochContext(30, 0, 2);

    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityResolver resolver;
    const VL::TerrainLayerExactFeedbackStats stats = resolver.ApplyExact(
        SummaryFrom(buffer, 7, 40),
        layerPages,
        pending,
        40,
        40,
        MakeConfig(1));

    EXPECT_EQ(stats.markedPages, 1u);
    EXPECT_EQ(stats.acceptedRequests, 0u);
    EXPECT_TRUE(pending.empty());
    const VL::TerrainLayerPageManager::PageState* page =
        layerPages.FindPage(Key(0, 0, 1, 1));
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->lastSeenEpoch, 40u);
    EXPECT_EQ(page->lastTouchedEpoch, 40u);
}

TEST(TerrainLayerExactFeedback, HonorsBudgetAndConvergesAcrossFrames)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 0;
    texel.mip = 0; // 512² mip0 = 16 页。
    texel.valid = true;
    texel.frameTag = 2;
    for (uint32_t pageIndex = 0; pageIndex < 16; ++pageIndex)
    {
        texel.pageIndex = pageIndex;
        WriteSlotTexel(buffer, 0, pageIndex, texel);
    }

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayers(1));
    layerPages.SetGpuEpochContext(1, 0, 2);
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityResolver resolver;

    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.maxRequestsPerFrame = 5;

    VL::TerrainLayerExactFeedbackStats stats = resolver.ApplyExact(
        SummaryFrom(buffer, 2, 50),
        layerPages,
        pending,
        50,
        50,
        config);
    EXPECT_EQ(stats.attemptedRequests, 5u);
    EXPECT_EQ(stats.acceptedRequests, 5u);
    // 16 个目标页 + mip1 4 页 + mip2 1 页 = 21 个候选。
    EXPECT_EQ(pending.size(), 16u);

    // 后续帧继续消费同一批候选，直到 21 页全部驻留、队列清空。
    uint64_t epoch = 50;
    uint32_t acceptedAfterFirstFrame = 0;
    for (uint32_t iteration = 0; iteration < 4; ++iteration)
    {
        ++epoch;
        layerPages.SetGpuEpochContext(epoch, 0, 2);
        stats = resolver.ApplyExact(
            SummaryFrom(buffer, 2, epoch),
            layerPages,
            pending,
            epoch,
            epoch,
            config);
        acceptedAfterFirstFrame += stats.acceptedRequests;
    }
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(layerPages.ResidentCount(), 21u);
    EXPECT_EQ(acceptedAfterFirstFrame, 16u);
}

TEST(TerrainLayerExactFeedback, RequestsEachMipReportedByDifferentSlots)
{
    std::vector<uint8_t> buffer = MakeFeedbackBuffer();
    VL::TerrainLayerFeedbackTexel fine;
    fine.layerId = 0;
    fine.mip = 0;
    fine.pageIndex = 0;
    fine.valid = true;
    fine.frameTag = 4;
    VL::TerrainLayerFeedbackTexel coarse = fine;
    coarse.mip = 2;
    WriteSlotTexel(buffer, 0, 0, fine);
    WriteSlotTexel(buffer, 1, 0, coarse);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayers(1));
    layerPages.SetGpuEpochContext(2, 0, 2);
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityResolver resolver;

    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.exactFeedbackNeighborhoodRadius = 1;
    const VL::TerrainLayerExactFeedbackStats stats = resolver.ApplyExact(
        SummaryFrom(buffer, 4, 2),
        layerPages,
        pending,
        2,
        2,
        config);

    // 两个 slot 报告同层的不同 mip：各自必须有独立的页集，不能只保留一个。
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 0, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 2, 0, 0)));
    EXPECT_EQ(stats.mipRequestCounts[0], 4u); // mip0 邻域 2×2
    EXPECT_EQ(stats.mipRequestCounts[2], 1u); // mip2 只有 1 页
}

// L4a 验收 MEDIUM 2：avgColor 默认值必须是中性灰；全黑会让 mode 1 的缺页
// 块在光照下像空洞。
TEST(TerrainLayerDefaults, AvgColorDefaultsToNeutralGrey)
{
    VL::TerrainLayerDesc layer;
    EXPECT_FLOAT_EQ(layer.avgColor.x(), 0.18f);
    EXPECT_FLOAT_EQ(layer.avgColor.y(), 0.18f);
    EXPECT_FLOAT_EQ(layer.avgColor.z(), 0.18f);
}
