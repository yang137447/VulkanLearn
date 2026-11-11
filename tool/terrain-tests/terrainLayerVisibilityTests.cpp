#include <gtest/gtest.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "resource/image/hostImage.h"
#include "terrain/terrainLayerVisibility.h"
#include "terrain/terrainPageManager.h"

namespace
{

HostImage MakeHeightmap(
    uint32_t width,
    uint32_t height,
    const std::function<float(uint32_t, uint32_t)>& value)
{
    HostImage image;
    image.width = width;
    image.height = height;
    image.channels = 4;
    image.format = HostImage::PixelFormat::RGBA32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = width * image.GetPixelSize();
    image.data.resize(static_cast<size_t>(image.rowStrideBytes) * height);
    float* pixels = reinterpret_cast<float*>(image.data.data());
    for (uint32_t y = 0; y < height; ++y)
    {
        for (uint32_t x = 0; x < width; ++x)
        {
            const float sample = value(x, y);
            const size_t index = (static_cast<size_t>(y) * width + x) * 4;
            pixels[index + 0] = sample;
            pixels[index + 1] = sample;
            pixels[index + 2] = sample;
            pixels[index + 3] = 1.0f;
        }
    }
    return image;
}

// 覆盖页的层分布由高度带决定：这里用左右两半不同高度，保证同一页内
// 会出现两个可见层，用来验证 top-4 -> 可见层推导。
VL::TerrainBuildPlan MakeTwoLayerPlan(uint32_t pageTilesPerSide = 1)
{
    VL::TerrainBuildPlan plan;
    plan.desc.assetPath = "Generated/Validation/TerrainTests/TR_visibility.json";
    plan.desc.name = "TR_visibility";
    plan.desc.size = Eigen::Vector2f(1000.0f, 1000.0f);
    plan.desc.heightScale = 100.0f;
    plan.desc.gridResolution = 16;
    plan.desc.chunkQuads = 8;
    plan.desc.pageOrigin = Eigen::Vector2i(0, 0);
    plan.desc.pageTilesPerSide = pageTilesPerSide;
    plan.desc.layers = {
        {"low", 0.0f, 0.45f, 0.0f, 90.0f, 0.0f},
        {"high", 0.55f, 1.0f, 0.0f, 90.0f, 0.0f}};
    plan.heightmap = std::make_shared<const HostImage>(MakeHeightmap(
        16, 16, [](uint32_t x, uint32_t) {
            return x < 8 ? 0.2f : 0.8f;
        }));
    plan.placementId = "terrain|Visibility";
    return plan;
}

VL::TerrainLayerVisibilityConfig MakeConfig(uint32_t layerCount)
{
    VL::TerrainLayerVisibilityConfig config;
    config.layerCount = layerCount;
    config.sourceMipLevels = 3;
    config.maxRequestsPerFrame = 64;
    config.maxCoverPageRebuildsPerFrame = 4;
    config.idleEvictionEpochs = 4;
    config.unselectedMipIdleEvictionEpochs = 4;
    return config;
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

std::vector<VL::TerrainLayerDesc> MakeLayerDesc(uint32_t count)
{
    std::vector<VL::TerrainLayerDesc> layers;
    for (uint32_t index = 0; index < count; ++index)
    {
        VL::TerrainLayerDesc layer;
        layer.name = "layer" + std::to_string(index);
        layer.sourceResolution = 512;
        layers.push_back(layer);
    }
    return layers;
}

} // namespace

// 覆盖页优先级 -> 层 mip 的阶梯必须与设计一致：近景细 mip，远景粗 mip。
TEST(TerrainLayerVisibility, MapsCoverPagePriorityToMipLadder)
{
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(255), 0u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(192), 0u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(191), 1u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(128), 1u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(127), 2u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(64), 2u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(63), 3u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(32), 3u);
    // 2K 的 mip4 与 4K/8K 的尾页（mip5/6）都必须能请求到，不能停在 mip3。
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(31), 4u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(0), 6u);
}

// 低优先级尾段继续细分：16..31 -> mip4，8..15 -> mip5，0..7 -> mip6。
TEST(TerrainLayerVisibility, MapsLowPriorityTailToMipFourThroughSix)
{
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(31), 4u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(15), 5u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(8), 5u);
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(7), 6u);
}

