#include <gtest/gtest.h>

#include "terrain/terrainCompositedVerifiedInputs.h"
#include "terrain/terrainRvtBudget.h"
#include "terrainCompositedProductionFixture.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <thread>
#include <type_traits>

#if defined(VL_FILE_IO_TEST_HOOKS)
namespace VL::TerrainCompositedVTStoreTesting
{
extern thread_local std::function<void(const std::filesystem::path&, bool)> InputObserver;
}
#endif

namespace
{
using namespace VL;
namespace fs = std::filesystem;

class TerrainCompositedVerifiedInputTest : public ::testing::Test
{
protected:
    fs::path root;
    fs::path temporaryRoot;
    std::shared_ptr<const TerrainCompositedVTStore> store;
    std::shared_ptr<TerrainRvtBudgetOwner> owner;
    std::shared_ptr<TerrainCompositedVerifiedInputs> session;
    TerrainCompositedVerifiedInputs::Binding binding;

    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        temporaryRoot = fs::weakly_canonical(fs::temp_directory_path());
        root = temporaryRoot / ("vl-verified-input-test-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence.fetch_add(1)));
        store = Publish("original");
        owner = Budget(TerrainCompositedVerifiedInputs::MaxCpuBytes);
        binding = {store, 17, store->Production().ContentHash(), "surface"};
        session = TerrainCompositedVerifiedInputs::Begin(
            store, binding.worldGeneration, binding.productionIdentity, owner);
    }

    void TearDown() override
    {
#if defined(VL_FILE_IO_TEST_HOOKS)
        TerrainCompositedVTStoreTesting::InputObserver = {};
#endif
        session.reset();
        store.reset();
        const auto resolved = fs::weakly_canonical(root);
        ASSERT_EQ(resolved.parent_path(), temporaryRoot);
        ASSERT_EQ(resolved.filename().string().rfind("vl-verified-input-test-", 0), 0u);
        std::error_code error;
        fs::remove_all(resolved, error);
        EXPECT_FALSE(error) << error.message();
    }

    static std::shared_ptr<TerrainRvtBudgetOwner> Budget(uint64_t bytes)
    {
        TerrainRvtBudgetCaps caps;
        caps.totalCpuBytes = bytes;
        caps.workspaceCpuBytes = bytes;
        caps.inputCpuBytes = bytes;
        return std::make_shared<TerrainRvtBudgetOwner>(caps);
    }

    std::shared_ptr<const TerrainCompositedVTStore> Publish(
        const std::string& name, double uvScale = 1)
    {
        const auto directory = root / name;
        TerrainCompositedVTPublication publication;
        publication.production = TerrainProductionFixture::Closure(
            directory, {"surface", "other", "unused"});
        std::vector<TerrainCompositedVTPageData> pages;
        for (auto& surface : publication.production.surfaces)
        {
            surface.parameters.uvScale = uvScale;
            // 多个 surface 和多个业务引用均指向同一个真实 Production 对象。
            surface.authorDependencies[0].productionObjectIds = {"compute", "albedo", "coverage-ids"};
            TerrainCompositedVTBuildDesc desc;
            desc.surfaceId = surface.surfaceId;
            desc.width = desc.height = 128;
            desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
                TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
            desc.dependencyHash = publication.production.SurfaceContentHash(surface.surfaceId);
            publication.surfaces.push_back(desc);
            const TerrainCompositedVTPageKey key{
                surface.surfaceId, desc.Layout().MipCount() - 1, 0, 0};
            publication.packagedPages.push_back(key);
            pages.push_back({key, std::vector<uint8_t>(static_cast<std::size_t>(desc.PageBytes()), 19)});
        }
        TerrainCompositedVTStore::Publish(directory / "world.json", publication, pages);
        return std::make_shared<const TerrainCompositedVTStore>(
            TerrainCompositedVTStore::Open(directory / "world.json"));
    }

