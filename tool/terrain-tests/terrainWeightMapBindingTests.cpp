#include <gtest/gtest.h>

#include <string>
#include <memory>
#include <stdexcept>
#include <vector>

#include "terrain/terrainWeightMapBinding.h"

namespace
{

const void* Identity(int& value)
{
    return static_cast<const void*>(&value);
}

} // namespace

TEST(TerrainWeightMapBindingTests, SinglePlacementIsValid)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|a");

    EXPECT_EQ(tracker.PlacementCount(), 1u);
    EXPECT_TRUE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, SharedIdentityReportsBothPlacements)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|b");

    EXPECT_EQ(tracker.PlacementCount(), 2u);
    const std::string error = tracker.SharedBindingError();
    EXPECT_FALSE(error.empty());
    EXPECT_NE(error.find("MI_terrain.json"), std::string::npos);
    EXPECT_NE(error.find("shared by 2 placements"), std::string::npos);
    EXPECT_NE(error.find("terrain|a"), std::string::npos);
    EXPECT_NE(error.find("terrain|b"), std::string::npos);
}

TEST(TerrainWeightMapBindingTests, DistinctIdentitiesAreValid)
{
    int weightsA = 0;
    int weightsB = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weightsA), "MI_a.json", "terrain|a");
    tracker.Record(Identity(weightsB), "MI_b.json", "terrain|b");

    EXPECT_EQ(tracker.PlacementCount(), 2u);
    EXPECT_TRUE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, DuplicatePlacementDoesNotGrowCount)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|a");

    EXPECT_EQ(tracker.PlacementCount(), 1u);
    EXPECT_TRUE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, ThreePlacementsAreListedInInsertionOrder)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|first");
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|second");
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|third");

    const std::string error = tracker.SharedBindingError();
    EXPECT_NE(error.find("shared by 3 placements"), std::string::npos);
    const size_t first = error.find("terrain|first");
    const size_t second = error.find("terrain|second");
    const size_t third = error.find("terrain|third");
    EXPECT_NE(first, std::string::npos);
    EXPECT_NE(second, std::string::npos);
    EXPECT_NE(third, std::string::npos);
    EXPECT_LT(first, second);
    EXPECT_LT(second, third);
}

TEST(TerrainWeightMapBindingTests, FirstMaterialPathIsUsed)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_first.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_second.json", "terrain|b");

    const std::string error = tracker.SharedBindingError();
    EXPECT_NE(error.find("MI_first.json"), std::string::npos);
    EXPECT_EQ(error.find("MI_second.json"), std::string::npos);
}

TEST(TerrainWeightMapBindingTests, MultipleSharedGroupsReportOnlyTheFirst)
{
    int weightsA = 0;
    int weightsB = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weightsA), "MI_first.json", "terrain|a1");
    tracker.Record(Identity(weightsA), "MI_first.json", "terrain|a2");
    tracker.Record(Identity(weightsB), "MI_second.json", "terrain|b1");
    tracker.Record(Identity(weightsB), "MI_second.json", "terrain|b2");

    const std::string error = tracker.SharedBindingError();
    EXPECT_NE(error.find("MI_first.json"), std::string::npos);
    EXPECT_NE(error.find("terrain|a1"), std::string::npos);
    EXPECT_NE(error.find("terrain|a2"), std::string::npos);
    EXPECT_EQ(error.find("MI_second.json"), std::string::npos);
    EXPECT_EQ(error.find("terrain|b1"), std::string::npos);
    EXPECT_EQ(error.find("terrain|b2"), std::string::npos);
}

TEST(TerrainWeightMapBindingTests, DuplicatePlacementKeepsFirstMaterialPath)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_first.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_other.json", "terrain|a");
    EXPECT_EQ(tracker.PlacementCount(), 1u);

    tracker.Record(Identity(weights), "MI_third.json", "terrain|b");
    const std::string error = tracker.SharedBindingError();
    EXPECT_NE(error.find("MI_first.json"), std::string::npos);
    EXPECT_EQ(error.find("MI_other.json"), std::string::npos);
    EXPECT_EQ(error.find("MI_third.json"), std::string::npos);
}

TEST(TerrainWeightMapBindingTests, MessageNamesBothRemedies)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|b");

    const std::string error = tracker.SharedBindingError();
    EXPECT_NE(error.find("set u_terrainWeightMapMode=0"), std::string::npos);
    EXPECT_NE(error.find("own layerWeightMaps texture"), std::string::npos);
}

TEST(TerrainWeightMapBindingTests, ClearRemovesAllBindings)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_terrain.json", "terrain|b");
    EXPECT_FALSE(tracker.SharedBindingError().empty());

    tracker.Clear();

    EXPECT_EQ(tracker.PlacementCount(), 0u);
    EXPECT_TRUE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, ClearAllowsRecordingAgain)
{
    int weightsA = 0;
    int weightsB = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weightsA), "MI_old.json", "terrain|old");
    tracker.Clear();

    tracker.Record(Identity(weightsB), "MI_new.json", "terrain|new");
    EXPECT_EQ(tracker.PlacementCount(), 1u);
    EXPECT_TRUE(tracker.SharedBindingError().empty());

    tracker.Record(Identity(weightsB), "MI_new.json", "terrain|new_second");
    EXPECT_NE(tracker.SharedBindingError().find("MI_new.json"), std::string::npos);
}