// 一个覆盖页有 top-4 层时，每个可见层都要请求该 mip 的**全部**页；未注册
// 的虚拟页必须跳过而不是抛错。
TEST(TerrainLayerVisibility, RequestsEveryPageOfEachVisibleLayerMip)
{
    VL::TerrainPageManager coverPages;
    VL::TerrainBuildPlan plan = MakeTwoLayerPlan();
    coverPages.RegisterTerrain(plan, false);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(2));
    layerPages.SetGpuEpochContext(1, 0, 2);

    VL::TerrainFeedbackFrameSummary summary;
    summary.copiedEpoch = 10;
    summary.pages = {
        {0, 0, 255, false, 16},
        // 未注册页（例如同 World 的 M_pbr 地形）只跳过。
        {31, 31, 255, false, 4}};

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    const VL::TerrainLayerVisibilityStats stats = resolver.Apply(
        summary,
        coverPages,
        layerPages,
        pending,
        10,
        10,
        MakeConfig(2));

    EXPECT_EQ(stats.coverPages, 2u);
    EXPECT_EQ(stats.skippedCoverPages, 1u);
    EXPECT_EQ(stats.resolvedCoverPages, 1u);
    EXPECT_EQ(stats.rebuiltCoverPages, 1u);
    EXPECT_EQ(stats.visibleLayers, 2u);
    // 512² 层 mip0 是 4×4=16 页；两个可见层共 32 个目标页。L4c 再为缺页
    // 补 mip1（4 页/层）与 mip2（1 页/层），共 32+8+2=42。
    EXPECT_EQ(stats.candidatePages, 42u);
    EXPECT_EQ(stats.acceptedRequests, 42u);
    EXPECT_EQ(stats.mipRequestCounts[0], 32u);
    EXPECT_EQ(stats.mipRequestCounts[1], 8u);
    EXPECT_EQ(stats.mipRequestCounts[2], 2u);
    EXPECT_EQ(stats.fallbackTargetPages, 32u);
    EXPECT_TRUE(pending.empty());

    // 同一层在多个覆盖页重复出现时，候选页必须去重。
    summary.copiedEpoch = 11;
    summary.pages = {{0, 0, 255, false, 16}};
    const VL::TerrainLayerVisibilityStats repeated = resolver.Apply(
        summary, coverPages, layerPages, pending, 11, 11, MakeConfig(2));
    EXPECT_EQ(repeated.candidatePages, 32u);
    EXPECT_EQ(repeated.rebuiltCoverPages, 0u); // 页层集合已缓存。
}

// 预算不足时按 priority 排序、每帧限量；deferred 轮转让所有页最终都能请求。
TEST(TerrainLayerVisibility, HonorsPerFrameBudgetAndConverges)
{
    VL::TerrainPageManager coverPages;
    VL::TerrainBuildPlan plan = MakeTwoLayerPlan();
    coverPages.RegisterTerrain(plan, false);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(2));
    layerPages.SetGpuEpochContext(1, 0, 2);
    layerPages.SetResidentCapacity(512);

    VL::TerrainFeedbackFrameSummary summary;
    summary.pages = {{0, 0, 255, false, 16}};

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(2);
    config.maxRequestsPerFrame = 5;

    uint32_t totalAccepted = 0;
    for (uint64_t frame = 0; frame < 20; ++frame)
    {
        summary.copiedEpoch = frame + 10;
        // 生产路径每帧都会把“本帧提交 epoch”写回 layer page manager；新驻留页
        // 以它为安全窗起点，否则会被 idle 淘汰当成过期页。
        layerPages.SetGpuEpochContext(frame + 10, frame + 10, 2);
        const VL::TerrainLayerVisibilityStats stats = resolver.Apply(
            summary, coverPages, layerPages, pending, frame, frame + 10, config);
        EXPECT_LE(stats.attemptedRequests, 5u);
        totalAccepted += stats.acceptedRequests;
        if (pending.empty()) break;
    }
    // 42 页 / 每帧 5 个 = 9 帧左右收敛（目标页先来，粗页随后）。
    EXPECT_EQ(totalAccepted, 42u);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(layerPages.ResidentCount(), 42u);
}

// 远 mip 的请求必须 clamp 到层自身 mip 链与源数组 mip 数。
TEST(TerrainLayerVisibility, ClampsMipToLayerChainAndSourceLevels)
{
    VL::TerrainPageManager coverPages;
    VL::TerrainBuildPlan plan = MakeTwoLayerPlan();
    coverPages.RegisterTerrain(plan, false);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(2));
    layerPages.SetGpuEpochContext(1, 0, 2);

    VL::TerrainFeedbackFrameSummary summary;
    summary.pages = {{0, 0, 0, false, 16}};

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(2);
    // 源数组只有 2 级 mip；priority 0 想取 mip3，必须 clamp 到 mip1。
    config.sourceMipLevels = 2;

    const VL::TerrainLayerVisibilityStats stats = resolver.Apply(
        summary, coverPages, layerPages, pending, 0, 10, config);
    EXPECT_EQ(stats.mipRequestCounts[0], 0u);
    // 512² mip1 是 2×2=4 页/层，两层共 8 页。
    EXPECT_EQ(stats.mipRequestCounts[1], 8u);
    EXPECT_EQ(stats.acceptedRequests, 8u);
    EXPECT_TRUE(pending.empty());

    // layerCount 之外（未绑定）的层号不能进入请求。
    summary.pages = {{0, 0, 255, false, 16}};
    const VL::TerrainLayerVisibilityStats limited = resolver.Apply(
        summary, coverPages, layerPages, pending, 1, 11, MakeConfig(1));
    EXPECT_EQ(limited.visibleLayers, 1u);
    for (const VL::TerrainLayerPageRequest& request : pending)
        EXPECT_EQ(request.key.layerId, 0u);
}

