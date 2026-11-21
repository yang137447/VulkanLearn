#include <gtest/gtest.h>

#include "terrain/terrainCompositedVT.h"
#include "terrain/terrainCompositedVTStore.h"
#include "shader/build/atomicFile.h"
#include "terrainCompositedProductionFixture.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace
{

using namespace VL;

TerrainCompositedVTBuildDesc MakeDesc(std::string identity = "terrain|placement")
{
    TerrainCompositedVTBuildDesc desc;
    desc.surfaceId = std::move(identity);
    desc.width = 259;
    desc.height = 131;
    desc.dependencyHash = ContentHasher::HashString("frozen-author-snapshot-and-placement");
    desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb, TerrainCompositedVTFormat::RGBA8UNorm,
        TerrainCompositedVTFormat::RGBA8UNorm};
    return desc;
}

std::vector<TerrainCompositedVTPageData> MakePages(const TerrainCompositedVTBuildDesc& desc)
{
    std::vector<TerrainCompositedVTPageData> pages;
    const auto layout = desc.Layout();
    for (uint32_t mip = 0; mip < layout.MipCount(); ++mip)
    {
        for (uint32_t pageY = 0; pageY < layout.Mip(mip).pagesY; ++pageY)
        {
            for (uint32_t pageX = 0; pageX < layout.Mip(mip).pagesX; ++pageX)
            {
                pages.push_back({{desc.surfaceId, mip, pageX, pageY},
                    std::vector<uint8_t>(static_cast<size_t>(desc.PageBytes()),
                        static_cast<uint8_t>(1 + pages.size()))});
            }
        }
    }
    return pages;
}

bool HasPage(const std::vector<TerrainCompositedVTPageKey>& pages,
    uint32_t mip, uint32_t pageX, uint32_t pageY)
{
    return std::any_of(pages.begin(), pages.end(), [&](const auto& page)
        { return page.mip == mip && page.x == pageX && page.y == pageY; });
}

TEST(TerrainCompositedVT, RectangularOddDimensionsReachOneTexelAndPreserveLastPage)
{
    const auto layout = MakeDesc().Layout();
    EXPECT_EQ(layout.MipCount(), 9u);
    EXPECT_EQ(layout.Mip(0).pagesX, 3u);
    EXPECT_EQ(layout.Mip(0).pagesY, 2u);
    EXPECT_EQ(layout.Mip(1).width, 129u);
    EXPECT_EQ(layout.Mip(1).height, 65u);
    EXPECT_EQ(layout.TailMip(), 2u);
    EXPECT_EQ(layout.Mip(8).width, 1u);
    EXPECT_EQ(layout.Mip(8).height, 1u);
    EXPECT_EQ(layout.ValidExtent({layout.SurfaceId(), 0, 2, 1}).width, 3u);
    EXPECT_EQ(layout.ValidExtent({layout.SurfaceId(), 0, 2, 1}).height, 3u);
    EXPECT_EQ(layout.PageCount(), 15u);
    EXPECT_EQ(layout.Parent({layout.SurfaceId(), 0, 2, 1}),
        (TerrainCompositedVTPageKey{layout.SurfaceId(), 2, 0, 0}));
    EXPECT_THROW(layout.Parent({layout.SurfaceId(), 8, 0, 0}), std::exception);
}

TEST(TerrainCompositedVT, OddExtentParentSkipsSplitBoundaryAndCoversWholeChild)
{
    const TerrainCompositedVTLayout layout("surface", 513, 257);
    const TerrainCompositedVTPageKey child{"surface", 0, 2, 0};
    const auto parent = layout.Parent(child);
    EXPECT_EQ(parent, (TerrainCompositedVTPageKey{"surface", 2, 0, 0}));
    const auto childRect = layout.PageUVRect(child);
    const auto parentRect = layout.PageUVRect(parent);
    EXPECT_LE(parentRect.minU, childRect.minU);
    EXPECT_LE(parentRect.minV, childRect.minV);
    EXPECT_GE(parentRect.maxU, childRect.maxU);
    EXPECT_GE(parentRect.maxV, childRect.maxV);
}

