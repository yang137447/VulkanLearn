#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "editor/terrain/terrainCompositedAuthorMonitor.h"
#include "terrain/terrainCompositedDependencies.h"
#include "terrainCompositedProductionFixture.h"
#include "terrain/terrainCompositedVerifiedInputs.h"
#include "terrain/terrainRvtBudget.h"
#include <map>

namespace
{
namespace fs = std::filesystem;
using Monitor = VL::Editor::TerrainCompositedAuthorMonitor;
using Json = nlohmann::json;

class TerrainCompositedAuthorMonitorTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        temporaryRoot = fs::canonical(fs::temp_directory_path());
        directory = temporaryRoot / ("vl-author-monitor-test-" +
            std::to_string(Monitor::Clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence++));
        roots = {directory / "resources", directory / "project"};
        fs::create_directories(roots.resourceRoot);
        fs::create_directories(roots.projectRoot);
        plan.desc.assetPath = "Maps/TR_test.json";
        plan.desc.heightmapPath = "Maps/height.bin";
        plan.desc.paintPath = "Maps/test.vlpaint";
        plan.desc.materialInstancePath = "Maps/MI_test.json";
        plan.desc.layers.push_back(VL::TerrainLayerDesc{});
        plan.placementId = "terrain|test";
        Write(plan.desc.assetPath, Json{
            {"type", "terrain"}, {"heightmapPath", plan.desc.heightmapPath},
            {"paintPath", plan.desc.paintPath},
            {"materialInstancePath", plan.desc.materialInstancePath}}.dump());
        Write(plan.desc.heightmapPath, "height-original");
        Write(plan.desc.paintPath, "paint-original");
        Write(plan.desc.materialInstancePath, Json{
            {"type", "materialInstance"}, {"material", "shader/glsl/M_test.json"},
            {"parameters", {{"u_uvScale", 1.0}}}}.dump());
        Write("shader/glsl/M_test.json", Json{
            {"type", "material"}, {"shaderEvaluation", {
                {"vertex", "M_test.vertex.glsl"},
                {"surface", "M_test.surface.glsl"}}}}.dump(), true);
        Write("shader/glsl/M_test.vertex.glsl", "vertex-original", true);
        Write("shader/glsl/M_test.surface.glsl", "surface-original", true);
        baseline = Build();
        ASSERT_TRUE(monitor.RegisterSurface("surface", roots, baseline, error)) << error;
    }

    void TearDown() override
    {
        monitor.UnregisterSurface("surface");
        monitor.UnregisterSurface("other");
        Drain();
        const auto resolved = fs::weakly_canonical(directory);
        ASSERT_EQ(resolved.parent_path(), temporaryRoot);
        ASSERT_EQ(resolved.filename().string().rfind("vl-author-monitor-test-", 0), 0u);
        std::error_code cleanupError;
        fs::remove_all(resolved, cleanupError);
        EXPECT_FALSE(cleanupError) << cleanupError.message();
    }

    void Write(const std::string& name, const std::string& bytes, bool project = false)
    {
        const auto path = (project ? roots.projectRoot : roots.resourceRoot) / name;
        fs::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(output.good()) << path;
    }

    std::string Read(const std::string& name) const
    {
        std::ifstream input(roots.resourceRoot / name, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(input), {});
    }

    Json Build() const
    {
        const std::array<float, 16> placement{
            1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        return VL::BuildTerrainCompositedDependencies(
            roots, plan, placement, plan.desc.materialInstancePath,
            Json{{"generatorVersion", 1}, {"encoding", "rgba8"}});
    }

    std::shared_ptr<const VL::TerrainCompositedVTStore> PublishStore(
        const std::string& name = "baseline-store", double uvScale = 1.0)
    {
        const auto root = directory / name;
        VL::TerrainCompositedVTPublication publication;
        publication.production = TerrainProductionFixture::Closure(root, {"surface", "other"});
        std::vector<VL::TerrainCompositedVTPageData> pages;
        for (auto& surface : publication.production.surfaces)
        {
            surface.parameters.uvScale = uvScale;
            surface.authorDependencies = {
                {"resource", plan.desc.heightmapPath, "bytes",
                    VL::ContentHasher::HashString(Read(plan.desc.heightmapPath)).ToHex(), {"compute"}},
                {"resource", plan.desc.paintPath, "bytes",
                    VL::ContentHasher::HashString(Read(plan.desc.paintPath)).ToHex(), {"compute"}}};
            VL::TerrainCompositedVTBuildDesc desc;
            desc.surfaceId = surface.surfaceId;
            desc.width = desc.height = 128;
            desc.channels = {VL::TerrainCompositedVTFormat::RGBA8Srgb,
                VL::TerrainCompositedVTFormat::RGBA8UNorm, VL::TerrainCompositedVTFormat::RGBA8UNorm};
            desc.dependencyHash = publication.production.SurfaceContentHash(surface.surfaceId);
            publication.surfaces.push_back(desc);
            const VL::TerrainCompositedVTPageKey key{surface.surfaceId, desc.Layout().MipCount() - 1, 0, 0};
            publication.packagedPages.push_back(key);
            pages.push_back({key, std::vector<uint8_t>(static_cast<size_t>(desc.PageBytes()), 19)});
        }
        VL::TerrainCompositedVTStore::Publish(root / "world.json", publication, pages);
        return std::make_shared<const VL::TerrainCompositedVTStore>(
            VL::TerrainCompositedVTStore::Open(root / "world.json"));
    }

    VL::TerrainCompositedAuthorBaseline StoredBaseline(
        const std::shared_ptr<const VL::TerrainCompositedVTStore>& store,
        const std::string& surface = "surface", uint64_t world = 1) const
    {
        const auto& production = store->Production();
        const auto found = std::find_if(production.surfaces.begin(), production.surfaces.end(),
            [&](const auto& value) { return value.surfaceId == surface; });
        const auto key = std::make_pair(store.get(), world);
        auto owner = verificationOwners.find(key);
        if (owner == verificationOwners.end())
        {
            VL::TerrainRvtBudgetCaps caps;
            caps.totalCpuBytes = uint64_t{512} * 1024 * 1024;
            caps.workspaceCpuBytes = caps.totalCpuBytes;
            auto budget = std::make_shared<VL::TerrainRvtBudgetOwner>(caps);
            auto verification = VL::TerrainCompositedVerifiedInputs::Begin(
                store, world, production.ContentHash(), budget);
            owner = verificationOwners.emplace(key, std::move(verification)).first;
        }
        return {world, surface, production.SurfaceContentHash(surface),
            store, found->authorDependencies, owner->second};
    }

    void Drain()
    {
        const auto deadline = Monitor::Clock::now() + std::chrono::seconds(10);
        while (monitor.Stats().scanPending && Monitor::Clock::now() < deadline)
        {
            auto next = monitor.Poll(now);
            events.insert(events.end(), next.begin(), next.end());
            if (monitor.Stats().scanPending)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_FALSE(monitor.Stats().scanPending);
    }

    void Scan()
    {
        events = monitor.Poll(now);
        ASSERT_NO_FATAL_FAILURE(Drain());
    }

    void AdvanceAndScan()
    {
        now += std::chrono::seconds(2);
        ASSERT_NO_FATAL_FAILURE(Scan());
    }

    Monitor::SurfaceStatus Status(const std::string& surfaceId = "surface") const
    {
        Monitor::SurfaceStatus result;
        EXPECT_TRUE(monitor.GetStatus(surfaceId, result));
        return result;
    }

    fs::path temporaryRoot;
    fs::path directory;
    VL::TerrainCompositedDependencyRoots roots;
    VL::TerrainBuildPlan plan;
    Json baseline;
    std::string error;
    Monitor monitor;
    Monitor::Clock::time_point now = Monitor::Clock::now();
    std::vector<Monitor::SurfaceStatus> events;
    mutable std::map<std::pair<const VL::TerrainCompositedVTStore*, uint64_t>,
        std::shared_ptr<VL::TerrainCompositedVerifiedInputs>> verificationOwners;
};
}

