#include <gtest/gtest.h>

#include "terrain/terrainCompositedSourceSnapshots.h"

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

namespace VL
{
namespace
{
TerrainRvtBudgetCaps SourceCaps(uint64_t cpuBytes = 64 * 1024 * 1024)
{
    TerrainRvtBudgetCaps caps;
    caps.totalCpuBytes = cpuBytes;
    caps.workspaceCpuBytes = cpuBytes;
    return caps;
}

TerrainBuildPlan SourcePlan(size_t heightCapacity = 4096,
    size_t weightCapacity = 2048, size_t opacityCapacity = 1024)
{
    TerrainBuildPlan plan;
    plan.placementId = "terrain|source-test";
    auto height = std::make_shared<HostImage>();
    height->width = 8;
    height->height = 8;
    height->channels = 1;
    height->format = HostImage::PixelFormat::R32_FLOAT;
    height->data.reserve(heightCapacity);
    height->data.resize(8 * 8 * sizeof(float), 0);
    plan.heightmap = height;
    auto paint = std::make_shared<TerrainLayerPaintData>();
    paint->width = 8;
    paint->height = 8;
    paint->layerCount = 2;
    paint->weights.reserve(weightCapacity);
    paint->weights.resize(128, 0.5f);
    paint->opacity.reserve(opacityCapacity);
    paint->opacity.resize(128, 255);
    plan.layerPaint = paint;
    return plan;
}

uint64_t PayloadBytes(const TerrainBuildPlan& plan)
{
    uint64_t bytes = 0;
    if (plan.heightmap)
        bytes += sizeof(HostImage) + plan.heightmap->data.capacity();
    if (plan.layerPaint)
        bytes += sizeof(TerrainLayerPaintData) +
            plan.layerPaint->weights.capacity() * sizeof(float) + plan.layerPaint->opacity.capacity();
    return bytes;
}

struct SourceBudgets
{
    std::shared_ptr<TerrainRvtBudgetOwner> outer =
        std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
    std::shared_ptr<TerrainRvtBudgetOwner> steady =
        std::make_shared<TerrainRvtBudgetOwner>(SourceCaps(), outer);
    std::shared_ptr<TerrainRvtBudgetOwner> geometry =
        std::make_shared<TerrainRvtBudgetOwner>(SourceCaps(), steady);

    void ExpectEmpty() const
    {
        for (const auto& owner : {geometry, steady, outer})
        {
            EXPECT_EQ(owner->Statistics().current, TerrainRvtCost{});
            EXPECT_EQ(owner->Statistics().allocations, 0u);
            EXPECT_EQ(owner->Statistics().metadataBytes, 0u);
            for (const auto& cost : owner->Statistics().byPhase)
                EXPECT_EQ(cost, TerrainRvtCost{});
        }
    }