TEST(TerrainCompositedVT, PowerOfTwoParentRemainsImmediateAndClipsRectangularTail)
{
    const TerrainCompositedVTLayout layout("surface", 1024, 256);
    const TerrainCompositedVTPageKey child{"surface", 0, 5, 1};
    EXPECT_EQ(layout.Parent(child), (TerrainCompositedVTPageKey{"surface", 1, 2, 0}));
    const TerrainCompositedVTLayout thin("thin", 1, 257);
    EXPECT_EQ(thin.Parent({"thin", 0, 0, 1}),
        (TerrainCompositedVTPageKey{"thin", 1, 0, 0}));
}

TEST(TerrainCompositedVT, ParentIsNearestWholePageCoverAcrossRectangularMipChains)
{
    for (const auto& extent : {std::pair<uint32_t, uint32_t>{259, 131},
        {513, 257}, {1025, 513}, {257, 1}, {1, 513}, {1024, 256}})
    {
        const TerrainCompositedVTLayout layout("surface", extent.first, extent.second);
        for (uint32_t mip = 0; mip + 1 < layout.MipCount(); ++mip)
            for (uint32_t pageX = 0; pageX < layout.Mip(mip).pagesX; ++pageX)
                for (uint32_t pageY = 0; pageY < layout.Mip(mip).pagesY; ++pageY)
                {
                    const TerrainCompositedVTPageKey key{"surface", mip, pageX, pageY};
                    const auto child = layout.PageUVRect(key);
                    const auto parent = layout.Parent(key);
                    const auto rect = layout.PageUVRect(parent);
                    EXPECT_LE(rect.minU, child.minU);
                    EXPECT_LE(rect.minV, child.minV);
                    EXPECT_GE(rect.maxU, child.maxU);
                    EXPECT_GE(rect.maxV, child.maxV);
                    for (uint32_t skipped = mip + 1; skipped < parent.mip; ++skipped)
                        for (uint32_t candidateX = 0; candidateX < layout.Mip(skipped).pagesX; ++candidateX)
                            for (uint32_t candidateY = 0; candidateY < layout.Mip(skipped).pagesY; ++candidateY)
                            {
                                const auto candidate = layout.PageUVRect({"surface", skipped, candidateX, candidateY});
                                EXPECT_FALSE(candidate.minU <= child.minU && candidate.minV <= child.minV &&
                                    candidate.maxU >= child.maxU && candidate.maxV >= child.maxV);
                            }
                }
    }
}

TEST(TerrainCompositedVT, RejectsInvalidDimensionsKeysAndModes)
{
    EXPECT_THROW((TerrainCompositedVTLayout{"surface", 0, 4}), std::exception);
    EXPECT_THROW((TerrainCompositedVTLayout{"surface", UINT32_MAX, 4}), std::exception);
    EXPECT_THROW((TerrainCompositedVTLayout{"", 4, 4}), std::exception);
    EXPECT_THROW((TerrainCompositedVTLayout{"surface", 4, 4,
        static_cast<TerrainCompositedVTAddressMode>(9)}), std::exception);
    const auto layout = MakeDesc().Layout();
    EXPECT_FALSE(layout.Contains({"another-placement", 0, 0, 0}));
    EXPECT_FALSE(layout.Contains({layout.SurfaceId(), UINT32_MAX, 0, 0}));
    EXPECT_THROW(layout.ValidExtent({layout.SurfaceId(), 0, 3, 0}), std::exception);
    EXPECT_THROW(layout.TexelUV({layout.SurfaceId(), 0, 0, 0}, 136, 0), std::exception);
}

TEST(TerrainCompositedVT, BorderSamplesAdjacentVirtualRegionAndClampsOuterEdge)
{
    TerrainCompositedVTLayout layout("surface", 256, 128);
    EXPECT_EQ(layout.StoredExtent, 136u);
    const auto rightBorder = layout.TexelUV({"surface", 0, 0, 0}, 132, 4);
    const auto nextInner = layout.TexelUV({"surface", 0, 1, 0}, 4, 4);
    EXPECT_DOUBLE_EQ(rightBorder.minU, nextInner.minU);
    EXPECT_DOUBLE_EQ(rightBorder.minU, 128.5 / 256);
    EXPECT_DOUBLE_EQ(layout.TexelUV({"surface", 0, 0, 0}, 0, 0).minU, 0.5 / 256);
    const auto finalRect = layout.PageUVRect({"surface", 0, 1, 0});
    EXPECT_DOUBLE_EQ(finalRect.minU, 0.5);
    EXPECT_DOUBLE_EQ(finalRect.maxU, 1.0);
}