    TerrainCompositedVerifiedInputs::Binding Surface(const std::string& id) const
    {
        auto result = binding;
        result.surfaceId = id;
        return result;
    }

#if defined(VL_FILE_IO_TEST_HOOKS)
    fs::path InputPath(const std::string& id)
    {
        fs::path result;
        TerrainCompositedVTStoreTesting::InputObserver =
            [&](const fs::path& path, bool opened) { if (opened) result = path; };
        store->ReadInput(id);
        TerrainCompositedVTStoreTesting::InputObserver = {};
        result.make_preferred();
        return result;
    }
#endif
};

TEST_F(TerrainCompositedVerifiedInputTest, UniqueReadOneAcrossSurfacesAndLoaders)
{
    session->VerifyObject(binding, "compute");
    session->VerifyObject(binding, "compute");
    session->VerifyObject(Surface("other"), "compute");
    EXPECT_TRUE(session->HasVerified(binding, "compute"));
    EXPECT_TRUE(session->HasVerified(Surface("other"), "compute"));
    const auto stats = session->Statistics();
    EXPECT_EQ(stats.readCalls, 1u);
    EXPECT_EQ(stats.verifiedObjects, 1u);
    EXPECT_EQ(stats.requests, 3u);
    EXPECT_EQ(stats.hits, 2u);
    EXPECT_EQ(stats.requestedBytes, store->Production().Object("compute").bytes * 3);
    EXPECT_EQ(stats.actualReadBytes, store->Production().Object("compute").bytes);
    EXPECT_EQ(stats.actualReadBytes, stats.validatedBytes);
    EXPECT_EQ(stats.retainedBytes, 0u);
}

TEST_F(TerrainCompositedVerifiedInputTest, ReturnedBytesAreImmutableSharedSnapshot)
{
    static_assert(std::is_const_v<typename TerrainCompositedVerifiedInputs::Bytes::element_type>);
    const auto first = session->ReadVerified(binding, "albedo");
    const auto second = session->ReadVerified(Surface("other"), "albedo");
    ASSERT_TRUE(first);
    EXPECT_EQ(first.get(), second.get());
    session->VerifyObject(binding, "albedo");
    const auto& object = store->Production().Object("albedo");
    EXPECT_EQ(ContentHasher::HashBytes(first->data(), first->size()), object.digest);
    EXPECT_EQ(session->Statistics().readCalls, 1u);
    EXPECT_EQ(session->Statistics().retainedBytes, object.bytes);
}

TEST_F(TerrainCompositedVerifiedInputTest, ProofOnlyCannotAuthorizePayloadReread)
{
    session->VerifyObject(binding, "compute");
    const auto before = owner->Statistics().current.cpuBytes;
    EXPECT_THROW(session->ReadVerified(binding, "compute"), std::runtime_error);
    EXPECT_TRUE(session->HasVerified(binding, "compute"));
    EXPECT_EQ(session->Statistics().readCalls, 1u);
    EXPECT_EQ(session->Statistics().failures, 1u);
    EXPECT_EQ(owner->Statistics().current.cpuBytes, before);
}

TEST_F(TerrainCompositedVerifiedInputTest, DifferentStoreHandleCannotShareEvenIdenticalContent)
{
    auto different = binding;
    different.store = Publish("same-content");
    ASSERT_EQ(different.store->Production().ContentHash(), binding.productionIdentity);
    EXPECT_FALSE(session->Matches(different));
    EXPECT_THROW(session->VerifyObject(different, "compute"), std::invalid_argument);
    EXPECT_EQ(session->Statistics().readCalls, 0u);
}

TEST_F(TerrainCompositedVerifiedInputTest, WorldPublicationAndSurfaceMismatchRejected)
{
    session->VerifyObject(binding, "compute");
    auto different = binding;
    ++different.worldGeneration;
    EXPECT_THROW(session->VerifyObject(different, "compute"), std::invalid_argument);
    EXPECT_FALSE(session->HasVerified(different, "compute"));
    different = binding;
    different.productionIdentity = ContentHasher::HashString("new publication");
    EXPECT_FALSE(session->Matches(different));
    EXPECT_THROW(session->ReadVerified(different, "compute"), std::invalid_argument);
    EXPECT_THROW(session->VerifyObject(Surface("missing"), "compute"), std::exception);
    EXPECT_THROW(session->VerifyObject(binding, "artifact"), std::invalid_argument);
    EXPECT_EQ(session->Statistics().readCalls, 1u);
}

TEST_F(TerrainCompositedVerifiedInputTest, CowPublicationRequiresFreshSession)
{
    const auto changed = Publish("cow", 2);
    TerrainCompositedVerifiedInputs::Binding next{
        changed, binding.worldGeneration, changed->Production().ContentHash(), "surface"};
    ASSERT_NE(next.productionIdentity, binding.productionIdentity);
    session->Close();
    EXPECT_FALSE(session->Matches(next));
    auto fresh = TerrainCompositedVerifiedInputs::Begin(
        changed, next.worldGeneration, next.productionIdentity, owner);
    fresh->VerifyObject(next, "compute");
    EXPECT_EQ(fresh->Statistics().readCalls, 1u);
}

TEST_F(TerrainCompositedVerifiedInputTest, CloseRejectsNewVerificationAndNewScopes)
{
    const auto bytes = session->ReadVerified(binding, "compute");
    session->Close();
    const auto end = session->Statistics().endNanoseconds;
    EXPECT_GE(end, session->Statistics().beginNanoseconds);
    session->Close();
    EXPECT_EQ(end, session->Statistics().endNanoseconds);
    EXPECT_TRUE(session->HasVerified(binding, "compute"));
    EXPECT_FALSE(session->HasVerified(Surface("unused"), "compute"));
    EXPECT_FALSE(session->Matches(Surface("unused")));
    EXPECT_NO_THROW(session->VerifyObject(binding, "compute"));
    EXPECT_THROW(session->VerifyObject(Surface("unused"), "albedo"), std::runtime_error);
    EXPECT_THROW(session->ReadVerified(binding, "compute"), std::runtime_error);
    session.reset();
    EXPECT_EQ(std::string(bytes->begin(), bytes->end()), "void main() {}");
}

TEST_F(TerrainCompositedVerifiedInputTest, ClosedRegisteredProofConsumptionDoesNotRead)
{
    session->VerifyObject(binding, "compute");
    session->VerifyObject(Surface("other"), "compute");
    session->Close();
    const auto before = session->Statistics();
    EXPECT_TRUE(session->HasVerified(binding, "compute"));
    EXPECT_TRUE(session->HasVerified(Surface("other"), "compute"));
    EXPECT_NO_THROW(session->VerifyObject(binding, "compute"));
    EXPECT_NO_THROW(session->VerifyObject(Surface("other"), "compute"));
    EXPECT_EQ(session->Statistics().readCalls, before.readCalls);
    EXPECT_EQ(session->Statistics().actualReadBytes, before.actualReadBytes);
    EXPECT_EQ(session->Statistics().verifiedObjects, before.verifiedObjects);
    EXPECT_EQ(session->Statistics().hits, before.hits + 2);
    EXPECT_EQ(session->Statistics().failures, before.failures);
    EXPECT_THROW(session->ReadVerified(binding, "compute"), std::runtime_error);
}

TEST_F(TerrainCompositedVerifiedInputTest, ClosedSessionCannotRegisterNewScopeObjectReference)
{
    session->VerifyObject(binding, "compute");
    session->VerifyObject(Surface("other"), "albedo");
    session->Close();
    const auto before = session->Statistics().readCalls;
    EXPECT_TRUE(session->Matches(Surface("other")));
    EXPECT_FALSE(session->HasVerified(Surface("other"), "compute"));
    EXPECT_THROW(session->VerifyObject(Surface("other"), "compute"), std::runtime_error);
    EXPECT_FALSE(session->HasVerified(binding, "albedo"));
    EXPECT_THROW(session->VerifyObject(binding, "albedo"), std::runtime_error);
    EXPECT_THROW(session->VerifyObject(binding, "coverage-ids"), std::runtime_error);
    EXPECT_FALSE(session->Matches(Surface("unused")));
    EXPECT_THROW(session->VerifyObject(Surface("unused"), "compute"), std::runtime_error);
    EXPECT_EQ(session->Statistics().readCalls, before);
}

TEST_F(TerrainCompositedVerifiedInputTest, RetainedLeaseOutlivesCloseAndSession)
{
    auto bytes = session->ReadVerified(binding, "coverage-ids");
    session->Close();
    session.reset();
    EXPECT_GE(owner->Statistics().current.cpuBytes, bytes->size());
    bytes.reset();
    EXPECT_EQ(owner->Statistics().current.cpuBytes, 0u);
}

TEST_F(TerrainCompositedVerifiedInputTest, ProofDropsWholeVectorReservation)
{
    const auto before = owner->Statistics().current.cpuBytes;
    session->VerifyObject(binding, "coverage-ids");
    EXPECT_EQ(owner->Statistics().current.cpuBytes, before);
    EXPECT_GE(owner->Statistics().peak.cpuBytes,
        before + store->Production().Object("coverage-ids").bytes +
        TerrainCompositedVerifiedInputs::ReadScratchBytes);
}

TEST_F(TerrainCompositedVerifiedInputTest, BudgetRejectedBeforeReadAndFailureRollsBack)
{
    const auto used = owner->Statistics().current.cpuBytes;
    auto blocker = owner->TryReserve(
        {0, owner->Caps().totalCpuBytes - used - 4096, 0, 0},
        TerrainRvtCostClass::Input, TerrainRvtResourcePhase::Active);
    ASSERT_TRUE(blocker);
    const auto before = owner->Statistics().current.cpuBytes;
    EXPECT_THROW(session->VerifyObject(binding, "coverage-ids"), std::runtime_error);
    EXPECT_EQ(session->Statistics().readCalls, 0u);
    EXPECT_EQ(session->Statistics().failures, 1u);
    EXPECT_FALSE(session->HasVerified(binding, "coverage-ids"));
    EXPECT_EQ(owner->Statistics().current.cpuBytes, before);
    blocker.reset();
    session->VerifyObject(binding, "coverage-ids");
    EXPECT_EQ(session->Statistics().readCalls, 1u);
}

TEST_F(TerrainCompositedVerifiedInputTest, NullOwnerMetadataAndObjectBoundsRejected)
{
    EXPECT_THROW(TerrainCompositedVerifiedInputs::Begin(
        store, 17, binding.productionIdentity, {}), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedVerifiedInputs::Begin(
        store, 17, binding.productionIdentity, owner, 1), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedVerifiedInputs::Begin(
        store, 17, binding.productionIdentity, owner, 65537), std::invalid_argument);
    EXPECT_THROW(TerrainCompositedVerifiedInputs::Begin(
        store, 17, ContentHasher::HashString("wrong"), owner), std::invalid_argument);
    auto tiny = Budget(256);
    EXPECT_THROW(TerrainCompositedVerifiedInputs::Begin(
        store, 17, binding.productionIdentity, tiny), std::runtime_error);
    EXPECT_EQ(tiny->Statistics().current.cpuBytes, 0u);
}

TEST_F(TerrainCompositedVerifiedInputTest, ConcurrentSurfacesReadOnce)
{
    std::atomic<unsigned> failures{0};
    std::vector<std::thread> workers;
    for (unsigned index = 0; index < 12; ++index)
        workers.emplace_back([&, index]
        {
            try
            {
                session->VerifyObject(Surface(index % 2 == 0 ? "surface" : "other"), "coverage-ids");
            }
            catch (...)
            {
                ++failures;
            }
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(failures.load(), 0u);
    EXPECT_EQ(session->Statistics().readCalls, 1u);
    EXPECT_EQ(session->Statistics().hits, 11u);
}

#if defined(VL_FILE_IO_TEST_HOOKS)
TEST_F(TerrainCompositedVerifiedInputTest, RealStoreReadCountAndPreallocationReservation)
{
    const auto before = owner->Statistics().current.cpuBytes;
    unsigned reads = 0;
    TerrainCompositedVTStoreTesting::InputObserver = [&](const fs::path&, bool opened)
    {
        if (!opened)
            ++reads;
        EXPECT_GE(owner->Statistics().current.cpuBytes,
            before + store->Production().Object("coverage-ids").bytes +
            TerrainCompositedVerifiedInputs::ReadScratchBytes);
    };
    session->VerifyObject(binding, "coverage-ids");
    session->VerifyObject(Surface("other"), "coverage-ids");
    EXPECT_EQ(reads, 1u);
}

TEST_F(TerrainCompositedVerifiedInputTest, DamagedPayloadRejectedAndRetryNotCached)
{
    const auto path = InputPath("compute");
    ASSERT_FALSE(path.empty());
    const auto original = store->ReadInput("compute");
    auto damaged = original;
    damaged[0] ^= 1;
    TerrainProductionFixture::WriteFixtureBytes(path, damaged);
    const auto before = owner->Statistics().current.cpuBytes;
    EXPECT_THROW(session->ReadVerified(binding, "compute"), std::exception);
    EXPECT_FALSE(session->HasVerified(binding, "compute"));
    EXPECT_EQ(owner->Statistics().current.cpuBytes, before);
    EXPECT_EQ(session->Statistics().validatedBytes, 0u);
    TerrainProductionFixture::WriteFixtureBytes(path, original);
    const auto restored = session->ReadVerified(binding, "compute");
    EXPECT_EQ(*restored, original);
    EXPECT_EQ(session->Statistics().readCalls, 2u);
    EXPECT_EQ(session->Statistics().failures, 1u);
}

TEST_F(TerrainCompositedVerifiedInputTest, DiskMutationCannotReplaceRetainedSnapshot)
{
    const auto path = InputPath("compute");
    const auto bytes = session->ReadVerified(binding, "compute");
    auto damaged = *bytes;
    damaged[0] ^= 1;
    TerrainProductionFixture::WriteFixtureBytes(path, damaged);
    EXPECT_THROW(store->ReadInput("compute"), std::exception);
    const auto same = session->ReadVerified(Surface("other"), "compute");
    EXPECT_EQ(same.get(), bytes.get());
    EXPECT_EQ(std::string(same->begin(), same->end()), "void main() {}");
    EXPECT_EQ(session->Statistics().readCalls, 1u);
}
#endif
}