// 覆盖页离开视野后，层页必须在安全窗外被 idle 淘汰，不能只增不减。
TEST(TerrainLayerVisibility, EvictsLayerPagesAfterLeavingView)
{
    VL::TerrainPageManager coverPages;
    VL::TerrainBuildPlan plan = MakeTwoLayerPlan();
    coverPages.RegisterTerrain(plan, false);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(10, 10, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.idleEvictionEpochs = 4;

    VL::TerrainFeedbackFrameSummary summary;
    summary.copiedEpoch = 10;
    summary.pages = {{0, 0, 255, false, 16}};
    resolver.Apply(summary, coverPages, layerPages, pending, 0, 10, config);
    ASSERT_GT(layerPages.ResidentCount(), 0u);
    const uint32_t residentBefore = layerPages.ResidentCount();

    // 空 feedback：可见层消失。安全窗 = max(2, framesInFlight+1) = 3，
    // idle 窗 = 4，因此 epoch 前进 8 后允许淘汰。
    summary.pages.clear();
    uint32_t totalEvicted = 0;
    for (uint64_t epoch = 11; epoch <= 24; ++epoch)
    {
        summary.copiedEpoch = epoch;
        const VL::TerrainLayerVisibilityStats stats = resolver.Apply(
            summary, coverPages, layerPages, pending, epoch, epoch, config);
        totalEvicted += stats.evictedPages;
        EXPECT_EQ(stats.acceptedRequests, 0u);
    }
    EXPECT_GT(totalEvicted, 0u);
    EXPECT_LT(layerPages.ResidentCount(), residentBefore);
}

// 常驻覆盖页走 CPU 镜像；不允许重建。
TEST(TerrainLayerVisibility, ResidentCoverPageSkipsRebuild)
{
    VL::TerrainPageManager coverPages;
    VL::TerrainBuildPlan plan = MakeTwoLayerPlan();
    const std::string placement = VL::TerrainPlacementIdentity(plan);
    coverPages.RegisterTerrain(plan, false);
    ASSERT_TRUE(coverPages.RequestPage(placement, 0, 0, 1));

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(2));
    layerPages.SetGpuEpochContext(1, 0, 2);

    VL::TerrainFeedbackFrameSummary summary;
    summary.copiedEpoch = 5;
    summary.pages = {{0, 0, 255, false, 16}};

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    const VL::TerrainLayerVisibilityStats stats = resolver.Apply(
        summary, coverPages, layerPages, pending, 0, 5, MakeConfig(2));
    EXPECT_EQ(stats.resolvedCoverPages, 1u);
    EXPECT_EQ(stats.rebuiltCoverPages, 0u);
    EXPECT_GT(stats.candidatePages, 0u);
}

// L4a 验收 MEDIUM 3：2K 层的 mip4 只有 1 页，远景必须也能请求到；
// 旧阶梯把上限钉在 mip3，导致远景永远停在 avgColor。
TEST(TerrainLayerVisibility, RequestsMip4ForDistantLayers)
{
    VL::TerrainPageManager coverPages;
    VL::TerrainBuildPlan plan = MakeTwoLayerPlan();
    coverPages.RegisterTerrain(plan, false);

    std::vector<VL::TerrainLayerDesc> layers = MakeLayerDesc(2);
    for (VL::TerrainLayerDesc& layer : layers)
        layer.sourceResolution = 2048;
    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(layers);
    layerPages.SetGpuEpochContext(1, 0, 2);

    VL::TerrainFeedbackFrameSummary summary;
    summary.copiedEpoch = 6;
    // priority 0 = 最远；2K 层 mip4 是 1 页/层。
    summary.pages = {{0, 0, 0, false, 16}};

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(2);
    config.sourceMipLevels = 5;
    const VL::TerrainLayerVisibilityStats stats = resolver.Apply(
        summary, coverPages, layerPages, pending, 0, 6, config);

    EXPECT_EQ(stats.mipRequestCounts[4], 2u);
    EXPECT_EQ(stats.acceptedRequests, 2u);
    EXPECT_TRUE(layerPages.IsResident(Key(0, 4, 0, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(1, 4, 0, 0)));
}

// L4a 验收 MEDIUM 1：不能把“可见层所有已驻留 mip”每帧续命，否则 capacity
// 对可见集完全无效。只有本帧 feedback 真正命中的页保持新鲜，其余走短 idle。
TEST(TerrainLayerVisibility, CapacityReclaimsUnselectedMipPages)
{
    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetResidentCapacity(512);
    layerPages.SetGpuEpochContext(100, 100, 2);

    // 预驻留 mip0 全部 16 页 + mip2 的 1 页。
    for (uint32_t pageY = 0; pageY < 4; ++pageY)
        for (uint32_t pageX = 0; pageX < 4; ++pageX)
            ASSERT_TRUE(layerPages.RequestPage(Key(0, 0, pageX, pageY)));
    ASSERT_TRUE(layerPages.RequestPage(Key(0, 2, 0, 0)));
    ASSERT_EQ(layerPages.ResidentCount(), 17u);

    std::vector<uint8_t> buffer(VL::TerrainLayerFeedbackBufferBytes, 0);
    VL::TerrainLayerFeedbackTexel texel;
    texel.layerId = 0;
    texel.mip = 0;
    texel.pageIndex = 0;
    texel.valid = true;
    texel.frameTag = 21;
    VL::EncodeTerrainLayerFeedbackTexel(
        texel, buffer.data());

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.idleEvictionEpochs = 4;
    config.unselectedMipIdleEvictionEpochs = 2;
    config.maxRequestsPerFrame = 0;

    uint64_t epoch = 100;
    for (uint32_t iteration = 0; iteration < 12; ++iteration)
    {
        ++epoch;
        layerPages.SetGpuEpochContext(epoch, epoch, 2);
        layerPages.SetResidentCapacityAtEpoch(4, epoch);
        VL::TerrainLayerFeedbackFrameSummary summary =
            VL::SummarizeTerrainLayerFeedback(buffer.data(), buffer.size(), 21);
        summary.copiedEpoch = epoch;
        resolver.ApplyExact(summary, layerPages, pending, epoch, epoch, config);
    }

    // 唯一被 feedback 命中的 mip0 page0 必须存活；其余 16 页必须被回收。
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 0, 0)));
    EXPECT_FALSE(layerPages.IsResident(Key(0, 2, 0, 0)));
    EXPECT_LE(layerPages.ResidentCount(), 4u);
}

