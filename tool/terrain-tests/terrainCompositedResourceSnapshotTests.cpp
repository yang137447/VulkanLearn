#include <gtest/gtest.h>
#include "render/resource/terrainCompositedResourceSnapshot.h"

namespace VL
{
namespace
{
void SubmitInputs(TerrainCompositedResourceSnapshot& snapshot)
{
    snapshot.RecordInputs();
    snapshot.SubmitInputs(4);
    ASSERT_TRUE(snapshot.InputsCompleted(4));
}
}

TEST(TerrainCompositedResourceSnapshot, InputFencePrecedesAnyCandidateProduction)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    snapshot.RecordInputs();
    snapshot.SubmitInputs(4);
    EXPECT_FALSE(snapshot.InputsCompleted(3));
    EXPECT_FALSE(snapshot.ObserveProduction(7, "terrain|a", true, 5, 0, true));
    EXPECT_TRUE(snapshot.InputsCompleted(4));
    EXPECT_TRUE(snapshot.ObserveProduction(7, "terrain|a", true, 5, 0, true));
}

TEST(TerrainCompositedResourceSnapshot, RootStatusAloneCannotPublishBeforeTableFence)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    SubmitInputs(snapshot);
    ASSERT_TRUE(snapshot.ObserveProduction(7, "terrain|a", true, 6, 0, true));
    EXPECT_FALSE(snapshot.ConfirmTables(5, true));
    EXPECT_FALSE(snapshot.ConfirmTables(6, false));
    EXPECT_THROW(snapshot.Publish(), std::logic_error);
    EXPECT_TRUE(snapshot.ConfirmTables(7, true));
    EXPECT_NO_THROW(snapshot.Publish());
}

TEST(TerrainCompositedResourceSnapshot, EntireWorldRootsMustUseTheCandidateSnapshot)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a", "terrain|b"});
    SubmitInputs(snapshot);
    ASSERT_TRUE(snapshot.ObserveProduction(7, "terrain|a", true, 5, 0, true));
    EXPECT_FALSE(snapshot.ConfirmTables(9, true));
    EXPECT_FALSE(snapshot.ObserveProduction(6, "terrain|b", true, 6, 0, true));
    EXPECT_FALSE(snapshot.ConfirmTables(9, true));
    ASSERT_TRUE(snapshot.ObserveProduction(7, "terrain|b", true, 7, 0, true));
    EXPECT_TRUE(snapshot.ConfirmTables(9, true));
}

TEST(TerrainCompositedResourceSnapshot, FinePageCompletionCannotPretendToBeRoot)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    SubmitInputs(snapshot);
    ASSERT_TRUE(snapshot.ObserveProduction(7, "terrain|a", false, 5, 0, true));
    EXPECT_FALSE(snapshot.ConfirmTables(9, true));
}

TEST(TerrainCompositedResourceSnapshot, BadStatusLeavesCandidateUnpublishable)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    SubmitInputs(snapshot);
    EXPECT_FALSE(snapshot.ObserveProduction(7, "terrain|a", true, 5, 2, true));
    EXPECT_EQ(snapshot.Phase(), TerrainCompositedSnapshotPhase::Failed);
    EXPECT_FALSE(snapshot.ConfirmTables(9, true));
    EXPECT_THROW(snapshot.Publish(), std::logic_error);
    EXPECT_EQ(snapshot.LastUseEpoch(), 5u);
}

TEST(TerrainCompositedResourceSnapshot, NamespaceRejectionDoesNotPublishNewIdentity)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    SubmitInputs(snapshot);
    EXPECT_FALSE(snapshot.ObserveProduction(7, "terrain|a", true, 5, 0, false));
    EXPECT_EQ(snapshot.Phase(), TerrainCompositedSnapshotPhase::Failed);
    EXPECT_FALSE(snapshot.ConfirmTables(9, true));
}

TEST(TerrainCompositedResourceSnapshot, SubmitFailureCanAbortAndReuseInputTransaction)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    snapshot.RecordInputs();
    EXPECT_THROW(snapshot.SubmitInputs(0), std::logic_error);
    snapshot.AbortInputs();
    EXPECT_EQ(snapshot.Phase(), TerrainCompositedSnapshotPhase::Prepared);
    SubmitInputs(snapshot);
    EXPECT_EQ(snapshot.InputEpoch(), 4u);
}

TEST(TerrainCompositedResourceSnapshot, CancelledSnapshotIgnoresLateStatus)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    SubmitInputs(snapshot);
    snapshot.Cancel();
    EXPECT_FALSE(snapshot.ObserveProduction(7, "terrain|a", true, 5, 0, true));
    EXPECT_FALSE(snapshot.ConfirmTables(9, true));
    EXPECT_THROW(snapshot.Publish(), std::logic_error);
}

TEST(TerrainCompositedResourceSnapshot, PublishedVersionCannotReturnToCandidateState)
{
    TerrainCompositedResourceSnapshot snapshot(1, 7, {"terrain|a"});
    SubmitInputs(snapshot);
    ASSERT_TRUE(snapshot.ObserveProduction(7, "terrain|a", true, 5, 0, true));
    ASSERT_TRUE(snapshot.ConfirmTables(6, true));
    snapshot.Publish();
    EXPECT_THROW(snapshot.Cancel(), std::logic_error);
    EXPECT_THROW(snapshot.Fail("late"), std::logic_error);
    EXPECT_EQ(snapshot.Phase(), TerrainCompositedSnapshotPhase::Published);
}
}
