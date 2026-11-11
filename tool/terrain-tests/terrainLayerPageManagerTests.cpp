#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "resource/image/hostImage.h"
#include "render/resource/rendererResourceLoadCoordinator.h"
#include "terrain/terrainAssetTypes.h"
#include "terrain/terrainLayerPageManager.h"
#include "terrain/terrainLayerTable.h"
#include "terrain/terrainLayerVT.h"

namespace
{

VL::TerrainLayerPageKey PageKey(
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

VL::TerrainLayerDesc MakeLayer(uint32_t sourceResolution)
{
    VL::TerrainLayerDesc layer;
    layer.name = "layer";
    layer.sourceResolution = sourceResolution;
    return layer;
}

constexpr uint64_t kNoWindowEpoch = std::numeric_limits<uint64_t>::max();

} // namespace

TEST(TerrainLayerTableV2, PacksCatalogRowsAndRoundTripsUnusedLayers)
{
    std::vector<VL::TerrainLayerDesc> layers(3);
    layers[0].name = "sand";
    layers[0].heightMin = 0.0f;
    layers[0].heightMax = 0.2f;
    layers[0].slopeMin = 0.0f;
    layers[0].slopeMax = 15.0f;
    layers[0].noise = 0.1f;
    layers[0].uvScale = 4.0f;
    layers[0].uvRotation = 90.0f;
    layers[0].sourceResolution = 512;
    layers[0].avgColor = Eigen::Vector3f(0.8f, 0.7f, 0.5f);
    layers[0].sourceSet = 1;
    layers[0].flags = 0x11;
    layers[1].name = "grass";
    layers[1].heightMin = 0.2f;
    layers[1].heightMax = 0.8f;
    layers[1].sourceResolution = 1024;
    layers[1].avgColor = Eigen::Vector3f(0.1f, 0.4f, 0.1f);
    layers[1].sourceSet = 2;
    layers[1].flags = 0x22;
    layers[2].name = "rock";
    layers[2].heightMin = 0.8f;
    layers[2].heightMax = 1.0f;
    layers[2].sourceResolution = 2048;
    layers[2].sourceSet = 3;
    layers[2].flags = 0x33;

    const HostImage image = VL::TerrainLayerTable::BuildImage(layers);
    ASSERT_EQ(image.width, VL::TerrainLayerTable::MaxLayers);
    ASSERT_EQ(image.height, 4u);
    ASSERT_EQ(image.format, HostImage::PixelFormat::RGBA32_FLOAT);

    // row2：512 -> 4 页/边（21 页/层），1024 -> 8（85），2048 -> 16（341）。
    const float* pixels = reinterpret_cast<const float*>(image.data.data());
    const size_t row2Layer0 = static_cast<size_t>(256) * 8 + 0;
    const size_t row2Layer1 = static_cast<size_t>(256) * 8 + 4;
    const size_t row2Layer2 = static_cast<size_t>(256) * 8 + 8;
    EXPECT_FLOAT_EQ(pixels[row2Layer0 + 0], 4.0f);
    EXPECT_FLOAT_EQ(pixels[row2Layer0 + 1], 0.0f);
    EXPECT_FLOAT_EQ(pixels[row2Layer0 + 2], 1.0f);
    EXPECT_FLOAT_EQ(pixels[row2Layer0 + 3], 17.0f);
    EXPECT_FLOAT_EQ(pixels[row2Layer1 + 0], 8.0f);
    EXPECT_FLOAT_EQ(pixels[row2Layer1 + 1], 21.0f);
    EXPECT_FLOAT_EQ(pixels[row2Layer2 + 0], 16.0f);
    EXPECT_FLOAT_EQ(pixels[row2Layer2 + 1], 21.0f + 85.0f);

    // row3：avgColor 逐通道 + reserved=0。
    const size_t row3Layer0 = static_cast<size_t>(256) * 12 + 0;
    EXPECT_FLOAT_EQ(pixels[row3Layer0 + 0], 0.8f);
    EXPECT_FLOAT_EQ(pixels[row3Layer0 + 1], 0.7f);
    EXPECT_FLOAT_EQ(pixels[row3Layer0 + 2], 0.5f);
    EXPECT_FLOAT_EQ(pixels[row3Layer0 + 3], 0.0f);

    // CPU accessor 往返。
    const VL::TerrainLayerCatalogEntry first =
        VL::TerrainLayerTable::ReadLayer(image, 0);
    EXPECT_FLOAT_EQ(first.heightMin, 0.0f);
    EXPECT_FLOAT_EQ(first.heightMax, 0.2f);
    EXPECT_FLOAT_EQ(first.slopeMax, 15.0f);
    EXPECT_FLOAT_EQ(first.noise, 0.1f);
    EXPECT_FLOAT_EQ(first.uvScale, 4.0f);
    EXPECT_FLOAT_EQ(first.uvRotationRadians, 1.5707964f);
    EXPECT_EQ(first.pagesPerSide, 4u);
    EXPECT_EQ(first.pageTableBase, 0u);
    EXPECT_EQ(first.sourceSet, 1u);
    EXPECT_EQ(first.flags, 0x11u);
    EXPECT_EQ(first.sourceResolution, 512u);
    EXPECT_FLOAT_EQ(first.avgColor.x(), 0.8f);

    const VL::TerrainLayerCatalogEntry second =
        VL::TerrainLayerTable::ReadLayer(image, 1);
    EXPECT_EQ(second.pageTableBase, 21u);
    EXPECT_EQ(second.sourceResolution, 1024u);
    const VL::TerrainLayerCatalogEntry third =
        VL::TerrainLayerTable::ReadLayer(image, 2);
    EXPECT_EQ(third.pageTableBase, 106u);
    EXPECT_EQ(third.pagesPerSide, 16u);
    EXPECT_EQ(third.sourceResolution, 2048u);

    // 未使用层整行填零（row0..row3 全零）。
    const VL::TerrainLayerCatalogEntry unused =
        VL::TerrainLayerTable::ReadLayer(image, 200);
    EXPECT_EQ(unused.pagesPerSide, 0u);
    EXPECT_EQ(unused.pageTableBase, 0u);
    EXPECT_EQ(unused.sourceResolution, 0u);
    EXPECT_FLOAT_EQ(unused.heightMax, 0.0f);
    EXPECT_FLOAT_EQ(unused.uvScale, 0.0f);
    EXPECT_FLOAT_EQ(unused.noise, 0.0f);
    EXPECT_FLOAT_EQ(unused.avgColor.x(), 0.0f);
    EXPECT_EQ(unused.flags, 0u);
    const size_t unusedRow1 = static_cast<size_t>(256) * 4 + 200 * 4;
    const size_t unusedRow3 = static_cast<size_t>(256) * 12 + 200 * 4;
    for (size_t channel = 0; channel < 4; ++channel)
    {
        EXPECT_FLOAT_EQ(pixels[unusedRow1 + channel], 0.0f);
        EXPECT_FLOAT_EQ(pixels[unusedRow3 + channel], 0.0f);
    }

    EXPECT_THROW(VL::TerrainLayerTable::ReadLayer(image, 256), std::exception);
}

TEST(TerrainLayerTableV2, RejectsUnsupportedSourceResolutionAndLayerCount)
{
    VL::TerrainLayerDesc bad = MakeLayer(1536);
    EXPECT_THROW(
        VL::TerrainLayerTable::BuildImage({bad}), std::exception);
    EXPECT_THROW(
        VL::TerrainLayerTable::BuildImage({MakeLayer(256)}), std::exception);

    std::vector<VL::TerrainLayerDesc> tooMany(257, MakeLayer(1024));
    EXPECT_THROW(VL::TerrainLayerTable::BuildImage(tooMany), std::exception);
    std::vector<VL::TerrainLayerDesc> maxLayers(256, MakeLayer(1024));
    EXPECT_NO_THROW(VL::TerrainLayerTable::BuildImage(maxLayers));
}

TEST(TerrainLayerPageSizing, MatchesDesignMipTable)
{
    // 512：mip0..2 = 4/2/1，页数 16+4+1 = 21。
    EXPECT_EQ(VL::TerrainLayerMipCount(512), 3u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(512, 0), 4u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(512, 1), 2u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(512, 2), 1u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(512, 3), 0u);
    EXPECT_EQ(VL::TerrainLayerTotalPages(512), 21u);

    // 1024：8/4/2/1，85 页/层。
    EXPECT_EQ(VL::TerrainLayerMipCount(1024), 4u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(1024, 0), 8u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(1024, 3), 1u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(1024, 4), 0u);
    EXPECT_EQ(VL::TerrainLayerTotalPages(1024), 85u);

    // 2048：16/8/4/2/1（mip4 是 128² 尾页），341 页/层。
    EXPECT_EQ(VL::TerrainLayerMipCount(2048), 5u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(2048, 0), 16u);
    EXPECT_EQ(VL::TerrainLayerPagesPerSide(2048, 4), 1u);
    EXPECT_EQ(VL::TerrainLayerPagesPerMip(2048, 0), 256u);
    EXPECT_EQ(VL::TerrainLayerTotalPages(2048), 341u);
    EXPECT_EQ(
        VL::TerrainLayerPageManager::TotalPagesForSource(2048), 341u);

    // 256 层 2K 的总项数与设计 §4.4 一致。
    EXPECT_EQ(256u * VL::TerrainLayerTotalPages(2048), 87296u);
}

TEST(TerrainLayerPageIndex, UsesPerMipAndFlatFormulas)
{
    VL::TerrainLayerPageManager manager;
    manager.ConfigureLayers({MakeLayer(1024), MakeLayer(2048)});

    // per-mip：layerId * pagesPerMip(mip) + y * side + x。
    EXPECT_EQ(manager.VirtualPageIndex(PageKey(0, 0, 0, 0)), 0u);
    EXPECT_EQ(manager.VirtualPageIndex(PageKey(0, 0, 7, 3)), 31u);
    // mip1 每层 16 页、每边 4 页：y=1,x=3 -> 1*4+3 = 7。
    EXPECT_EQ(manager.VirtualPageIndex(PageKey(0, 1, 3, 1)), 7u);
    EXPECT_EQ(manager.VirtualPageIndex(PageKey(1, 0, 15, 15)), 511u);
    EXPECT_EQ(manager.VirtualPageIndex(PageKey(1, 4, 0, 0)), 1u);

    // 扁平：layer 1 的 base 是 layer 0 的 85 页；mip4 再偏移 340。
    EXPECT_EQ(manager.PageTableBase(0), 0u);
    EXPECT_EQ(manager.PageTableBase(1), 85u);
    EXPECT_EQ(manager.FlatPageIndex(PageKey(1, 0, 0, 0)), 85u);
    EXPECT_EQ(manager.FlatPageIndex(PageKey(1, 4, 0, 0)), 425u);

    EXPECT_EQ(manager.PagesPerSide(0, 0), 8u);
    EXPECT_EQ(manager.PagesPerSide(1, 0), 16u);
    EXPECT_EQ(manager.TotalPagesForLayer(0), 85u);
    EXPECT_EQ(manager.TotalPagesForLayer(1), 341u);
    // 统一 stride 取该 mip 的最大页数：1K + 2K 时 mip0 为 256。
    EXPECT_EQ(manager.VirtualPagesPerLayer(0), 256u);
    EXPECT_EQ(manager.VirtualPagesPerLayer(4), 1u);
    EXPECT_EQ(manager.VirtualPageTableSize(0), 256u * 256u);
}

TEST(TerrainLayerPageIndex, MixedResolutionsDoNotAlias)
{
    // 2K 在前、1K 在后：旧公式（layerId * 该层 pagesPerMip）会让
    // (0,0,8,0) 与 (1,0,0,0) 都得到 64；统一 stride 后必须不相同。
    VL::TerrainLayerPageManager manager;
    const std::vector<VL::TerrainLayerDesc> layers = {
        MakeLayer(2048), MakeLayer(1024), MakeLayer(512)};
    manager.ConfigureLayers(layers);
    EXPECT_NE(
        manager.VirtualPageIndex(PageKey(0, 0, 8, 0)),
        manager.VirtualPageIndex(PageKey(1, 0, 0, 0)));

    // 穷举三层所有合法页。每个 mip 是独立页表，所以唯一性按 (mip, 页号)
    // 判断；不同 mip 的同一数值不冲突。
    std::set<std::pair<uint32_t, uint64_t>> indices;
    const uint32_t layerCount = static_cast<uint32_t>(layers.size());
    for (uint32_t layer = 0; layer < layerCount; ++layer)
    {
        const uint32_t resolution = layers[layer].sourceResolution;
        for (uint32_t mip = 0; mip < VL::TerrainLayerMipCount(resolution); ++mip)
        {
            const uint32_t side = VL::TerrainLayerPagesPerSide(resolution, mip);
            for (uint32_t y = 0; y < side; ++y)
                for (uint32_t x = 0; x < side; ++x)
                    indices.insert(
                        {mip, manager.VirtualPageIndex(PageKey(layer, mip, x, y))});
        }
    }
    EXPECT_EQ(indices.size(), 341u + 85u + 21u);
}

TEST(TerrainLayerPageIndex, RejectsLayerMipAndPageBounds)
{
    VL::TerrainLayerPageManager manager;
    manager.ConfigureLayers({MakeLayer(512), MakeLayer(1024)});

    EXPECT_THROW(
        manager.VirtualPageIndex(PageKey(256, 0, 0, 0)), std::out_of_range);
    // 1K 只有 mip0..3。
    EXPECT_THROW(
        manager.VirtualPageIndex(PageKey(1, 4, 0, 0)), std::out_of_range);
    // 512 只有 mip0..2。
    EXPECT_THROW(
        manager.VirtualPageIndex(PageKey(0, 3, 0, 0)), std::out_of_range);
    // mip0 的 1K 边长为 8：7 合法，8 越界。
    EXPECT_NO_THROW(manager.ValidateKey(PageKey(1, 0, 7, 7)));
    EXPECT_THROW(
        manager.VirtualPageIndex(PageKey(1, 0, 8, 0)), std::out_of_range);
    EXPECT_THROW(
        manager.VirtualPageIndex(PageKey(1, 0, 0, 8)), std::out_of_range);
}

TEST(TerrainLayerPageManagerTest, AllocatesPhysicalPagesAndKeepsFallbackReserved)
{
    // 4 个物理页 = 3 个普通槽位 + 1 个 fallback。
    VL::TerrainLayerPageManager manager(4);
    ASSERT_EQ(manager.UsablePhysicalPageCount(), 3u);
    ASSERT_EQ(manager.FallbackPhysicalPage(), 3u);
    ASSERT_EQ(manager.FreePhysicalPageCount(), 3u);

    EXPECT_TRUE(manager.RequestPage(PageKey(0, 0, 0, 0)));
    EXPECT_TRUE(manager.RequestPage(PageKey(0, 0, 1, 0)));
    EXPECT_TRUE(manager.RequestPage(PageKey(0, 0, 2, 0)));
    EXPECT_EQ(manager.ResidentCount(), 3u);
    EXPECT_EQ(manager.FreePhysicalPageCount(), 0u);
    EXPECT_EQ(manager.FindPage(PageKey(0, 0, 0, 0))->physicalPage, 0u);
    EXPECT_EQ(manager.FindPage(PageKey(0, 0, 1, 0))->physicalPage, 1u);
    EXPECT_EQ(manager.FindPage(PageKey(0, 0, 2, 0))->physicalPage, 2u);

    // 满了以后在安全 epoch 下淘汰并复用普通槽位；fallback 永不被分配。
    EXPECT_TRUE(manager.RequestPageAtEpoch(
        PageKey(0, 0, 3, 0), 0, 0, kNoWindowEpoch));
    EXPECT_EQ(manager.ResidentCount(), 3u);
    EXPECT_EQ(manager.SnapshotStats().evictionCount, 1u);
    for (const auto& entry : manager.Pages())
        EXPECT_NE(entry.second.physicalPage, manager.FallbackPhysicalPage());
}

TEST(TerrainLayerPageManagerTest, EvictionRespectsSafetyWindowAndQuarantinesSlot)
{
    VL::TerrainLayerPageManager manager(4);
    // framesInFlight=2 -> 窗口 max(2, 3) = 3。
    manager.SetGpuEpochContext(10, 2);
    ASSERT_EQ(manager.EvictionWindowFrames(), 3u);
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 0, 0)));
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 1, 0)));
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 2, 0)));
    const uint32_t firstPhysical =
        manager.FindPage(PageKey(0, 0, 0, 0))->physicalPage;

    // lastSeen=10：13 之前不允许淘汰。
    EXPECT_FALSE(manager.CanEvictPageAt(PageKey(0, 0, 0, 0), 12));
    EXPECT_TRUE(manager.CanEvictPageAt(PageKey(0, 0, 0, 0), 13));

    // 窗口内第 4 页请求被推迟，驻留集合不变。
    EXPECT_FALSE(manager.RequestPageAtEpoch(
        PageKey(0, 0, 3, 0), 0, 0, 12));
    EXPECT_EQ(manager.ResidentCount(), 3u);
    EXPECT_EQ(manager.SnapshotStats().deferredCount, 1u);

    // 显式淘汰立即生效；槽位进入 quarantine，releaseEpoch = 10 + 2 + 1 = 13。
    EXPECT_TRUE(manager.EvictPage(PageKey(0, 0, 0, 0)));
    EXPECT_EQ(manager.ResidentCount(), 2u);
    EXPECT_EQ(manager.SnapshotStats().evictionCount, 1u);
    EXPECT_EQ(manager.QuarantinedPhysicalPageCount(), 1u);
    EXPECT_EQ(manager.PhysicalPageReleaseEpoch(firstPhysical), 13u);

    // 新页和淘汰页争用同一个槽位：完成的 epoch 尚未追平就不能分配，
    // 请求推迟而不是覆盖仍可能被 in-flight 帧采样的内容。
    EXPECT_FALSE(manager.IsPhysicalPageAvailable(firstPhysical, 12));
    EXPECT_TRUE(manager.IsPhysicalPageAvailable(firstPhysical, 13));
    EXPECT_FALSE(manager.RequestPageAtEpoch(
        PageKey(0, 0, 3, 0), 0, 0, 12));
    EXPECT_EQ(manager.QuarantinedPhysicalPageCount(), 1u);
    EXPECT_TRUE(manager.RequestPageAtEpoch(
        PageKey(0, 0, 3, 0), 0, 0, 13));
    EXPECT_EQ(manager.QuarantinedPhysicalPageCount(), 0u);
    EXPECT_EQ(
        manager.FindPage(PageKey(0, 0, 3, 0))->physicalPage, firstPhysical);
}