TEST(TerrainCompositedVT, RepeatBorderAndDirtyRegionWrapAcrossSurface)
{
    TerrainCompositedVTLayout layout("surface", 512, 128, TerrainCompositedVTAddressMode::Repeat);
    EXPECT_DOUBLE_EQ(layout.TexelUV({"surface", 0, 0, 0}, 0, 4).minU, 508.5 / 512);
    const auto dirty = layout.DirtyPages({0, 0.2, 0.001, 0.3}, 0);
    EXPECT_TRUE(HasPage(dirty, 0, 0, 0));
    EXPECT_TRUE(HasPage(dirty, 0, 3, 0));
    EXPECT_FALSE(HasPage(dirty, 0, 1, 0));
    EXPECT_EQ(std::set<TerrainCompositedVTPageKey>(dirty.begin(), dirty.end()).size(), dirty.size());
}

TEST(TerrainCompositedVT, DirtyExpandsBorderAndFilterAtEveryMip)
{
    TerrainCompositedVTLayout layout("surface", 1024, 512);
    const auto dirty = layout.DirtyPages({124.0 / 1024, 125.0 / 512, 125.0 / 1024, 126.0 / 512}, 1);
    EXPECT_TRUE(HasPage(dirty, 0, 0, 0));
    EXPECT_TRUE(HasPage(dirty, 0, 1, 0));
    EXPECT_TRUE(HasPage(dirty, 0, 0, 1));
    EXPECT_TRUE(HasPage(dirty, 0, 1, 1));
    EXPECT_FALSE(HasPage(dirty, 0, 2, 0));
    for (uint32_t mip = 1; mip < layout.MipCount(); ++mip)
    {
        EXPECT_TRUE(HasPage(dirty, mip, 0, 0));
    }
    EXPECT_TRUE(layout.DirtyPages({0.2, 0.2, 0.2, 0.3}).empty());
    EXPECT_TRUE(layout.DirtyPages({2, 2, 3, 3}).empty());
    EXPECT_EQ(layout.DirtyPages({0, 0, 1, 1}).size(), layout.PageCount());
}

TEST(TerrainCompositedVT, DirtyRejectsNonfiniteInvertedAndInvalidFilter)
{
    const auto layout = MakeDesc().Layout();
    EXPECT_THROW(layout.DirtyPages({0.5, 0, 0.4, 1}), std::exception);
    EXPECT_THROW(layout.DirtyPages({0, 0, std::numeric_limits<double>::infinity(), 1}), std::exception);
    EXPECT_THROW(layout.DirtyPages({0, 0, 1, 1}, -1), std::exception);
    EXPECT_THROW(layout.DirtyPages({0, 0, 1, 1},
        std::numeric_limits<double>::quiet_NaN()), std::exception);
}

TEST(TerrainCompositedVT, StableContentHashTracksDependenciesLayoutAndEncoding)
{
    const auto base = MakeDesc();
    EXPECT_EQ(base.ContentHash(), MakeDesc().ContentHash());
    auto changed = base;
    changed.dependencyHash = ContentHasher::HashString("new-paint");
    EXPECT_NE(base.ContentHash(), changed.ContentHash());
    changed = base;
    ++changed.generatorVersion;
    EXPECT_NE(base.ContentHash(), changed.ContentHash());
    changed = base;
    changed.encoding = "cook-bc-v1";
    EXPECT_NE(base.ContentHash(), changed.ContentHash());
    changed = base;
    ++changed.width;
    EXPECT_NE(base.ContentHash(), changed.ContentHash());
    changed = base;
    changed.addressMode = TerrainCompositedVTAddressMode::Repeat;
    EXPECT_NE(base.ContentHash(), changed.ContentHash());
    changed = base;
    changed.surfaceId = "another-placement";
    EXPECT_NE(base.ContentHash(), changed.ContentHash());
    changed = base;
    std::reverse(changed.channels.begin(), changed.channels.end());
    EXPECT_NE(base.ContentHash(), changed.ContentHash());
}

