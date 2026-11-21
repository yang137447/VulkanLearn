#include <gtest/gtest.h>

#include "editor/terrain/terrainEditSession.h"
#include "editor/terrain/terrainEditApply.h"
#include "editor/terrain/terrainPaintSession.h"
#include "editor/terrain/terrainCompositedAuthorMonitor.h"
#include "terrain/terrainCompositedDependencies.h"
#include "terrainCompositedProductionFixture.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace
{
using namespace VL;
using namespace VL::Editor;

TerrainCollisionInstance Instance(uint32_t margin = 0)
{
    TerrainCollisionInstance instance;
    instance.placementId = "terrain|refresh";
    auto& plan = instance.plan;
    plan.placementId = instance.placementId;
    plan.desc.assetPath = "Maps/TR_refresh.json";
    plan.desc.name = "refresh";
    plan.desc.heightmapPath = "Maps/height.exr";
    plan.desc.materialInstancePath = "Maps/MI_refresh.json";
    plan.desc.size = Eigen::Vector2f(1000, 1000);
    plan.desc.heightScale = 100;
    plan.desc.gridResolution = 32;
    plan.desc.chunkQuads = 16;
    plan.desc.lodLevels = 1;
    plan.desc.heightmapMargin = margin;
    TerrainLayerDesc first;
    first.name = "first";
    TerrainLayerDesc second;
    second.name = "second";
    plan.desc.layers = {first, second};
    auto image = std::make_shared<HostImage>();
    image->width = 33 + 2 * margin;
    image->height = image->width;
    image->channels = 1;
    image->format = HostImage::PixelFormat::R32_FLOAT;
    image->rowStrideBytes = image->width * sizeof(float);
    image->data.resize(size_t(image->rowStrideBytes) * image->height);
    const float value = 0.25f;
    for (size_t offset = 0; offset < image->data.size(); offset += sizeof(float))
        std::memcpy(image->data.data() + offset, &value, sizeof(value));
    plan.heightmap = image;
    return instance;
}

TerrainPaintBrush PaintBrush(float horizontal = 0.5f)
{
    TerrainPaintBrush brush;
    brush.layerIndex = 1;
    brush.centerUv = Eigen::Vector2f(horizontal, 0.5f);
    brush.radiusUv = 0.04f;
    brush.flow = 1;
    brush.opacity = 1;
    brush.falloff = 0;
    return brush;
}

TerrainSculptBrush SculptBrush(float horizontal = 0.5f)
{
    TerrainSculptBrush brush;
    brush.centerUv = Eigen::Vector2f(horizontal, 0.5f);
    brush.radiusUv = 0.08f;
    brush.strength = 0.1f;
    brush.falloff = 0;
    return brush;
}

void ExpectLocalized(const TerrainCompositedEditDirty& dirty)
{
    ASSERT_TRUE(dirty.valid);
    EXPECT_GT(dirty.rect.minU, 0);
    EXPECT_GT(dirty.rect.minV, 0);
    EXPECT_LT(dirty.rect.maxU, 1);
    EXPECT_LT(dirty.rect.maxV, 1);
    EXPECT_LT(dirty.rect.minU, dirty.rect.maxU);
    EXPECT_LT(dirty.rect.minV, dirty.rect.maxV);
}
}

TEST(TerrainCompositedRefresh, LateAcknowledgementPreservesUnionOfNewEdits)
{
    TerrainCompositedEditDirty dirty;
    dirty.Include({0.1, 0.2, 0.3, 0.4}, 1);
    dirty.Include({0.6, 0.7, 0.8, 0.9}, 2);
    EXPECT_FALSE(dirty.Acknowledge(1));
    EXPECT_TRUE(dirty.valid);
    EXPECT_DOUBLE_EQ(dirty.rect.minU, 0.1);
    EXPECT_DOUBLE_EQ(dirty.rect.maxV, 0.9);
    EXPECT_TRUE(dirty.Acknowledge(2));
    EXPECT_FALSE(dirty.valid);
    EXPECT_FALSE(dirty.Acknowledge(2));
    EXPECT_THROW(dirty.Include({0, 0, 1, 1}, 1), std::exception);
}