TEST_F(TerrainCompositedAuthorMonitorTest, RequiresDiskVerificationBeforeAllowingCook)
{
    EXPECT_FALSE(monitor.CanRecook("surface"));
    EXPECT_EQ(monitor.Stats().uncheckedSurfaces, 1u);
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_TRUE(monitor.CanRecook("surface"));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events.front().checked);
    EXPECT_EQ(Status().manifestHash, VL::TerrainCompositedDependencyHash(baseline).ToHex());
    EXPECT_EQ(monitor.Stats().completedChecks, 1u);
}

TEST_F(TerrainCompositedAuthorMonitorTest, PollWithinIntervalDoesNotStartAnotherScan)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto completed = monitor.Stats().completedChecks;
    EXPECT_TRUE(monitor.Poll(now + std::chrono::milliseconds(999)).empty());
    EXPECT_FALSE(monitor.Stats().scanPending);
    EXPECT_EQ(monitor.Stats().completedChecks, completed);
}

TEST_F(TerrainCompositedAuthorMonitorTest, DirtySurfaceRejectsCookAndBaselineReplacement)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto generation = Status().generation;
    ASSERT_TRUE(monitor.MarkEdited("surface"));
    EXPECT_FALSE(monitor.CanRecook("surface"));
    EXPECT_FALSE(monitor.CheckBeforeRecook("surface", error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(monitor.RegisterSurface("surface", roots, baseline, error));
    EXPECT_EQ(Status().generation, generation);
    EXPECT_TRUE(Status().dirty);
    EXPECT_EQ(monitor.Stats().dirtySurfaces, 1u);
    EXPECT_EQ(Read(plan.desc.paintPath), "paint-original");
    EXPECT_EQ(Read(plan.desc.heightmapPath), "height-original");
}

TEST_F(TerrainCompositedAuthorMonitorTest, SameSizeRestoredTimestampEditStillRaisesDirtyConflict)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    ASSERT_TRUE(monitor.MarkEdited("surface"));
    const auto path = roots.resourceRoot / plan.desc.heightmapPath;
    const auto originalTime = fs::last_write_time(path);
    const auto originalSize = fs::file_size(path);
    Write(plan.desc.heightmapPath, "height-external");
    ASSERT_EQ(fs::file_size(path), originalSize);
    fs::last_write_time(path, originalTime);
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_FALSE(Status().externalChange);
    now += std::chrono::seconds(60);
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto status = Status();
    EXPECT_TRUE(status.externalChange);
    EXPECT_TRUE(status.conflict);
    EXPECT_TRUE(status.dirty);
    EXPECT_NE(status.diagnostic.find(plan.desc.heightmapPath), std::string::npos);
    EXPECT_EQ(monitor.Stats().conflictSurfaces, 1u);
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(events.front().conflict);
    EXPECT_EQ(Read(plan.desc.heightmapPath), "height-external");
    EXPECT_EQ(Read(plan.desc.paintPath), "paint-original");
}