TEST(TerrainCompositedVT, ProductionDefaultsUseApprovedAdjacencyPaddingIdentity)
{
    const TerrainCompositedVTBuildDesc defaults;
    EXPECT_EQ(defaults.generatorVersion, 3u);
    EXPECT_EQ(defaults.encoding, "gpu-rvt-adjacency-area4-rgba8-v3");
    EXPECT_STREQ(TerrainCompositedFilterRule,
        "mip0-center-coarse-area-local-normal-rvt-adjacency-padding-v3");
    EXPECT_EQ(TerrainCompositedVTStore::Version, 2u);
}

TEST(TerrainCompositedVT, ChangedPaddingIdentityRejectsOldTaskContent)
{
    const auto current = MakeDesc();
    auto legacy = current;
    legacy.generatorVersion = 2;
    legacy.encoding = "gpu-rvt-adjacency-area4-rgba8-v2";
    EXPECT_NE(current.ContentHash(), legacy.ContentHash());
    const TerrainCompositedVTTaskToken expected{
        12, 4, current.ContentHash(), {current.surfaceId, 0, 0, 0}};
    auto obsolete = expected;
    obsolete.contentHash = legacy.ContentHash();
    EXPECT_FALSE(obsolete.Matches(expected));
}

TEST(TerrainCompositedVT, ProductionOutputRejectsLegacyAndIncorrectEncoding)
{
    const auto current = MakeDesc();
    EXPECT_NO_THROW(ValidateTerrainCompositedProductionOutput(current));
    auto changed = current;
    changed.generatorVersion = 2;
    EXPECT_THROW(ValidateTerrainCompositedProductionOutput(changed), std::invalid_argument);
    changed = current;
    changed.encoding = "rgba8-srgb-rgba8-local-normal-rgba8-orm-rvt-adjacency-padding-v3";
    EXPECT_THROW(ValidateTerrainCompositedProductionOutput(changed), std::invalid_argument);
}

TEST(TerrainCompositedVT, ProductionOutputUsesExactlyApprovedUncompressedChannels)
{
    const auto current = MakeDesc();
    for (const auto format : {TerrainCompositedVTFormat::RG8UNorm,
        TerrainCompositedVTFormat::BC5UNorm, TerrainCompositedVTFormat::BC7UNorm})
    {
        auto changed = current;
        changed.channels[1] = format;
        EXPECT_THROW(ValidateTerrainCompositedProductionOutput(changed), std::invalid_argument);
    }
    auto changed = current;
    changed.channels[0] = TerrainCompositedVTFormat::RGBA8UNorm;
    EXPECT_THROW(ValidateTerrainCompositedProductionOutput(changed), std::invalid_argument);
}

TEST(TerrainCompositedVT, TaskIdentityRejectsEveryKindOfStaleResult)
{
    TerrainCompositedVTTaskToken current{4, 7, MakeDesc().ContentHash(), {"surface", 0, 0, 0}};
    EXPECT_TRUE(current.Matches(current));
    auto stale = current;
    --stale.worldGeneration;
    EXPECT_FALSE(stale.Matches(current));
    stale = current;
    --stale.revision;
    EXPECT_FALSE(stale.Matches(current));
    stale = current;
    stale.contentHash = ContentHasher::HashString("old-content");
    EXPECT_FALSE(stale.Matches(current));
    stale = current;
    ++stale.key.mip;
    EXPECT_FALSE(stale.Matches(current));
    stale = current;
    stale.key.surfaceId = "other-placement";
    EXPECT_FALSE(stale.Matches(current));
}

TEST(TerrainCompositedVT, EncodedSizesIncludeBlockAlignedBorder)
{
    EXPECT_EQ(TerrainCompositedVTFormatBytes(TerrainCompositedVTFormat::BC7UNorm), 34u * 34u * 16u);
    EXPECT_EQ(TerrainCompositedVTFormatBytes(TerrainCompositedVTFormat::BC5UNorm), 18496u);
    EXPECT_EQ(TerrainCompositedVTFormatBytes(TerrainCompositedVTFormat::R16UNorm), 136u * 136u * 2u);
    EXPECT_THROW(TerrainCompositedVTFormatBytes(static_cast<TerrainCompositedVTFormat>(99)), std::exception);
    auto desc = MakeDesc();
    desc.channels.clear();
    EXPECT_THROW(desc.ContentHash(), std::exception);
}

class TerrainCompositedVTStoreTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        root = std::filesystem::temp_directory_path() /
            ("vl-composited-vt-" + std::to_string(
                std::chrono::high_resolution_clock::now().time_since_epoch().count()) +
                "-" + std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(root);
        manifest = root / "surface.json";
        desc = MakeDesc();
        pages = MakePages(desc);
    }

    void TearDown() override
    {
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    }

    void Publish()
    {
        TerrainCompositedVTStore::Publish(manifest, Publication(), pages);
    }

    TerrainCompositedVTPublication Publication()
    {
        auto publication = TerrainProductionFixture::Publication(root, {desc}, pages);
        desc = publication.surfaces.front();
        return publication;
    }
    nlohmann::json Document() const
    {
        std::ifstream input(manifest);
        return nlohmann::json::parse(input);
    }

    void WriteDocument(const nlohmann::json& document)
    {
        WriteTextFileAtomically(manifest, document.dump());
    }

    std::filesystem::path root;
    std::filesystem::path manifest;
    TerrainCompositedVTBuildDesc desc;
    std::vector<TerrainCompositedVTPageData> pages;
};

TEST_F(TerrainCompositedVTStoreTest, RoundTripMultipleSurfacesAndRandomPageOrder)
{
    const auto other = MakeDesc("terrain|second-placement");
    const auto otherPages = MakePages(other);
    pages.insert(pages.end(), otherPages.begin(), otherPages.end());
    std::reverse(pages.begin(), pages.end());
    TerrainCompositedVTStore::Publish(manifest,
        TerrainProductionFixture::Publication(root, {desc, other}, pages), pages);
    const auto store = TerrainCompositedVTStore::Open(manifest);
    EXPECT_EQ(store.Surfaces().size(), 2u);
    EXPECT_EQ(store.Pages().size(), pages.size());
    for (const auto& page : pages)
    {
        EXPECT_EQ(store.ReadPage(page.key), page.bytes);
    }
    EXPECT_THROW(store.ReadPage({"unknown", 0, 0, 0}), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, PublishRejectsMissingTailDuplicateAndWrongSizeWithoutReplacingOld)
{
    Publish();
    const auto oldManifest = ReadBinaryFile(manifest);
    pages.pop_back();
    EXPECT_THROW(Publish(), std::exception);
    EXPECT_EQ(ReadBinaryFile(manifest), oldManifest);
    pages = MakePages(desc);
    pages.back() = pages.front();
    EXPECT_THROW(Publish(), std::exception);
    EXPECT_EQ(ReadBinaryFile(manifest), oldManifest);
    pages = MakePages(desc);
    pages.front().bytes.pop_back();
    EXPECT_THROW(Publish(), std::exception);
    EXPECT_EQ(ReadBinaryFile(manifest), oldManifest);
}

TEST_F(TerrainCompositedVTStoreTest, ValidatesPhysicalOffsetsIndependentlyOfPageKeyOrder)
{
    Publish();
    const auto originalStore = TerrainCompositedVTStore::Open(manifest);
    const auto originalBytes = ReadBinaryFile(originalStore.PackagePath());
    auto document = Document();
    auto records = document.at("pages");
    std::reverse(records.begin(), records.end());
    std::vector<uint8_t> reordered(originalBytes.begin(), originalBytes.begin() + 16);
    for (auto& record : records)
    {
        const auto originalOffset = record.at("offset").get<size_t>();
        const auto size = record.at("size").get<size_t>();
        record["offset"] = reordered.size();
        reordered.insert(reordered.end(), originalBytes.begin() + originalOffset,
            originalBytes.begin() + originalOffset + size);
    }
    const auto digest = ContentHasher::HashBytes(reordered.data(), reordered.size()).ToHex();
    document["pages"] = records;
    document["packageHash"] = digest;
    document["package"] = digest + ".vlcvt";
    WriteBinaryFileAtomically(root / (digest + ".vlcvt"), reordered);
    WriteDocument(document);
    const auto reopened = TerrainCompositedVTStore::Open(manifest);
    ASSERT_GT(reopened.Pages().size(), 1u);
    EXPECT_GT(reopened.Pages().begin()->second.offset, reopened.Pages().rbegin()->second.offset);
    for (const auto& page : pages)
        EXPECT_EQ(reopened.ReadPage(page.key), page.bytes);
}

TEST_F(TerrainCompositedVTStoreTest, RejectsPageHashEvenWhenPackageHashRemainsValid)
{
    Publish();
    auto document = Document();
    document["pages"][0]["hash"] = ContentHasher::HashString("incorrect-page-digest").ToHex();
    WriteDocument(document);
    try
    {
        TerrainCompositedVTStore::Open(manifest).ValidateAll();
        FAIL() << "Incorrect per-page hash accepted";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("VT page integrity mismatch"), std::string::npos);
    }
}