TEST(TerrainLayerPageManagerTest, LruPrefersLowPriorityThenOldestTouch)
{
    VL::TerrainLayerPageManager manager(4);
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 0, 0), 1, 5));
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 1, 0), 1, 1));
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 2, 0), 1, 9));

    // 低优先级先淘汰，而不是注册顺序。
    EXPECT_TRUE(manager.EvictLeastRecentlyUsed(kNoWindowEpoch));
    EXPECT_FALSE(manager.IsResident(PageKey(0, 0, 1, 0)));
    EXPECT_TRUE(manager.IsResident(PageKey(0, 0, 0, 0)));
    EXPECT_TRUE(manager.IsResident(PageKey(0, 0, 2, 0)));

    // 同优先级下比较 lastTouchedEpoch：更早触碰的先淘汰。
    VL::TerrainLayerPageManager tied(4);
    ASSERT_TRUE(tied.RequestPage(PageKey(0, 0, 0, 0)));
    ASSERT_TRUE(tied.RequestPage(PageKey(0, 0, 1, 0)));
    ASSERT_TRUE(tied.RequestPage(PageKey(0, 0, 2, 0)));
    ASSERT_TRUE(tied.MarkPageUsedAtEpoch(PageKey(0, 0, 0, 0), 50));
    ASSERT_TRUE(tied.MarkPageUsedAtEpoch(PageKey(0, 0, 1, 0), 40));
    ASSERT_TRUE(tied.MarkPageUsedAtEpoch(PageKey(0, 0, 2, 0), 60));
    EXPECT_TRUE(tied.EvictLeastRecentlyUsed(kNoWindowEpoch));
    EXPECT_FALSE(tied.IsResident(PageKey(0, 0, 1, 0)));
}