    void ExpectParents(uint64_t allocations) const
    {
        const auto child = geometry->Statistics();
        const auto parent = steady->Statistics();
        const auto peak = outer->Statistics();
        EXPECT_EQ(child.allocations, allocations);
        EXPECT_EQ(parent.allocations, allocations);
        EXPECT_EQ(peak.allocations, allocations);
        const auto metadata = allocations * TerrainRvtBudgetOwner::MetadataBytesPerLease;
        EXPECT_EQ(parent.current.cpuBytes, child.current.cpuBytes + metadata);
        EXPECT_EQ(peak.current.cpuBytes, parent.current.cpuBytes + metadata);
        EXPECT_EQ(child.metadataBytes, metadata);
        EXPECT_EQ(parent.metadataBytes, metadata);
        EXPECT_EQ(peak.metadataBytes, metadata);
        EXPECT_EQ(child.current.gpuBytes, 0u);
        EXPECT_EQ(child.current.physicalSlots, 0u);
        EXPECT_EQ(child.current.productionTasks, 0u);
    }
};

TEST(TerrainCompositedSourceSnapshots, SharesActualIdentityAcrossPlacementsAndAliases)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto input = SourcePlan();
    const auto* pixels = input.heightmap->data.data();
    auto first = registry.Retain(input);
    const auto charged = budgets.geometry->Statistics().current.cpuBytes;
    EXPECT_GT(charged, PayloadBytes(input) +
        2 * TerrainRvtBudgetOwner::MetadataBytesPerLease);
    input.placementId = "terrain|other-placement";
    auto second = registry.Retain(input);
    auto queued = registry.Retain(first);
    EXPECT_EQ(first.heightmap.get(), input.heightmap.get());
    EXPECT_EQ(first.heightmap->data.data(), pixels);
    EXPECT_EQ(first.layerPaint.get(), input.layerPaint.get());
    EXPECT_FALSE(first.heightmap.owner_before(second.heightmap));
    EXPECT_FALSE(second.heightmap.owner_before(first.heightmap));
    EXPECT_FALSE(first.heightmap.owner_before(queued.heightmap));
    EXPECT_EQ(budgets.geometry->Statistics().current.cpuBytes, charged);
    EXPECT_EQ(registry.LiveSources(), 2u);
    budgets.ExpectParents(2);
    first = {};
    second = {};
    EXPECT_EQ(registry.LiveSources(), 2u);
    queued = {};
    EXPECT_EQ(registry.LiveSources(), 0u);
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, DifferentObjectsWithEqualContentsAreChargedSeparately)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto input = SourcePlan();
    auto equal = SourcePlan();
    ASSERT_EQ(input.heightmap->data, equal.heightmap->data);
    ASSERT_EQ(input.layerPaint->weights, equal.layerPaint->weights);
    ASSERT_EQ(input.layerPaint->opacity, equal.layerPaint->opacity);
    auto first = registry.Retain(input);
    const auto firstCost = budgets.geometry->Statistics().current.cpuBytes;
    auto second = registry.Retain(equal);
    EXPECT_EQ(budgets.geometry->Statistics().current.cpuBytes, 2 * firstCost);
    EXPECT_EQ(registry.LiveSources(), 4u);
    budgets.ExpectParents(4);
    first = {};
    EXPECT_EQ(budgets.geometry->Statistics().current.cpuBytes, firstCost);
    second = {};
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, ChangedHeightAndPaintOverlapUntilTheirLastConsumer)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto input = SourcePlan();
    auto active = registry.Retain(input, TerrainRvtResourcePhase::Active);
    auto changed = SourcePlan();
    auto height = std::make_shared<HostImage>(*changed.heightmap);
    height->data[0] = 1;
    changed.heightmap = height;
    auto paint = std::make_shared<TerrainLayerPaintData>(*changed.layerPaint);
    paint->weights[0] = 0.75f;
    paint->opacity[0] = 127;
    changed.layerPaint = paint;
    auto candidate = registry.Retain(changed);
    EXPECT_EQ(registry.LiveSources(), 4u);
    budgets.ExpectParents(4);
    auto retired = active;
    registry.Promote(retired, TerrainRvtResourcePhase::Retiring);
    active = {};
    candidate = {};
    EXPECT_EQ(registry.LiveSources(), 2u);
    EXPECT_EQ(budgets.geometry->Statistics().byPhase[2].cpuBytes,
        budgets.geometry->Statistics().current.cpuBytes);
    retired = {};
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, SingleInputEditsKeepTheOtherLeafShared)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto input = SourcePlan();
    auto active = registry.Retain(input);
    auto heightEdit = SourcePlan();
    heightEdit.layerPaint = active.layerPaint;
    auto changedHeight = registry.Retain(heightEdit);
    EXPECT_EQ(registry.LiveSources(), 3u);
    EXPECT_FALSE(active.layerPaint.owner_before(changedHeight.layerPaint));
    EXPECT_FALSE(changedHeight.layerPaint.owner_before(active.layerPaint));
    budgets.ExpectParents(3);
    auto paintEdit = SourcePlan();
    paintEdit.heightmap = active.heightmap;
    auto changedPaint = registry.Retain(paintEdit);
    EXPECT_EQ(registry.LiveSources(), 4u);
    EXPECT_FALSE(active.heightmap.owner_before(changedPaint.heightmap));
    EXPECT_FALSE(changedPaint.heightmap.owner_before(active.heightmap));
    budgets.ExpectParents(4);
    heightEdit = {};
    paintEdit = {};
    active = {};
    EXPECT_EQ(registry.LiveSources(), 4u);
    changedHeight = {};
    EXPECT_EQ(registry.LiveSources(), 2u);
    changedPaint = {};
    EXPECT_EQ(registry.LiveSources(), 0u);
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, QuotesRealVectorCapacitiesRatherThanSizes)
{
    auto small = SourcePlan(256, 128, 128);
    auto large = SourcePlan(8192, 4096, 2048);
    auto owner = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
    TerrainCompositedSourceSnapshots registry(owner);
    auto first = registry.Retain(small);
    const auto firstCost = owner->Statistics().current.cpuBytes;
    first = {};
    auto second = registry.Retain(large);
    EXPECT_EQ(owner->Statistics().current.cpuBytes - firstCost,
        PayloadBytes(large) - PayloadBytes(small));
    EXPECT_GT(firstCost, PayloadBytes(small));
}

