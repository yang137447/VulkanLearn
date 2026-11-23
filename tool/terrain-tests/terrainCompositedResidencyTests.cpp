#include <gtest/gtest.h>

#include "terrain/terrainCompositedResidency.h"
#include "terrainCompositedProductionFixture.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

namespace VL
{
namespace
{
TEST(IndependentP2, RevisionChangesWhileRootProductionInFlight)
{
    TerrainCompositedVTBuildDesc desc;
    desc.surfaceId = "independent";
    desc.width = 256;
    desc.height = 128;
    desc.encoding = "cpu-audit";
    desc.channels = {TerrainCompositedVTFormat::RGBA8UNorm};
    TerrainCompositedResidencyBudget caps;
    caps.slots = 4;
    TerrainCompositedResidency residency({}, desc, 1, 0, caps);
    const auto requests = residency.ProductionRequests();
    ASSERT_EQ(requests.size(), 1u);
    const auto root = requests.front();
    const auto reserved = residency.ReserveProduction(root, 0);
    ASSERT_TRUE(reserved);
    residency.InvalidatePreview(desc, 1, 1, {root.key});
    EXPECT_FALSE(residency.CompleteProduction(*reserved, 1, true));
    const auto batch = residency.BeginUploads(1);
    residency.Abort(batch.id);
    EXPECT_FALSE(residency.Ready());
    const auto replacement = residency.ProductionRequests();
    ASSERT_EQ(replacement.size(), 1u);
    EXPECT_EQ(replacement.front().key, root.key);
    EXPECT_EQ(replacement.front().revision, 1u);
}

class TerrainCompositedResidencyTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        directory = std::filesystem::temp_directory_path() /
            ("vl-composited-residency-" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++sequence));
        std::filesystem::create_directories(directory);
        desc.surfaceId = "surface";
        desc.width = 256;
        desc.height = 128;
        desc.encoding = "residency-test";
        desc.channels = {TerrainCompositedVTFormat::RGBA8UNorm};
        auto layout = desc.Layout();
        std::vector<TerrainCompositedVTPageData> pages;
        for (uint32_t mip = 0; mip < layout.MipCount(); ++mip)
            for (uint32_t y = 0; y < layout.Mip(mip).pagesY; ++y)
                for (uint32_t x = 0; x < layout.Mip(mip).pagesX; ++x)
                    pages.push_back({{desc.surfaceId, mip, x, y},
                        std::vector<uint8_t>(desc.PageBytes(), static_cast<uint8_t>(mip + x))});
        const auto publication = TerrainProductionFixture::Publication(directory, {desc}, pages);
        desc = publication.surfaces.front();
        TerrainCompositedVTStore::Publish(directory / "manifest.json", publication, pages);
        store = std::make_shared<const TerrainCompositedVTStore>(
            TerrainCompositedVTStore::Open(directory / "manifest.json"));
        baseline = {desc.surfaceId, layout.MipCount() - 1, 0, 0};
        limits.slots = 3;
        limits.concurrentReads = 1;
        limits.uploadsPerFrame = 1;
        limits.cpuBytes = desc.PageBytes() * 2;
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    TerrainCompositedResidencyBatch WaitBatch(TerrainCompositedResidency& residency,
        uint64_t completed = 0)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < deadline)
        {
            auto batch = residency.BeginUploads(completed);
            if (!batch.uploads.empty() || !batch.removals.empty())
                return batch;
            residency.Abort(batch.id);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ADD_FAILURE() << "Timed out waiting for asynchronous page read";
        return {};
    }

    void LoadBaseline(TerrainCompositedResidency& residency)
    {
        const auto batch = WaitBatch(residency);
        ASSERT_EQ(batch.uploads.size(), 1u);
        EXPECT_EQ(batch.uploads[0].token.key, baseline);
        residency.Commit(batch.id, 1);
    }

    void FillUnselectedSlot(TerrainCompositedResidency& residency)
    {
        residency.ConsumeFeedback({residency.Token({"surface", 1, 0, 0})});
        const auto batch = WaitBatch(residency);
        ASSERT_EQ(batch.uploads.size(), 1u);
        residency.Commit(batch.id, 2);
    }

    std::filesystem::path directory;
    TerrainCompositedVTBuildDesc desc;
    std::shared_ptr<const TerrainCompositedVTStore> store;
    TerrainCompositedResidencyBudget limits;
    TerrainCompositedVTPageKey baseline;
};

TEST_F(TerrainCompositedResidencyTest, BaselineRequiresSubmitAndFineMissUsesParent)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    EXPECT_FALSE(residency.Ready());
    auto batch = WaitBatch(residency);
    EXPECT_FALSE(residency.Ready());
    ASSERT_EQ(batch.uploads.size(), 1u);
    const auto slot = batch.uploads[0].slot;
    residency.Commit(batch.id, 1);
    EXPECT_TRUE(residency.Ready());
    EXPECT_EQ(residency.Resolve({"surface", 0, 1, 0}), std::optional<uint32_t>(slot));
    EXPECT_EQ(residency.Statistics().parentFallbacks, 1u);
}

TEST_F(TerrainCompositedResidencyTest, AbortRetainsPayloadAndDoesNotPublishMapping)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    auto first = WaitBatch(residency);
    ASSERT_EQ(first.uploads.size(), 1u);
    auto retained = first.uploads[0].bytes;
    residency.Abort(first.id);
    EXPECT_FALSE(residency.Ready());
    auto retry = residency.BeginUploads(0);
    ASSERT_EQ(retry.uploads.size(), 1u);
    EXPECT_EQ(retry.uploads[0].bytes, retained);
    EXPECT_EQ(residency.Statistics().readsStarted, 1u);
    residency.Commit(retry.id, 1);
    EXPECT_TRUE(residency.Ready());
}