TEST(TerrainLayerPageManagerTest, ReferenceAndUploadPinBlockEviction)
{
    VL::TerrainLayerPageManager manager(4);
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 0, 0)));
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 1, 0)));
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 2, 0)));

    ASSERT_TRUE(manager.AddPageReference(PageKey(0, 0, 0, 0)));
    EXPECT_FALSE(manager.EvictPage(PageKey(0, 0, 0, 0)));
    EXPECT_TRUE(manager.EvictLeastRecentlyUsed(kNoWindowEpoch));
    EXPECT_TRUE(manager.IsResident(PageKey(0, 0, 0, 0)));
    EXPECT_FALSE(manager.IsResident(PageKey(0, 0, 1, 0)));
    ASSERT_TRUE(manager.ReleasePageReference(PageKey(0, 0, 0, 0)));

    ASSERT_TRUE(manager.PinPageForUpload(PageKey(0, 0, 2, 0)));
    EXPECT_TRUE(manager.IsPagePinned(PageKey(0, 0, 2, 0)));
    EXPECT_FALSE(manager.EvictPage(PageKey(0, 0, 2, 0)));
    EXPECT_TRUE(manager.EvictLeastRecentlyUsed(kNoWindowEpoch));
    // 0 已被引用保护过、2 被 pin；只能淘汰 0（引用已释放）。
    EXPECT_FALSE(manager.IsResident(PageKey(0, 0, 0, 0)));
    EXPECT_TRUE(manager.IsResident(PageKey(0, 0, 2, 0)));
    EXPECT_TRUE(manager.UnpinPage(PageKey(0, 0, 2, 0)));
}