TEST_F(TerrainCompositedAuthorMonitorTest, SavingDoesNotClearAnExternalConflict)
{
    ASSERT_TRUE(monitor.MarkEdited("surface"));
    Write(plan.desc.paintPath, "external-paint");
    ASSERT_NO_FATAL_FAILURE(Scan());
    ASSERT_TRUE(Status().conflict);
    ASSERT_TRUE(monitor.MarkSaved("surface"));
    EXPECT_FALSE(Status().dirty);
    EXPECT_TRUE(Status().conflict);
    EXPECT_FALSE(Status().checked);
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_FALSE(monitor.CanRecook("surface"));
    EXPECT_FALSE(monitor.CheckBeforeRecook("surface", error));
    EXPECT_TRUE(Status().externalChange);
    EXPECT_TRUE(Status().conflict);
    EXPECT_EQ(Read(plan.desc.paintPath), "external-paint");
}

TEST_F(TerrainCompositedAuthorMonitorTest, CleanMaterialEditRequiresExplicitReloadBaseline)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto path = roots.resourceRoot / plan.desc.materialInstancePath;
    const auto previousTime = fs::last_write_time(path);
    Write(plan.desc.materialInstancePath, Json{
        {"type", "materialInstance"}, {"material", "shader/glsl/M_test.json"},
        {"parameters", {{"u_uvScale", 9.0}}}}.dump());
    fs::last_write_time(path, previousTime + std::chrono::seconds(2));
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_TRUE(Status().externalChange);
    EXPECT_FALSE(Status().conflict);
    EXPECT_FALSE(monitor.CanRecook("surface"));
    const auto refreshed = Build();
    ASSERT_TRUE(monitor.RegisterSurface("surface", roots, refreshed, error)) << error;
    EXPECT_FALSE(monitor.CanRecook("surface"));
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_TRUE(monitor.CanRecook("surface"));
    EXPECT_FALSE(Status().externalChange);
    EXPECT_EQ(Status().manifestHash, VL::TerrainCompositedDependencyHash(refreshed).ToHex());
}