namespace
{

// 4 张 128×128 RGBA8 层页 feedback，写一个 (layer,mip,pageIndex) texel。
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

VL::TerrainLayerFeedbackFrameSummary SummarizeLayerFeedback(
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

// L4c：目标 mip 页缺页时，同一 UV 区域的更粗 1–2 级 mip 页必须一起进候选，
// 否则 shader 回退到粗 mip 后仍然没有页可采，只能显示 avgColor。
TEST(TerrainLayerVisibility, PrefetchesCoarserMipsWhenTargetPageMissing)
{
    // 512² 层 mip0 是 4×4；pageIndex 5 == (pageX=1, pageY=1)。
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 0, 5, 8);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.exactFeedbackNeighborhoodRadius = 0;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            SummarizeLayerFeedback(buffer, 8, 10),
            layerPages,
            pending,
            10,
            10,
            config);

    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 1, 1)));
    // +1 级 mip1 (1>>1=0, 1>>1=0)，+2 级 mip2 只有 1 页。
    EXPECT_TRUE(layerPages.IsResident(Key(0, 1, 0, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 2, 0, 0)));
    EXPECT_EQ(stats.fallbackTargetPages, 1u);
    EXPECT_EQ(stats.fallbackLevelPages[0], 1u);
    EXPECT_EQ(stats.fallbackLevelPages[1], 1u);
    EXPECT_TRUE(pending.empty());

    // 请求名额必须先给目标 mip，粗 mip 只做兜底。
    const VL::TerrainLayerPageManager::PageState* target =
        layerPages.FindPage(Key(0, 0, 1, 1));
    const VL::TerrainLayerPageManager::PageState* coarse =
        layerPages.FindPage(Key(0, 1, 0, 0));
    ASSERT_NE(target, nullptr);
    ASSERT_NE(coarse, nullptr);
    EXPECT_GT(target->priority, coarse->priority);
}

// 目标页已驻留时不能再扩粗 mip：否则候选集会凭空变大，capacity 被
// “假可见”页撑爆。
TEST(TerrainLayerVisibility, SkipsCoarserPrefetchWhenTargetResident)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 0, 5, 4);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(20, 0, 2);
    ASSERT_TRUE(layerPages.RequestPageAtEpoch(
        Key(0, 0, 1, 1), 1, 60, 20));

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.exactFeedbackNeighborhoodRadius = 0;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            SummarizeLayerFeedback(buffer, 4, 24),
            layerPages,
            pending,
            24,
            24,
            config);

    EXPECT_EQ(stats.candidatePages, 1u);
    EXPECT_EQ(stats.markedPages, 1u);
    EXPECT_EQ(stats.acceptedRequests, 0u);
    EXPECT_EQ(stats.fallbackTargetPages, 0u);
    EXPECT_EQ(stats.fallbackLevelPages[0], 0u);
    EXPECT_EQ(stats.fallbackLevelPages[1], 0u);
    EXPECT_FALSE(layerPages.IsResident(Key(0, 1, 0, 0)));
    EXPECT_TRUE(pending.empty());
}

// 粗 mip 预取只能排在目标页之后使用同一份请求预算，不能绕过每帧上限。
// 粗 mip 页是预取，不是本帧可见页；它已驻留时不能把 feedbackHits 抬高。
TEST(TerrainLayerVisibility, CoarserPrefetchDoesNotCountAsFeedbackHit)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 0, 5, 4);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(20, 0, 2);
    // 只让粗 mip1 页驻留，目标 mip0 页仍缺页。
    ASSERT_TRUE(layerPages.RequestPageAtEpoch(Key(0, 1, 0, 0), 1, 60, 20));

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.exactFeedbackNeighborhoodRadius = 0;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            SummarizeLayerFeedback(buffer, 4, 24),
            layerPages,
            pending,
            24,
            24,
            config);

    EXPECT_EQ(stats.markedPages, 0u);
    EXPECT_EQ(stats.fallbackLevelPages[0], 1u);
}