TEST(TerrainLayerPageManagerTest, OverlappingBatchesHoldIndependentPins)
{
    VL::TerrainLayerPageManager manager(8);
    const VL::TerrainLayerPageKey key = PageKey(0, 0, 0, 0);
    ASSERT_TRUE(manager.RequestPage(key));
    ASSERT_EQ(manager.DirtyPageCount(), 1u);

    const VL::TerrainLayerUploadBatch first = manager.BuildUploadBatch();
    const VL::TerrainLayerUploadBatch second = manager.BuildUploadBatch();
    ASSERT_EQ(first.uploads.size(), 1u);
    ASSERT_EQ(second.uploads.size(), 1u);
    // 两个批次各持一份 pin；Abort 只释放自己那份。
    manager.AbortUploadBatch(first);
    EXPECT_TRUE(manager.IsPagePinned(key));
    EXPECT_EQ(manager.DirtyPageCount(), 1u);
    manager.CommitUploadBatch(second);
    EXPECT_FALSE(manager.IsPagePinned(key));
    EXPECT_EQ(manager.DirtyPageCount(), 0u);
    EXPECT_EQ(manager.SnapshotStats().uploadedPageCount, 1u);
}

TEST(TerrainLayerPageManagerTest, CommitRejectsRevisionChangedAfterBuild)
{
    VL::TerrainLayerPageManager manager(8);
    const VL::TerrainLayerPageKey key = PageKey(0, 0, 0, 0);
    ASSERT_TRUE(manager.RequestPage(key));
    const VL::TerrainLayerUploadBatch stale = manager.BuildUploadBatch();

    // 收集后页被淘汰并重新驻留：contentRevision 前进，旧上传不能清脏。
    // Abort 先释放本批 pin，淘汰才被允许；stale 的上传描述仍然指向旧 revision。
    manager.AbortUploadBatch(stale);
    ASSERT_TRUE(manager.EvictPage(key));
    ASSERT_TRUE(manager.RequestPage(key));
    manager.CommitUploadBatch(stale);
    EXPECT_EQ(manager.DirtyPageCount(), 1u);
    EXPECT_EQ(manager.SnapshotStats().uploadedPageCount, 0u);
    EXPECT_FALSE(manager.IsPagePinned(key));

    const VL::TerrainLayerUploadBatch fresh = manager.BuildUploadBatch();
    manager.CommitUploadBatch(fresh);
    EXPECT_EQ(manager.DirtyPageCount(), 0u);
    EXPECT_EQ(manager.SnapshotStats().uploadedPageCount, 1u);
}

TEST(TerrainLayerPageManagerTest, UploadBatchTruncatesAndResumes)
{
    VL::TerrainLayerPageManager manager(16);
    for (uint32_t x = 0; x < 5; ++x)
        ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, x, 0)));
    ASSERT_EQ(manager.DirtyPageCount(), 5u);

    const VL::TerrainLayerUploadBatch first = manager.BuildUploadBatch(2);
    ASSERT_EQ(first.uploads.size(), 2u);
    EXPECT_EQ(first.uploads[0].virtualPageIndex, 0u);
    EXPECT_EQ(first.uploads[1].virtualPageIndex, 1u);
    manager.CommitUploadBatch(first);
    EXPECT_EQ(manager.DirtyPageCount(), 3u);
    const VL::TerrainLayerUploadBatch second = manager.BuildUploadBatch(2);
    ASSERT_EQ(second.uploads.size(), 2u);
    manager.CommitUploadBatch(second);
    EXPECT_EQ(manager.DirtyPageCount(), 1u);
    const VL::TerrainLayerUploadBatch third = manager.BuildUploadBatch(2);
    ASSERT_EQ(third.uploads.size(), 1u);
    manager.CommitUploadBatch(third);
    EXPECT_EQ(manager.DirtyPageCount(), 0u);
}