TEST(TerrainCompositedRefresh, PaintPublishesImmutableDataAndLocalizedDirty)
{
    TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(TerrainPaintSession::Create(Instance(), session, error)) << error;
    const auto baseline = session.PaintData();
    const auto bytes = baseline->weights;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(PaintBrush()));
    const auto edited = session.PaintData();
    EXPECT_NE(edited, baseline);
    EXPECT_EQ(session.PaintData(), edited);
    EXPECT_EQ(baseline->weights, bytes);
    ExpectLocalized(session.CompositedDirty());
    EXPECT_EQ(session.CompositedDirty().revision, session.Revision());
}

TEST(TerrainCompositedRefresh, PaintUndoRedoProducesNewRevisionWithoutMutatingSnapshots)
{
    TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(TerrainPaintSession::Create(Instance(), session, error)) << error;
    const auto baseline = session.PaintData();
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(PaintBrush()));
    ASSERT_TRUE(session.EndStroke());
    const auto painted = session.PaintData();
    const auto paintedValues = painted->weights;
    const uint64_t paintedRevision = session.Revision();
    ASSERT_TRUE(session.AcknowledgeCompositedDirty(paintedRevision));
    ASSERT_TRUE(session.Undo());
    EXPECT_GT(session.Revision(), paintedRevision);
    EXPECT_EQ(session.PaintData()->weights, baseline->weights);
    EXPECT_EQ(painted->weights, paintedValues);
    ExpectLocalized(session.CompositedDirty());
    ASSERT_TRUE(session.AcknowledgeCompositedDirty(session.Revision()));
    ASSERT_TRUE(session.Redo());
    EXPECT_EQ(session.PaintData()->weights, paintedValues);
    ExpectLocalized(session.CompositedDirty());
}

TEST(TerrainCompositedRefresh, PaintCancelInvalidatesAlreadyPublishedStroke)
{
    TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(TerrainPaintSession::Create(Instance(), session, error)) << error;
    const auto baseline = session.PaintData();
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(PaintBrush()));
    const auto painted = session.PaintData();
    const auto revision = session.Revision();
    ASSERT_TRUE(session.AcknowledgeCompositedDirty(revision));
    session.CancelStroke();
    EXPECT_GT(session.Revision(), revision);
    EXPECT_EQ(session.PaintData()->weights, baseline->weights);
    EXPECT_NE(painted->weights, baseline->weights);
    ExpectLocalized(session.CompositedDirty());
    EXPECT_FALSE(session.CanUndo());
}

TEST(TerrainCompositedRefresh, PaintStaleAckKeepsSubsequentStroke)
{
    TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(TerrainPaintSession::Create(Instance(), session, error)) << error;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(PaintBrush(0.25f)));
    const auto revision = session.Revision();
    ASSERT_TRUE(session.ApplyBrush(PaintBrush(0.75f)));
    EXPECT_FALSE(session.AcknowledgeCompositedDirty(revision));
    EXPECT_LT(session.CompositedDirty().rect.minU, 0.25);
    EXPECT_GT(session.CompositedDirty().rect.maxU, 0.75);
}

TEST(TerrainCompositedRefresh, NoOpPaintDoesNotCreateDirtyOrRevision)
{
    TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(TerrainPaintSession::Create(Instance(), session, error)) << error;
    ASSERT_TRUE(session.BeginStroke());
    auto brush = PaintBrush();
    brush.flow = 0;
    EXPECT_FALSE(session.ApplyBrush(brush));
    EXPECT_EQ(session.Revision(), 0u);
    EXPECT_FALSE(session.CompositedDirty().valid);
    session.CancelStroke();
    EXPECT_FALSE(session.CompositedDirty().valid);
}