TEST_F(TerrainCompositedVTStoreTest, RejectsPackageHashEvenWhenAllPageHashesRemainValid)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    auto document = Document();
    const auto wrong = ContentHasher::HashString("incorrect-package-digest").ToHex();
    const auto bytes = ReadBinaryFile(store.PackagePath());
    WriteBinaryFileAtomically(root / (wrong + ".vlcvt"), bytes);
    document["packageHash"] = wrong;
    document["package"] = wrong + ".vlcvt";
    WriteDocument(document);
    try
    {
        TerrainCompositedVTStore::Open(manifest).ValidateAll();
        FAIL() << "Incorrect package hash accepted";
    }
    catch (const std::runtime_error& error)
    {
        EXPECT_NE(std::string(error.what()).find("VT package integrity mismatch"), std::string::npos);
    }
}

TEST_F(TerrainCompositedVTStoreTest, RejectsInteriorGapAndUnsortedManifestOffsets)
{
    Publish();
    const auto original = Document();
    auto document = original;
    document["pages"][1]["offset"] = document["pages"][1]["offset"].get<uint64_t>() + 1;
    WriteDocument(document);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::runtime_error);
    document = original;
    std::swap(document["pages"][0], document["pages"][1]);
    WriteDocument(document);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::runtime_error);
}

TEST_F(TerrainCompositedVTStoreTest, AtomicReplacementRetainsOldPackageAndOldReader)
{
    Publish();
    const auto oldStore = TerrainCompositedVTStore::Open(manifest);
    const auto oldPage = oldStore.ReadPage(pages.front().key);
    pages.front().bytes.front() ^= 0x55;
    desc.dependencyHash = ContentHasher::HashString("new-snapshot");
    Publish();
    const auto current = TerrainCompositedVTStore::Open(manifest);
    EXPECT_NE(oldStore.PackagePath(), current.PackagePath());
    EXPECT_TRUE(std::filesystem::exists(oldStore.PackagePath()));
    EXPECT_EQ(oldStore.ReadPage(pages.front().key), oldPage);
    EXPECT_EQ(current.ReadPage(pages.front().key), pages.front().bytes);
    const auto before = ReadBinaryFile(manifest);
    Publish();
    EXPECT_EQ(ReadBinaryFile(manifest), before);
}

TEST_F(TerrainCompositedVTStoreTest, ManifestRejectsVersionDimensionsFormatsAndIdentityChanges)
{
    Publish();
    const auto original = Document();
    auto changed = original;
    changed["version"] = 1;
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    changed = original;
    changed["storedExtent"] = 128;
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    changed = original;
    changed["surfaces"][0]["width"] = uint64_t(1) << 32;
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    changed = original;
    changed["surfaces"][0]["channels"][0] = "unknown";
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    changed = original;
    changed["surfaces"][0]["dependencyHash"] = ContentHasher::HashString("changed").ToHex();
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, ManifestRejectsOffsetsOverflowOverlapGapsAndWrongSizes)
{
    Publish();
    const auto original = Document();
    for (const auto offset : {uint64_t(0), uint64_t(17), UINT64_MAX})
    {
        auto changed = original;
        changed["pages"][0]["offset"] = offset;
        WriteDocument(changed);
        EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    }
    auto changed = original;
    changed["pages"][1]["offset"] = changed["pages"][0]["offset"];
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    changed = original;
    changed["pages"][0]["size"] = UINT64_MAX;
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, ManifestRejectsMissingDuplicateAndOutOfRangePages)
{
    Publish();
    const auto original = Document();
    auto changed = original;
    changed["pages"].erase(changed["pages"].end() - 1);
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    changed = original;
    changed["pages"][1]["mip"] = changed["pages"][0]["mip"];
    changed["pages"][1]["x"] = changed["pages"][0]["x"];
    changed["pages"][1]["y"] = changed["pages"][0]["y"];
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    changed = original;
    changed["pages"][0]["x"] = UINT32_MAX;
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, ManifestRejectsTraversalAbsoluteAdsAndTemporaryPaths)
{
    Publish();
    const auto original = Document();
    for (const auto* filename : {"../outside.vlcvt", "C:/outside.vlcvt",
        "/outside.vlcvt", "page.vlcvt:stream", "page.vlcvt.tmp", "..\\outside.vlcvt"})
    {
        auto changed = original;
        changed["package"] = filename;
        WriteDocument(changed);
        EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    }
}