TEST_F(TerrainCompositedAuthorMonitorTest, RestoringDiskBytesDoesNotSilentlyClearExternalChange)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto path = roots.resourceRoot / plan.desc.paintPath;
    const auto previousTime = fs::last_write_time(path);
    Write(plan.desc.paintPath, "external-paint");
    fs::last_write_time(path, previousTime + std::chrono::seconds(2));
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    ASSERT_TRUE(Status().externalChange);
    Write(plan.desc.paintPath, "paint-original");
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_TRUE(Status().externalChange);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, PreCookCheckCatchesChangesBeforeNextPoll)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    Write(plan.desc.heightmapPath, "changed-between-polls");
    ASSERT_FALSE(monitor.CheckBeforeRecook("surface", error));
    EXPECT_NE(error.find(plan.desc.heightmapPath), std::string::npos);
    EXPECT_TRUE(Status().externalChange);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, MissingRequiredDependencyBlocksCook)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    ASSERT_TRUE(fs::remove(roots.resourceRoot / plan.desc.heightmapPath));
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_TRUE(Status().externalChange);
    EXPECT_NE(Status().diagnostic.find("missing"), std::string::npos);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, PreviouslyAbsentPaintMetadataIsDetected)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto metadata = plan.desc.paintPath + ".meta.json";
    ASSERT_FALSE(fs::exists(roots.resourceRoot / metadata));
    Write(metadata, "{}");
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_TRUE(Status().externalChange);
    EXPECT_NE(Status().diagnostic.find(metadata), std::string::npos);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, SharedShaderChangeNotifiesEveryAffectedSurface)
{
    ASSERT_TRUE(monitor.RegisterSurface("other", roots, baseline, error)) << error;
    ASSERT_TRUE(monitor.MarkEdited("surface"));
    Write("shader/glsl/M_test.surface.glsl", "changed-shader", true);
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_TRUE(Status().conflict);
    EXPECT_TRUE(Status("other").externalChange);
    EXPECT_FALSE(Status("other").conflict);
    EXPECT_EQ(events.size(), 2u);
    const auto stats = monitor.Stats();
    EXPECT_EQ(stats.surfaces, 2u);
    EXPECT_EQ(stats.changedSurfaces, 2u);
    EXPECT_EQ(stats.conflictSurfaces, 1u);
    EXPECT_EQ(stats.failedChecks, 2u);
}

TEST_F(TerrainCompositedAuthorMonitorTest, LateScanCannotClearDirtyStateOrUnregisterReplacement)
{
    monitor.Poll(now);
    ASSERT_TRUE(monitor.Stats().scanPending);
    const auto oldGeneration = Status().generation;
    monitor.UnregisterSurface("surface");
    ASSERT_TRUE(monitor.RegisterSurface("surface", roots, baseline, error)) << error;
    ASSERT_GT(Status().generation, oldGeneration);
    ASSERT_TRUE(monitor.MarkEdited("surface"));
    ASSERT_NO_FATAL_FAILURE(Drain());
    EXPECT_GT(monitor.Stats().discardedChecks, 0u);
    EXPECT_TRUE(Status().dirty);
    EXPECT_TRUE(Status().checked);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, SaveInvalidatesAnInFlightOldBaselineCheck)
{
    monitor.Poll(now);
    ASSERT_TRUE(monitor.Stats().scanPending);
    const auto oldGeneration = Status().generation;
    ASSERT_TRUE(monitor.MarkEdited("surface"));
    Write(plan.desc.paintPath, "saved-new-paint");
    ASSERT_TRUE(monitor.MarkSaved("surface"));
    EXPECT_GT(Status().generation, oldGeneration);
    EXPECT_FALSE(Status().checked);
    ASSERT_NO_FATAL_FAILURE(Drain());
    EXPECT_GT(monitor.Stats().discardedChecks, 0u);
    EXPECT_TRUE(Status().externalChange);
    EXPECT_FALSE(Status().conflict);
    EXPECT_FALSE(monitor.CanRecook("surface"));
    ASSERT_TRUE(monitor.RegisterSurface("surface", roots, Build(), error)) << error;
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_TRUE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, InvalidManifestDoesNotReplaceVerifiedBaseline)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto previous = Status();
    auto invalid = baseline;
    invalid["hash"] = "invalid";
    EXPECT_FALSE(monitor.RegisterSurface("surface", roots, invalid, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Status().generation, previous.generation);
    EXPECT_EQ(Status().manifestHash, previous.manifestHash);
    EXPECT_TRUE(monitor.CanRecook("surface"));
    EXPECT_FALSE(monitor.RegisterSurface("", roots, baseline, error));
}