TEST(TerrainCompositedRefresh, SculptSnapshotsRemainImmutableAcrossEdits)
{
    TerrainEditSession session;
    std::string error;
    ASSERT_TRUE(TerrainEditSession::Create(Instance(), session, error)) << error;
    const auto baseline = session.BuildUpdate();
    const auto bytes = baseline.heightmap->data;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(SculptBrush()));
    const auto edited = session.BuildUpdate();
    EXPECT_EQ(edited.sourceRevision, session.Revision());
    EXPECT_NE(edited.heightmap, baseline.heightmap);
    EXPECT_EQ(session.BuildUpdate().heightmap, edited.heightmap);
    EXPECT_EQ(baseline.heightmap->data, bytes);
    EXPECT_NE(edited.heightmap->data, bytes);
    ExpectLocalized(edited.compositedDirty);
}

TEST(TerrainCompositedRefresh, SculptUndoRedoKeepsPreciseMaterialDirtyDespiteFullMeshRebuild)
{
    TerrainEditSession session;
    std::string error;
    ASSERT_TRUE(TerrainEditSession::Create(Instance(), session, error)) << error;
    const auto baseline = session.BuildUpdate();
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(SculptBrush()));
    ASSERT_TRUE(session.EndStroke());
    const auto edited = session.BuildUpdate();
    ASSERT_TRUE(session.AcknowledgeCompositedDirty(edited.sourceRevision));
    session.ResetDirty();
    ASSERT_TRUE(session.Undo());
    const auto undone = session.BuildUpdate();
    EXPECT_EQ(undone.heightmap->data, baseline.heightmap->data);
    EXPECT_GT(undone.sourceRevision, edited.sourceRevision);
    ExpectLocalized(undone.compositedDirty);
    EXPECT_EQ(undone.chunks.size(), 4u);
    ASSERT_TRUE(session.AcknowledgeCompositedDirty(undone.sourceRevision));
    session.ResetDirty();
    ASSERT_TRUE(session.Redo());
    const auto redone = session.BuildUpdate();
    EXPECT_EQ(redone.heightmap->data, edited.heightmap->data);
    ExpectLocalized(redone.compositedDirty);
}

TEST(TerrainCompositedRefresh, SculptCancelRestoresDataAndRetainsRefreshAfterGeometryReset)
{
    TerrainEditSession session;
    std::string error;
    ASSERT_TRUE(TerrainEditSession::Create(Instance(), session, error)) << error;
    const auto baseline = session.BuildUpdate();
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(SculptBrush()));
    const auto edited = session.BuildUpdate();
    ASSERT_TRUE(session.AcknowledgeCompositedDirty(edited.sourceRevision));
    session.ResetDirty();
    session.CancelStroke();
    const auto canceled = session.BuildUpdate();
    EXPECT_EQ(canceled.heightmap->data, baseline.heightmap->data);
    EXPECT_NE(edited.heightmap->data, baseline.heightmap->data);
    session.ResetDirty();
    ExpectLocalized(session.CompositedDirty());
    EXPECT_FALSE(session.AcknowledgeCompositedDirty(edited.sourceRevision));
    EXPECT_TRUE(session.AcknowledgeCompositedDirty(canceled.sourceRevision));
}

TEST(TerrainCompositedRefresh, SculptMarginConvertsHeightmapSamplesIntoSurfaceUv)
{
    TerrainEditSession session;
    std::string error;
    ASSERT_TRUE(TerrainEditSession::Create(Instance(2), session, error)) << error;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(SculptBrush()));
    ExpectLocalized(session.CompositedDirty());
    const auto dirty = session.CompositedDirty();
    EXPECT_NEAR(dirty.rect.minU, 1 - dirty.rect.maxU, 0.04);
    EXPECT_LE(dirty.rect.minU, 0.5);
    EXPECT_GE(dirty.rect.maxU, 0.5);
}

