#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>
#include <nlohmann/json.hpp>
#include "engine/testing/terrainRvtFaultTests.h"

namespace VL
{
namespace
{
TerrainFaultRequest StatusRequest()
{
    TerrainFaultRequest request;
    request.kind = TerrainFaultKind::StatusNonzero;
    request.faultId = "cpu-status-001";
    request.worldGeneration = 42;
    request.page = {"terrain|Landscape_00", 3, 0, 0, 7};
    request.status = 2;
    return request;
}

TerrainFaultStatusObservation CompletedStatus()
{
    return {42, StatusRequest().page, 10, 10, 0};
}

nlohmann::json StatusJson()
{
    return {{"kind", "statusNonzero"}, {"faultId", "cpu-status-001"}, {"worldGeneration", 42},
        {"surfaceId", "terrain|Landscape_00"}, {"mip", 3}, {"x", 0}, {"y", 0},
        {"revision", 7}, {"status", 2}};
}

TerrainFaultSubmitRequest SubmitRequest()
{
    auto fault = StatusRequest();
    fault.kind = TerrainFaultKind::SubmitFail;
    fault.faultId = "cpu-submit-001";
    fault.submitResult = -4;
    return {fault, {19, std::string(64, 'a'), 73}};
}

TerrainFaultSubmitObservation BeforeSubmit()
{
    const auto request = SubmitRequest();
    return {request.fault.worldGeneration, request.fault.page, request.token};
}

TerrainFaultSubmitAbortObservation AbortedSubmit(uint64_t ticket)
{
    return {ticket, BeforeSubmit(), 42, 42, 1, 1, 0, 0};
}

TEST(TerrainFaultInjection, DefaultAndUnregisteredOwnersNeverArm)
{
    TerrainFaultInjectionController controller;
    EXPECT_EQ(controller.Queue({}).code, TerrainFaultQueueCode::InvalidRequest);
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::PendingHookMissing);
    EXPECT_FALSE(controller.ConsumeStatus(1, CompletedStatus()));
    EXPECT_EQ(TerrainRvtFaultTestingService::Submit(StatusRequest()).code,
        TerrainFaultQueueCode::PendingHookMissing);
}

TEST(TerrainFaultInjection, ExactCompletedStatusConsumesOnceAndPreservesOriginal)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterStatusHook(42);
    ASSERT_NE(registration, 0u);
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
    const auto original = CompletedStatus();
    const auto result = controller.ConsumeStatus(registration, original);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->faultId, "cpu-status-001");
    EXPECT_EQ(result->originalStatus, 0u);
    EXPECT_EQ(result->injectedStatus, 2u);
    EXPECT_EQ(original.originalStatus, 0u);
    EXPECT_FALSE(controller.ConsumeStatus(registration, original));
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Duplicate);
}

TEST(TerrainFaultInjection, TestingServiceAndRealOwnerApiShareTheSameQueue)
{
    auto& controller = TerrainFaultInjection();
    const auto registration = controller.RegisterStatusHook(42);
    EXPECT_NE(registration, 0u);
    auto request = StatusRequest();
    request.faultId = "cpu-service-owner-roundtrip";
    const auto queued = TerrainRvtFaultTestingService::Submit(request);
    EXPECT_EQ(queued.code, TerrainFaultQueueCode::Queued);
    const auto result = controller.ConsumeStatus(registration, CompletedStatus());
    EXPECT_TRUE(result);
    if (result)
        EXPECT_EQ(result->faultId, request.faultId);
    EXPECT_FALSE(controller.ConsumeStatus(registration, CompletedStatus()));
    controller.UnregisterStatusHook(registration);
}

TEST(TerrainFaultInjection, DirectDtosCannotBypassBoundedStringStorage)
{
    TerrainFaultInjectionController controller;
    controller.RegisterStatusHook(42);
    auto request = StatusRequest();
    request.objectId = std::string(257, 'x');
    EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::InvalidRequest);
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
}

