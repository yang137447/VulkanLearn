#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include "core/runtimeEvidence.h"
#include "terrain/terrainFaultInjection.h"

namespace VL
{
namespace
{
class SlotControlTest : public ::testing::Test
{
protected:
    TerrainFaultInjectionController controller;
    std::shared_ptr<TerrainCompositedResidencyPool> pool =
        std::make_shared<TerrainCompositedResidencyPool>(2);
    std::unique_ptr<TerrainRvtBudgetOwner> budget;
    std::unique_ptr<TerrainCompositedResidency> previous;
    std::unique_ptr<TerrainCompositedResidency> sentinel;
    TerrainCompositedVTBuildDesc desc;
    TerrainCompositedResidencyBudget caps;
    TerrainFaultRequest request;
    uint64_t registration = 0;

    void SetUp() override
    {
        TerrainRvtBudgetCaps limits;
        limits.totalCpuBytes = limits.workspaceCpuBytes = 1024 * 1024;
        budget = std::make_unique<TerrainRvtBudgetOwner>(limits);
        desc.surfaceId = "terrain|real-cpu-pool";
        desc.width = desc.height = 256;
        desc.channels = {TerrainCompositedVTFormat::RGBA8UNorm};
        caps.slots = 2;
        previous = std::make_unique<TerrainCompositedResidency>(nullptr, desc, 7, 1, caps, pool);
        const auto token = previous->ProductionRequests().front();
        const auto upload = previous->ReserveProduction(token, 0);
        ASSERT_TRUE(upload);
        ASSERT_TRUE(previous->CompleteProduction(*upload, 5, true));
        const auto batch = previous->BeginUploads(5);
        previous->Commit(batch.id, 6);
        auto sentinelDesc = desc;
        sentinelDesc.surfaceId = "terrain|sentinel";
        sentinel = std::make_unique<TerrainCompositedResidency>(nullptr, sentinelDesc, 7, 1, caps, pool);
        const auto sentinelUpload = sentinel->ReserveProduction(sentinel->ProductionRequests().front(), 6);
        ASSERT_TRUE(sentinelUpload);
        ASSERT_TRUE(sentinel->CompleteProduction(*sentinelUpload, 6, true));
        const auto sentinelBatch = sentinel->BeginUploads(6);
        sentinel->Commit(sentinelBatch.id, 7);
        request.kind = TerrainFaultKind::SlotReuse;
        request.faultId = "cpu-real-pool-slot";
        request.worldGeneration = 7;
        request.page = {token.key.surfaceId, token.key.mip, token.key.x, token.key.y, token.revision};
        request.slot = upload->slot;
        request.writeEpoch = 6;
        registration = controller.RegisterSlotHook(7, {
            [this](uint32_t slot, uint64_t epoch) { return pool->SlotDiagnostics(slot, epoch); },
            [this](uint32_t slot) { return pool->ArmSlotReuseDiagnostics(slot, *budget); },
            [this](uint64_t epoch) { return pool->SlotReuseDiagnostics(epoch); },
            [this] { pool->CancelSlotReuseDiagnostics(); }});
        ASSERT_NE(registration, 0u);
    }

    void TearDown() override
    {
        controller.UnregisterSlotHook(registration);
        EXPECT_FALSE(pool->SlotReuseDiagnostics(100));
        EXPECT_EQ(budget->Statistics().current.cpuBytes, 0u);
    }