namespace
{
class TerrainCompositedRefreshMonitor : public testing::Test
{
protected:
    using Monitor = TerrainCompositedAuthorMonitor;
    std::filesystem::path directory;
    TerrainCompositedDependencyRoots roots;
    std::unique_ptr<Monitor> monitor;

    void SetUp() override
    {
        directory = std::filesystem::canonical(std::filesystem::temp_directory_path()) /
            ("vl-p4-refresh-" + std::to_string(Monitor::Clock::now().time_since_epoch().count()));
        roots = {directory / "resources", directory / "project"};
        std::filesystem::create_directories(roots.resourceRoot / "Maps");
        std::filesystem::create_directories(roots.projectRoot / "shader/glsl");
        const auto instance = Instance();
        std::ofstream(roots.resourceRoot / "Maps/TR_refresh.json") <<
            R"({"type":"terrain","heightmapPath":"Maps/height.exr","materialInstancePath":"Maps/MI_refresh.json"})";
        WriteHeight("height-before");
        std::ofstream(roots.resourceRoot / "Maps/MI_refresh.json") <<
            R"({"type":"materialInstance","material":"shader/glsl/M_refresh.json"})";
        std::ofstream(roots.projectRoot / "shader/glsl/M_refresh.json") << R"({"type":"material"})";
        const std::array<float, 16> placement{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        const auto manifest = BuildTerrainCompositedDependencies(roots, instance.plan, placement,
            instance.plan.desc.materialInstancePath, nlohmann::json::object());
        monitor = std::make_unique<Monitor>(std::chrono::milliseconds(0));
        std::string error;
        ASSERT_TRUE(monitor->RegisterSurface(instance.placementId, roots, manifest, error)) << error;
    }

    void TearDown() override
    {
        monitor.reset();
        const auto resolved = std::filesystem::canonical(directory);
        ASSERT_EQ(resolved.parent_path(), std::filesystem::canonical(std::filesystem::temp_directory_path()));
        ASSERT_EQ(resolved.filename().string().rfind("vl-p4-refresh-", 0), 0u);
        std::filesystem::remove_all(resolved);
    }

    void WriteHeight(const std::string& bytes)
    {
        std::ofstream output(roots.resourceRoot / "Maps/height.exr", std::ios::binary | std::ios::trunc);
        output << bytes;
    }

    Monitor::SurfaceStatus WaitForCheck(bool external = false)
    {
        const auto deadline = Monitor::Clock::now() + std::chrono::seconds(3);
        Monitor::SurfaceStatus status;
        while (Monitor::Clock::now() < deadline)
        {
            monitor->Poll(Monitor::Clock::now());
            if (monitor->GetStatus("terrain|refresh", status) &&
                status.checked && (!external || status.externalChange))
                return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ADD_FAILURE() << "Timed out waiting for author dependency scan";
        return status;
    }
};
}

TEST_F(TerrainCompositedRefreshMonitor, LocalChangesRetainTypedConsumersUntilSaved)
{
    WaitForCheck();
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Paint));
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::DynamicGlobal));
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Paint));
    Monitor::SurfaceStatus status;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", status));
    ASSERT_EQ(status.localChanges.size(), 2u);
    EXPECT_TRUE(status.dirty);
    EXPECT_EQ(status.localChanges[0], TerrainCompositedDependencyChange::Paint);
    EXPECT_EQ(status.localChanges[1], TerrainCompositedDependencyChange::DynamicGlobal);
    ASSERT_TRUE(monitor->MarkSaved("terrain|refresh"));
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", status));
    EXPECT_FALSE(status.dirty);
    EXPECT_TRUE(status.localChanges.empty());
}

TEST_F(TerrainCompositedRefreshMonitor, ExternalChangeReportsExactDependencyAndBothDigests)
{
    WaitForCheck();
    WriteHeight("height-after-with-new-size");
    const auto status = WaitForCheck(true);
    ASSERT_FALSE(status.changedDependencies.empty());
    const auto& changed = status.changedDependencies.front();
    EXPECT_EQ(changed.root, "resource");
    EXPECT_EQ(changed.path, "Maps/height.exr");
    EXPECT_EQ(changed.mode, "bytes");
    EXPECT_NE(changed.expectedDigest, changed.observedDigest);
    EXPECT_FALSE(changed.observedDigest.empty());
    EXPECT_FALSE(changed.diagnostic.empty());
}