// fallbackLevelPages 只有 3 个槽位；过大的配置必须被夹住而不是越界写。
TEST(TerrainLayerVisibility, CoarserPrefetchLevelCountIsClamped)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 0, 0, 8);

    VL::TerrainLayerPageManager layerPages;
    std::vector<VL::TerrainLayerDesc> layers = MakeLayerDesc(1);
    // 2048² 有 mip0..4；配置请求 16 级时必须被夹到统计数组的 3 项。
    layers[0].sourceResolution = 2048;
    layerPages.ConfigureLayers(layers);
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.sourceMipLevels = 5;
    config.exactFeedbackNeighborhoodRadius = 0;
    config.fallbackPrefetchMipLevels = 16;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            SummarizeLayerFeedback(buffer, 8, 10),
            layerPages,
            pending,
            10,
            10,
            config);

    EXPECT_EQ(stats.fallbackLevelPages.size(), 3u);
    EXPECT_LE(stats.fallbackLevelPages[0] + stats.fallbackLevelPages[1] +
        stats.fallbackLevelPages[2], 3u);
}

TEST(TerrainLayerVisibility, CoarserPrefetchStaysWithinRequestBudget)
{
    const std::vector<uint8_t> buffer = MakeLayerFeedback(0, 0, 5, 6);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(30, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.exactFeedbackNeighborhoodRadius = 0;
    config.maxRequestsPerFrame = 1;
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            SummarizeLayerFeedback(buffer, 6, 30),
            layerPages,
            pending,
            30,
            30,
            config);

    // 3 个候选（目标 + mip1 + mip2）但每帧只允许 1 次请求。
    EXPECT_EQ(stats.candidatePages, 3u);
    EXPECT_EQ(stats.attemptedRequests, 1u);
    EXPECT_EQ(stats.acceptedRequests, 1u);
    EXPECT_EQ(pending.size(), 2u);
    // 第一帧必须优先请求目标 mip 页。
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 1, 1)));
    EXPECT_FALSE(layerPages.IsResident(Key(0, 1, 0, 0)));
}

namespace
{

// 预驻留 512² 层 mip0 的全部 16 页，用于饥饿场景。
void FillLayerMip0(VL::TerrainLayerPageManager& layerPages)
{
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetResidentCapacity(512);
    layerPages.SetGpuEpochContext(100, 100, 2);
    for (uint32_t pageY = 0; pageY < 4; ++pageY)
    {
        for (uint32_t pageX = 0; pageX < 4; ++pageX)
            ASSERT_TRUE(layerPages.RequestPage(Key(0, 0, pageX, pageY)));
    }
}

// 全零 feedback（tag 不匹配/未写出）就是精确路径的“饥饿帧”。
VL::TerrainLayerFeedbackFrameSummary EmptyLayerFeedback(uint64_t epoch)
{
    VL::TerrainLayerFeedbackFrameSummary summary;
    summary.copiedEpoch = epoch;
    summary.frameTag = 1;
    return summary;
}

} // namespace

// 精确 feedback 整帧没有有效 texel 时，不能按 idle 阈值清空驻留页；也不能
// 把页的 lastSeen 伪造成当前帧（那会让“没看见”变成“刚看见”）。
TEST(TerrainLayerVisibility, EmptyExactFeedbackPreservesResidency)
{
    VL::TerrainLayerPageManager layerPages;
    FillLayerMip0(layerPages);
    ASSERT_EQ(layerPages.ResidentCount(), 16u);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    // idle 窗口 4：没有饥饿保护时 5 个 epoch 就会把 mip0 全部清掉。
    config.idleEvictionEpochs = 4;
    config.starvationGraceFrames = 480;

    // 先给一帧有效 feedback，建立 lastSeenEpoch=100 与 lastValidFeedbackEpoch。
    const std::vector<uint8_t> valid = MakeLayerFeedback(0, 0, 0, 9);
    const VL::TerrainLayerExactFeedbackStats applied =
        resolver.ApplyExact(
            SummarizeLayerFeedback(valid, 9, 100),
            layerPages,
            pending,
            0,
            100,
            config);
    EXPECT_FALSE(applied.starved);
    EXPECT_EQ(applied.markedPages, 1u);
    ASSERT_EQ(resolver.FeedbackHealth().lastValidFeedbackEpoch, 100u);

    for (uint64_t frame = 1; frame <= 30; ++frame)
    {
        const uint64_t epoch = 100 + frame;
        layerPages.SetGpuEpochContext(epoch, epoch, 2);
        const VL::TerrainLayerExactFeedbackStats stats =
            resolver.ApplyExact(
                EmptyLayerFeedback(epoch),
                layerPages,
                pending,
                frame,
                epoch,
                config);
        EXPECT_TRUE(stats.starved);
        EXPECT_EQ(stats.evictedPages, 0u);
        EXPECT_EQ(stats.acceptedRequests, 0u);
        EXPECT_EQ(layerPages.ResidentCount(), 16u);
    }

    const VL::TerrainLayerFeedbackHealth& health =
        resolver.FeedbackHealth();
    EXPECT_EQ(health.starvedFrames, 30u);
    EXPECT_EQ(health.lastValidFeedbackEpoch, 100u);
    // 补偿随饥饿帧数增长（封顶前 1:1），idle 时钟因此不会推进。
    EXPECT_EQ(health.starvationCreditEpochs, 30u);

    const VL::TerrainLayerPageManager::PageState* page =
        layerPages.FindPage(Key(0, 0, 0, 0));
    ASSERT_NE(page, nullptr);
    EXPECT_EQ(page->lastSeenEpoch, 100u);
}

