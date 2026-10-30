#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "terrain/terrainLayerFeedback.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainLayerVT.h"
#include "terrain/terrainLayerVisibility.h"

namespace
{

std::vector<VL::TerrainLayerDesc> MakeLayerDesc(uint32_t count)
{
    std::vector<VL::TerrainLayerDesc> layers;
    for (uint32_t index = 0; index < count; ++index)
    {
        VL::TerrainLayerDesc layer;
        layer.name = "layer" + std::to_string(index);
        // 512² -> mip0..2（4/2/1 页每边）。
        layer.sourceResolution = 512;
        layers.push_back(layer);
    }
    return layers;
}

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

// 4 张 128×128 RGBA8 层页 feedback，只写一个有效 texel。
std::vector<uint8_t> MakeLayerFeedback(
    uint32_t layerId,
    uint32_t mip,
    uint32_t pageIndex,
    uint8_t frameTag)
{
    std::vector<uint8_t> buffer(VL::TerrainLayerFeedbackBufferBytes, 0);
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = layerId;
    texel.mip = mip;
    texel.pageIndex = pageIndex;
    texel.valid = true;
    texel.frameTag = frameTag;
    VL::EncodeTerrainLayerFeedbackTexel(texel, buffer.data());
    return buffer;
}

VL::TerrainLayerFeedbackFrameSummary Summarize(
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

VL::TerrainLayerVisibilityConfig MakeConfig(uint32_t layerCount)
{
    VL::TerrainLayerVisibilityConfig config;
    config.layerCount = layerCount;
    config.sourceMipLevels = 3;
    config.maxRequestsPerFrame = 64;
    config.idleEvictionEpochs = 64;
    config.unselectedMipIdleEvictionEpochs = 64;
    return config;
}

} // namespace

// L4e：console `terrainlayer forcemip` 的 CPU 参考。-1 保留自动结果，
// 0..4 覆盖后再按该层实际 mip 链 clamp（512² 只到 mip2）。
TEST(TerrainLayerStreamingQuality, ForceMipOverridesAutoAndClamps)
{
    EXPECT_EQ(VL::TerrainLayerVT::ResolveTargetMip(2, -1, 2), 2u);
    EXPECT_EQ(VL::TerrainLayerVT::ResolveTargetMip(2, 0, 2), 0u);
    EXPECT_EQ(VL::TerrainLayerVT::ResolveTargetMip(2, 1, 2), 1u);
    EXPECT_EQ(VL::TerrainLayerVT::ResolveTargetMip(0, 2, 2), 2u);
    // 强制 4 在 512² 层上必须 clamp 到 mip2，而不是请求不存在的页表级。
    EXPECT_EQ(VL::TerrainLayerVT::ResolveTargetMip(0, 4, 2), 2u);
    // 自动结果本身也必须 clamp（防御式，正常调用点已 clamp）。
    EXPECT_EQ(VL::TerrainLayerVT::ResolveTargetMip(7, -1, 4), 4u);
}

// L4e：向粗回退的混合系数。gap=0 必须为 0（完整驻留不改变像素），
// 每级 20%，最大 60%。shader TerrainLayerFallbackBlend 是同一公式。
TEST(TerrainLayerStreamingQuality, FallbackBlendStepsAndCaps)
{
    EXPECT_FLOAT_EQ(
        VL::TerrainLayerVT::FallbackBlendForMipGap(1, 1), 0.0f);
    EXPECT_FLOAT_EQ(
        VL::TerrainLayerVT::FallbackBlendForMipGap(1, 2), 0.2f);
    EXPECT_FLOAT_EQ(
        VL::TerrainLayerVT::FallbackBlendForMipGap(1, 3), 0.4f);
    EXPECT_FLOAT_EQ(
        VL::TerrainLayerVT::FallbackBlendForMipGap(1, 4), 0.6f);
    EXPECT_FLOAT_EQ(
        VL::TerrainLayerVT::FallbackBlendForMipGap(0, 4), 0.6f);
    // usedMip 不可能细于 target；即使传入也必须退化为 0。
    EXPECT_FLOAT_EQ(
        VL::TerrainLayerVT::FallbackBlendForMipGap(2, 1), 0.0f);
}

// L4e：mip N 的页在 mip N-1 上覆盖 2×2 子页，页面几何必须对得上。
TEST(TerrainLayerStreamingQuality, FinerMipChildrenCoverParentPage)
{
    const std::array<VL::TerrainLayerVT::LayerChildPage, 4> children =
        VL::TerrainLayerVT::FinerMipChildren(1, 2);
    EXPECT_EQ(children[0].pageX, 2u);
    EXPECT_EQ(children[0].pageY, 4u);
    EXPECT_EQ(children[1].pageX, 3u);
    EXPECT_EQ(children[1].pageY, 4u);
    EXPECT_EQ(children[2].pageX, 2u);
    EXPECT_EQ(children[2].pageY, 5u);
    EXPECT_EQ(children[3].pageX, 3u);
    EXPECT_EQ(children[3].pageY, 5u);
}

// L4e：相机前向移动时，可见页（已驻留）的细一级 2×2 子页应被预取；
// 预取不计入 feedback 命中，也不占用可见页每帧请求名额。
TEST(TerrainLayerStreamingQuality, PrefetchesFinerMipChildrenWhenMoving)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 1, 1, 16);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(10, 0, 2);
    // mip1 pageIndex 1 == (pageX=1, pageY=0)；先让可见页驻留。
    ASSERT_TRUE(layerPages.RequestPage(Key(0, 1, 1, 0)));

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.prefetchFinerMipEnabled = true;
    config.prefetchPagesPerFrame = 4;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            Summarize(buffer, 16, 10),
            layerPages,
            pending,
            10,
            10,
            config);

    // 子页 = mip0 的 (2,0)/(3,0)/(2,1)/(3,1)。
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 2, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 3, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 2, 1)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 3, 1)));
    EXPECT_EQ(stats.prefetchPages, 1u);
    EXPECT_EQ(stats.prefetchRequests, 4u);
    EXPECT_EQ(stats.prefetchAccepted, 4u);
    // 预取不是本帧可见页：markedPages 只统计 feedback 命中的可见页。
    EXPECT_EQ(stats.markedPages, 1u);
    EXPECT_EQ(stats.mipRequestCounts[0], 0u);
    EXPECT_TRUE(pending.empty());
}