TEST(TerrainFaultInjection, MismatchedTaskCannotStealOrDestroyPendingRequest)
{
    for (int field = 0; field < 7; ++field)
    {
        TerrainFaultInjectionController controller;
        const auto registration = controller.RegisterStatusHook(42);
        ASSERT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
        auto observed = CompletedStatus();
        switch (field)
        {
        case 0: ++observed.worldGeneration; break;
        case 1: observed.page.surfaceId = "terrain|different"; break;
        case 2: ++observed.page.mip; break;
        case 3: ++observed.page.x; break;
        case 4: ++observed.page.y; break;
        case 5: ++observed.page.revision; break;
        case 6: observed.originalStatus = 1; break;
        }
        EXPECT_FALSE(controller.ConsumeStatus(registration, observed));
        EXPECT_TRUE(controller.ConsumeStatus(registration, CompletedStatus()));
    }
}

TEST(TerrainFaultInjection, FenceMustReallyCompleteAndEpochZeroCannotInject)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterStatusHook(42);
    ASSERT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
    auto observed = CompletedStatus();
    observed.fenceCompletedEpoch = 9;
    EXPECT_FALSE(controller.ConsumeStatus(registration, observed));
    observed.writeEpoch = 0;
    observed.fenceCompletedEpoch = 0;
    EXPECT_FALSE(controller.ConsumeStatus(registration, observed));
    EXPECT_TRUE(controller.ConsumeStatus(registration, CompletedStatus()));
}

TEST(TerrainFaultInjection, ActualInstalledGameInitialRevisionZeroIsAnExactIdentity)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterStatusHook(42);
    auto request = StatusRequest();
    request.page.revision = 0;
    ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    auto observed = CompletedStatus();
    EXPECT_FALSE(controller.ConsumeStatus(registration, observed));
    observed.page.revision = 0;
    EXPECT_TRUE(controller.ConsumeStatus(registration, observed));
}

TEST(TerrainFaultInjection, OwnerTeardownCancelsAndGenerationCannotLeak)
{
    TerrainFaultInjectionController controller;
    const auto oldOwner = controller.RegisterStatusHook(42);
    ASSERT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
    controller.UnregisterStatusHook(oldOwner);
    const auto newOwner = controller.RegisterStatusHook(43);
    ASSERT_NE(newOwner, 0u);
    EXPECT_FALSE(controller.ConsumeStatus(oldOwner, CompletedStatus()));
    EXPECT_FALSE(controller.ConsumeStatus(newOwner, CompletedStatus()));
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::PendingHookMissing);
    auto request = StatusRequest();
    request.worldGeneration = 43;
    request.faultId = "new-generation";
    ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    auto observed = CompletedStatus();
    observed.worldGeneration = 43;
    EXPECT_TRUE(controller.ConsumeStatus(newOwner, observed));
}

TEST(TerrainFaultInjection, PendingQueueHasOneSlotAndCancellationIsExact)
{
    TerrainFaultInjectionController controller;
    controller.RegisterStatusHook(42);
    ASSERT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
    auto another = StatusRequest();
    another.faultId = "another";
    EXPECT_EQ(controller.Queue(another).code, TerrainFaultQueueCode::Busy);
    TerrainFaultRequest cancel;
    cancel.kind = TerrainFaultKind::Cancel;
    cancel.faultId = "wrong";
    EXPECT_EQ(controller.Queue(cancel).code, TerrainFaultQueueCode::NotFound);
    cancel.faultId = StatusRequest().faultId;
    EXPECT_EQ(controller.Queue(cancel).code, TerrainFaultQueueCode::Cancelled);
    EXPECT_EQ(controller.Queue(another).code, TerrainFaultQueueCode::Queued);
}