// 饥饿时调用方要回退到覆盖页推导，并把它标记成 usedCoverFallback；
// 可见层估计仍然来自覆盖页 top-4，不受精确 feedback 断流影响。
TEST(TerrainLayerVisibility, CoverFallbackEstimatesLayersWhileStarved)
{
    VL::TerrainPageManager coverPages;
    VL::TerrainBuildPlan plan = MakeTwoLayerPlan();
    coverPages.RegisterTerrain(plan, false);

    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(2));
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(2);
    // 只验证估计，不发请求，避免候选页掩盖“可见层来自回退”这一点。
    config.maxRequestsPerFrame = 0;

    ASSERT_TRUE(
        resolver
            .ApplyExact(EmptyLayerFeedback(10), layerPages, pending, 0, 10, config)
            .starved);

    VL::TerrainFeedbackFrameSummary cover;
    cover.copiedEpoch = 11;
    cover.pages = {{0, 0, 255, false, 16}};
    const VL::TerrainLayerVisibilityStats stats = resolver.Apply(
        cover, coverPages, layerPages, pending, 1, 11, config);

    EXPECT_TRUE(stats.usedCoverFallback);
    EXPECT_EQ(stats.resolvedCoverPages, 1u);
    EXPECT_EQ(stats.visibleLayers, 2u);
    EXPECT_EQ(stats.evictedPages, 0u);

    // 覆盖页推导在饥饿期间也不能提前清空已驻留页。
    for (uint64_t frame = 2; frame <= 20; ++frame)
    {
        const uint64_t epoch = 9 + frame;
        layerPages.SetGpuEpochContext(epoch, epoch, 2);
        ASSERT_TRUE(
            resolver
                .ApplyExact(
                    EmptyLayerFeedback(epoch),
                    layerPages,
                    pending,
                    frame,
                    epoch,
                    config)
                .starved);
        cover.copiedEpoch = epoch;
        resolver.Apply(
            cover, coverPages, layerPages, pending, frame, epoch, config);
    }
    EXPECT_EQ(resolver.FeedbackHealth().starvedFrames, 20u);
}

// 精确 feedback 恢复后，饥饿计数清零、lastValidEpoch 前进，精确路径重新
// 接管（下一帧不再标记 usedCoverFallback）。
TEST(TerrainLayerVisibility, ExactFeedbackResumesAfterStarvation)
{
    VL::TerrainLayerPageManager layerPages;
    FillLayerMip0(layerPages);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.idleEvictionEpochs = 4;

    for (uint64_t frame = 0; frame < 12; ++frame)
    {
        const uint64_t epoch = 100 + frame;
        layerPages.SetGpuEpochContext(epoch, epoch, 2);
        ASSERT_TRUE(
            resolver
                .ApplyExact(
                    EmptyLayerFeedback(epoch),
                    layerPages,
                    pending,
                    frame,
                    epoch,
                    config)
                .starved);
    }
    ASSERT_EQ(resolver.FeedbackHealth().starvedFrames, 12u);
    ASSERT_EQ(layerPages.ResidentCount(), 16u);

    // 恢复：pageIndex 5 == (pageX=1, pageY=1)，且该页原本就驻留。
    const std::vector<uint8_t> valid = MakeLayerFeedback(0, 0, 5, 41);
    const VL::TerrainLayerExactFeedbackStats resumed = resolver.ApplyExact(
        SummarizeLayerFeedback(valid, 41, 112),
        layerPages,
        pending,
        12,
        112,
        config);

    EXPECT_FALSE(resumed.starved);
    EXPECT_EQ(resumed.markedPages, 1u);
    EXPECT_EQ(resolver.FeedbackHealth().starvedFrames, 0u);
    EXPECT_EQ(resolver.FeedbackHealth().lastValidFeedbackEpoch, 112u);
    // 恢复后补偿开始逐帧衰减，但这一帧仍保留大部分宽限窗口。
    EXPECT_EQ(resolver.FeedbackHealth().starvationCreditEpochs, 11u);
    EXPECT_EQ(layerPages.ResidentCount(), 16u);
}

