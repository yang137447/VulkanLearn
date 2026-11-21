#include "render/frame/frameSubmitFault.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include "core/runtimeEvidence.h"
#include "render/backend/frameTimingEvidence.h"

namespace VL
{
namespace
{
std::mutex faultMutex;
FrameSubmitFaultProvider provider;
std::atomic<bool> providerInstalled{false};
std::atomic<bool> recoveryPending{false};
struct Recovery
{
    std::string faultId;
    uint64_t ticket = 0;
    uint64_t worldGeneration = 0;
    uint64_t recoveryEpoch = 0;
    uint64_t successfulEpoch = 0;
    std::optional<uint64_t> successfulFrame;
    std::chrono::steady_clock::time_point expires;
};
std::optional<Recovery> recovery;
std::atomic<bool> descriptorPending{false};
struct DescriptorRecovery
{
    RenderFrameIdentity skipped;
    FrameBindingIdentity requested;
    uint64_t recoveryEpoch = 0;
    std::optional<RenderFrameIdentity> submitted;
    std::optional<FrameBindingIdentity> actualBindings;
    std::chrono::steady_clock::time_point expires;
};
std::optional<DescriptorRecovery> descriptorRecovery;
}

void SetFrameSubmitFaultProvider(FrameSubmitFaultProvider value)
{
    std::lock_guard<std::mutex> lock(faultMutex);
    provider = std::move(value);
    providerInstalled.store(bool(provider), std::memory_order_release);
}

std::optional<FrameSubmitFault> TakeFrameSubmitFault(
    const std::shared_ptr<TerrainCompositedService>& service, uint64_t generation, uint64_t epoch,
    std::optional<uint64_t> renderFrame)
{
    if (!providerInstalled.load(std::memory_order_acquire))
        return {};
    FrameSubmitFaultProvider snapshot;
    {
        std::lock_guard<std::mutex> lock(faultMutex);
        snapshot = provider;
    }
    if (!snapshot || !service)
        return {};
    auto fault = snapshot(service, generation, epoch);
    if (fault && (!fault->registration || !fault->failure.abortTicket || !fault->ownerLease ||
        !fault->observeAfterAbort || fault->task.worldGeneration != generation ||
        fault->task.token.submissionEpoch != epoch || fault->failure.injectedResult >= 0))
        throw std::logic_error("Frame fault provider returned an incomplete or mismatched recorded task");
    if (fault)
    {
        const auto& task = fault->task;
        RuntimeEvidence::Record("RVT_EVIDENCE", {
            {"event", "submit-fault-consumed"}, {"faultId", fault->failure.faultId},
            {"renderFrame", renderFrame ? nlohmann::json(*renderFrame) : nlohmann::json(nullptr)},
            {"abortTicket", fault->failure.abortTicket}, {"worldGeneration", task.worldGeneration},
            {"surfaceId", task.page.surfaceId}, {"mip", task.page.mip}, {"x", task.page.x}, {"y", task.page.y},
            {"revision", task.page.revision}, {"tokenGeneration", task.token.generation},
            {"contentIdentity", task.token.contentIdentity}, {"submissionEpoch", task.token.submissionEpoch},
            {"failureMode", "injected-before-submit"}, {"injectedSubmitResult", fault->failure.injectedResult},
            {"rawSubmitResult", nullptr}});
    }
    return fault;
}

void ObserveRecoveredFrameAbort(const FrameSubmitFault& fault, const FrameAbortResult& result,
    std::optional<uint64_t> renderFrame)
{
    const auto raw = fault.observeAfterAbort();
    if (raw.abortTicket != fault.failure.abortTicket ||
        raw.task.worldGeneration != fault.task.worldGeneration ||
        raw.task.token.generation != fault.task.token.generation ||
        raw.task.token.contentIdentity != fault.task.token.contentIdentity ||
        raw.task.token.submissionEpoch != fault.task.token.submissionEpoch ||
        raw.task.page.surfaceId != fault.task.page.surfaceId || raw.task.page.mip != fault.task.page.mip ||
        raw.task.page.x != fault.task.page.x || raw.task.page.y != fault.task.page.y ||
        raw.task.page.revision != fault.task.page.revision)
        throw std::logic_error("Frame Abort observer changed the consumed task identity");
    if (!TerrainFaultInjection().ObserveSubmitAbort(fault.registration, raw))
        throw std::logic_error("Actual frame Abort observation did not match consumed fault ticket");
    const nlohmann::json event{{"event", "submit-abort-observed"},
        {"renderFrame", renderFrame ? nlohmann::json(*renderFrame) : nlohmann::json(nullptr)},
        {"faultId", fault.failure.faultId}, {"abortTicket", raw.abortTicket},
        {"worldGeneration", raw.task.worldGeneration}, {"recoveryEpoch", result.recoveryEpoch},
        {"activeGenerationBefore", raw.activeGenerationBefore},
        {"activeGenerationAfter", raw.activeGenerationAfter}, {"recordedTasks", raw.recordedTasks},
        {"abortedTasks", raw.abortedTasks}, {"publishedChannels", raw.publishedChannels},
        {"sceneSubmitCalls", raw.backendSubmitCalls}, {"recoverySyncSubmits", 1},
        {"injectedSubmitResult", fault.failure.injectedResult}, {"rawSubmitResult", nullptr},
        {"surfaceId", raw.task.page.surfaceId}, {"mip", raw.task.page.mip}, {"x", raw.task.page.x},
        {"y", raw.task.page.y}, {"revision", raw.task.page.revision},
        {"tokenGeneration", raw.task.token.generation}, {"contentIdentity", raw.task.token.contentIdentity},
        {"submissionEpoch", raw.task.token.submissionEpoch}, {"failureMode", "injected-before-submit"}};
    RuntimeEvidence::Record("RVT_EVIDENCE", event);
    std::cout << "[FRAME_ABORT] " << event.dump() << std::endl;
    std::lock_guard<std::mutex> lock(faultMutex);
    recovery = Recovery{fault.failure.faultId, raw.abortTicket, raw.task.worldGeneration,
        result.recoveryEpoch, 0, {}, std::chrono::steady_clock::now() + std::chrono::seconds(60)};
    recoveryPending.store(true, std::memory_order_release);
}

void ObservePostAbortSubmittedFrame(uint64_t generation, uint64_t epoch,
    std::optional<uint64_t> renderFrame)
{
    if (!recoveryPending.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(faultMutex);
    if (recovery && generation == recovery->worldGeneration && epoch > recovery->recoveryEpoch &&
        !recovery->successfulEpoch)
    {
        recovery->successfulEpoch = epoch;
        recovery->successfulFrame = renderFrame;
    }
}

void ObservePostAbortCompletedFrame(uint64_t completedEpoch)
{
    if (!recoveryPending.load(std::memory_order_acquire) && !descriptorPending.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(faultMutex);
    if (recovery && recovery->successfulEpoch && completedEpoch >= recovery->successfulEpoch)
    {
        const nlohmann::json event{{"event", "old-world-frame-completed"},
            {"faultId", recovery->faultId}, {"abortTicket", recovery->ticket},
            {"worldGeneration", recovery->worldGeneration}, {"submissionEpoch", recovery->successfulEpoch},
            {"renderFrame", recovery->successfulFrame ?
                nlohmann::json(*recovery->successfulFrame) : nlohmann::json(nullptr)},
            {"fenceCompletedEpoch", completedEpoch}};
        RuntimeEvidence::Record("FRAME_ABORT", event);
        std::cout << "[FRAME_ABORT] " << event.dump() << std::endl;
        recovery.reset();
    }
    else if (recovery && std::chrono::steady_clock::now() >= recovery->expires)
        recovery.reset();
    recoveryPending.store(bool(recovery), std::memory_order_release);
    if (descriptorRecovery && descriptorRecovery->submitted &&
        completedEpoch >= descriptorRecovery->submitted->submitEpoch)
    {
        auto event = RawFrameIdentityPayload(*descriptorRecovery->submitted);
        event.update({{"event", "descriptor-retry-frame-completed"},
            {"skippedRenderFrame", descriptorRecovery->skipped.renderFrame},
            {"recoveryEpoch", descriptorRecovery->recoveryEpoch},
            {"requestedBindingWorld", descriptorRecovery->requested.worldGeneration},
            {"requestedBindingRevision", descriptorRecovery->requested.revision},
            {"fenceCompletedEpoch", completedEpoch},
            {"renderedBindingWorld", descriptorRecovery->actualBindings->worldGeneration},
            {"renderedBindingRevision", descriptorRecovery->actualBindings->revision}});
        RuntimeEvidence::Record("FRAME_SKIPPED", event);
        descriptorRecovery.reset();
    }
    else if (descriptorRecovery && std::chrono::steady_clock::now() >= descriptorRecovery->expires)
    {
        RuntimeEvidence::Record("FRAME_SKIPPED", {{"event", "descriptor-continuation-unobserved"},
            {"skippedRenderFrame", descriptorRecovery->skipped.renderFrame},
            {"worldGeneration", descriptorRecovery->skipped.worldGeneration},
            {"recoveryEpoch", descriptorRecovery->recoveryEpoch}});
        descriptorRecovery.reset();
    }
    descriptorPending.store(bool(descriptorRecovery), std::memory_order_release);
}

void ObserveRecoveredDescriptorFrame(const RenderFrameIdentity& frame, const FrameAbortResult& result,
    const FrameBindingIdentity& requested, const char* operation, int32_t rawResult, uint32_t retryCause)
{
    auto event = RawFrameIdentityPayload(frame);
    event.update({{"event", "frameSkipped"}, {"reason", "retryable-world-descriptor"},
        {"operation", operation}, {"retryCause", retryCause},
        {"rawVkResult", rawResult ? nlohmann::json(rawResult) : nlohmann::json(nullptr)},
        {"requestedBindingWorld", requested.worldGeneration}, {"requestedBindingRevision", requested.revision},
        {"imageIndex", result.imageIndex}, {"frameSlot", result.frameIndex},
        {"recoveryEpoch", result.recoveryEpoch}, {"sceneSubmitted", false},
        {"gpuQueryState", "aborted-unsubmitted"}});
    RuntimeEvidence::Record("FRAME_SKIPPED", event);
    auto cpu = event;
    cpu["event"] = "frame-skipped";
    cpu["renderFrameIntervalMs"] = nullptr;
    cpu["acquiredReady"] = true;
    RuntimeEvidence::Record("CPU_RAW", cpu);
    std::lock_guard<std::mutex> lock(faultMutex);
    if (descriptorRecovery)
        RuntimeEvidence::Record("FRAME_SKIPPED", {{"event", "descriptor-continuation-superseded"},
            {"skippedRenderFrame", descriptorRecovery->skipped.renderFrame},
            {"worldGeneration", descriptorRecovery->skipped.worldGeneration},
            {"recoveryEpoch", descriptorRecovery->recoveryEpoch}});
    descriptorRecovery = DescriptorRecovery{frame, requested, result.recoveryEpoch, {}, {},
        std::chrono::steady_clock::now() + std::chrono::seconds(60)};
    descriptorPending.store(true, std::memory_order_release);
}

bool NeedsDescriptorRecoveryWitness() noexcept
{
    return descriptorPending.load(std::memory_order_acquire);
}

void ObserveDescriptorRecoverySubmittedFrame(
    const RenderFrameIdentity& frame, const FrameBindingIdentity* actualBindings)
{
    if (!descriptorPending.load(std::memory_order_acquire))
        return;
    std::lock_guard<std::mutex> lock(faultMutex);
    if (descriptorRecovery && actualBindings && !descriptorRecovery->submitted &&
        frame.worldGeneration == descriptorRecovery->skipped.worldGeneration &&
        frame.submitEpoch > descriptorRecovery->recoveryEpoch)
    {
        descriptorRecovery->submitted = frame;
        descriptorRecovery->actualBindings = *actualBindings;
    }
}
}