TEST(TerrainLayerPageManagerTest, CapacityChangeEvictsOnlyOutsideWindow)
{
    VL::TerrainLayerPageManager manager(8);
    manager.SetGpuEpochContext(20, 2);
    for (uint32_t x = 0; x < 4; ++x)
        ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, x, 0)));
    ASSERT_EQ(manager.ResidentCount(), 4u);

    // 窗口 = 3；completedEpoch=22 时都不可淘汰，预算暂时超限。
    manager.SetResidentCapacityAtEpoch(2, 22);
    EXPECT_EQ(manager.ResidentCount(), 4u);
    EXPECT_EQ(manager.ResidentCapacity(), 2u);
    manager.SetResidentCapacityAtEpoch(2, 23);
    EXPECT_EQ(manager.ResidentCount(), 2u);
    EXPECT_EQ(manager.QuarantinedPhysicalPageCount(), 2u);

    // 隔离槽位到期后可回收，free 计数回到可用总数。
    EXPECT_EQ(manager.ReclaimExpiredQuarantines(26), 2u);
    EXPECT_EQ(manager.QuarantinedPhysicalPageCount(), 0u);
    EXPECT_EQ(
        manager.FreePhysicalPageCount(),
        manager.UsablePhysicalPageCount() - manager.ResidentCount());
}

TEST(TerrainLayerPageManagerTest, RejectsLayerCountAndLateReconfigure)
{
    VL::TerrainLayerPageManager manager;
    EXPECT_THROW(
        manager.ConfigureLayers(std::vector<VL::TerrainLayerDesc>(
            257, MakeLayer(1024))), std::exception);
    EXPECT_NO_THROW(manager.ConfigureLayers(
        std::vector<VL::TerrainLayerDesc>(256, MakeLayer(1024))));

    VL::TerrainLayerPageManager late;
    ASSERT_TRUE(late.RequestPage(PageKey(0, 0, 0, 0)));
    EXPECT_THROW(late.ConfigureLayers({MakeLayer(2048)}), std::exception);
}

TEST(TerrainLayerPageManagerTest, ResidentCapacityCoversConfiguredVirtualPages)
{
    // 层页驻留容量与覆盖页共用同一公式：下限是 DefaultResidentCapacity，
    // 上限是可用物理槽位，中间必须覆盖世界的全部虚拟页。
    const auto derivedCapacity = [](VL::TerrainLayerPageManager& manager,
                                     uint32_t layerCount) {
        uint32_t virtualPages = 0;
        for (uint32_t layerId = 0; layerId < layerCount; ++layerId)
            virtualPages += manager.TotalPagesForLayer(layerId);
        return VL::ComputeTerrainResidentCapacity(
            virtualPages,
            VL::TerrainLayerPageManager::DefaultResidentCapacity,
            manager.UsablePhysicalPageCount());
    };

    // 4×4 tile 场景去重后是 4 个 1K 全局层：每层 85 页，共 340 页。
    // 低于下限 512，容量保持 512。
    VL::TerrainLayerPageManager oneK;
    oneK.ConfigureLayers(std::vector<VL::TerrainLayerDesc>(4, MakeLayer(1024)));
    EXPECT_EQ(oneK.TotalPagesForLayer(0), 85u);
    EXPECT_EQ(derivedCapacity(oneK, 4), 512u);

    // 2K 层每层 341 页：8 层 = 2728 页，超过 1024 个物理槽位。
    // 容量必须抬到可用槽位上限，而不是停在 512 让 LRU 在世界内部追尾。
    VL::TerrainLayerPageManager twoK;
    twoK.ConfigureLayers(std::vector<VL::TerrainLayerDesc>(8, MakeLayer(2048)));
    EXPECT_EQ(twoK.TotalPagesForLayer(0), 341u);
    EXPECT_EQ(derivedCapacity(twoK, 8),
        VL::TerrainLayerPageManager::DefaultPhysicalPageCount - 1);
}

TEST(TerrainLayerPageManagerTest, DecodesPhysicalPageIntoSlices)
{
    const auto first = VL::TerrainLayerPageManager::DecodePhysicalPage(0);
    EXPECT_EQ(first.slice, 0u);
    EXPECT_EQ(first.tileX, 0u);
    EXPECT_EQ(first.tileY, 0u);
    const auto last = VL::TerrainLayerPageManager::DecodePhysicalPage(1023);
    EXPECT_EQ(last.slice, 0u);
    EXPECT_EQ(last.tileX, 31u);
    EXPECT_EQ(last.tileY, 31u);
    const auto secondSlice = VL::TerrainLayerPageManager::DecodePhysicalPage(1024);
    EXPECT_EQ(secondSlice.slice, 1u);
    EXPECT_EQ(secondSlice.tileX, 0u);
    EXPECT_EQ(secondSlice.tileY, 0u);
    // 多 slice 池：页号跨过 1024 后 slice+1，tile 仍按 32×32 回绕。
    const auto lastOfSecondSlice =
        VL::TerrainLayerPageManager::DecodePhysicalPage(2047);
    EXPECT_EQ(lastOfSecondSlice.slice, 1u);
    EXPECT_EQ(lastOfSecondSlice.tileX, 31u);
    EXPECT_EQ(lastOfSecondSlice.tileY, 31u);
    const auto thirdSlice = VL::TerrainLayerPageManager::DecodePhysicalPage(2048);
    EXPECT_EQ(thirdSlice.slice, 2u);
    EXPECT_EQ(thirdSlice.tileX, 0u);
    EXPECT_EQ(thirdSlice.tileY, 0u);
}

TEST(TerrainLayerPageTableLayout, CapsWidthAndKeepsMixedResolutionIndicesUnique)
{
    // 只有 1K/512 层时 mip0 的 stride 是 64 -> 16384 项 -> 4096×4。
    VL::TerrainLayerPageManager oneK;
    oneK.ConfigureLayers({MakeLayer(1024)});
    const auto layout = oneK.PageTableLayoutForMip(0);
    EXPECT_EQ(layout.width, 4096u);
    EXPECT_EQ(layout.height, 4u);
    EXPECT_EQ(oneK.VirtualPageTableSize(0), 256u * 64u);
    // mip3 只有 1 页/层：256 项 -> 256×1。
    const auto tail = oneK.PageTableLayoutForMip(3);
    EXPECT_EQ(tail.width, 256u);
    EXPECT_EQ(tail.height, 1u);
    // 没有 mip4 的层仍是 1×1 占位表，不能出现 0 尺寸 image。
    const auto emptyMip = oneK.PageTableLayoutForMip(4);
    EXPECT_EQ(emptyMip.width, 1u);
    EXPECT_EQ(emptyMip.height, 1u);

    // 2K 让 mip4 从占位表变成真实 256 项。
    VL::TerrainLayerPageManager mixed;
    mixed.ConfigureLayers(
        {MakeLayer(512), MakeLayer(1024), MakeLayer(2048)});
    const auto mip4 = mixed.PageTableLayoutForMip(4);
    EXPECT_EQ(mip4.width, 256u);
    EXPECT_EQ(mip4.height, 1u);
    EXPECT_EQ(mixed.VirtualPageTableSize(4), 256u);
    // MaxMipLevel 为 6；mip5/6 合法，7 才是越界。
    EXPECT_THROW(mixed.PageTableLayoutForMip(7), std::out_of_range);
}