TEST(TerrainCompositedRefresh, NoOpPaintKeepsPublishedStorage)
{
    TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(TerrainPaintSession::Create(Instance(), session, error)) << error;
    const auto baseline = session.PaintData();
    ASSERT_TRUE(session.BeginStroke());
    auto brush = PaintBrush();
    brush.flow = 0;
    EXPECT_FALSE(session.ApplyBrush(brush));
    EXPECT_EQ(session.PaintData(), baseline);
    session.CancelStroke();
    EXPECT_EQ(session.PaintData(), baseline);
}

TEST(TerrainCompositedRefresh, CommandKeepsPaintStorageAcrossFurtherStrokeEdits)
{
    auto instance = Instance();
    TerrainPaintSession session;
    std::string error;
    ASSERT_TRUE(TerrainPaintSession::Create(instance, session, error)) << error;
    ASSERT_TRUE(session.BeginStroke());
    ASSERT_TRUE(session.ApplyBrush(PaintBrush(0.25f)));
    instance.plan.layerPaint = session.PaintData();
    const auto command = TerrainCompositedRefreshCommand::Freeze(instance.plan,
        {instance.placementId, TerrainCompositedDependencyChange::Paint,
            session.CompositedDirty().rect}, 12, 101, session.Revision());
    const auto weights = command.inputs->layerPaint->weights;
    EXPECT_EQ(command.inputs->layerPaint, session.PaintData());
    ASSERT_TRUE(session.ApplyBrush(PaintBrush(0.75f)));
    EXPECT_NE(command.inputs->layerPaint, session.PaintData());
    EXPECT_EQ(command.inputs->layerPaint->weights, weights);
    EXPECT_NE(session.PaintData()->weights, weights);
    EXPECT_EQ(command.changes.front().kind, TerrainCompositedDependencyChange::Paint);
}

TEST(TerrainCompositedRefresh, CommandRejectsOldWorldAndRevision)
{
    const auto instance = Instance();
    const auto command = TerrainCompositedRefreshCommand::Freeze(instance.plan,
        {instance.placementId, TerrainCompositedDependencyChange::Sculpt,
            {0.2, 0.2, 0.4, 0.4}}, 12, 101, 3);
    EXPECT_TRUE(command.IsCurrent(12, 101));
    EXPECT_FALSE(command.IsCurrent(13, 101));
    EXPECT_FALSE(command.IsCurrent(12, 102));
    EXPECT_EQ(command.sourceRevision, 3u);
    EXPECT_EQ(command.changes.front().kind, TerrainCompositedDependencyChange::Sculpt);
}

TEST(TerrainCompositedRefresh, CommandRejectsDifferentPlacementAndEmptyDirty)
{
    const auto instance = Instance();
    EXPECT_THROW(TerrainCompositedRefreshCommand::Freeze(instance.plan,
        {"terrain|other", TerrainCompositedDependencyChange::Paint,
            {0, 0, 1, 1}}, 12, 101, 1), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedRefreshCommand::Freeze(instance.plan,
        {instance.placementId, TerrainCompositedDependencyChange::Paint,
            {}}, 12, 101, 1), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedRefreshCommand::Freeze(instance.plan,
        {instance.placementId, TerrainCompositedDependencyChange::Paint,
            {0, 0, 1, 1}}, 12, 0, 1), std::invalid_argument);
}