// 饥饿超过宽限期后必须允许慢速降级：先保持完整驻留集，之后按 idle 窗口
// 逐帧推进淘汰，最终不会永久锁死显存。
TEST(TerrainLayerVisibility, StarvationBeyondGraceAllowsSlowDecay)
{
    VL::TerrainLayerPageManager layerPages;
    FillLayerMip0(layerPages);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.idleEvictionEpochs = 4;
    config.starvationGraceFrames = 6;
    config.starvationCreditDecayPerFrame = 1;

    // lastSeenEpoch=100、idle=4、补偿封顶 6：epoch 111 之前都不能淘汰。
    for (uint64_t frame = 1; frame <= 10; ++frame)
    {
        const uint64_t epoch = 100 + frame;
        layerPages.SetGpuEpochContext(epoch, epoch, 2);
        const VL::TerrainLayerExactFeedbackStats stats =
            resolver.ApplyExact(
                EmptyLayerFeedback(epoch),
                layerPages,
                pending,
                frame,
                epoch,
                config);
        EXPECT_TRUE(stats.starved);
        EXPECT_EQ(stats.evictedPages, 0u);
        EXPECT_EQ(layerPages.ResidentCount(), 16u);
    }
    EXPECT_EQ(resolver.FeedbackHealth().starvationCreditEpochs, 6u);

    // 超过宽限窗口后恢复老化，但按 starvationEvictionPagesPerFrame（默认 2）
    // 限速：不能因为所有页同时越过 idle 阈值而一次性清空。
    uint64_t frame = 10;
    uint64_t epoch = 110;
    ++frame;
    ++epoch;
    layerPages.SetGpuEpochContext(epoch, epoch, 2);
    const VL::TerrainLayerExactFeedbackStats firstDecay =
        resolver.ApplyExact(
            EmptyLayerFeedback(epoch),
            layerPages,
            pending,
            frame,
            epoch,
            config);
    EXPECT_TRUE(firstDecay.starved);
    EXPECT_EQ(firstDecay.evictedPages, 2u);
    EXPECT_EQ(layerPages.ResidentCount(), 14u);

    // 持续饥饿会让驻留集继续按每帧 2 页降级，最终清空而不是永久保留。
    for (uint32_t step = 0; step < 10; ++step)
    {
        ++frame;
        ++epoch;
        layerPages.SetGpuEpochContext(epoch, epoch, 2);
        resolver.ApplyExact(
            EmptyLayerFeedback(epoch),
            layerPages,
            pending,
            frame,
            epoch,
            config);
        if (layerPages.ResidentCount() == 0) break;
    }
    EXPECT_EQ(layerPages.ResidentCount(), 0u);
}

// 预算路径的核心不变量：选中的 (layer, mip) 必须请求该 mip 的全部页。
// feedback 只命中 1 页，但 shader 的平铺 UV 可能落到任意页。
TEST(TerrainLayerVisibility, BudgetExactFeedbackRequestsWholeSelectedMip)
{
    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.layerPageBudget = 512;
    config.exactFeedbackNeighborhoodRadius = 0;

    VL::TerrainLayerFeedbackFrameSummary summary;
    summary.copiedEpoch = 10;
    summary.validTexelCount = 10;
    summary.pages = {{0, 0, 5, 10, 255}};
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            summary, layerPages, pending, 10, 10, config);

    EXPECT_EQ(stats.budgetTotalPages, 17u);
    EXPECT_EQ(stats.budgetAllocatedSets, 2u);
    EXPECT_EQ(stats.budgetSetsByMip[0], 1u);
    EXPECT_EQ(stats.budgetSetsByMip[2], 1u);
    EXPECT_EQ(stats.candidatePages, 17u);
    EXPECT_EQ(stats.acceptedRequests, 17u);
    EXPECT_TRUE(pending.empty());
    for (uint32_t y = 0; y < 4; ++y)
    {
        for (uint32_t x = 0; x < 4; ++x)
            EXPECT_TRUE(layerPages.IsResident(Key(0, 0, x, y)));
    }
    // 最粗尾页作为 feedback 欠采样时的回退内容。
    EXPECT_TRUE(layerPages.IsResident(Key(0, 2, 0, 0)));
}

// 分配不能越过 feedback 目标：目标 mip2 时只驻留 4 页，不装 mip0 的 64 页。
TEST(TerrainLayerVisibility, BudgetExactFeedbackStopsAtTargetMip)
{
    std::vector<VL::TerrainLayerDesc> layers = MakeLayerDesc(1);
    layers[0].sourceResolution = 1024;
    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(layers);
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.sourceMipLevels = 4;
    config.layerPageBudget = 512;
    config.exactFeedbackNeighborhoodRadius = 0;

    VL::TerrainLayerFeedbackFrameSummary summary;
    summary.copiedEpoch = 10;
    summary.validTexelCount = 10;
    // 1K mip2 每边 2 页；pageIndex 0 是 (0,0)。
    summary.pages = {{0, 2, 0, 10, 255}};
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            summary, layerPages, pending, 10, 10, config);

    EXPECT_EQ(stats.budgetTotalPages, 5u);
    EXPECT_EQ(stats.budgetEntriesByMip[2], 1u);
    EXPECT_EQ(stats.budgetSetsByMip[2], 1u);
    EXPECT_EQ(stats.budgetSetsByMip[3], 1u);
    EXPECT_EQ(stats.mipRequestCounts[2], 4u);
    EXPECT_EQ(stats.mipRequestCounts[3], 1u);
    EXPECT_TRUE(layerPages.IsResident(Key(0, 2, 0, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 2, 1, 1)));
    EXPECT_FALSE(layerPages.IsResident(Key(0, 0, 0, 0)));
}