TEST(TerrainLayerPageTableEncoding, WritesPhysicalMipValidAndPriority)
{
    const std::array<float, 4> valid =
        VL::TerrainLayerPageManager::EncodePageTableEntry(37, 2, true, 9);
    EXPECT_FLOAT_EQ(valid[0], 37.0f);
    EXPECT_FLOAT_EQ(valid[1], 2.0f);
    EXPECT_FLOAT_EQ(valid[2], 1.0f);
    EXPECT_FLOAT_EQ(valid[3], 9.0f);

    // 无效项必须四通道全零：shader 依赖 B 通道判断 fallback。
    const std::array<float, 4> invalid =
        VL::TerrainLayerPageManager::EncodePageTableEntry(37, 2, false, 9);
    for (float channel : invalid)
        EXPECT_FLOAT_EQ(channel, 0.0f);
}

TEST(TerrainLayerPageTableLayout, VirtualPagesMapToInBoundsTexelsForEveryMip)
{
    VL::TerrainLayerPageManager manager;
    manager.ConfigureLayers(
        {MakeLayer(512), MakeLayer(1024), MakeLayer(2048)});
    for (uint32_t mip = 0; mip <= VL::TerrainLayerPageManager::MaxMipLevel;
         ++mip)
    {
        const auto layout = manager.PageTableLayoutForMip(mip);
        ASSERT_GE(layout.width, 1u);
        ASSERT_GE(layout.height, 1u);
        const uint32_t tableSize = manager.VirtualPageTableSize(mip);
        EXPECT_GE(
            static_cast<uint64_t>(layout.width) * layout.height,
            tableSize);
        for (uint32_t layer = 0; layer < 3; ++layer)
        {
            const uint32_t mipCount = VL::TerrainLayerMipCount(
                layer == 0 ? 512 : (layer == 1 ? 1024 : 2048));
            if (mip >= mipCount)
                continue;
            const uint32_t side = manager.PagesPerSide(layer, mip);
            for (uint32_t y = 0; y < side; ++y)
            {
                for (uint32_t x = 0; x < side; ++x)
                {
                    const uint64_t index =
                        manager.VirtualPageIndex(PageKey(layer, mip, x, y));
                    ASSERT_LT(index, tableSize);
                    const uint32_t texelX =
                        static_cast<uint32_t>(index % layout.width);
                    const uint32_t texelY =
                        static_cast<uint32_t>(index / layout.width);
                    EXPECT_LT(texelX, layout.width);
                    EXPECT_LT(texelY, layout.height);
                }
            }
        }
    }
}

TEST(TerrainLayerUploadBatchL2, ResidentPagesEmitPageTableEntries)
{
    VL::TerrainLayerPageManager manager(8);
    manager.ConfigureLayers({MakeLayer(1024)});
    ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, 3, 2), 0, 7));
    ASSERT_EQ(manager.DirtyPageCount(), 1u);
    ASSERT_EQ(manager.DirtyPageTableEntryCount(), 1u);

    const VL::TerrainLayerUploadBatch batch = manager.BuildUploadBatch();
    ASSERT_EQ(batch.uploads.size(), 1u);
    ASSERT_EQ(batch.pageTableUpdates.size(), 1u);
    const auto& update = batch.pageTableUpdates[0];
    EXPECT_TRUE(update.valid);
    EXPECT_EQ(update.virtualPageIndex, 2u * 8u + 3u);
    EXPECT_EQ(update.physicalPage, 0u);
    EXPECT_FLOAT_EQ(update.entry[0], 0.0f);
    EXPECT_FLOAT_EQ(update.entry[1], 0.0f);
    EXPECT_FLOAT_EQ(update.entry[2], 1.0f);
    EXPECT_FLOAT_EQ(update.entry[3], 7.0f);

    manager.CommitUploadBatch(batch);
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 0u);
}

TEST(TerrainLayerUploadBatchL2, EvictionEmitsInvalidationAndCommitClearsIt)
{
    VL::TerrainLayerPageManager manager(8);
    manager.ConfigureLayers({MakeLayer(1024)});
    const VL::TerrainLayerPageKey key = PageKey(0, 0, 1, 1);
    ASSERT_TRUE(manager.RequestPage(key));
    const VL::TerrainLayerUploadBatch initial = manager.BuildUploadBatch();
    manager.CommitUploadBatch(initial);
    ASSERT_EQ(manager.DirtyPageTableEntryCount(), 0u);

    ASSERT_TRUE(manager.EvictPage(key));
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 1u);
    const VL::TerrainLayerUploadBatch invalidation =
        manager.BuildUploadBatch();
    ASSERT_EQ(invalidation.pageTableUpdates.size(), 1u);
    const auto& update = invalidation.pageTableUpdates[0];
    EXPECT_FALSE(update.valid);
    EXPECT_EQ(update.virtualPageIndex, 1u * 8u + 1u);
    for (float channel : update.entry)
        EXPECT_FLOAT_EQ(channel, 0.0f);

    manager.CommitUploadBatch(invalidation);
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 0u);
}

TEST(TerrainLayerUploadBatchL2, AbortKeepsInvalidationDirty)
{
    VL::TerrainLayerPageManager manager(8);
    manager.ConfigureLayers({MakeLayer(1024)});
    const VL::TerrainLayerPageKey key = PageKey(0, 0, 0, 0);
    ASSERT_TRUE(manager.RequestPage(key));
    const VL::TerrainLayerUploadBatch initial = manager.BuildUploadBatch();
    manager.CommitUploadBatch(initial);
    ASSERT_TRUE(manager.EvictPage(key));

    const VL::TerrainLayerUploadBatch batch = manager.BuildUploadBatch();
    manager.AbortUploadBatch(batch);
    // Abort 只释放 pin，失效项必须留到下一批重试。
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 1u);
    const VL::TerrainLayerUploadBatch retry = manager.BuildUploadBatch();
    ASSERT_EQ(retry.pageTableUpdates.size(), 1u);
    EXPECT_FALSE(retry.pageTableUpdates[0].valid);
    manager.CommitUploadBatch(retry);
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 0u);
}