// 相机静止（或未启用预取）时不得请求更细 mip。
TEST(TerrainLayerStreamingQuality, PrefetchDisabledDoesNotRequest)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 1, 1, 8);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(10, 0, 2);
    ASSERT_TRUE(layerPages.RequestPage(Key(0, 1, 1, 0)));

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.prefetchFinerMipEnabled = false;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            Summarize(buffer, 8, 10),
            layerPages,
            pending,
            10,
            10,
            config);

    EXPECT_EQ(stats.prefetchRequests, 0u);
    EXPECT_FALSE(layerPages.IsResident(Key(0, 0, 2, 0)));
}

// 当前可见页本身缺页时不能预取更细一级：否则细页会先占掉目标页/粗页
// 需要的物理槽位，缺页窗口反而变长。
TEST(TerrainLayerStreamingQuality, PrefetchSkipsWhenTargetPageMissing)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 1, 3, 4);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.prefetchFinerMipEnabled = true;
    config.prefetchPagesPerFrame = 4;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            Summarize(buffer, 4, 10),
            layerPages,
            pending,
            10,
            10,
            config);

    EXPECT_EQ(stats.prefetchRequests, 0u);
    // 目标页本帧被可见页路径请求成功；粗 mip 兜底页也一起入候选。
    EXPECT_GT(stats.fallbackTargetPages, 0u);
    EXPECT_TRUE(layerPages.IsResident(Key(0, 1, 1, 1)));
    EXPECT_TRUE(pending.empty());
}

// 预取有独立每帧名额，且不得挤掉本帧可见页请求。
TEST(TerrainLayerStreamingQuality, PrefetchRespectsPerFrameBudget)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 1, 1, 2);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(10, 0, 2);
    ASSERT_TRUE(layerPages.RequestPage(Key(0, 1, 1, 0)));

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.prefetchFinerMipEnabled = true;
    config.prefetchPagesPerFrame = 1;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            Summarize(buffer, 2, 10),
            layerPages,
            pending,
            10,
            10,
            config);

    EXPECT_EQ(stats.prefetchRequests, 1u);
    EXPECT_EQ(stats.prefetchAccepted, 1u);
}

// L4e：缺页分类。有更粗驻留页 -> 渐变；连粗页都没有 -> 纯 avgColor。
TEST(TerrainLayerStreamingQuality, ClassifiesFallbackBlendVersusAvgColor)
{
    // mip0 pageIndex 5 == (pageX=1, pageY=1)，粗页是 mip1 (0,0)/mip2 (0,0)。
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 0, 5, 1);

    VL::TerrainLayerPageManager withoutCoarse;
    withoutCoarse.ConfigureLayers(MakeLayerDesc(1));
    withoutCoarse.SetGpuEpochContext(10, 0, 2);
    VL::TerrainLayerVisibilityResolver resolverA;
    std::vector<VL::TerrainLayerPageRequest> pendingA;
    const VL::TerrainLayerExactFeedbackStats noCoarse =
        resolverA.ApplyExact(
            Summarize(buffer, 1, 10),
            withoutCoarse,
            pendingA,
            10,
            10,
            MakeConfig(1));
    EXPECT_EQ(noCoarse.fallbackSlots, 1u);
    EXPECT_EQ(noCoarse.avgColorPages, 1u);
    EXPECT_EQ(noCoarse.fallbackBlendPages, 0u);

    VL::TerrainLayerPageManager withCoarse;
    withCoarse.ConfigureLayers(MakeLayerDesc(1));
    withCoarse.SetGpuEpochContext(10, 0, 2);
    ASSERT_TRUE(withCoarse.RequestPage(Key(0, 1, 0, 0)));
    VL::TerrainLayerVisibilityResolver resolverB;
    std::vector<VL::TerrainLayerPageRequest> pendingB;
    const VL::TerrainLayerExactFeedbackStats withCoarseStats =
        resolverB.ApplyExact(
            Summarize(buffer, 1, 10),
            withCoarse,
            pendingB,
            10,
            10,
            MakeConfig(1));
    EXPECT_EQ(withCoarseStats.fallbackSlots, 1u);
    EXPECT_EQ(withCoarseStats.fallbackBlendPages, 1u);
    EXPECT_EQ(withCoarseStats.avgColorPages, 0u);
}