TEST_F(TerrainCompositedResidencyTest, ZeroFrameAllowanceLeavesRequestsQueued)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    for (const auto& allowance : std::vector<std::pair<uint64_t, uint32_t>>{
        {0, 1}, {desc.PageBytes(), 0}, {desc.PageBytes() - 1, 1}})
    {
        const auto batch = residency.BeginUploads(0, allowance.first, allowance.second);
        EXPECT_TRUE(batch.uploads.empty());
        EXPECT_TRUE(batch.removals.empty());
        EXPECT_EQ(residency.Statistics().pending, 1u);
        EXPECT_EQ(residency.Statistics().reading, 0u);
        EXPECT_EQ(residency.Statistics().readsStarted, 0u);
        residency.Abort(batch.id);
    }
    const auto upload = WaitBatch(residency);
    ASSERT_EQ(upload.uploads.size(), 1u);
    residency.Commit(upload.id, 1);
    EXPECT_TRUE(residency.Ready());
}

TEST_F(TerrainCompositedResidencyTest, FrameAllowanceLimitsPartialBatchAndAbortRetainsAllPayloads)
{
    limits.slots = 4;
    limits.uploadsPerFrame = 4;
    limits.cpuBytes = desc.PageBytes() * 4;
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    residency.SubmitPreviewPages(desc, 10, 20, {
        {baseline, std::vector<uint8_t>(desc.PageBytes(), 127)},
        {{"surface", 0, 0, 0}, std::vector<uint8_t>(desc.PageBytes(), 128)},
        {{"surface", 0, 1, 0}, std::vector<uint8_t>(desc.PageBytes(), 129)}});
    const auto first = residency.BeginUploads(0, desc.PageBytes() * 2 - 1, 4);
    ASSERT_EQ(first.uploads.size(), 1u);
    const auto retained = first.uploads[0].bytes;
    residency.Abort(first.id);
    const auto zero = residency.BeginUploads(0, 0, 4);
    EXPECT_TRUE(zero.uploads.empty());
    residency.Commit(zero.id, 1);
    const auto retry = residency.BeginUploads(1, desc.PageBytes() * 4, 1);
    ASSERT_EQ(retry.uploads.size(), 1u);
    EXPECT_EQ(retry.uploads[0].bytes, retained);
    residency.Commit(retry.id, 2);
    const auto rest = residency.BeginUploads(2, desc.PageBytes() * 2, 4);
    ASSERT_EQ(rest.uploads.size(), 2u);
    residency.Commit(rest.id, 3);
    EXPECT_EQ(residency.Statistics().uploads, 3u);
    EXPECT_TRUE(residency.Ready());
}

TEST_F(TerrainCompositedResidencyTest, SharedAllowanceWithRotatingSurfacesPreventsStarvation)
{
    limits.slots = 4;
    limits.uploadsPerFrame = 4;
    limits.cpuBytes = desc.PageBytes() * 4;
    std::vector<std::unique_ptr<TerrainCompositedResidency>> surfaces;
    for (uint32_t index = 0; index < 3; ++index)
    {
        auto surface = desc;
        surface.surfaceId = "surface" + std::to_string(index);
        surfaces.push_back(std::make_unique<TerrainCompositedResidency>(
            nullptr, surface, 10, 20, limits));
        surfaces.back()->SubmitPreviewPages(surface, 10, 20, {
            {{surface.surfaceId, surface.Layout().MipCount() - 1, 0, 0},
                std::vector<uint8_t>(surface.PageBytes(), 127)},
            {{surface.surfaceId, 0, 0, 0}, std::vector<uint8_t>(surface.PageBytes(), 128)},
            {{surface.surfaceId, 0, 1, 0}, std::vector<uint8_t>(surface.PageBytes(), 129)}});
    }
    for (uint64_t frame = 0; frame < 3; ++frame)
    {
        uint64_t remainingBytes = desc.PageBytes();
        uint32_t remainingPages = 1;
        uint32_t uploaded = 0;
        for (uint32_t offset = 0; offset < surfaces.size(); ++offset)
        {
            auto& surface = *surfaces[(frame + offset) % surfaces.size()];
            const auto batch = surface.BeginUploads(frame, remainingBytes, remainingPages);
            const auto count = static_cast<uint32_t>(batch.uploads.size());
            ASSERT_LE(count, remainingPages);
            ASSERT_LE(uint64_t(count) * desc.PageBytes(), remainingBytes);
            remainingPages -= count;
            remainingBytes -= uint64_t(count) * desc.PageBytes();
            uploaded += count;
            surface.Commit(batch.id, frame + 1);
        }
        EXPECT_EQ(uploaded, 1u);
        EXPECT_EQ(remainingPages, 0u);
        EXPECT_EQ(remainingBytes, 0u);
    }
    for (const auto& surface : surfaces)
        EXPECT_EQ(surface->Statistics().uploads, 1u);
}