TEST(TerrainLayerUploadBatchL2, TruncationDefersPageTableUntilAtlasIncluded)
{
    VL::TerrainLayerPageManager manager(16);
    manager.ConfigureLayers({MakeLayer(1024)});
    for (uint32_t x = 0; x < 3; ++x)
        ASSERT_TRUE(manager.RequestPage(PageKey(0, 0, x, 0)));
    ASSERT_EQ(manager.DirtyPageCount(), 3u);
    ASSERT_EQ(manager.DirtyPageTableEntryCount(), 3u);

    // 只允许 1 页 atlas：本批页表也只能写这 1 页，绝不能先指向未上传页。
    const VL::TerrainLayerUploadBatch first = manager.BuildUploadBatch(1);
    ASSERT_EQ(first.uploads.size(), 1u);
    ASSERT_EQ(first.pageTableUpdates.size(), 1u);
    EXPECT_TRUE(first.pageTableUpdates[0].valid);
    EXPECT_EQ(first.pageTableUpdates[0].physicalPage, 0u);
    manager.CommitUploadBatch(first);
    EXPECT_EQ(manager.DirtyPageCount(), 2u);
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 2u);

    // 后续批次的截断位置必须继续按物理页升序推进，并最终收敛。
    uint32_t batches = 0;
    while (manager.HasDirtyPages() && batches < 8)
    {
        const VL::TerrainLayerUploadBatch next =
            manager.BuildUploadBatch(1);
        ASSERT_FALSE(next.Empty());
        manager.CommitUploadBatch(next);
        ++batches;
    }
    EXPECT_EQ(manager.DirtyPageCount(), 0u);
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 0u);
}

TEST(TerrainLayerUploadBatchL2, EvictedSlotReuseWritesInvalidationAndNewMapping)
{
    VL::TerrainLayerPageManager manager(3);
    manager.ConfigureLayers({MakeLayer(1024)});
    const VL::TerrainLayerPageKey first = PageKey(0, 0, 0, 0);
    const VL::TerrainLayerPageKey second = PageKey(0, 0, 1, 0);
    ASSERT_TRUE(manager.RequestPage(first));
    manager.CommitUploadBatch(manager.BuildUploadBatch());

    ASSERT_TRUE(manager.EvictPage(first));
    // 显式淘汰不检查安全窗；这里 completedEpoch 取无窗口值以立即复用槽位。
    ASSERT_TRUE(manager.RequestPage(second));
    const VL::TerrainLayerUploadBatch batch = manager.BuildUploadBatch();
    ASSERT_EQ(batch.pageTableUpdates.size(), 2u);
    // 旧虚拟页写零，新虚拟页写同一物理槽位；两者顺序按页键稳定。
    const auto& invalid = batch.pageTableUpdates[0];
    const auto& valid = batch.pageTableUpdates[1];
    EXPECT_EQ(invalid.key, first);
    EXPECT_FALSE(invalid.valid);
    EXPECT_EQ(
        invalid.physicalPage,
        VL::TerrainLayerPageManager::InvalidPhysicalPage);
    EXPECT_EQ(valid.key, second);
    EXPECT_TRUE(valid.valid);
    EXPECT_EQ(valid.physicalPage, manager.FindPage(second)->physicalPage);
    EXPECT_EQ(valid.physicalPage, 0u);
    manager.CommitUploadBatch(batch);
    EXPECT_EQ(manager.DirtyPageTableEntryCount(), 0u);
}

TEST(TerrainLayerVTL3, MaxMipAndPagesPerSideFollowCatalogResolution)
{
    using namespace VL::TerrainLayerVT;
    EXPECT_EQ(MaxMipForPagesPerSide(4), 2u);
    EXPECT_EQ(MaxMipForPagesPerSide(8), 3u);
    EXPECT_EQ(MaxMipForPagesPerSide(16), 4u);
    EXPECT_EQ(PagesPerSideAtMip(8, 0), 8u);
    EXPECT_EQ(PagesPerSideAtMip(8, 1), 4u);
    EXPECT_EQ(PagesPerSideAtMip(8, 2), 2u);
    EXPECT_EQ(PagesPerSideAtMip(8, 3), 1u);
    EXPECT_EQ(PagesPerSideAtMip(8, 4), 1u);
}

TEST(TerrainLayerVTL3, SelectMipRoundsAndClampsToLayerMax)
{
    using namespace VL::TerrainLayerVT;
    EXPECT_EQ(SelectMip(0.5f, 8, 3), 0u);
    EXPECT_EQ(SelectMip(1.0f, 8, 3), 0u);
    EXPECT_EQ(SelectMip(2.0f, 8, 3), 1u);
    EXPECT_EQ(SelectMip(4.0f, 8, 3), 2u);
    EXPECT_EQ(SelectMip(8.0f, 8, 3), 3u);
    EXPECT_EQ(SelectMip(1024.0f, 8, 3), 3u);
    EXPECT_EQ(SelectMip(0.0f, 8, 3), 0u);
    // catalog 只允许 512/1K/2K；maxMip 由 pagesPerSide 再钳制一次。
    EXPECT_EQ(SelectMip(1024.0f, 4, 4), 2u);
}

TEST(TerrainLayerVTL3, PageTableStrideMatchesManagerLayout)
{
    using namespace VL::TerrainLayerVT;
    VL::TerrainLayerPageManager manager;
    manager.ConfigureLayers({MakeLayer(512), MakeLayer(1024), MakeLayer(2048)});
    // 页表 texel 总数 = 每层 stride * 256；shader 不需要额外 stride uniform。
    for (uint32_t mip = 0; mip <= VL::TerrainLayerMaxMipLevel; ++mip)
    {
        const VL::TerrainLayerPageManager::PageTableLayout layout =
            manager.PageTableLayoutForMip(mip);
        const uint64_t texelCount =
            static_cast<uint64_t>(layout.width) * layout.height;
        EXPECT_EQ(
            PerLayerStrideFromTexelCount(texelCount),
            manager.VirtualPagesPerLayer(mip))
            << "mip=" << mip;
    }

    // 混合分辨率时 stride 取该 mip 所有层的最大页数：mip1 的 2K 层是
    // 8 页/边 -> 64 页/层。layer 2 是 2K 层，side=8。
    const uint32_t stride = PerLayerStrideFromTexelCount(
        static_cast<uint64_t>(
            manager.PageTableLayoutForMip(1).width) *
        manager.PageTableLayoutForMip(1).height);
    EXPECT_EQ(stride, 64u);
    VL::TerrainLayerPageKey key = PageKey(2, 1, 1, 2);
    EXPECT_EQ(
        VirtualPageIndex(2, 1, 2, 8, stride),
        manager.VirtualPageIndex(key));
    EXPECT_EQ(
        VirtualPageIndex(2, 1, 2, 8, stride),
        2u * 64u + 2u * 8u + 1u);
}