TEST_F(TerrainCompositedAuthorMonitorTest, UnregisteredSurfacesCannotMutateOrCook)
{
    monitor.UnregisterSurface("surface");
    Monitor::SurfaceStatus status;
    EXPECT_FALSE(monitor.GetStatus("surface", status));
    EXPECT_FALSE(monitor.MarkEdited("surface"));
    EXPECT_FALSE(monitor.MarkSaved("surface"));
    EXPECT_FALSE(monitor.CanRecook("surface"));
    EXPECT_FALSE(monitor.CheckBeforeRecook("surface", error));
    EXPECT_EQ(monitor.Stats().surfaces, 0u);
    EXPECT_TRUE(monitor.Poll(now).empty());
    EXPECT_FALSE(monitor.Stats().scanPending);
}

TEST_F(TerrainCompositedAuthorMonitorTest, UnchangedMetadataPollsPerformNoFullReads)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto initial = monitor.Stats();
    ASSERT_GT(initial.fullReadCount, 0u);
    for (size_t iteration = 0; iteration < 5; ++iteration)
        ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    const auto current = monitor.Stats();
    EXPECT_EQ(current.fullReadCount, initial.fullReadCount);
    EXPECT_EQ(current.fullReadBytes, initial.fullReadBytes);
    EXPECT_GT(current.metadataChecks, initial.metadataChecks);
    EXPECT_GT(current.metadataCacheHits, initial.metadataCacheHits);
    EXPECT_TRUE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, SharedDependenciesReadOncePerRoundAcrossSurfaces)
{
    ASSERT_TRUE(monitor.RegisterSurface("other", roots, baseline, error)) << error;
    ASSERT_NO_FATAL_FAILURE(Scan());
    size_t presentDependencies = 0;
    for (const auto& entry : baseline.at("dependencies"))
        presentDependencies += entry.at("mode") == "absent" ? 0 : 1;
    EXPECT_EQ(monitor.Stats().fullReadCount, presentDependencies);
    EXPECT_EQ(monitor.Stats().metadataChecks, baseline.at("dependencies").size());
    const auto initial = monitor.Stats();
    Write(plan.desc.heightmapPath, "changed-height-with-new-size");
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_EQ(monitor.Stats().fullReadCount, initial.fullReadCount + 1);
    EXPECT_TRUE(Status().externalChange);
    EXPECT_TRUE(Status("other").externalChange);
}

TEST_F(TerrainCompositedAuthorMonitorTest, MetadataChangeWithIdenticalBytesIsDigestConfirmed)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto initial = monitor.Stats();
    const auto path = roots.resourceRoot / plan.desc.heightmapPath;
    fs::last_write_time(path, fs::last_write_time(path) + std::chrono::seconds(2));
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_EQ(monitor.Stats().fullReadCount, initial.fullReadCount + 1);
    EXPECT_FALSE(Status().externalChange);
    EXPECT_TRUE(monitor.CanRecook("surface"));
    const auto confirmed = monitor.Stats();
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_EQ(monitor.Stats().fullReadCount, confirmed.fullReadCount);
}