TEST(TerrainFaultInjection, RegistrationsAreBoundedAndCannotAliasWorld)
{
    TerrainFaultInjectionController controller;
    EXPECT_EQ(controller.RegisterStatusHook(0), 0u);
    std::vector<uint64_t> registrations;
    for (uint64_t generation = 1; generation <= TerrainFaultInjectionController::MaxOwners; ++generation)
        registrations.push_back(controller.RegisterStatusHook(generation));
    for (const auto registration : registrations)
        EXPECT_NE(registration, 0u);
    EXPECT_EQ(controller.RegisterStatusHook(100), 0u);
    controller.UnregisterStatusHook(registrations.front());
    EXPECT_EQ(controller.RegisterStatusHook(2), 0u);
    EXPECT_NE(controller.RegisterStatusHook(100), 0u);
}

TEST(TerrainFaultInjection, ConcurrentCompletionOnlyConsumesOnce)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterStatusHook(42);
    ASSERT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
    std::atomic<int> consumed{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 8; ++index)
        workers.emplace_back([&] {
            if (controller.ConsumeStatus(registration, CompletedStatus()))
                ++consumed;
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(consumed.load(), 1);
}

TEST(TerrainFaultInjection, OtherKindsStayMissingEvenWithRegisteredStatusOwner)
{
    TerrainFaultInjectionController controller;
    controller.RegisterStatusHook(42);
    for (const auto kind : {TerrainFaultKind::SubmitFail, TerrainFaultKind::StaleRevision,
        TerrainFaultKind::BudgetReject, TerrainFaultKind::SlotReuse, TerrainFaultKind::WorldCandidateBadInput})
    {
        auto request = StatusRequest();
        request.kind = kind;
        request.submitResult = -4;
        request.replacementRevision = 8;
        request.testAdmissionLimitBytes = 1;
        request.writeEpoch = 10;
        request.candidateScene = "Private/SC_candidate.json";
        request.objectId = "author-raw-55";
        request.xorMask = 1;
        EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::PendingHookMissing);
    }
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
}

TEST(TerrainFaultCommand, StrictStatusJsonAndControlDiagnosticNeverClaimPass)
{
    const auto request = ParseTerrainRvtFaultCommand(StatusJson().dump());
    EXPECT_EQ(request.worldGeneration, 42u);
    EXPECT_EQ(request.page.revision, 7u);
    EXPECT_EQ(request.status, 2u);
    const auto result = TerrainRvtFaultTestingService::Submit(request);
    const auto text = TerrainRvtFaultTestingService::ControlDiagnostic(request, result);
    EXPECT_EQ(text.find("[RVT_FAULT_CONTROL] "), 0u);
    EXPECT_EQ(text.find("[RVT_EVIDENCE]"), std::string::npos);
    EXPECT_EQ(text.find("\"passed\""), std::string::npos);
    EXPECT_EQ(text.find("\"queued\""), std::string::npos);
    EXPECT_NE(text.find("pendinghookmissing"), std::string::npos);
}

TEST(TerrainFaultCommand, RejectsBooleanFractionNegativeOverflowAndUnknownFields)
{
    for (const auto value : {nlohmann::json(true), nlohmann::json(1.5),
        nlohmann::json(-1), nlohmann::json(uint64_t{1} << 32)})
    {
        auto document = StatusJson();
        document["status"] = value;
        EXPECT_THROW(ParseTerrainRvtFaultCommand(document.dump()), std::exception);
    }
    auto document = StatusJson();
    document["status"] = 0;
    EXPECT_THROW(ParseTerrainRvtFaultCommand(document.dump()), std::exception);
    document = StatusJson();
    document["passed"] = true;
    EXPECT_THROW(ParseTerrainRvtFaultCommand(document.dump()), std::exception);
    EXPECT_THROW(ParseTerrainRvtFaultCommand("[]"), std::exception);
    EXPECT_THROW(ParseTerrainRvtFaultCommand(std::string(4097, ' ')), std::exception);
    EXPECT_THROW(ParseTerrainRvtFaultCommand("{\"kind\":\"cancel\",\"kind\":\"cancel\",\"faultId\":\"x\"}"),
        std::exception);
}