TEST_F(TerrainCompositedResidencyTest, ZeroAllowanceDoesNotEvictResidentPages)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    FillUnselectedSlot(residency);
    residency.ConsumeFeedback({residency.Token({"surface", 0, 0, 0})});
    const auto first = WaitBatch(residency);
    residency.Commit(first.id, 3);
    residency.ConsumeFeedback({residency.Token({"surface", 0, 1, 0})});
    const auto before = residency.Statistics();
    const auto denied = residency.BeginUploads(3, desc.PageBytes(), 0);
    EXPECT_TRUE(denied.removals.empty());
    EXPECT_TRUE(denied.uploads.empty());
    EXPECT_EQ(residency.Statistics().readsStarted, before.readsStarted);
    EXPECT_EQ(residency.Statistics().pending, before.pending);
    residency.Abort(denied.id);
    EXPECT_EQ(residency.Statistics().resident, 3u);
    const auto allowed = WaitBatch(residency, 3);
    ASSERT_EQ(allowed.removals.size(), 1u);
    residency.Abort(allowed.id);
    EXPECT_EQ(residency.Statistics().resident, 3u);
}

TEST_F(TerrainCompositedResidencyTest, RejectsGenerationRevisionHashAndInvalidAddress)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    auto token = residency.Token({"surface", 0, 0, 0});
    auto oldGeneration = token;
    oldGeneration.worldGeneration = 9;
    auto oldRevision = token;
    oldRevision.revision = 19;
    auto oldHash = token;
    oldHash.contentHash.bytes[0] ^= 1;
    auto invalid = residency.Token({"surface", 0, 2, 0});
    residency.ConsumeFeedback({oldGeneration, oldRevision, oldHash, invalid});
    EXPECT_EQ(residency.Statistics().discardedResults, 4u);
    EXPECT_EQ(residency.Statistics().pending, 0u);
}

TEST_F(TerrainCompositedResidencyTest, DuplicateFeedbackStartsSingleRead)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    auto token = residency.Token({"surface", 0, 0, 0});
    residency.ConsumeFeedback(std::vector<TerrainCompositedVTTaskToken>(100, token));
    auto batch = WaitBatch(residency);
    ASSERT_EQ(batch.uploads.size(), 1u);
    EXPECT_EQ(batch.uploads[0].token.key, token.key);
    EXPECT_EQ(residency.Statistics().readsStarted, 2u);
    residency.Commit(batch.id, 2);
}

TEST_F(TerrainCompositedResidencyTest, EvictionNeedsUnmapFenceAndKeepsBaselinePinned)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    FillUnselectedSlot(residency);
    const auto first = residency.Token({"surface", 0, 0, 0});
    residency.ConsumeFeedback({first});
    auto firstUpload = WaitBatch(residency);
    ASSERT_EQ(firstUpload.uploads.size(), 1u);
    const auto physicalSlot = firstUpload.uploads[0].slot;
    residency.Commit(firstUpload.id, 3);
    residency.ConsumeFeedback({residency.Token({"surface", 0, 1, 0})});
    auto unmap = WaitBatch(residency);
    ASSERT_EQ(unmap.removals.size(), 1u);
    EXPECT_EQ(unmap.removals[0], (TerrainCompositedVTPageKey{"surface", 1, 0, 0}));
    EXPECT_TRUE(unmap.uploads.empty());
    residency.Commit(unmap.id, 4);
    auto tooEarly = residency.BeginUploads(3);
    EXPECT_TRUE(tooEarly.uploads.empty());
    residency.Abort(tooEarly.id);
    EXPECT_EQ(residency.Statistics().retiring, 1u);
    auto safe = residency.BeginUploads(4);
    ASSERT_EQ(safe.uploads.size(), 1u);
    EXPECT_NE(safe.uploads[0].slot, physicalSlot);
    residency.Commit(safe.id, 5);
    EXPECT_TRUE(residency.Ready());
}

TEST_F(TerrainCompositedResidencyTest, EvictedPagesCanBeReadMoreThanRetryLimit)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    uint64_t epoch = 1;
    for (uint32_t iteration = 0; iteration < 12; ++iteration)
    {
        auto token = residency.Token({"surface", iteration % 3 == 2 ? 1u : 0u,
            iteration % 3 == 1 ? 1u : 0u, 0});
        residency.ConsumeFeedback({token});
        auto batch = WaitBatch(residency, epoch);
        if (!batch.removals.empty())
        {
            residency.Commit(batch.id, ++epoch);
            batch = WaitBatch(residency, epoch);
        }
        ASSERT_EQ(batch.uploads.size(), 1u);
        EXPECT_EQ(batch.uploads[0].token.key, token.key);
        residency.Commit(batch.id, ++epoch);
    }
    EXPECT_EQ(residency.Statistics().readFailures, 0u);
    EXPECT_EQ(residency.Statistics().uploads, 13u);
}