TEST_F(TerrainCompositedRefreshMonitor, SavingPaintRetainsSculptAndMaterialDirty)
{
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Paint));
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Sculpt));
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::BaseMaterial));
    Monitor::SurfaceStatus before;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", before));
    ASSERT_TRUE(monitor->MarkSaved("terrain|refresh", TerrainCompositedDependencyChange::Paint,
        before.editRevision, before.generation));
    Monitor::SurfaceStatus after;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", after));
    EXPECT_TRUE(after.dirty);
    ASSERT_EQ(after.localChanges.size(), 2u);
    EXPECT_EQ(after.localChanges[0], TerrainCompositedDependencyChange::Sculpt);
    EXPECT_EQ(after.localChanges[1], TerrainCompositedDependencyChange::BaseMaterial);
    EXPECT_GT(after.generation, before.generation);
    EXPECT_FALSE(monitor->CanRecook("terrain|refresh"));
}

TEST_F(TerrainCompositedRefreshMonitor, LateSaveAcknowledgementRetainsNewEdit)
{
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Paint));
    Monitor::SurfaceStatus before;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", before));
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Paint));
    EXPECT_FALSE(monitor->MarkSaved("terrain|refresh", TerrainCompositedDependencyChange::Paint,
        before.editRevision, before.generation));
    Monitor::SurfaceStatus after;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", after));
    EXPECT_TRUE(after.dirty);
    EXPECT_GT(after.editRevision, before.editRevision);
}

TEST_F(TerrainCompositedRefreshMonitor, TypedSavePreservesExternalConflict)
{
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Paint));
    WriteHeight("externally-changed-height");
    const auto before = WaitForCheck(true);
    ASSERT_TRUE(before.conflict);
    ASSERT_TRUE(monitor->MarkSaved("terrain|refresh", TerrainCompositedDependencyChange::Paint,
        before.editRevision, before.generation));
    Monitor::SurfaceStatus after;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", after));
    EXPECT_FALSE(after.dirty);
    EXPECT_TRUE(after.externalChange);
    EXPECT_TRUE(after.conflict);
    EXPECT_FALSE(monitor->CanRecook("terrain|refresh"));
}

TEST_F(TerrainCompositedRefreshMonitor, SaveFromOldGenerationCannotClearAnotherConsumer)
{
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Paint));
    ASSERT_TRUE(monitor->MarkEdited("terrain|refresh", TerrainCompositedDependencyChange::Sculpt));
    Monitor::SurfaceStatus before;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", before));
    ASSERT_TRUE(monitor->MarkSaved("terrain|refresh", TerrainCompositedDependencyChange::Paint,
        before.editRevision, before.generation));
    EXPECT_FALSE(monitor->MarkSaved("terrain|refresh", TerrainCompositedDependencyChange::Sculpt,
        before.editRevision, before.generation));
    Monitor::SurfaceStatus after;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", after));
    EXPECT_TRUE(after.dirty);
    ASSERT_EQ(after.localChanges.size(), 1u);
    EXPECT_EQ(after.localChanges.front(), TerrainCompositedDependencyChange::Sculpt);
    ASSERT_TRUE(monitor->MarkSaved("terrain|refresh", TerrainCompositedDependencyChange::Sculpt,
        after.editRevision, after.generation));
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", after));
    EXPECT_FALSE(after.dirty);
}

namespace
{
TerrainCompositedRefreshReceipt Receipt()
{
    const auto instance = Instance();
    return {TerrainCompositedRefreshCommand::Freeze(instance.plan,
        {instance.placementId, TerrainCompositedDependencyChange::Paint, {0.1, 0.1, 0.2, 0.2}},
        12, 101, 7, 4), {31, 12, 101, instance.placementId}, 9};
}
}

TEST(TerrainCompositedRefresh, OnlyPublishedTransactionAcknowledgesDirty)
{
    const auto receipt = Receipt();
    for (const auto state : {TerrainCompositedRefreshState::Queued,
        TerrainCompositedRefreshState::Prepared, TerrainCompositedRefreshState::Submitted,
        TerrainCompositedRefreshState::Failed, TerrainCompositedRefreshState::Stale})
    {
        const TerrainCompositedRefreshStatus status{receipt.ticket, state, 5, {}};
        EXPECT_FALSE(receipt.Acknowledges(status, 12, 9, 7));
    }
    const TerrainCompositedRefreshStatus published{
        receipt.ticket, TerrainCompositedRefreshState::Published, 5, {}};
    EXPECT_TRUE(receipt.Acknowledges(published, 12, 9, 7));
}