TEST(TerrainFaultCommand, CandidatePathsCannotEscapeAndBudgetNeverUsesGlobalCapField)
{
    const nlohmann::json candidate{{"kind", "worldCandidateBadInput"}, {"faultId", "input-1"},
        {"worldGeneration", 42}, {"candidateScene", "Private/SC_candidate.json"},
        {"objectId", "author-raw-55"}, {"byteOffset", 0}, {"xorMask", 1}};
    EXPECT_EQ(ParseTerrainRvtFaultCommand(candidate.dump()).kind, TerrainFaultKind::WorldCandidateBadInput);
    for (const auto* path : {"../SC_candidate.json", "C:/SC_candidate.json",
        "/SC_candidate.json", "Private\\SC_candidate.json"})
    {
        auto escaped = candidate;
        escaped["candidateScene"] = path;
        EXPECT_THROW(ParseTerrainRvtFaultCommand(escaped.dump()), std::exception);
    }
    nlohmann::json budget{{"kind", "budgetReject"}, {"faultId", "budget-1"},
        {"worldGeneration", 42}, {"testAdmissionLimitBytes", 1}};
    EXPECT_EQ(ParseTerrainRvtFaultCommand(budget.dump()).testAdmissionLimitBytes, 1u);
    budget["globalCapBytes"] = 10000000000ull;
    EXPECT_THROW(ParseTerrainRvtFaultCommand(budget.dump()), std::exception);
}

TEST(TerrainFaultSubmit, SelectorNeedsLiveOwnerAndCannotArmFromWrongRegistration)
{
    TerrainFaultInjectionController controller;
    EXPECT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::PendingHookMissing);
    const auto owner = controller.RegisterSubmitHook(42);
    EXPECT_EQ(controller.Queue(SubmitRequest().fault).code, TerrainFaultQueueCode::Queued);
    EXPECT_FALSE(controller.ConsumeBeforeSubmit(owner + 1, BeforeSubmit()));
    EXPECT_TRUE(controller.ConsumeBeforeSubmit(owner, BeforeSubmit()));
}

TEST(TerrainFaultSubmit, ExactConsumeAndAbortAreSeparateOneShotTransitions)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterSubmitHook(42);
    ASSERT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
    EXPECT_FALSE(controller.ObserveSubmitAbort(registration, AbortedSubmit(1)));
    const auto failure = controller.ConsumeBeforeSubmit(registration, BeforeSubmit());
    ASSERT_TRUE(failure);
    EXPECT_NE(failure->abortTicket, 0u);
    EXPECT_EQ(failure->injectedResult, -4);
    EXPECT_FALSE(controller.ConsumeBeforeSubmit(registration, BeforeSubmit()));
    EXPECT_TRUE(controller.ObserveSubmitAbort(registration, AbortedSubmit(failure->abortTicket)));
    EXPECT_FALSE(controller.ObserveSubmitAbort(registration, AbortedSubmit(failure->abortTicket)));
    EXPECT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Duplicate);
}

TEST(TerrainFaultSubmit, EveryWorldPageTokenAndEpochComponentMustMatch)
{
    for (int field = 0; field < 9; ++field)
    {
        TerrainFaultInjectionController controller;
        const auto registration = controller.RegisterSubmitHook(42);
        ASSERT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
        auto observed = BeforeSubmit();
        switch (field)
        {
        case 0: ++observed.worldGeneration; break;
        case 1: observed.page.surfaceId += "_other"; break;
        case 2: ++observed.page.mip; break;
        case 3: ++observed.page.x; break;
        case 4: ++observed.page.y; break;
        case 5: ++observed.page.revision; break;
        case 6: ++observed.token.generation; break;
        case 7: observed.token.contentIdentity[0] = 'b'; break;
        case 8: ++observed.token.submissionEpoch; break;
        }
        EXPECT_FALSE(controller.ConsumeBeforeSubmit(registration, observed));
        EXPECT_TRUE(controller.ConsumeBeforeSubmit(registration, BeforeSubmit()));
    }
}