    void Arm()
    {
        ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
        EXPECT_FALSE(pool->SlotReuseDiagnostics(7));
        controller.PumpSlotHook(registration, 7);
        ASSERT_TRUE(pool->SlotReuseDiagnostics(7));
        EXPECT_GT(budget->Statistics().current.cpuBytes, 0u);
    }
};

TEST_F(SlotControlTest, ActualBoundaryCountsAndReplacementArePolledWithoutForcingAllocator)
{
    Arm();
    auto binding = std::make_shared<int>(1);
    previous->RetireAll(9, binding);
    previous->MarkReferenced(12);
    auto replacementDesc = desc;
    replacementDesc.surfaceId = "terrain|replacement";
    TerrainCompositedResidency replacement(nullptr, replacementDesc, 7, 2, caps, pool);
    const auto next = replacement.ProductionRequests().front();
    EXPECT_FALSE(replacement.ReserveProduction(next, 10));
    const auto blocked = pool->SlotReuseDiagnostics(10);
    ASSERT_TRUE(blocked);
    EXPECT_GT(blocked->blockedByEpoch, 0u);
    EXPECT_GT(blocked->blockedByOwner, 0u);
    controller.PumpSlotHook(registration, 10);
    const auto polled = pool->SlotReuseDiagnostics(10);
    ASSERT_TRUE(polled);
    EXPECT_EQ(polled->boundaryChecks, blocked->boundaryChecks);
    EXPECT_EQ(polled->current.reservation, blocked->current.reservation);
    binding.reset();
    const auto allocated = replacement.ReserveProduction(next, 12);
    ASSERT_TRUE(allocated);
    const auto actual = pool->SlotReuseDiagnostics(12);
    ASSERT_TRUE(actual);
    EXPECT_EQ(allocated->slot, request.slot);
    EXPECT_NE(actual->original.reservation, allocated->reservation);
    EXPECT_EQ(actual->replacementReservation, allocated->reservation);
    EXPECT_TRUE(actual->releasedAtBoundary);
    EXPECT_TRUE(actual->current.reservedToken.Matches(next));
    controller.PumpSlotHook(registration, 12);
    EXPECT_FALSE(pool->SlotReuseDiagnostics(12));
    EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Duplicate);
}

TEST_F(SlotControlTest, ExactPublishedIdentityRequiredBeforeChargedArm)
{
    ++request.writeEpoch;
    EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    controller.PumpSlotHook(registration, 7);
    EXPECT_FALSE(pool->SlotReuseDiagnostics(7));
    EXPECT_EQ(budget->Statistics().current.cpuBytes, 0u);
}

TEST_F(SlotControlTest, CancelIsDeferredUntilSerialOwnerAndReleasesActualLease)
{
    Arm();
    TerrainFaultRequest cancel;
    cancel.kind = TerrainFaultKind::Cancel;
    cancel.faultId = request.faultId;
    EXPECT_EQ(controller.Queue(cancel).code, TerrainFaultQueueCode::Queued);
    EXPECT_TRUE(pool->SlotReuseDiagnostics(7));
    controller.PumpSlotHook(registration, 7);
    EXPECT_FALSE(pool->SlotReuseDiagnostics(7));
}

TEST_F(SlotControlTest, TimeoutCancelsActualWatchWithoutSleeping)
{
    Arm();
    controller.PumpSlotHook(registration, 7,
        std::chrono::steady_clock::now() + TerrainFaultInjectionController::RequestLifetime);
    EXPECT_FALSE(pool->SlotReuseDiagnostics(7));
}

TEST_F(SlotControlTest, TeardownCancelsActualWatchAndRemovesCapability)
{
    Arm();
    controller.UnregisterSlotHook(registration);
    EXPECT_FALSE(pool->SlotReuseDiagnostics(7));
    auto next = request;
    next.faultId = "cpu-after-teardown";
    EXPECT_EQ(controller.Queue(next).code, TerrainFaultQueueCode::PendingHookMissing);
}

TEST_F(SlotControlTest, ActualAdmissionFailureDoesNotAlterSlotOrScheduling)
{
    TerrainRvtBudgetCaps limits;
    limits.totalCpuBytes = limits.workspaceCpuBytes = 1;
    budget = std::make_unique<TerrainRvtBudgetOwner>(limits);
    const auto before = pool->SlotDiagnostics(request.slot, 7);
    ASSERT_TRUE(before);
    EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    controller.PumpSlotHook(registration, 7);
    EXPECT_FALSE(pool->SlotReuseDiagnostics(7));
    const auto after = pool->SlotDiagnostics(request.slot, 7);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->reservation, before->reservation);
    EXPECT_EQ(after->publishEpoch, before->publishEpoch);
    EXPECT_TRUE(after->publishedToken.Matches(before->publishedToken));
}