TEST_F(TerrainCompositedResidencyTest, EnforcesCpuAndFrameBudget)
{
    limits.cpuBytes = desc.PageBytes();
    limits.uploadBytesPerFrame = desc.PageBytes();
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    residency.ConsumeFeedback({residency.Token({"surface", 0, 0, 0}),
        residency.Token({"surface", 0, 1, 0})});
    for (uint64_t epoch = 1; epoch <= 10; ++epoch)
    {
        auto batch = residency.BeginUploads(epoch - 1);
        EXPECT_LE(batch.uploads.size(), 1u);
        EXPECT_LE(residency.Statistics().cpuBytes, limits.cpuBytes);
        residency.Commit(batch.id, epoch);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

TEST_F(TerrainCompositedResidencyTest, CorruptBaselineFailsWithBoundedRetries)
{
    {
        std::fstream package(store->PackagePath(), std::ios::in | std::ios::out | std::ios::binary);
        package.seekp(static_cast<std::streamoff>(store->Pages().at(baseline).offset));
        package.put(static_cast<char>(0xff));
    }
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    for (uint64_t epoch = 1; epoch <= 200 && !residency.Statistics().baselineFailed; ++epoch)
    {
        auto batch = residency.BeginUploads(epoch - 1);
        EXPECT_TRUE(batch.uploads.empty());
        residency.Commit(batch.id, epoch);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_FALSE(residency.Ready());
    EXPECT_TRUE(residency.Statistics().baselineFailed);
    EXPECT_EQ(residency.Statistics().readsStarted, limits.maxReadAttempts);
    EXPECT_EQ(residency.Statistics().readFailures, limits.maxReadAttempts);
}

TEST_F(TerrainCompositedResidencyTest, RejectsUndersizedBudgetsAndMismatchedStore)
{
    limits.cpuBytes = desc.PageBytes() - 1;
    EXPECT_THROW(TerrainCompositedResidency(store, desc, 10, 20, limits), std::invalid_argument);
    limits.cpuBytes = desc.PageBytes();
    limits.uploadBytesPerFrame = desc.PageBytes() - 1;
    EXPECT_THROW(TerrainCompositedResidency(store, desc, 10, 20, limits), std::invalid_argument);
    limits.uploadBytesPerFrame = desc.PageBytes();
    desc.dependencyHash.bytes[0] ^= 1;
    EXPECT_THROW(TerrainCompositedResidency(store, desc, 10, 20, limits), std::invalid_argument);
}

TEST_F(TerrainCompositedResidencyTest, TransactionIdsAndEpochsAreStrict)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    auto batch = WaitBatch(residency);
    EXPECT_THROW(residency.BeginUploads(0), std::logic_error);
    EXPECT_THROW(residency.Commit(batch.id + 1, 1), std::logic_error);
    EXPECT_THROW(residency.Commit(batch.id, 0), std::logic_error);
    residency.Commit(batch.id, 1);
    EXPECT_THROW(residency.Abort(batch.id), std::logic_error);
    auto next = residency.BeginUploads(1);
    EXPECT_THROW(residency.Commit(next.id, 1), std::logic_error);
    residency.Abort(next.id);
}

TEST_F(TerrainCompositedResidencyTest, PreviewCanStartWithoutDiskStore)
{
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    const auto requests = residency.PreviewRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].key, baseline);
    residency.SubmitPreviewPages(desc, 10, 20,
        {{baseline, std::vector<uint8_t>(desc.PageBytes(), 127)}});
    auto batch = residency.BeginUploads(0);
    ASSERT_EQ(batch.uploads.size(), 1u);
    EXPECT_EQ(residency.Statistics().readsStarted, 0u);
    EXPECT_FALSE(residency.Ready());
    residency.Commit(batch.id, 1);
    EXPECT_TRUE(residency.Ready());
    EXPECT_EQ(residency.Statistics().dirty, 0u);
}

TEST_F(TerrainCompositedResidencyTest, PreviewReplacesBaselineWithoutOverwritingOldSlot)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    const auto oldSlot = residency.Resolve(baseline);
    desc.dependencyHash.bytes[0] ^= 1;
    residency.InvalidatePreview(desc, 10, 21, {baseline});
    EXPECT_TRUE(residency.Ready());
    EXPECT_EQ(residency.Statistics().stale, 1u);
    residency.SubmitPreviewPages(desc, 10, 21,
        {{baseline, std::vector<uint8_t>(desc.PageBytes(), 127)}});
    auto batch = residency.BeginUploads(1);
    ASSERT_EQ(batch.uploads.size(), 1u);
    EXPECT_NE(batch.uploads[0].slot, *oldSlot);
    EXPECT_EQ(residency.Resolve(baseline), oldSlot);
    residency.Abort(batch.id);
    EXPECT_EQ(residency.Statistics().dirty, 1u);
    auto retry = residency.BeginUploads(1);
    const auto newSlot = retry.uploads[0].slot;
    residency.Commit(retry.id, 2);
    EXPECT_EQ(residency.Resolve(baseline), std::optional<uint32_t>(newSlot));
    EXPECT_EQ(residency.Statistics().dirty, 0u);
    EXPECT_EQ(residency.Statistics().retiring, 1u);
    auto safe = residency.BeginUploads(2);
    residency.Abort(safe.id);
    EXPECT_EQ(residency.Statistics().retiring, 0u);
}

TEST_F(TerrainCompositedResidencyTest, LatePreviewRevisionCannotClearNewDirty)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    auto previous = desc;
    desc.dependencyHash.bytes[0] ^= 1;
    residency.InvalidatePreview(desc, 10, 21, {baseline});
    const std::vector<TerrainCompositedVTPageData> pages{
        {baseline, std::vector<uint8_t>(desc.PageBytes(), 127)}};
    EXPECT_THROW(residency.SubmitPreviewPages(previous, 10, 20, pages), std::invalid_argument);
    EXPECT_THROW(residency.SubmitPreviewPages(desc, 9, 21, pages), std::invalid_argument);
    EXPECT_EQ(residency.Statistics().dirty, 1u);
    EXPECT_EQ(residency.Resolve(baseline).has_value(), true);
}

TEST_F(TerrainCompositedResidencyTest, PreviewDemandAndCpuBudgetAreExplicit)
{
    limits.cpuBytes = desc.PageBytes();
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    residency.SubmitPreviewPages(desc, 10, 20,
        {{baseline, std::vector<uint8_t>(desc.PageBytes(), 127)}});
    EXPECT_THROW(residency.SubmitPreviewPages(desc, 10, 20,
        {{{"surface", 0, 0, 0}, std::vector<uint8_t>(desc.PageBytes(), 127)}}),
        std::runtime_error);
    auto batch = residency.BeginUploads(0);
    residency.Commit(batch.id, 1);
    residency.ConsumeFeedback({residency.Token({"surface", 0, 1, 0})});
    const auto requests = residency.PreviewRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].key.x, 1u);
}