TEST(TerrainFaultSubmit, InvalidTokensEpochsAndResultsRejectBeforeQueue)
{
    TerrainFaultInjectionController controller;
    controller.RegisterSubmitHook(42);
    for (int field = 0; field < 6; ++field)
    {
        auto request = SubmitRequest();
        switch (field)
        {
        case 0: request.token.generation = 0; break;
        case 1: request.token.submissionEpoch = 0; break;
        case 2: request.token.contentIdentity = "digest"; break;
        case 3: request.token.contentIdentity[0] = 'A'; break;
        case 4: request.fault.submitResult = 0; break;
        case 5: request.fault.writeEpoch = request.token.submissionEpoch + 1; break;
        }
        EXPECT_EQ(controller.QueueSubmit(request).code, TerrainFaultQueueCode::InvalidRequest);
    }
    EXPECT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
}

TEST(TerrainFaultSubmit, AbortOutcomeRemainsRawEvenWhenOldWorldOrCountsAreWrong)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterSubmitHook(42);
    ASSERT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
    const auto failure = controller.ConsumeBeforeSubmit(registration, BeforeSubmit());
    ASSERT_TRUE(failure);
    auto observed = AbortedSubmit(failure->abortTicket);
    observed.activeGenerationAfter = 0;
    observed.abortedTasks = 0;
    observed.publishedChannels = 3;
    observed.backendSubmitCalls = 1;
    const auto raw = controller.ObserveSubmitAbort(registration, observed);
    ASSERT_TRUE(raw);
    EXPECT_EQ(raw->activeGenerationAfter, 0u);
    EXPECT_EQ(raw->abortedTasks, 0u);
    EXPECT_EQ(raw->publishedChannels, 3u);
    EXPECT_EQ(raw->backendSubmitCalls, 1u);
}

TEST(TerrainFaultSubmit, WrongAbortTicketAndTaskCannotStealTheObservation)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterSubmitHook(42);
    ASSERT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
    const auto failure = controller.ConsumeBeforeSubmit(registration, BeforeSubmit());
    ASSERT_TRUE(failure);
    auto wrong = AbortedSubmit(failure->abortTicket + 1);
    EXPECT_FALSE(controller.ObserveSubmitAbort(registration, wrong));
    wrong = AbortedSubmit(failure->abortTicket);
    ++wrong.task.token.submissionEpoch;
    EXPECT_FALSE(controller.ObserveSubmitAbort(registration, wrong));
    EXPECT_TRUE(controller.ObserveSubmitAbort(registration, AbortedSubmit(failure->abortTicket)));
}

TEST(TerrainFaultSubmit, StatusAndSubmitShareCapacityAndNeverConsumeEachOther)
{
    TerrainFaultInjectionController controller;
    const auto statusOwner = controller.RegisterStatusHook(42);
    const auto submitOwner = controller.RegisterSubmitHook(42);
    ASSERT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Busy);
    EXPECT_FALSE(controller.ConsumeStatus(statusOwner, CompletedStatus()));
    controller.UnregisterStatusHook(submitOwner);
    const auto failure = controller.ConsumeBeforeSubmit(submitOwner, BeforeSubmit());
    ASSERT_TRUE(failure);
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Busy);
    TerrainFaultRequest cancel;
    cancel.kind = TerrainFaultKind::Cancel;
    cancel.faultId = SubmitRequest().fault.faultId;
    EXPECT_EQ(controller.Queue(cancel).code, TerrainFaultQueueCode::Busy);
    EXPECT_TRUE(controller.ObserveSubmitAbort(submitOwner, AbortedSubmit(failure->abortTicket)));
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
    EXPECT_FALSE(controller.ConsumeBeforeSubmit(submitOwner, BeforeSubmit()));
    EXPECT_TRUE(controller.ConsumeStatus(statusOwner, CompletedStatus()));
}