TEST_F(TerrainCompositedVTStoreTest, ManifestRejectsDuplicateUnknownFieldsAndNonintegerNumbers)
{
    Publish();
    const auto original = Document();
    WriteTextFileAtomically(manifest, "{\"version\":1," + original.dump().substr(1));
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    auto changed = original;
    changed["extra"] = true;
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    for (const auto& value : {nlohmann::json(-1), nlohmann::json(1.0), nlohmann::json(true)})
    {
        changed = original;
        changed["version"] = value;
        WriteDocument(changed);
        EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    }
}

TEST_F(TerrainCompositedVTStoreTest, CorruptionAfterOpenFailsReadAndReopen)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    auto bytes = ReadBinaryFile(store.PackagePath());
    bytes[static_cast<size_t>(store.Pages().at(pages.front().key).offset)] ^= 1;
    WriteBinaryFileAtomically(store.PackagePath(), bytes);
    EXPECT_THROW(store.ReadPage(pages.front().key), std::exception);
    EXPECT_NO_THROW(TerrainCompositedVTStore::Open(manifest));
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest).ValidateAll(), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, CorruptHeaderTruncationAndMissingPackageFail)
{
    Publish();
    const auto path = TerrainCompositedVTStore::Open(manifest).PackagePath();
    const auto original = ReadBinaryFile(path);
    auto bytes = original;
    bytes[8] = 1;
    WriteBinaryFileAtomically(path, bytes);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    bytes = original;
    bytes.pop_back();
    WriteBinaryFileAtomically(path, bytes);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    std::filesystem::remove(path);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, ByteAndPageLimitsRejectBeforeReplacingManifest)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    TerrainCompositedVTStoreLimits limits;
    limits.maxPages = 1;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, Publication(), pages, limits), std::exception);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest, limits), std::exception);
    limits = {};
    limits.maxPackageBytes = 16;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, Publication(), pages, limits), std::exception);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest, limits), std::exception);
    limits = {};
    limits.maxManifestBytes = 1;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, Publication(), pages, limits), std::exception);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest, limits), std::exception);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
}

TEST_F(TerrainCompositedVTStoreTest, IndividualPageDigestMismatchFailsEvenWithValidPackageDigest)
{
    Publish();
    auto changed = Document();
    changed["pages"][0]["hash"] = ContentHasher::HashString("invalid-page").ToHex();
    WriteDocument(changed);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest).ValidateAll(), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, SymlinkPackageIsRejectedWhenPlatformAllowsCreation)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto backup = root / "preserved-by-test";
    std::filesystem::rename(store.PackagePath(), backup);
    std::error_code error;
    std::filesystem::create_symlink(backup, store.PackagePath(), error);
    if (error)
    {
        GTEST_SKIP() << "Symlink creation unavailable: " << error.message();
    }
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    EXPECT_THROW(store.ReadPage(pages.front().key), std::exception);
    EXPECT_THROW(Publish(), std::exception);
}

TEST_F(TerrainCompositedVTStoreTest, StreamProducerMatchesVectorPackageAndCanonicalOrder)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto oldStore = TerrainCompositedVTStore::Open(manifest);
    std::map<TerrainCompositedVTPageKey, std::vector<uint8_t>> content;
    for (const auto& page : pages)
        content.emplace(page.key, page.bytes);
    std::vector<TerrainCompositedVTPageKey> requested;
    TerrainCompositedVTStore::Publish(manifest, Publication(),
        TerrainCompositedVTStore::PageProducer([&](const auto& key)
        {
            requested.push_back(key);
            return content.at(key);
        }));
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_TRUE(std::is_sorted(requested.begin(), requested.end()));
    EXPECT_EQ(requested.size(), pages.size());
    const auto current = TerrainCompositedVTStore::Open(manifest);
    EXPECT_EQ(current.PackagePath(), oldStore.PackagePath());
    for (const auto& page : pages)
        EXPECT_EQ(current.ReadPage(page.key), page.bytes);
}

