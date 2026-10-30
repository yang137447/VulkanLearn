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
    return config;
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
    EXPECT_EQ(VL::TerrainLayerVisibilityResolver::MipForPriority(0), 3u);
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
    // 512² 层 mip0 是 4×4=16 页；两个可见层共 32 页候选。
    EXPECT_EQ(stats.candidatePages, 32u);
    EXPECT_EQ(stats.acceptedRequests, 32u);
    EXPECT_EQ(stats.mipRequestCounts[0], 32u);
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
    // 32 页 / 每帧 5 个 = 7 帧左右收敛。
    EXPECT_EQ(totalAccepted, 32u);
    EXPECT_TRUE(pending.empty());
    EXPECT_EQ(layerPages.ResidentCount(), 32u);
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