TEST(TerrainFaultSubmit, TeardownCannotLeaveArmedFaultForReplacementOwner)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterSubmitHook(42);
    ASSERT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
    const auto failure = controller.ConsumeBeforeSubmit(registration, BeforeSubmit());
    ASSERT_TRUE(failure);
    controller.UnregisterSubmitHook(registration);
    const auto replacement = controller.RegisterSubmitHook(42);
    EXPECT_NE(replacement, registration);
    EXPECT_FALSE(controller.ObserveSubmitAbort(replacement, AbortedSubmit(failure->abortTicket)));
    auto next = SubmitRequest();
    next.fault.faultId += "-next";
    EXPECT_EQ(controller.QueueSubmit(next).code, TerrainFaultQueueCode::Queued);
}

TEST(TerrainFaultSubmit, OwnerLimitIsSharedAcrossSupportedPoints)
{
    TerrainFaultInjectionController controller;
    for (uint64_t index = 1; index <= TerrainFaultInjectionController::MaxOwners; ++index)
        EXPECT_NE(controller.RegisterSubmitHook(index), 0u);
    EXPECT_EQ(controller.RegisterStatusHook(100), 0u);
    EXPECT_EQ(controller.RegisterSubmitHook(100), 0u);
}

TEST(TerrainFaultSubmit, ConcurrentBeforeSubmitConsumesExactlyOneAbortTicket)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterSubmitHook(42);
    ASSERT_EQ(controller.QueueSubmit(SubmitRequest()).code, TerrainFaultQueueCode::Queued);
    std::atomic<int> consumed{0};
    std::vector<std::thread> workers;
    for (int index = 0; index < 8; ++index)
        workers.emplace_back([&] {
            if (controller.ConsumeBeforeSubmit(registration, BeforeSubmit()))
                ++consumed;
        });
    for (auto& worker : workers)
        worker.join();
    EXPECT_EQ(consumed.load(), 1);
}

TEST(TerrainFaultSubmit, ConsoleSelectorBindsOnlyAtActualOwnerObservationThenRequiresActualAbort)
{
    auto& controller = TerrainFaultInjection();
    const auto registration = controller.RegisterSubmitHook(42);
    ASSERT_NE(registration, 0u);
    auto document = StatusJson();
    document["kind"] = "submitFail";
    document["faultId"] = "cpu-console-submit-selector";
    document.erase("status");
    document["submitResult"] = -1;
    const auto request = ParseTerrainRvtFaultCommand(document.dump());
    ASSERT_EQ(TerrainRvtFaultTestingService::Submit(request).code, TerrainFaultQueueCode::Queued);
    auto actual = BeforeSubmit();
    actual.token.contentIdentity = std::string(64, 'e');
    actual.token.submissionEpoch = 913;
    auto wrong = actual;
    ++wrong.page.revision;
    EXPECT_FALSE(controller.ConsumeBeforeSubmit(registration, wrong));
    const auto consumed = controller.ConsumeBeforeSubmit(registration, actual);
    ASSERT_TRUE(consumed);
    EXPECT_EQ(consumed->faultId, request.faultId);
    EXPECT_EQ(consumed->injectedResult, -1);
    EXPECT_FALSE(controller.ConsumeBeforeSubmit(registration, actual));
    auto aborted = AbortedSubmit(consumed->abortTicket);
    EXPECT_FALSE(controller.ObserveSubmitAbort(registration, aborted));
    aborted.task = actual;
    EXPECT_TRUE(controller.ObserveSubmitAbort(registration, aborted));
    controller.UnregisterSubmitHook(registration);
}

TEST(TerrainFaultSubmit, SelectorCannotBindWrongWorldPageOrInvalidNativeToken)
{
    for (int mismatch = 0; mismatch < 8; ++mismatch)
    {
        TerrainFaultInjectionController controller;
        const auto registration = controller.RegisterSubmitHook(42);
        ASSERT_EQ(controller.Queue(SubmitRequest().fault).code, TerrainFaultQueueCode::Queued);
        auto actual = BeforeSubmit();
        switch (mismatch)
        {
        case 0: ++actual.worldGeneration; break;
        case 1: ++actual.page.revision; break;
        case 2: ++actual.page.mip; break;
        case 3: ++actual.page.x; break;
        case 4: actual.page.surfaceId += "-other"; break;
        case 5: actual.token.submissionEpoch = 0; break;
        case 6: actual.token.contentIdentity = "not-an-actual-content-digest"; break;
        case 7: actual.token.generation = 0; break;
        }
        EXPECT_FALSE(controller.ConsumeBeforeSubmit(registration, actual));
        EXPECT_TRUE(controller.ConsumeBeforeSubmit(registration, BeforeSubmit()));
    }
}