TEST(TerrainCompositedSourceSnapshots, QueueSupersessionAndFailedCandidateReleaseOnlyTheirInputs)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto input = SourcePlan();
    auto active = registry.Retain(input, TerrainRvtResourcePhase::Active);
    const auto activeBytes = budgets.geometry->Statistics().current.cpuBytes;
    auto queue = registry.Retain(SourcePlan());
    auto inFlight = queue;
    queue = registry.Retain(SourcePlan());
    EXPECT_EQ(registry.LiveSources(), 6u);
    queue = {};
    EXPECT_EQ(registry.LiveSources(), 4u);
    inFlight = {};
    EXPECT_EQ(registry.LiveSources(), 2u);
    EXPECT_EQ(budgets.geometry->Statistics().current.cpuBytes, activeBytes);
    active = {};
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, SecondInputFailureAtomicallyRollsBackNewHeightAndParents)
{
    auto input = SourcePlan();
    uint64_t heightBytes = 0;
    {
        auto probe = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
        TerrainCompositedSourceSnapshots registry(probe);
        auto heightOnly = input;
        heightOnly.layerPaint.reset();
        auto retained = registry.Retain(heightOnly);
        heightBytes = probe->Statistics().current.cpuBytes;
    }
    SourceBudgets budgets;
    budgets.geometry = std::make_shared<TerrainRvtBudgetOwner>(
        SourceCaps(heightBytes), budgets.steady);
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    EXPECT_THROW(registry.Retain(input), std::runtime_error);
    EXPECT_EQ(budgets.geometry->Statistics().peak.cpuBytes, heightBytes);
    EXPECT_EQ(budgets.geometry->Statistics().rejected, 1u);
    EXPECT_EQ(registry.LiveSources(), 0u);
    budgets.ExpectEmpty();
    auto heightOnly = input;
    heightOnly.layerPaint.reset();
    auto retry = registry.Retain(heightOnly);
    EXPECT_EQ(registry.LiveSources(), 1u);
    budgets.ExpectParents(1);
    retry = {};
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, ParentFailureAlsoReleasesTemporaryLeaves)
{
    SourceBudgets budgets;
    budgets.steady = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps(1), budgets.outer);
    budgets.geometry = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps(), budgets.steady);
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    EXPECT_THROW(registry.Retain(SourcePlan()), std::runtime_error);
    EXPECT_EQ(registry.LiveSources(), 0u);
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, FailedRetainPreservesExistingAliasAndItsPhase)
{
    auto input = SourcePlan();
    uint64_t heightBytes = 0;
    {
        auto probe = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
        TerrainCompositedSourceSnapshots registry(probe);
        auto heightOnly = input;
        heightOnly.layerPaint.reset();
        auto retained = registry.Retain(heightOnly);
        heightBytes = probe->Statistics().current.cpuBytes;
    }
    SourceBudgets budgets;
    budgets.geometry = std::make_shared<TerrainRvtBudgetOwner>(
        SourceCaps(heightBytes), budgets.steady);
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto heightOnly = input;
    heightOnly.layerPaint.reset();
    auto original = registry.Retain(heightOnly);
    EXPECT_THROW(registry.Retain(input, TerrainRvtResourcePhase::Active), std::runtime_error);
    EXPECT_EQ(registry.LiveSources(), 1u);
    EXPECT_EQ(budgets.geometry->Statistics().byPhase[0].cpuBytes, heightBytes);
    EXPECT_EQ(original.heightmap.get(), input.heightmap.get());
    budgets.ExpectParents(1);
    original = {};
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, LastAliasOutlivesRegistryAndBudgetOwnerFacade)
{
    auto outer = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps());
    std::weak_ptr<TerrainRvtBudgetOwner> oldOwner;
    std::weak_ptr<const HostImage> pixels;
    TerrainBuildPlan escaped;
    {
        auto geometry = std::make_shared<TerrainRvtBudgetOwner>(SourceCaps(), outer);
        oldOwner = geometry;
        TerrainCompositedSourceSnapshots registry(geometry);
        auto input = SourcePlan();
        pixels = input.heightmap;
        escaped = registry.Retain(input);
    }
    EXPECT_TRUE(oldOwner.expired());
    EXPECT_FALSE(pixels.expired());
    EXPECT_EQ(outer->Statistics().allocations, 2u);
    auto lastHeight = escaped.heightmap;
    escaped = {};
    EXPECT_EQ(outer->Statistics().allocations, 1u);
    EXPECT_EQ(lastHeight->data.size(), 256u);
    lastHeight.reset();
    EXPECT_TRUE(pixels.expired());
    EXPECT_EQ(outer->Statistics().current, TerrainRvtCost{});
    EXPECT_EQ(outer->Statistics().metadataBytes, 0u);
}