TEST(TerrainCompositedRefresh, PublishedReceiptRejectsRecreatedSessionAndNewerSource)
{
    const auto receipt = Receipt();
    const TerrainCompositedRefreshStatus published{
        receipt.ticket, TerrainCompositedRefreshState::Published, 5, {}};
    EXPECT_FALSE(receipt.Acknowledges(published, 13, 9, 7));
    EXPECT_FALSE(receipt.Acknowledges(published, 12, 10, 7));
    EXPECT_FALSE(receipt.Acknowledges(published, 12, 9, 8));
}

TEST(TerrainCompositedRefresh, PublishedReceiptRequiresExactTicketAndAdvancedRevision)
{
    const auto receipt = Receipt();
    TerrainCompositedRefreshStatus status{
        receipt.ticket, TerrainCompositedRefreshState::Published, 4, {}};
    EXPECT_FALSE(receipt.Acknowledges(status, 12, 9, 7));
    status.publishedRevision = 5;
    ++status.ticket.editSeq;
    EXPECT_FALSE(receipt.Acknowledges(status, 12, 9, 7));
    status.ticket = receipt.ticket;
    ++status.ticket.id;
    EXPECT_FALSE(receipt.Acknowledges(status, 12, 9, 7));
    status.ticket = receipt.ticket;
    status.ticket.surfaceId = "terrain|other";
    EXPECT_FALSE(receipt.Acknowledges(status, 12, 9, 7));
}

TEST(TerrainCompositedRefresh, PendingAndFailedReceiptsPreserveDirtyUntilPublished)
{
    const auto receipt = Receipt();
    TerrainCompositedEditDirty dirty;
    dirty.Include(receipt.command.changes.front().rect, 7);
    for (const auto state : {TerrainCompositedRefreshState::Queued,
        TerrainCompositedRefreshState::Prepared, TerrainCompositedRefreshState::Submitted,
        TerrainCompositedRefreshState::Failed})
    {
        const TerrainCompositedRefreshStatus status{receipt.ticket, state, 5, {}};
        if (receipt.Acknowledges(status, 12, 9, 7))
            dirty.Acknowledge(receipt.command.sourceRevision);
        EXPECT_TRUE(dirty.valid);
    }
    const TerrainCompositedRefreshStatus status{
        receipt.ticket, TerrainCompositedRefreshState::Published, 5, {}};
    ASSERT_TRUE(receipt.Acknowledges(status, 12, 9, 7));
    EXPECT_TRUE(dirty.Acknowledge(receipt.command.sourceRevision));
    EXPECT_FALSE(dirty.valid);
}

namespace
{
class TerrainCompositedRefreshV2Baseline : public TerrainCompositedRefreshMonitor
{
protected:
    TerrainCompositedAuthorBaseline baseline;
    std::filesystem::path manifestPath;

    void SetUp() override
    {
        TerrainCompositedRefreshMonitor::SetUp();
        ASSERT_FALSE(HasFatalFailure());
        const auto packageRoot = directory / "v2";
        manifestPath = packageRoot / "world.json";
        TerrainCompositedVTBuildDesc surface;
        surface.surfaceId = "terrain|refresh";
        surface.width = surface.height = 128;
        surface.generatorVersion = TerrainCompositedGeneratorVersion;
        surface.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
            TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
        std::vector<TerrainCompositedVTPageData> pages{
            {{surface.surfaceId, surface.Layout().MipCount() - 1, 0, 0},
                std::vector<uint8_t>(size_t(surface.PageBytes()), 17)}};
        auto publication = TerrainProductionFixture::Publication(packageRoot, {surface}, pages);
        publication.production.surfaces.front().authorDependencies = {
            {"resource", "Maps/height.exr", "bytes",
                ContentHasher::HashString("height-before").ToHex(), {"frame-normal"}}};
        TerrainCompositedVTStore::Publish(manifestPath, publication, pages);
        auto store = std::make_shared<TerrainCompositedVTStore>(TerrainCompositedVTStore::Open(manifestPath));
        baseline = {12, surface.surfaceId, store->Production().SurfaceContentHash(surface.surfaceId),
            store, store->Production().surfaces.front().authorDependencies};
    }
};
}