TEST_F(TerrainCompositedResidencyTest, QueuedFeedbackIsBounded)
{
    limits.slots = 4;
    limits.queuedRequests = 1;
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    residency.ConsumeFeedback({residency.Token({"surface", 0, 0, 0}),
        residency.Token({"surface", 0, 1, 0})});
    EXPECT_EQ(residency.Statistics().pending, 1u);
    EXPECT_EQ(residency.Statistics().throttledRequests, 1u);
}

TEST_F(TerrainCompositedResidencyTest, PreviewInvalidationDiscardsOutstandingStoreRead)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    auto started = residency.BeginUploads(0);
    residency.Abort(started.id);
    ASSERT_EQ(residency.Statistics().readsStarted, 1u);
    desc.dependencyHash.bytes[0] ^= 1;
    residency.InvalidatePreview(desc, 10, 21, {baseline});
    for (uint64_t epoch = 1; epoch <= 200 && residency.Statistics().reading; ++epoch)
    {
        auto batch = residency.BeginUploads(epoch - 1);
        EXPECT_TRUE(batch.uploads.empty());
        residency.Commit(batch.id, epoch);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    EXPECT_EQ(residency.Statistics().discardedResults, 1u);
    EXPECT_EQ(residency.Statistics().readFailures, 0u);
    EXPECT_FALSE(residency.Ready());
    const auto requests = residency.PreviewRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests[0].revision, 21u);
    EXPECT_EQ(requests[0].key, baseline);
}

TEST_F(TerrainCompositedResidencyTest, StreamsFullMipChainBeyondFirstSinglePageTail)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    const auto layout = desc.Layout();
    EXPECT_EQ(baseline.mip, layout.MipCount() - 1);
    ASSERT_GT(baseline.mip, layout.TailMip());
    LoadBaseline(residency);
    const auto target = residency.Token({"surface", baseline.mip - 1, 0, 0});
    residency.ConsumeFeedback({target});
    auto batch = WaitBatch(residency);
    ASSERT_EQ(batch.uploads.size(), 1u);
    EXPECT_EQ(batch.uploads[0].token.key, target.key);
    residency.Commit(batch.id, 2);
    EXPECT_EQ(residency.Statistics().discardedResults, 0u);
    EXPECT_TRUE(residency.Resolve(target.key).has_value());
}

TEST_F(TerrainCompositedResidencyTest, ReadOnlySeedUsesLoadingEpochZeroWithoutOffsettingRealEpochs)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    residency.SeedReadOnlyBaseline();
    EXPECT_TRUE(residency.PreviewRequests().empty());
    auto loading = residency.BeginUploads(0);
    ASSERT_EQ(loading.uploads.size(), 1u);
    EXPECT_EQ(loading.uploads[0].token.key, baseline);
    residency.Commit(loading.id, 0);
    EXPECT_TRUE(residency.Ready());
    EXPECT_THROW(residency.SeedReadOnlyBaseline(), std::logic_error);
    residency.ConsumeFeedback({residency.Token({"surface", 0, 0, 0})});
    EXPECT_TRUE(residency.PreviewRequests().empty());
    auto frame = WaitBatch(residency, 0);
    ASSERT_EQ(frame.uploads.size(), 1u);
    EXPECT_THROW(residency.Commit(frame.id, 0), std::logic_error);
    residency.Commit(frame.id, 1);
    EXPECT_EQ(residency.Statistics().dirty, 0u);
    EXPECT_EQ(residency.Statistics().uploads, 2u);
}

TEST_F(TerrainCompositedResidencyTest, AbortedReadOnlySeedCanRetryZeroButPreviewCannotUseZero)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    residency.SeedReadOnlyBaseline();
    auto cancelled = residency.BeginUploads(0);
    residency.Abort(cancelled.id);
    auto retry = residency.BeginUploads(0);
    residency.Commit(retry.id, 0);
    auto empty = residency.BeginUploads(0);
    EXPECT_THROW(residency.Commit(empty.id, 0), std::logic_error);
    residency.Abort(empty.id);
    TerrainCompositedResidency preview({}, desc, 10, 20, limits);
    EXPECT_THROW(preview.SeedReadOnlyBaseline(), std::logic_error);
    preview.SubmitPreviewPages(desc, 10, 20,
        {{baseline, std::vector<uint8_t>(desc.PageBytes(), 127)}});
    auto invalid = preview.BeginUploads(0);
    EXPECT_THROW(preview.Commit(invalid.id, 0), std::logic_error);
    preview.Abort(invalid.id);
}

TEST_F(TerrainCompositedResidencyTest, GpuProductionPublishesOnlyAfterCompletionAndTableCommit)
{
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    auto reserved = residency.ReserveProduction(residency.Token(baseline), 0);
    ASSERT_TRUE(reserved);
    EXPECT_FALSE(residency.Ready());
    auto before = residency.BeginUploads(0);
    EXPECT_TRUE(before.uploads.empty());
    residency.Abort(before.id);
    EXPECT_TRUE(residency.CompleteProduction(*reserved, 1, true));
    EXPECT_FALSE(residency.Ready());
    auto publish = residency.BeginUploads(1);
    ASSERT_EQ(publish.uploads.size(), 1u);
    EXPECT_FALSE(publish.uploads.front().bytes);
    residency.Abort(publish.id);
    EXPECT_FALSE(residency.Ready());
    auto retry = residency.BeginUploads(1);
    ASSERT_EQ(retry.uploads.size(), 1u);
    EXPECT_EQ(retry.uploads.front().slot, reserved->slot);
    EXPECT_THROW(residency.Commit(retry.id, 0), std::logic_error);
    residency.Commit(retry.id, 2);
    EXPECT_TRUE(residency.Ready());
}