TEST(TerrainFaultSubmit, CancelledSelectorDoesNotReachTheNativeSubmitBoundary)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterSubmitHook(42);
    const auto request = SubmitRequest().fault;
    ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    TerrainFaultRequest cancel;
    cancel.kind = TerrainFaultKind::Cancel;
    cancel.faultId = request.faultId;
    EXPECT_EQ(controller.Queue(cancel).code, TerrainFaultQueueCode::Cancelled);
    EXPECT_FALSE(controller.ConsumeBeforeSubmit(registration, BeforeSubmit()));
}

TerrainFaultRequest ActionRequest(TerrainFaultKind kind)
{
    auto request = StatusRequest();
    request.kind = kind;
    request.faultId = kind == TerrainFaultKind::BudgetReject ? "cpu-budget-ingress" : "cpu-stale-ingress";
    request.testAdmissionLimitBytes = 1;
    request.replacementRevision = request.page.revision + 1;
    return request;
}

TEST(TerrainFaultActions, ExactKindRegistrationIsRequiredAndSharesTheExistingBound)
{
    TerrainFaultInjectionController controller;
    EXPECT_EQ(controller.RegisterActionHook(0, TerrainFaultKind::BudgetReject), 0u);
    EXPECT_EQ(controller.RegisterActionHook(42, TerrainFaultKind::SubmitFail), 0u);
    const auto budget = controller.RegisterActionHook(42, TerrainFaultKind::BudgetReject);
    ASSERT_NE(budget, 0u);
    EXPECT_EQ(controller.RegisterActionHook(42, TerrainFaultKind::BudgetReject), 0u);
    EXPECT_EQ(controller.Queue(ActionRequest(TerrainFaultKind::StaleRevision)).code,
        TerrainFaultQueueCode::PendingHookMissing);
    for (uint64_t index = 1; index < TerrainFaultInjectionController::MaxOwners; ++index)
        EXPECT_NE(controller.RegisterStatusHook(index), 0u);
    EXPECT_EQ(controller.RegisterSubmitHook(42), 0u);
    EXPECT_EQ(controller.RegisterActionHook(42, TerrainFaultKind::StaleRevision), 0u);
}

TEST(TerrainFaultActions, BudgetConsoleRequestReachesLiveOwnerOnceWithoutGlobalCapMutation)
{
    auto& controller = TerrainFaultInjection();
    const auto registration = controller.RegisterActionHook(42, TerrainFaultKind::BudgetReject);
    ASSERT_NE(registration, 0u);
    const auto request = ParseTerrainRvtFaultCommand(
        R"({"kind":"budgetReject","faultId":"cpu-command-budget-owner","worldGeneration":42,"testAdmissionLimitBytes":17})");
    ASSERT_EQ(TerrainRvtFaultTestingService::Submit(request).code, TerrainFaultQueueCode::Queued);
    const auto before = controller.ReadAction(registration);
    ASSERT_TRUE(before);
    EXPECT_FALSE(before->consumed);
    EXPECT_FALSE(before->cancelRequested);
    EXPECT_FALSE(controller.ConsumeAction(registration, 43));
    const auto consumed = controller.ConsumeAction(registration, 42);
    ASSERT_TRUE(consumed);
    EXPECT_EQ(consumed->testAdmissionLimitBytes, 17u);
    EXPECT_FALSE(controller.ConsumeAction(registration, 42));
    EXPECT_TRUE(controller.ReadAction(registration)->consumed);
    EXPECT_FALSE(controller.FinishAction(registration, "wrong-fault"));
    EXPECT_TRUE(controller.FinishAction(registration, request.faultId));
    EXPECT_FALSE(controller.ReadAction(registration));
    controller.UnregisterActionHook(registration);
}