TEST_F(SlotControlTest, UnconsumedCancellationCannotCancelAnExistingForeignNativeWatch)
{
    ASSERT_TRUE(pool->ArmSlotReuseDiagnostics(request.slot, *budget));
    EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    TerrainFaultRequest cancel;
    cancel.kind = TerrainFaultKind::Cancel;
    cancel.faultId = request.faultId;
    EXPECT_EQ(controller.Queue(cancel).code, TerrainFaultQueueCode::Queued);
    controller.PumpSlotHook(registration, 7);
    EXPECT_TRUE(pool->SlotReuseDiagnostics(7));
    pool->CancelSlotReuseDiagnostics();
}

TEST_F(SlotControlTest, FailedArmCannotTakeOwnershipOfAnExistingNativeWatch)
{
    ASSERT_TRUE(pool->ArmSlotReuseDiagnostics(request.slot, *budget));
    const auto before = pool->SlotReuseDiagnostics(7);
    ASSERT_TRUE(before);
    EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    controller.PumpSlotHook(registration, 7);
    const auto after = pool->SlotReuseDiagnostics(7);
    ASSERT_TRUE(after);
    EXPECT_EQ(after->original.reservation, before->original.reservation);
    EXPECT_EQ(after->boundaryChecks, before->boundaryChecks);
    pool->CancelSlotReuseDiagnostics();
}
}
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    VL::RuntimeEvidence::ApplicationScope evidence;
    try
    {
        const char* configured = std::getenv("VULKANLEARN_RUNTIME_EVIDENCE");
        const std::string output = configured ? configured : "";
        if (!output.empty())
        {
            const auto package = std::filesystem::path(output).parent_path() / "cpu-pool-fixture-package";
            std::filesystem::create_directory(package);
            VL::RuntimeEvidence::Initialize(package);
            VL::RuntimeEvidence::Record("CPU_TEST_SCOPE",
                {{"scope", "real CPU Residency pool and controller, no Vulkan or Game World execution"}});
        }
        const auto result = RUN_ALL_TESTS();
        const auto exitCode = evidence.Finish(result);
        if (exitCode || output.empty())
            return exitCode;
        std::ifstream records(output);
        std::string line;
        bool armed = false, replaced = false, timedOut = false, cancelled = false, teardown = false;
        while (std::getline(records, line))
        {
            const auto record = nlohmann::json::parse(line);
            const auto& payload = record.at("payload");
            const auto event = payload.value("event", "");
            armed |= event == "slot-watch-armed";
            timedOut |= event == "slot-watch-timeout";
            cancelled |= event == "slot-watch-cancelled";
            teardown |= event == "slot-watch-owner-teardown";
            if (event == "slot-watch-replacement-observed")
            {
                const auto& observed = payload.at("observation");
                replaced = observed.at("releasedAtBoundary").get<bool>() &&
                    observed.at("replacementObserved").get<bool>() &&
                    observed.at("blockedByEpoch").get<uint64_t>() > 0 &&
                    observed.at("blockedByOwner").get<uint64_t>() > 0 &&
                    observed.at("original").at("reservation") != observed.at("current").at("reservation") &&
                    observed.at("current").at("reservation") == observed.at("replacementReservation") &&
                    observed.at("current").at("reservedToken").at("surfaceId") == "terrain|replacement";
            }
        }
        if (!(armed && replaced && timedOut && cancelled && teardown))
            throw std::runtime_error("Dedicated sink did not preserve actual CPU watch lifecycle DTOs");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return evidence.Finish(1);
    }
}