TEST_F(TerrainCompositedResidencyTest, CorruptSeedQueuesGpuRootWhileInstallationValidationStillFails)
{
    {
        std::fstream package(store->PackagePath(), std::ios::in | std::ios::out | std::ios::binary);
        package.seekp(static_cast<std::streamoff>(store->Pages().at(baseline).offset));
        package.put(static_cast<char>(0xff));
    }
    EXPECT_THROW(store->ValidateAll(), std::exception);
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    EXPECT_NO_THROW(residency.SeedReadOnlyBaseline());
    EXPECT_FALSE(residency.Ready());
    const auto requests = residency.ProductionRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_EQ(requests.front().key, baseline);
    auto reserved = residency.ReserveProduction(requests.front(), 0);
    ASSERT_TRUE(reserved);
    EXPECT_TRUE(residency.CompleteProduction(*reserved, 0, true));
    auto loading = residency.BeginUploads(0);
    ASSERT_EQ(loading.uploads.size(), 1u);
    EXPECT_FALSE(loading.uploads.front().bytes);
    residency.Commit(loading.id, 0);
    EXPECT_TRUE(residency.Ready());
    EXPECT_EQ(residency.Statistics().readsStarted, 0u);
    EXPECT_THROW(store->ValidateAll(), std::exception);
}

TEST_F(TerrainCompositedResidencyTest, SparseMissingPageRequestsProductionWithoutIoFailure)
{
    std::vector<TerrainCompositedVTPageData> roots{
        {baseline, std::vector<uint8_t>(desc.PageBytes(), 127)}};
    const auto publication = TerrainProductionFixture::Publication(directory, {desc}, roots);
    TerrainCompositedVTStore::Publish(directory / "sparse.json", publication, roots);
    auto sparse = std::make_shared<const TerrainCompositedVTStore>(
        TerrainCompositedVTStore::Open(directory / "sparse.json"));
    TerrainCompositedResidency residency(sparse, publication.surfaces.front(), 10, 20, limits);
    LoadBaseline(residency);
    const auto token = residency.Token({"surface", 0, 1, 0});
    residency.ConsumeFeedback({token});
    for (uint64_t epoch = 2; epoch < 8; ++epoch)
    {
        auto batch = residency.BeginUploads(epoch - 1);
        EXPECT_TRUE(batch.uploads.empty());
        residency.Commit(batch.id, epoch);
    }
    const auto requests = residency.ProductionRequests();
    ASSERT_EQ(requests.size(), 1u);
    EXPECT_TRUE(requests.front().Matches(token));
    EXPECT_EQ(residency.Statistics().readsStarted, 1u);
    EXPECT_EQ(residency.Statistics().readFailures, 0u);
}

TEST_F(TerrainCompositedResidencyTest, FailedGpuStatusRetiresSlotAndKeepsRootUnpublished)
{
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    auto reserved = residency.ReserveProduction(residency.Token(baseline), 0);
    ASSERT_TRUE(reserved);
    EXPECT_FALSE(residency.CompleteProduction(*reserved, 7, false));
    EXPECT_FALSE(residency.Ready());
    EXPECT_EQ(residency.Statistics().retiring, 1u);
    EXPECT_EQ(residency.Statistics().readFailures, 1u);
    auto batch = residency.BeginUploads(6);
    EXPECT_TRUE(batch.uploads.empty());
    EXPECT_EQ(residency.Statistics().retiring, 1u);
    residency.Abort(batch.id);
}

TEST_F(TerrainCompositedResidencyTest, LateGpuRevisionCannotPublishOrReuseBeforeCompletedEpoch)
{
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    const auto old = residency.ReserveProduction(residency.Token(baseline), 0);
    ASSERT_TRUE(old);
    auto revised = desc;
    revised.dependencyHash.bytes[0] ^= 1;
    residency.InvalidatePreview(revised, 10, 21, {baseline});
    EXPECT_FALSE(residency.CurrentRoot());
    EXPECT_FALSE(residency.CompleteProduction(*old, 7, true));
    EXPECT_FALSE(residency.Ready());
    auto batch = residency.BeginUploads(6);
    EXPECT_TRUE(batch.uploads.empty());
    EXPECT_EQ(residency.Statistics().retiring, 1u);
    residency.Abort(batch.id);
    auto fresh = residency.ReserveProduction(residency.Token(baseline), 6);
    ASSERT_TRUE(fresh);
    EXPECT_NE(fresh->slot, old->slot);
    EXPECT_TRUE(residency.CompleteProduction(*fresh, 8, true));
    auto publish = residency.BeginUploads(8);
    ASSERT_EQ(publish.uploads.size(), 1u);
    EXPECT_EQ(publish.uploads.front().token.revision, 21u);
    residency.Commit(publish.id, 9);
    EXPECT_TRUE(residency.Ready());
    EXPECT_TRUE(residency.CurrentRoot());
    EXPECT_EQ(residency.Statistics().discardedResults, 1u);
}