TEST_F(TerrainCompositedAuthorMonitorTest, LowFrequencyHashRechecksSharedFilesOnlyOnce)
{
    ASSERT_TRUE(monitor.RegisterSurface("other", roots, baseline, error)) << error;
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto initial = monitor.Stats();
    now += std::chrono::seconds(59);
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_EQ(monitor.Stats().fullReadCount, initial.fullReadCount);
    now += std::chrono::seconds(1);
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_EQ(monitor.Stats().fullReadCount, initial.fullReadCount * 2);
    EXPECT_FALSE(Status().externalChange);
    EXPECT_FALSE(Status("other").externalChange);
}

TEST_F(TerrainCompositedAuthorMonitorTest, AddingSurfaceRechecksSharedDependencyForAllSurfaces)
{
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto path = roots.resourceRoot / plan.desc.heightmapPath;
    const auto timestamp = fs::last_write_time(path);
    Write(plan.desc.heightmapPath, "height-external");
    fs::last_write_time(path, timestamp);
    ASSERT_TRUE(monitor.RegisterSurface("other", roots, baseline, error)) << error;
    const auto readsBefore = monitor.Stats().fullReadCount;
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_TRUE(Status().externalChange);
    EXPECT_TRUE(Status("other").externalChange);
    size_t presentDependencies = 0;
    for (const auto& entry : baseline.at("dependencies"))
        presentDependencies += entry.at("mode") == "absent" ? 0 : 1;
    EXPECT_EQ(monitor.Stats().fullReadCount, readsBefore + presentDependencies);
}

TEST_F(TerrainCompositedAuthorMonitorTest, TerrainJsonDigestPreservesPackagePathExclusion)
{
    auto terrain = Json::parse(Read(plan.desc.assetPath));
    terrain["compositedVT"] = {{"packagePath", "Generated/first.json"}};
    Write(plan.desc.assetPath, terrain.dump());
    ASSERT_TRUE(monitor.RegisterSurface("surface", roots, Build(), error)) << error;
    ASSERT_NO_FATAL_FAILURE(Scan());
    const auto initial = monitor.Stats();
    terrain["compositedVT"]["packagePath"] = "Generated/longer-second-package.json";
    Write(plan.desc.assetPath, terrain.dump());
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_EQ(monitor.Stats().fullReadCount, initial.fullReadCount + 1);
    EXPECT_FALSE(Status().externalChange);
    EXPECT_TRUE(monitor.CanRecook("surface"));
    terrain["heightScale"] = 321.0;
    Write(plan.desc.assetPath, terrain.dump());
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_TRUE(Status().externalChange);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, StaticTerrainMaterialDigestMatchesCookAndIgnoresRetainedNumericChanges)
{
    Write("shader/glsl/M_terrain.json", Json{
        {"type", "material"}, {"shaderEvaluation", {
            {"vertex", "M_test.vertex.glsl"}, {"surface", "M_test.surface.glsl"}}}}.dump(), true);
    Json material{
        {"type", "materialInstance"}, {"material", "shader/glsl/M_terrain.json"},
        {"parameters", {{"u_uvScale", 4.0}, {"u_detailStrength", 0.25}}}};
    Write(plan.desc.materialInstancePath, material.dump());
    const auto manifest = Build();
    bool projected = false;
    for (const auto& entry : manifest.at("dependencies"))
        if (entry.at("path") == plan.desc.materialInstancePath)
            projected = entry.at("mode") == "terrain-material-instance-static-json-v2";
    ASSERT_TRUE(projected);
    ASSERT_TRUE(monitor.RegisterSurface("surface", roots, manifest, error)) << error;
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_FALSE(Status().externalChange);
    EXPECT_TRUE(monitor.CanRecook("surface"));
    material["parameters"]["u_detailStrength"] = 0.75;
    Write(plan.desc.materialInstancePath, material.dump());
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_FALSE(Status().externalChange);
    material["parameters"]["u_uvScale"] = 128.0;
    Write(plan.desc.materialInstancePath, material.dump());
    ASSERT_NO_FATAL_FAILURE(AdvanceAndScan());
    EXPECT_TRUE(Status().externalChange);
}