TEST_F(TerrainCompositedRefreshV2Baseline, LoadsVerifiedProductionWithoutV1Sidecar)
{
    std::string error;
    ASSERT_TRUE(monitor->LoadBaseline(baseline, roots, error)) << error;
    const auto status = WaitForCheck();
    EXPECT_EQ(status.worldGeneration, 12u);
    EXPECT_EQ(status.manifestHash, baseline.productionIdentity.ToHex());
    EXPECT_FALSE(status.externalChange);
    EXPECT_TRUE(monitor->CanRecook("terrain|refresh"));
    WriteHeight("external-new-height");
    EXPECT_TRUE(WaitForCheck(true).externalChange);
}

TEST_F(TerrainCompositedRefreshV2Baseline, RejectsWrongProductionIdentityWithoutReplacingBaseline)
{
    std::string error;
    ASSERT_TRUE(monitor->LoadBaseline(baseline, roots, error)) << error;
    baseline.productionIdentity = ContentHasher::HashString("wrong");
    EXPECT_FALSE(monitor->LoadBaseline(baseline, roots, error));
    Monitor::SurfaceStatus status;
    ASSERT_TRUE(monitor->GetStatus("terrain|refresh", status));
    EXPECT_NE(status.manifestHash, baseline.productionIdentity.ToHex());
}

TEST_F(TerrainCompositedRefreshV2Baseline, RejectsMissingObjectProof)
{
    std::string error;
    baseline.dependencies.front().productionObjectIds = {"unknown"};
    EXPECT_FALSE(monitor->LoadBaseline(baseline, roots, error));
    EXPECT_FALSE(error.empty());
    baseline.dependencies.front().productionObjectIds.clear();
    EXPECT_FALSE(monitor->LoadBaseline(baseline, roots, error));
}

TEST_F(TerrainCompositedRefreshV2Baseline, RejectsCorruptedValidatedObject)
{
    const auto& object = baseline.store->Production().Object("frame-normal");
    std::ofstream(manifestPath.parent_path() / object.relativePath,
        std::ios::binary | std::ios::trunc) << "corrupted";
    std::string error;
    EXPECT_FALSE(monitor->LoadBaseline(baseline, roots, error));
    EXPECT_FALSE(error.empty());
}

TEST_F(TerrainCompositedRefreshV2Baseline, RejectsAuthorDigestForgedFromCurrentFile)
{
    WriteHeight("modified-after-publication");
    baseline.dependencies.front().digest = ContentHasher::HashString("modified-after-publication").ToHex();
    std::string error;
    EXPECT_FALSE(monitor->LoadBaseline(baseline, roots, error));
    EXPECT_NE(error.find("not bound to the publication"), std::string::npos);
}

TEST_F(TerrainCompositedRefreshV2Baseline, RejectsExistingButUnboundProductionObject)
{
    baseline.dependencies.front().productionObjectIds = {"compute"};
    std::string error;
    EXPECT_FALSE(monitor->LoadBaseline(baseline, roots, error));
    EXPECT_NE(error.find("binding differs"), std::string::npos);
}

TEST_F(TerrainCompositedRefreshV2Baseline, RejectsMissingCapturedDependency)
{
    baseline.dependencies.clear();
    std::string error;
    EXPECT_FALSE(monitor->LoadBaseline(baseline, roots, error));
}