TEST(TerrainLayerVTL3, PageTableTexelAndAtlasUvCoverPageBoundaries)
{
    using namespace VL::TerrainLayerVT;
    EXPECT_EQ(PageTableTexelFor(0, 4096).x, 0u);
    EXPECT_EQ(PageTableTexelFor(0, 4096).y, 0u);
    EXPECT_EQ(PageTableTexelFor(4095, 4096).x, 4095u);
    EXPECT_EQ(PageTableTexelFor(4095, 4096).y, 0u);
    EXPECT_EQ(PageTableTexelFor(4096, 4096).x, 0u);
    EXPECT_EQ(PageTableTexelFor(4096, 4096).y, 1u);

    // physicalPage 1057 = slice 1、slice 内 tile 33 = (1, 1)。
    EXPECT_EQ(SliceForPhysicalPage(1057, 1024), 1u);
    const AtlasUv first = AtlasUvForPhysicalPage(
        1057,
        0.0f,
        0.0f,
        128,
        4096,
        32,
        1024);
    EXPECT_NEAR(first.u, (128.0f + 0.5f) / 4096.0f, 1.0e-7f);
    EXPECT_NEAR(first.v, (128.0f + 0.5f) / 4096.0f, 1.0e-7f);
    const AtlasUv last = AtlasUvForPhysicalPage(
        1023,
        1.0f,
        1.0f,
        128,
        4096,
        32,
        1024);
    EXPECT_NEAR(last.u, (31.0f * 128.0f + 127.5f) / 4096.0f, 1.0e-7f);
    EXPECT_NEAR(last.v, (31.0f * 128.0f + 127.5f) / 4096.0f, 1.0e-7f);
}

TEST(TerrainLayerVTL3, LayerUvWrapsBeforePaging)
{
    using namespace VL::TerrainLayerVT;
    // repeat 语义：1.0 / -0.25 / 1.25 都先 fract 再切页。
    const LayerPageUv atOne = LayerUvToPage(1.0f, 1.0f, 8);
    EXPECT_EQ(atOne.pageX, 0u);
    EXPECT_EQ(atOne.pageY, 0u);
    EXPECT_NEAR(atOne.localU, 0.0f, 1.0e-6f);

    const LayerPageUv negative = LayerUvToPage(-0.25f, -0.25f, 8);
    EXPECT_EQ(negative.pageX, 6u);
    EXPECT_NEAR(negative.localU, 0.0f, 1.0e-6f);

    const LayerPageUv wrapped = LayerUvToPage(1.25f, 1.25f, 8);
    EXPECT_EQ(wrapped.pageX, 2u);
    EXPECT_NEAR(wrapped.localU, 0.0f, 1.0e-6f);

    const LayerPageUv lastPage = LayerUvToPage(0.999999f, 0.999999f, 8);
    EXPECT_EQ(lastPage.pageX, 7u);
    EXPECT_GT(lastPage.localU, 0.99f);
}

TEST(TerrainLayerVTL3, InvalidPageTableEntryIsAllZero)
{
    const std::array<float, 4> invalid =
        VL::TerrainLayerPageManager::EncodePageTableEntry(
            7,
            2,
            false,
            5);
    EXPECT_EQ(invalid[0], 0.0f);
    EXPECT_EQ(invalid[1], 0.0f);
    EXPECT_EQ(invalid[2], 0.0f);
    EXPECT_EQ(invalid[3], 0.0f);
    const std::array<float, 4> valid =
        VL::TerrainLayerPageManager::EncodePageTableEntry(
            7,
            2,
            true,
            5);
    EXPECT_EQ(valid[0], 7.0f);
    EXPECT_EQ(valid[1], 2.0f);
    EXPECT_EQ(valid[2], 1.0f);
}

TEST(TerrainLayerVTL3, CatalogSourceResolutionMustMatchSquareSourceExtent)
{
    using namespace VL::TerrainLayerVT;
    const std::vector<VL::TerrainLayerDesc> layers = {
        MakeLayer(512), MakeLayer(512)};
    EXPECT_TRUE(CatalogMatchesSourceExtent(layers, 512, 512));
    // 32² 验证资产不能冒充 catalog 的 1K/512 页布局。
    EXPECT_FALSE(CatalogMatchesSourceExtent(layers, 32, 32));
    EXPECT_FALSE(CatalogMatchesSourceExtent(layers, 512, 256));
    EXPECT_FALSE(CatalogMatchesSourceExtent({}, 512, 512));
    EXPECT_FALSE(CatalogMatchesSourceExtent(
        {MakeLayer(512), MakeLayer(1024)},
        512,
        512));
}

TEST(TerrainLayerPageManagerTest, ProtectedWorkingSetSurvivesCapacityEviction)
{
    VL::TerrainLayerPageManager manager;
    manager.ConfigureLayers({MakeLayer(512)});
    manager.SetResidentCapacity(2);
    manager.SetGpuEpochContext(10, 10, 2);

    const VL::TerrainLayerPageKey first = PageKey(0, 0, 0, 0);
    const VL::TerrainLayerPageKey second = PageKey(0, 0, 1, 0);
    const VL::TerrainLayerPageKey third = PageKey(0, 0, 2, 0);
    ASSERT_TRUE(manager.RequestPage(first));
    ASSERT_TRUE(manager.RequestPage(second));

    // 选中页集不能被当成普通 LRU 候选。
    const std::map<VL::TerrainLayerPageKey, uint32_t> protectedFirst = {
        {first, 255}};
    manager.SetProtectedPages(protectedFirst);
    EXPECT_FALSE(manager.CanEvictPageAt(first, 20));
    EXPECT_TRUE(manager.CanEvictPageAt(second, 20));
    EXPECT_FALSE(manager.EvictPage(first));

    // 请求新页时只能淘汰未受保护的 second。
    ASSERT_TRUE(manager.RequestPageAtEpoch(third, 1, 200, 20));
    EXPECT_TRUE(manager.IsResident(first));
    EXPECT_FALSE(manager.IsResident(second));
    EXPECT_TRUE(manager.IsResident(third));

    // 工作集大于容量时允许暂时超预算，但不能一边请求一边淘汰自己。
    const std::map<VL::TerrainLayerPageKey, uint32_t> protectedAll = {
        {first, 255}, {third, 255}};
    manager.SetProtectedPages(protectedAll);
    manager.SetResidentCapacityAtEpoch(1, 30);
    EXPECT_EQ(manager.ResidentCount(), 2u);
    EXPECT_FALSE(manager.EvictPage(first));
    EXPECT_FALSE(manager.EvictPage(third));
}