TEST_F(TerrainCompositedAuthorMonitorTest, BaselineSharedObjectsReadOnceAcrossSurfaceReferences)
{
    const auto store = PublishStore();
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(store), roots, error)) << error;
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(store, "other"), roots, error)) << error;
    EXPECT_EQ(monitor.BaselineStats().objectRequests, 4u);
    EXPECT_EQ(monitor.BaselineStats().objectReads, 1u);
    EXPECT_EQ(monitor.BaselineStats().cacheHits, 3u);
    EXPECT_EQ(monitor.BaselineStats().readBytes, store->Production().Object("compute").bytes);
    EXPECT_FALSE(Status().checked);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, TwoMonitorsShareOneScopedStoreVerification)
{
    const auto store = PublishStore();
    const auto session = std::make_shared<Monitor::BaselineLoadSession>();
    Monitor other;
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(store), roots, session, error)) << error;
    ASSERT_TRUE(other.LoadBaseline(StoredBaseline(store, "other"), roots, session, error)) << error;
    EXPECT_EQ(session->Stats().objectReads, 1u);
    EXPECT_EQ(session->Stats().cacheHits, 3u);
    session->Close();
    EXPECT_FALSE(other.LoadBaseline(StoredBaseline(store), roots, session, error));
    other.UnregisterSurface("other");
}

TEST_F(TerrainCompositedAuthorMonitorTest, EqualPublicationsWithDifferentStoreHandlesDoNotShare)
{
    const auto first = PublishStore();
    const auto second = PublishStore("different-store");
    ASSERT_EQ(first->Production().ContentHash(), second->Production().ContentHash());
    const auto session = std::make_shared<Monitor::BaselineLoadSession>();
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(first), roots, session, error)) << error;
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(second, "other"), roots, session, error)) << error;
    EXPECT_EQ(session->Stats().bindings, 2u);
    EXPECT_EQ(session->Stats().objectReads, 2u);
}

TEST_F(TerrainCompositedAuthorMonitorTest, ChangedPublicationAndWorldInvalidateScopedVerification)
{
    const auto first = PublishStore();
    const auto second = PublishStore("changed-publication", 2.0);
    ASSERT_NE(first->Production().ContentHash(), second->Production().ContentHash());
    const auto session = std::make_shared<Monitor::BaselineLoadSession>();
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(first), roots, session, error)) << error;
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(second), roots, session, error)) << error;
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(second, "other", 2), roots, session, error)) << error;
    EXPECT_EQ(session->Stats().bindings, 3u);
    EXPECT_EQ(session->Stats().objectReads, 3u);
}

TEST_F(TerrainCompositedAuthorMonitorTest, CorruptPackagedObjectIsRejectedAndNotCached)
{
    const auto store = PublishStore();
    const auto& object = store->Production().Object("compute");
    TerrainProductionFixture::WriteFixtureText(directory / "baseline-store" / object.relativePath,
        std::string(static_cast<size_t>(object.bytes), 'X'));
    const auto session = std::make_shared<Monitor::BaselineLoadSession>();
    EXPECT_FALSE(monitor.LoadBaseline(StoredBaseline(store), roots, session, error));
    EXPECT_FALSE(monitor.LoadBaseline(StoredBaseline(store), roots, session, error));
    EXPECT_EQ(session->Stats().objectReads, 2u);
    EXPECT_EQ(session->Stats().cacheHits, 0u);
    EXPECT_EQ(session->Stats().failedReads, 2u);
}

TEST_F(TerrainCompositedAuthorMonitorTest, PollClosesInitializationScopeAndLaterCorruptionIsRejected)
{
    const auto store = PublishStore();
    const auto session = std::make_shared<Monitor::BaselineLoadSession>();
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(store), roots, session, error)) << error;
    ASSERT_NO_FATAL_FAILURE(Scan());
    EXPECT_TRUE(session->Stats().closed);
    EXPECT_FALSE(monitor.LoadBaseline(StoredBaseline(store), roots, session, error));
    const auto& object = store->Production().Object("compute");
    TerrainProductionFixture::WriteFixtureText(directory / "baseline-store" / object.relativePath,
        std::string(static_cast<size_t>(object.bytes), 'X'));
    verificationOwners.clear();
    EXPECT_FALSE(monitor.LoadBaseline(StoredBaseline(store), roots, error));
    EXPECT_EQ(monitor.BaselineStats().objectReads, 2u);
}