TEST_F(TerrainCompositedVTStoreTest, InterruptedStreamKeepsOldManifestPackageAndRemovesStaging)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto oldStore = TerrainCompositedVTStore::Open(manifest);
    size_t calls = 0;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, Publication(),
        TerrainCompositedVTStore::PageProducer([&](const auto&)
        {
            if (++calls == 3)
                throw std::runtime_error("injected GPU producer failure");
            return std::vector<uint8_t>(static_cast<size_t>(desc.PageBytes()), 99);
        })), std::runtime_error);
    EXPECT_EQ(calls, 3u);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_EQ(oldStore.ReadPage(pages.front().key), pages.front().bytes);
    for (const auto& entry : std::filesystem::directory_iterator(root))
        EXPECT_EQ(entry.path().filename().string().find(".vlcvt-publish-"), std::string::npos);
}

TEST_F(TerrainCompositedVTStoreTest, StreamRejectsWrongPageSizeAndDiskBudgetBeforePublication)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, Publication(),
        TerrainCompositedVTStore::PageProducer([](const auto&)
        {
            return std::vector<uint8_t>(1);
        })), std::runtime_error);
    TerrainCompositedVTStoreLimits limits;
    limits.maxPackageBytes = 64;
    uint32_t calls = 0;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, Publication(),
        TerrainCompositedVTStore::PageProducer([&](const auto&)
        {
            ++calls;
            return std::vector<uint8_t>(static_cast<size_t>(desc.PageBytes()));
        }), limits), std::runtime_error);
    EXPECT_EQ(calls, 0u);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, Publication(),
        TerrainCompositedVTStore::PageProducer{}), std::runtime_error);
}

TEST_F(TerrainCompositedVTStoreTest, StreamsFourKThreeChannelPackageAbove64MiBWithOnePagePayload)
{
    desc.width = desc.height = 4096;
    desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
        TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
    const auto layout = desc.Layout();
    uint64_t calls = 0;
    size_t largestPayload = 0;
    TerrainCompositedVTStoreLimits limits;
    limits.maxPackageBytes = 512ull * 1024 * 1024;
    const auto valueFor = [](const TerrainCompositedVTPageKey& key)
    {
        return static_cast<uint8_t>((key.mip * 31 + key.x * 7 + key.y) % 251);
    };
    auto publication = Publication();
    publication.packagedPages.clear();
    for (uint32_t mip = 0; mip < layout.MipCount(); ++mip)
        for (uint32_t pageX = 0; pageX < layout.Mip(mip).pagesX; ++pageX)
            for (uint32_t pageY = 0; pageY < layout.Mip(mip).pagesY; ++pageY)
                publication.packagedPages.push_back({desc.surfaceId, mip, pageX, pageY});
    TerrainCompositedVTStore::Publish(manifest, publication,
        TerrainCompositedVTStore::PageProducer([&](const auto& key)
        {
            ++calls;
            std::vector<uint8_t> bytes(static_cast<size_t>(desc.PageBytes()), valueFor(key));
            largestPayload = std::max(largestPayload, bytes.size());
            return bytes;
        }), limits);
    EXPECT_EQ(calls, layout.PageCount());
    EXPECT_EQ(largestPayload, desc.PageBytes());
    const auto store = TerrainCompositedVTStore::Open(manifest, limits);
    EXPECT_GT(std::filesystem::file_size(store.PackagePath()), 64ull * 1024 * 1024);
    EXPECT_EQ(std::filesystem::file_size(store.PackagePath()), 16 + layout.PageCount() * desc.PageBytes());
    EXPECT_EQ(store.Pages().size(), layout.PageCount());
    for (const auto& key : {TerrainCompositedVTPageKey{desc.surfaceId, 0, 31, 19},
        TerrainCompositedVTPageKey{desc.surfaceId, layout.MipCount() - 1, 0, 0}})
    {
        const auto bytes = store.ReadPage(key);
        EXPECT_TRUE(std::all_of(bytes.begin(), bytes.end(), [&](uint8_t byte)
            { return byte == valueFor(key); }));
    }
}

}