TEST_F(TerrainCompositedResidencyTest, FeedbackObservesEstimatedMissParentAndExactWithoutClaimingPixels)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    const auto fine = residency.Token({"surface", 0, 0, 0});
    residency.ConsumeFeedback({fine});
    EXPECT_EQ(residency.Statistics().feedbackEstimatedMisses, 1u);
    LoadBaseline(residency);
    residency.ConsumeFeedback({fine, residency.Token(baseline)});
    const auto stats = residency.Statistics();
    EXPECT_EQ(stats.feedbackRequests, 3u);
    EXPECT_EQ(stats.feedbackTargetMipCounts[0], 2u);
    EXPECT_EQ(stats.feedbackTargetMipCounts[baseline.mip], 1u);
    EXPECT_EQ(stats.feedbackEstimatedParentFallbacks, 1u);
    EXPECT_EQ(stats.feedbackEstimatedExactHits, 1u);
    EXPECT_EQ(stats.feedbackEstimatedResolvedMipCounts[baseline.mip], 2u);
    EXPECT_EQ(stats.feedbackEstimatedMisses, 1u);
}

TEST_F(TerrainCompositedResidencyTest, FeedbackRejectsStaleTokensAndLabelsStaleContentEstimate)
{
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    auto old = residency.Token({"surface", 0, 0, 0});
    desc.dependencyHash.bytes[0] ^= 1;
    residency.InvalidatePreview(desc, 10, 21, {baseline});
    residency.ConsumeFeedback({old, residency.Token({"surface", 0, 0, 0})});
    const auto stats = residency.Statistics();
    EXPECT_EQ(stats.feedbackRequests, 1u);
    EXPECT_EQ(stats.discardedResults, 1u);
    EXPECT_EQ(stats.feedbackEstimatedParentFallbacks, 1u);
    EXPECT_EQ(stats.feedbackEstimatedStaleHits, 1u);
}

TEST_F(TerrainCompositedResidencyTest, OddPageFallbackSkipsPartiallyOverlappingImmediateParent)
{
    desc.width = 513;
    desc.height = 257;
    const auto layout = desc.Layout();
    const TerrainCompositedVTPageKey root{"surface", layout.MipCount() - 1, 0, 0};
    const TerrainCompositedVTPageKey partial{"surface", 1, 1, 0};
    const TerrainCompositedVTPageKey child{"surface", 0, 2, 0};
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    residency.SubmitPreviewPages(desc, 10, 20,
        {{root, std::vector<uint8_t>(desc.PageBytes(), 127)}});
    auto rootBatch = residency.BeginUploads(0);
    ASSERT_EQ(rootBatch.uploads.size(), 1u);
    const auto rootSlot = rootBatch.uploads[0].slot;
    residency.Commit(rootBatch.id, 1);
    residency.SubmitPreviewPages(desc, 10, 20,
        {{partial, std::vector<uint8_t>(desc.PageBytes(), 127)}});
    auto partialBatch = residency.BeginUploads(1);
    ASSERT_EQ(partialBatch.uploads.size(), 1u);
    EXPECT_NE(partialBatch.uploads[0].slot, rootSlot);
    residency.Commit(partialBatch.id, 2);
    EXPECT_EQ(residency.Resolve(child), std::optional<uint32_t>(rootSlot));
    residency.ConsumeFeedback({residency.Token(child)});
    const auto stats = residency.Statistics();
    EXPECT_EQ(stats.feedbackEstimatedResolvedMipCounts[root.mip], 1u);
    EXPECT_EQ(stats.feedbackEstimatedResolvedMipCounts[partial.mip], 0u);
}