// 预算紧张时，近处需求先拿细 mip，远处需求停在粗 mip。
TEST(TerrainLayerVisibility, BudgetPrioritizesNearerFeedback)
{
    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(2));
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(2);
    // 512² mip0 是 16 页，另有 1 页最粗回退。20 页预算让近处层拿满
    // mip0（16+1 页），远处层停在最粗 mip2（1 页），仍然比近处粗。
    config.layerPageBudget = 20;
    config.maxRequestsPerFrame = 128;
    config.exactFeedbackNeighborhoodRadius = 0;

    VL::TerrainLayerFeedbackFrameSummary summary;
    summary.copiedEpoch = 10;
    summary.validTexelCount = 20;
    // layer 1 是近处（priority 255），layer 0 是远处（priority 64）。
    summary.pages = {
        {0, 0, 5, 10, 64},
        {1, 0, 5, 10, 255},
    };
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            summary, layerPages, pending, 10, 10, config);

    EXPECT_EQ(stats.budgetEntriesByMip[0], 1u);
    EXPECT_EQ(stats.budgetEntriesByMip[2], 1u);
    EXPECT_EQ(stats.budgetSetsByMip[0], 1u);
    EXPECT_EQ(stats.budgetSetsByMip[2], 2u);
    EXPECT_EQ(stats.mipRequestCounts[0], 16u);
    EXPECT_EQ(stats.mipRequestCounts[2], 2u);
    EXPECT_TRUE(layerPages.IsResident(Key(1, 0, 0, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 2, 0, 0)));
}

// 同一个全局层同时出现在近处和远处时，必须同时保留两个页集；只保留最细
// 一级会让远处像素采样到未请求的粗 mip 页。
TEST(TerrainLayerVisibility, BudgetKeepsNearAndFarMipsForSameLayer)
{
    std::vector<VL::TerrainLayerDesc> layers = MakeLayerDesc(1);
    layers[0].sourceResolution = 1024;
    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(layers);
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.sourceMipLevels = 4;
    config.layerPageBudget = 512;
    config.maxRequestsPerFrame = 128;
    config.exactFeedbackNeighborhoodRadius = 0;

    VL::TerrainLayerFeedbackFrameSummary summary;
    summary.copiedEpoch = 10;
    summary.validTexelCount = 20;
    // 同一层的近处目标 mip0 + 远处目标 mip3。
    summary.pages = {
        {0, 0, 0, 10, 255},
        {0, 3, 0, 10, 32},
    };
    const VL::TerrainLayerExactFeedbackStats stats =
        resolver.ApplyExact(
            summary, layerPages, pending, 10, 10, config);

    EXPECT_EQ(stats.budgetTotalPages, 65u);
    EXPECT_EQ(stats.budgetAllocatedSets, 2u);
    EXPECT_EQ(stats.budgetEntriesByMip[0], 1u);
    EXPECT_EQ(stats.budgetEntriesByMip[3], 1u);
    EXPECT_EQ(stats.mipRequestCounts[0], 64u);
    EXPECT_EQ(stats.mipRequestCounts[3], 1u);
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 0, 0)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 0, 7, 7)));
    EXPECT_TRUE(layerPages.IsResident(Key(0, 3, 0, 0)));
}

// 预算与容量相等时，选中页集必须稳定驻留：后续帧不能出现
// “请求 -> 淘汰 -> 再请求”的追尾。
TEST(TerrainLayerVisibility, BudgetSelectedWorkingSetStaysStable)
{
    VL::TerrainLayerPageManager layerPages;
    layerPages.ConfigureLayers(MakeLayerDesc(1));
    layerPages.SetResidentCapacity(17);
    layerPages.SetGpuEpochContext(10, 0, 2);

    VL::TerrainLayerVisibilityResolver resolver;
    std::vector<VL::TerrainLayerPageRequest> pending;
    VL::TerrainLayerVisibilityConfig config = MakeConfig(1);
    config.layerPageBudget = 17;
    config.maxRequestsPerFrame = 64;
    config.exactFeedbackNeighborhoodRadius = 0;

    VL::TerrainLayerFeedbackFrameSummary summary;
    summary.validTexelCount = 10;
    summary.pages = {{0, 0, 5, 10, 255}};
    for (uint64_t frame = 0; frame < 8; ++frame)
    {
        const uint64_t epoch = 10 + frame;
        summary.copiedEpoch = epoch;
        layerPages.SetGpuEpochContext(epoch, epoch, 2);
        const VL::TerrainLayerExactFeedbackStats stats =
            resolver.ApplyExact(
                summary, layerPages, pending, frame, epoch, config);
        EXPECT_EQ(layerPages.ResidentCount(), 17u);
        EXPECT_TRUE(pending.empty());
        if (frame == 0)
        {
            EXPECT_EQ(stats.acceptedRequests, 17u);
        }
        else
        {
            EXPECT_EQ(stats.acceptedRequests, 0u);
            EXPECT_EQ(stats.evictedPages, 0u);
        }
    }
}