TEST(TerrainFaultActions, StaleRequiresTheExactSelectedNativeTaskPageAndRevision)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterActionHook(42, TerrainFaultKind::StaleRevision);
    const auto request = ActionRequest(TerrainFaultKind::StaleRevision);
    ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    auto wrong = request.page;
    ++wrong.revision;
    EXPECT_FALSE(controller.ConsumeAction(registration, 42, wrong));
    EXPECT_FALSE(controller.ConsumeAction(registration, 43, request.page));
    const auto actual = controller.ConsumeAction(registration, 42, request.page);
    ASSERT_TRUE(actual);
    EXPECT_EQ(actual->replacementRevision, 8u);
    EXPECT_FALSE(controller.ConsumeAction(registration, 42, request.page));
    EXPECT_TRUE(controller.FinishAction(registration, request.faultId));
}

TEST(TerrainFaultActions, CancellationRemainsPendingUntilSerialOwnerCleanupIsAcknowledged)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterActionHook(42, TerrainFaultKind::StaleRevision);
    const auto request = ActionRequest(TerrainFaultKind::StaleRevision);
    ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    ASSERT_TRUE(controller.ConsumeAction(registration, 42, request.page));
    TerrainFaultRequest cancel;
    cancel.kind = TerrainFaultKind::Cancel;
    cancel.faultId = request.faultId;
    EXPECT_EQ(controller.Queue(cancel).code, TerrainFaultQueueCode::Queued);
    const auto action = controller.ReadAction(registration);
    ASSERT_TRUE(action);
    EXPECT_TRUE(action->consumed);
    EXPECT_TRUE(action->cancelRequested);
    EXPECT_FALSE(controller.ConsumeAction(registration, 42, request.page));
    controller.RegisterStatusHook(42);
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Busy);
    EXPECT_TRUE(controller.FinishAction(registration, request.faultId));
    EXPECT_EQ(controller.Queue(StatusRequest()).code, TerrainFaultQueueCode::Queued);
}

TEST(TerrainFaultActions, TimeoutReadIsConstAndRetainsCleanupIdentityUntilOwnerFinishes)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterActionHook(42, TerrainFaultKind::StaleRevision);
    const auto request = ActionRequest(TerrainFaultKind::StaleRevision);
    ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    ASSERT_TRUE(controller.ConsumeAction(registration, 42, request.page));
    const auto later = std::chrono::steady_clock::now() + TerrainFaultInjectionController::RequestLifetime;
    const auto timedOut = controller.ReadAction(registration, later);
    ASSERT_TRUE(timedOut);
    EXPECT_TRUE(timedOut->timedOut);
    EXPECT_EQ(timedOut->request.faultId, request.faultId);
    EXPECT_EQ(timedOut->request.page.revision, request.page.revision);
    const auto repeated = controller.ReadAction(registration, later);
    ASSERT_TRUE(repeated);
    EXPECT_TRUE(repeated->consumed);
    EXPECT_TRUE(repeated->timedOut);
    EXPECT_TRUE(controller.FinishAction(registration, request.faultId));
    EXPECT_FALSE(controller.ReadAction(registration, later));
}

TEST(TerrainFaultActions, TeardownDoesNotLeakRequestsIntoTheNextWorld)
{
    TerrainFaultInjectionController controller;
    const auto registration = controller.RegisterActionHook(42, TerrainFaultKind::BudgetReject);
    const auto request = ActionRequest(TerrainFaultKind::BudgetReject);
    ASSERT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::Queued);
    controller.UnregisterActionHook(registration);
    EXPECT_FALSE(controller.ReadAction(registration));
    const auto next = controller.RegisterActionHook(43, TerrainFaultKind::BudgetReject);
    EXPECT_FALSE(controller.ReadAction(next));
    EXPECT_EQ(controller.Queue(request).code, TerrainFaultQueueCode::PendingHookMissing);
}
}
}