TEST(TerrainCompositedSourceSnapshots, ReuseAndPromotionNeverRegressLeafOrParentPhase)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto input = SourcePlan();
    auto active = registry.Retain(input, TerrainRvtResourcePhase::Active);
    auto candidate = registry.Retain(input);
    registry.Promote(candidate, TerrainRvtResourcePhase::Candidate);
    for (const auto& owner : {budgets.geometry, budgets.steady, budgets.outer})
        EXPECT_EQ(owner->Statistics().byPhase[1].cpuBytes, owner->Statistics().current.cpuBytes);
    registry.Promote(active, TerrainRvtResourcePhase::Retiring);
    registry.Promote(active, TerrainRvtResourcePhase::Active);
    auto retry = registry.Retain(candidate, TerrainRvtResourcePhase::Active);
    for (const auto& owner : {budgets.geometry, budgets.steady, budgets.outer})
        EXPECT_EQ(owner->Statistics().byPhase[2].cpuBytes, owner->Statistics().current.cpuBytes);
    active = {};
    candidate = {};
    retry = {};
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, ConcurrentReuseSharesOneLeafPerActualSource)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto input = SourcePlan();
    auto anchor = registry.Retain(input);
    const auto charged = budgets.geometry->Statistics().current.cpuBytes;
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < 8; ++worker)
        workers.emplace_back([&]
        {
            try
            {
                for (size_t iteration = 0; iteration < 1000; ++iteration)
                {
                    auto retained = registry.Retain(input);
                    registry.Promote(retained, TerrainRvtResourcePhase::Active);
                    if (retained.heightmap.get() != input.heightmap.get() ||
                        registry.LiveSources() != 2)
                        valid = false;
                }
            }
            catch (...)
            {
                valid = false;
            }
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_TRUE(valid.load());
    EXPECT_EQ(budgets.geometry->Statistics().current.cpuBytes, charged);
    budgets.ExpectParents(2);
    anchor = {};
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, ConcurrentLastAliasReleaseAndFreshRetain)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    const auto input = SourcePlan();
    std::atomic<bool> valid{true};
    std::vector<std::thread> workers;
    for (size_t worker = 0; worker < 8; ++worker)
        workers.emplace_back([&]
        {
            try
            {
                for (size_t iteration = 0; iteration < 1000; ++iteration)
                {
                    auto retained = registry.Retain(input);
                    if (retained.heightmap->data.data() != input.heightmap->data.data())
                        valid = false;
                    registry.Promote(retained, TerrainRvtResourcePhase::Retiring);
                }
            }
            catch (...)
            {
                valid = false;
            }
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_TRUE(valid.load());
    EXPECT_EQ(registry.LiveSources(), 0u);
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, TenThousandShortEditsLeaveNoChargesOrWeakRegistrations)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    for (size_t iteration = 0; iteration < 10000; ++iteration)
    {
        auto input = SourcePlan(512, 256, 256);
        {
            auto retained = registry.Retain(input);
            registry.Promote(retained, TerrainRvtResourcePhase::Active);
        }
        ASSERT_EQ(budgets.geometry->Statistics().allocations, 0u);
        ASSERT_EQ(budgets.steady->Statistics().current, TerrainRvtCost{});
        ASSERT_EQ(budgets.outer->Statistics().current, TerrainRvtCost{});
    }
    EXPECT_EQ(registry.LiveSources(), 0u);
    EXPECT_EQ(budgets.geometry->Statistics().rejected, 0u);
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, EmptyInputsAndInvalidOwnerOrPhaseHaveNoCharge)
{
    EXPECT_THROW(TerrainCompositedSourceSnapshots(nullptr), std::invalid_argument);
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto empty = registry.Retain({});
    EXPECT_FALSE(empty.heightmap);
    EXPECT_FALSE(empty.layerPaint);
    EXPECT_THROW(registry.Retain(SourcePlan(), TerrainRvtResourcePhase::Count), std::invalid_argument);
    EXPECT_THROW(registry.Promote({}, TerrainRvtResourcePhase::Count), std::invalid_argument);
    EXPECT_EQ(registry.LiveSources(), 0u);
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, PreparingPixelPhaseRecordsDoesNotPromoteOrReserve)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    auto retained = registry.Retain(SourcePlan());
    const auto before = budgets.geometry->Statistics();
    const auto changes = registry.PreparePromote(retained, TerrainRvtResourcePhase::Active);
    ASSERT_EQ(changes.size(), 2u);
    const auto prepared = budgets.geometry->Statistics();
    EXPECT_EQ(prepared.current, before.current);
    EXPECT_EQ(prepared.allocations, before.allocations);
    EXPECT_EQ(prepared.byPhase, before.byPhase);
    for (const auto& change : changes)
    {
        EXPECT_EQ(change.lease.Phase(), TerrainRvtResourcePhase::Candidate);
        EXPECT_EQ(change.phase, TerrainRvtResourcePhase::Active);
    }
}