TEST(TerrainWeightMapBindingTests, SharedImportedBaseWithSixteenFinalOverlaysIsValid)
{
    const auto imported = std::make_shared<std::vector<float>>(7 * 16, 0.25f);
    std::vector<std::shared_ptr<std::vector<float>>> overlays;
    VL::TerrainWeightMapBindingTracker tracker;
    for (size_t tile = 0; tile < 16; ++tile)
    {
        auto overlay = std::make_shared<std::vector<float>>(*imported);
        overlay->at(tile) = 0.75f;
        overlays.push_back(overlay);
        const auto placement = "terrain|Landscape_" + std::to_string(tile);
        tracker.RecordFrozenOverlay(imported.get(), overlay.get(),
            "MI_base_" + std::to_string(tile) + ".json", placement);
        tracker.RecordFrozenOverlay(imported.get(), overlay.get(),
            "MI_detail_" + std::to_string(tile) + ".json", placement);
    }
    EXPECT_EQ(tracker.PlacementCount(), 16u);
    EXPECT_TRUE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, EqualOverlayBytesDoNotRequireDistinctImportedBases)
{
    const std::vector<float> imported(7 * 16, 0.25f);
    const auto overlayA = std::make_unique<std::vector<float>>(imported);
    const auto overlayB = std::make_unique<std::vector<float>>(imported);
    ASSERT_EQ(*overlayA, *overlayB);
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.RecordFrozenOverlay(&imported, overlayA.get(), "MI_a.json", "terrain|a");
    tracker.RecordFrozenOverlay(&imported, overlayB.get(), "MI_b.json", "terrain|b");
    EXPECT_TRUE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, ImportedBaseCannotMasqueradeAsFinalOverlay)
{
    int imported = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    EXPECT_THROW(tracker.RecordFrozenOverlay(Identity(imported), Identity(imported),
        "MI_a.json", "terrain|a"), std::invalid_argument);
    EXPECT_EQ(tracker.PlacementCount(), 0u);
}

TEST(TerrainWeightMapBindingTests, MissingFinalOverlayDoesNotRecordImportedBase)
{
    int imported = 0;
    int overlay = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    EXPECT_THROW(tracker.RecordFrozenOverlay(Identity(imported), nullptr,
        "MI_a.json", "terrain|a"), std::invalid_argument);
    EXPECT_THROW(tracker.RecordFrozenOverlay(nullptr, Identity(overlay),
        "MI_a.json", "terrain|a"), std::invalid_argument);
    EXPECT_EQ(tracker.PlacementCount(), 0u);
}

TEST(TerrainWeightMapBindingTests, SharedFinalOverlayStillRejectsDistinctPlacements)
{
    int imported = 0;
    int overlay = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.RecordFrozenOverlay(Identity(imported), Identity(overlay), "MI_a.json", "terrain|a");
    tracker.RecordFrozenOverlay(Identity(imported), Identity(overlay), "MI_b.json", "terrain|b");
    EXPECT_FALSE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, VerifiedPublicationMayShareOneRealIdentity)
{
    const auto weights = std::make_shared<std::vector<float>>(7 * 16, 0.25f);
    VL::TerrainWeightMapBindingTracker tracker;
    for (size_t tile = 0; tile < 16; ++tile)
    {
        const auto placement = "terrain|Landscape_" + std::to_string(tile);
        tracker.Record(weights.get(), "MI_base_" + std::to_string(tile), placement,
            VL::TerrainWeightMapBindingTracker::BindingPolicy::VerifiedImmutablePublication);
        tracker.Record(weights.get(), "MI_detail_" + std::to_string(tile), placement,
            VL::TerrainWeightMapBindingTracker::BindingPolicy::VerifiedImmutablePublication);
    }
    EXPECT_EQ(tracker.PlacementCount(), 16u);
    EXPECT_TRUE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, NativeRecordAfterVerifiedPublicationStillRejectsSharing)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_published.json", "terrain|a",
        VL::TerrainWeightMapBindingTracker::BindingPolicy::VerifiedImmutablePublication);
    tracker.Record(Identity(weights), "MI_author.json", "terrain|b");
    EXPECT_FALSE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, VerifiedPublicationCannotRelaxExistingNativeRecord)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_author.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_published.json", "terrain|b",
        VL::TerrainWeightMapBindingTracker::BindingPolicy::VerifiedImmutablePublication);
    EXPECT_FALSE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, DuplicatePlacementCannotHideMutableBinding)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_base.json", "terrain|a",
        VL::TerrainWeightMapBindingTracker::BindingPolicy::VerifiedImmutablePublication);
    tracker.Record(Identity(weights), "MI_detail.json", "terrain|a");
    EXPECT_EQ(tracker.PlacementCount(), 1u);
    tracker.Record(Identity(weights), "MI_b.json", "terrain|b",
        VL::TerrainWeightMapBindingTracker::BindingPolicy::VerifiedImmutablePublication);
    EXPECT_FALSE(tracker.SharedBindingError().empty());
}

TEST(TerrainWeightMapBindingTests, NativeRecordRequiresConcreteBindingIdentity)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    EXPECT_THROW(tracker.Record(nullptr, "MI_a.json", "terrain|a"), std::invalid_argument);
    EXPECT_THROW(tracker.Record(Identity(weights), "", "terrain|a"), std::invalid_argument);
    EXPECT_THROW(tracker.Record(Identity(weights), "MI_a.json", ""), std::invalid_argument);
    EXPECT_EQ(tracker.PlacementCount(), 0u);
}

TEST(TerrainWeightMapBindingTests, ClearRemovesPublicationSharingExemption)
{
    int weights = 0;
    VL::TerrainWeightMapBindingTracker tracker;
    tracker.Record(Identity(weights), "MI_published.json", "terrain|a",
        VL::TerrainWeightMapBindingTracker::BindingPolicy::VerifiedImmutablePublication);
    tracker.Clear();
    tracker.Record(Identity(weights), "MI_a.json", "terrain|a");
    tracker.Record(Identity(weights), "MI_b.json", "terrain|b");
    EXPECT_FALSE(tracker.SharedBindingError().empty());
}