TEST_F(TerrainCompositedResidencyTest, FourSlotsCoarsenDeduplicatedDemandAndStopReadingForOneHundredEpochs)
{
    desc.width = desc.height = 1024;
    const auto layout = desc.Layout();
    std::vector<TerrainCompositedVTPageData> pages;
    for (uint32_t mip = 0; mip < layout.MipCount(); ++mip)
        for (uint32_t row = 0; row < layout.Mip(mip).pagesY; ++row)
            for (uint32_t column = 0; column < layout.Mip(mip).pagesX; ++column)
                pages.push_back({{"surface", mip, column, row},
                    std::vector<uint8_t>(desc.PageBytes(), static_cast<uint8_t>(mip + 10))});
    const auto publication = TerrainProductionFixture::Publication(directory, {desc}, pages);
    desc = publication.surfaces.front();
    TerrainCompositedVTStore::Publish(directory / "large.json", publication, pages);
    auto largeStore = std::make_shared<const TerrainCompositedVTStore>(
        TerrainCompositedVTStore::Open(directory / "large.json"));
    limits.slots = 4;
    limits.cpuBytes = desc.PageBytes() * 4;
    TerrainCompositedResidency residency(largeStore, desc, 10, 20, limits);
    std::vector<TerrainCompositedVTTaskToken> demand;
    for (uint32_t row = 0; row < 8; ++row)
        for (uint32_t column = 0; column < 8; ++column)
            demand.push_back(residency.Token({"surface", 0, column, row}));
    demand.insert(demand.end(), 50, demand.front());
    uint64_t epoch = 0;
    const auto settle = [&]() {
        for (uint32_t step = 0; step < 100; ++step)
        {
            residency.ConsumeFeedback(demand);
            auto batch = residency.BeginUploads(epoch);
            residency.Commit(batch.id, ++epoch);
            const auto current = residency.Statistics();
            EXPECT_LE(current.resident + current.retiring, 4u);
            EXPECT_LE(current.budgetSelectedPages, 3u);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    settle();
    const auto stable = residency.Statistics();
    EXPECT_GT(stable.budgetMipBias, 0u);
    EXPECT_TRUE(residency.Ready());
    ASSERT_TRUE(residency.Resolve(demand.front().key));
    EXPECT_GT(stable.feedbackEstimatedParentFallbacks, 0u);
    settle();
    EXPECT_EQ(residency.Statistics().readsStarted, stable.readsStarted);
    EXPECT_EQ(residency.Statistics().uploads, stable.uploads);
    EXPECT_EQ(residency.Statistics().pending, 0u);
    EXPECT_EQ(residency.Statistics().reading, 0u);
    demand = {residency.Token({"surface", 0, 0, 0}), residency.Token({"surface", 0, 7, 7})};
    settle();
    const auto changed = residency.Statistics();
    EXPECT_EQ(changed.budgetMipBias, 0u);
    EXPECT_EQ(changed.budgetSelectedPages, 3u);
    EXPECT_GT(changed.readsStarted, stable.readsStarted);
    EXPECT_GE(changed.feedbackEstimatedExactHits, 1u);
    settle();
    EXPECT_EQ(residency.Statistics().readsStarted, changed.readsStarted);
    EXPECT_EQ(residency.Statistics().uploads, changed.uploads);
    RecordProperty("slotLimit", 4);
    RecordProperty("coarseSelectedPages", stable.budgetSelectedPages);
    RecordProperty("coarseMipBias", stable.budgetMipBias);
    RecordProperty("coarseSteadyReadStarts", static_cast<int>(stable.readsStarted));
    RecordProperty("cameraChangedReadStarts", static_cast<int>(changed.readsStarted));
    RecordProperty("stableEpochsPerView", 100);
}

TEST_F(TerrainCompositedResidencyTest, CameraSelectionCancelsQueuedAndCompletedObsoleteReads)
{
    limits.slots = 4;
    TerrainCompositedResidency residency(store, desc, 10, 20, limits);
    LoadBaseline(residency);
    const auto first = residency.Token({"surface", 0, 0, 0});
    const auto second = residency.Token({"surface", 0, 1, 0});
    residency.ConsumeFeedback({first});
    auto started = residency.BeginUploads(1);
    residency.Abort(started.id);
    residency.ConsumeFeedback({second});
    auto next = WaitBatch(residency, 1);
    ASSERT_EQ(next.uploads.size(), 1u);
    EXPECT_EQ(next.uploads.front().token.key, second.key);
    residency.Commit(next.id, 2);
    EXPECT_GE(residency.Statistics().evictedReads, 1u);
    residency.ConsumeFeedback({first});
    residency.ConsumeFeedback({second});
    EXPECT_GE(residency.Statistics().queuedCancel, 1u);
    EXPECT_TRUE(residency.Ready());
}

TEST_F(TerrainCompositedResidencyTest, PreviewCoarsensDemandAndBakesOnlySelectedDirtyPages)
{
    desc.width = desc.height = 1024;
    limits.slots = 4;
    limits.cpuBytes = desc.PageBytes() * 4;
    TerrainCompositedResidency residency({}, desc, 10, 20, limits);
    std::vector<TerrainCompositedVTTaskToken> demand;
    for (uint32_t row = 0; row < 8; ++row)
        for (uint32_t column = 0; column < 8; ++column)
            demand.push_back(residency.Token({"surface", 0, column, row}));
    residency.ConsumeFeedback(demand);
    auto requested = residency.PreviewRequests();
    ASSERT_LE(requested.size(), 3u);
    std::vector<TerrainCompositedVTPageData> prepared;
    for (const auto& token : requested)
        prepared.push_back({token.key, std::vector<uint8_t>(desc.PageBytes(), 127)});
    residency.SubmitPreviewPages(desc, 10, 20, prepared);
    uint64_t epoch = 0;
    for (size_t index = 0; index < prepared.size(); ++index)
    {
        const auto batch = residency.BeginUploads(epoch);
        ASSERT_EQ(batch.uploads.size(), 1u);
        residency.Commit(batch.id, ++epoch);
    }
    ASSERT_TRUE(residency.Ready());
    desc.dependencyHash.bytes[0] ^= 1;
    std::vector<TerrainCompositedVTPageKey> dirtyPages;
    const auto layout = desc.Layout();
    for (uint32_t mip = 0; mip < layout.MipCount(); ++mip)
        for (uint32_t row = 0; row < layout.Mip(mip).pagesY; ++row)
            for (uint32_t column = 0; column < layout.Mip(mip).pagesX; ++column)
                dirtyPages.push_back({"surface", mip, column, row});
    residency.InvalidatePreview(desc, 10, 21, dirtyPages);
    requested = residency.PreviewRequests();
    EXPECT_EQ(requested.size(), prepared.size());
    EXPECT_GT(residency.Statistics().dirty, requested.size());
    prepared.clear();
    for (const auto& token : requested)
        prepared.push_back({token.key, std::vector<uint8_t>(desc.PageBytes(), 128)});
    residency.SubmitPreviewPages(desc, 10, 21, prepared);
    auto cancelled = residency.BeginUploads(epoch);
    ASSERT_EQ(cancelled.uploads.size(), 1u);
    const auto retained = cancelled.uploads.front().bytes;
    residency.Abort(cancelled.id);
    auto retry = residency.BeginUploads(epoch);
    ASSERT_EQ(retry.uploads.size(), 1u);
    EXPECT_EQ(retry.uploads.front().bytes, retained);
    residency.Commit(retry.id, ++epoch);
    for (uint32_t step = 0; step < 20; ++step)
    {
        auto batch = residency.BeginUploads(epoch);
        residency.Commit(batch.id, ++epoch);
        EXPECT_LE(residency.Statistics().resident + residency.Statistics().retiring, 4u);
    }
    EXPECT_TRUE(residency.PreviewRequests().empty());
    EXPECT_TRUE(residency.Ready());
}
}
}