TEST(TerrainCompositedSourceSnapshots, PixelPromotionJoinsOneAtomicBudgetBatch)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    {
        auto retained = registry.Retain(SourcePlan());
        auto changes = registry.PreparePromote(retained, TerrainRvtResourcePhase::Active);
        auto batch = TerrainRvtBudgetPhaseBatch::Prepare(changes, *budgets.geometry);
        ASSERT_TRUE(batch.has_value());
        for (const auto& change : changes)
            EXPECT_EQ(change.lease.Phase(), TerrainRvtResourcePhase::Candidate);
        ASSERT_TRUE(batch->TryCommitNoThrow());
        for (const auto& change : changes)
            EXPECT_EQ(change.lease.Phase(), TerrainRvtResourcePhase::Active);
        EXPECT_TRUE(registry.PreparePromote(retained, TerrainRvtResourcePhase::Candidate).empty());
        EXPECT_TRUE(registry.PreparePromote(retained, TerrainRvtResourcePhase::Active).empty());
    }
    EXPECT_EQ(registry.LiveSources(), 0u);
    budgets.ExpectEmpty();
}

TEST(TerrainCompositedSourceSnapshots, CancelledPixelPhaseBatchKeepsOldPhases)
{
    SourceBudgets budgets;
    TerrainCompositedSourceSnapshots registry(budgets.geometry);
    {
        auto retained = registry.Retain(SourcePlan());
        const auto changes = registry.PreparePromote(retained, TerrainRvtResourcePhase::Active);
        {
            auto batch = TerrainRvtBudgetPhaseBatch::Prepare(changes, *budgets.geometry);
            ASSERT_TRUE(batch.has_value());
        }
        for (const auto& change : changes)
            EXPECT_EQ(change.lease.Phase(), TerrainRvtResourcePhase::Candidate);
    }
    budgets.ExpectEmpty();
}
}
}