TEST_F(TerrainCompositedAuthorMonitorTest, InvalidBaselineCannotReplaceDirtyObserverState)
{
    const auto store = PublishStore();
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(store), roots, error)) << error;
    ASSERT_NO_FATAL_FAILURE(Scan());
    ASSERT_TRUE(monitor.MarkEdited("surface"));
    const auto before = Status();
    auto invalid = StoredBaseline(store);
    invalid.dependencies.front().digest = VL::ContentHasher::HashString("wrong").ToHex();
    EXPECT_FALSE(monitor.LoadBaseline(invalid, roots, error));
    const auto after = Status();
    EXPECT_TRUE(after.dirty);
    EXPECT_EQ(after.generation, before.generation);
    EXPECT_EQ(after.editRevision, before.editRevision);
    EXPECT_EQ(after.manifestHash, before.manifestHash);
    EXPECT_FALSE(monitor.CanRecook("surface"));
}

TEST_F(TerrainCompositedAuthorMonitorTest, WarmSessionStillRejectsForgedDependencyBinding)
{
    const auto store = PublishStore();
    const auto session = std::make_shared<Monitor::BaselineLoadSession>();
    ASSERT_TRUE(monitor.LoadBaseline(StoredBaseline(store), roots, session, error)) << error;
    auto invalid = StoredBaseline(store, "other");
    invalid.dependencies.front().productionObjectIds = {"artifact"};
    EXPECT_FALSE(monitor.LoadBaseline(invalid, roots, session, error));
    EXPECT_EQ(session->Stats().objectReads, 1u);
    Monitor::SurfaceStatus absent;
    EXPECT_FALSE(monitor.GetStatus("other", absent));
}

TEST_F(TerrainCompositedAuthorMonitorTest, ServiceVerifiedOwnerIsConsumedWithoutAnotherObjectRead)
{
    const auto store = PublishStore();
    const auto baseline = StoredBaseline(store);
    const auto adjacent = StoredBaseline(store, "other");
    const VL::TerrainCompositedVerifiedInputBinding binding{
        store, 1, store->Production().ContentHash(), "surface"};
    const auto owner = baseline.verificationOwner;
    owner->VerifyObject(binding, "compute");
    const auto before = owner->Statistics();
    ASSERT_EQ(before.readCalls, 1u);
    const auto session = std::make_shared<Monitor::BaselineLoadSession>();
    ASSERT_TRUE(monitor.LoadBaseline(baseline, roots, session, error)) << error;
    ASSERT_TRUE(monitor.LoadBaseline(adjacent, roots, session, error)) << error;
    EXPECT_EQ(owner->Statistics().readCalls, before.readCalls);
    EXPECT_EQ(owner->Statistics().actualReadBytes, before.actualReadBytes);
    EXPECT_EQ(owner->Statistics().retainedBytes, 0u);
    EXPECT_EQ(session->Stats().objectReads, 0u);
    EXPECT_EQ(session->Stats().cacheHits, 4u);
    session->Close();
    EXPECT_TRUE(owner->Statistics().closed);
}

TEST_F(TerrainCompositedAuthorMonitorTest, MissingOrMismatchedTerrainOwnerCannotImportBaseline)
{
    const auto first = PublishStore();
    const auto second = PublishStore("different-owner-store");
    auto invalid = StoredBaseline(first);
    invalid.verificationOwner.reset();
    EXPECT_FALSE(monitor.LoadBaseline(invalid, roots, error));
    invalid.verificationOwner = StoredBaseline(second).verificationOwner;
    EXPECT_FALSE(monitor.LoadBaseline(invalid, roots, error));
}

TEST_F(TerrainCompositedAuthorMonitorTest, ClosedOwnerOnlyAllowsPreviouslyRegisteredImmutableMetadata)
{
    const auto store = PublishStore();
    const auto baseline = StoredBaseline(store);
    const VL::TerrainCompositedVerifiedInputBinding binding{
        store, 1, store->Production().ContentHash(), "surface"};
    baseline.verificationOwner->VerifyObject(binding, "compute");
    baseline.verificationOwner->Close();
    ASSERT_TRUE(monitor.LoadBaseline(baseline, roots, error)) << error;
    EXPECT_EQ(monitor.BaselineStats().objectReads, 0u);
    EXPECT_FALSE(monitor.LoadBaseline(StoredBaseline(store, "other"), roots, error));
}
