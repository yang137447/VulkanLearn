#pragma once

#include <cstdlib>
#include <optional>
#include <vector>
#include <string>
#include <nlohmann/json.hpp>
#include "core/runtimeEvidence.h"
#include "render/backend/gpuFrameIdentity.h"

namespace VL
{
inline bool FrameTimingEvidenceRequested()
{
    const char* configured = std::getenv("VULKANLEARN_RUNTIME_EVIDENCE");
    return RuntimeEvidence::IsEnabled() || (configured && *configured);
}

inline nlohmann::json RawFrameIdentityPayload(const RenderFrameIdentity& identity)
{
    return {{"identityVersion", 2}, {"frame", identity.renderFrame},
        {"renderFrame", identity.renderFrame}, {"worldGeneration", identity.worldGeneration},
        {"submitEpoch", identity.submitEpoch}};
}

inline nlohmann::json CpuFrameTimingPayload(const RenderFrameIdentity* identity,
    std::optional<uint32_t> slot, std::optional<bool> ready, uint64_t completedEpoch,
    const char* queryState, std::optional<double> intervalMs, int64_t sampleNs, bool skipped = false)
{
    auto payload = identity ? RawFrameIdentityPayload(*identity) :
        nlohmann::json{{"identityVersion", 2}, {"frame", nullptr}, {"renderFrame", nullptr},
            {"worldGeneration", nullptr}, {"submitEpoch", nullptr}};
    payload["event"] = skipped ? "frame-skipped" : "frame-timing";
    payload["renderFrameIntervalMs"] = intervalMs ? nlohmann::json(*intervalMs) : nlohmann::json(nullptr);
    payload["frameSlot"] = slot ? nlohmann::json(*slot) : nlohmann::json(nullptr);
    payload["acquiredReady"] = ready ? nlohmann::json(*ready) : nlohmann::json(nullptr);
    payload["completedEpoch"] = completedEpoch;
    payload["gpuQueryState"] = queryState;
    payload["sampleMonotonicNs"] = sampleNs;
    payload["phase"] = skipped ? "acquire-not-ready" : identity ? "record-begin" : "identity-unavailable";
    payload["submitEpochPlanned"] = true;
    if (skipped)
        payload["reason"] = "acquire-not-ready";
    return payload;
}

inline nlohmann::json GpuFrameTimingPayload(const RenderFrameIdentity& identity,
    uint32_t slot, uint64_t readbackSequence, uint64_t completedEpoch, int64_t readbackNs,
    const std::vector<std::string>& names, const std::vector<std::optional<double>>& durations,
    uint32_t count)
{
    auto payload = RawFrameIdentityPayload(identity);
    payload["slot"] = slot;
    payload["readbackSequence"] = readbackSequence;
    payload["fenceCompletedEpoch"] = completedEpoch;
    payload["readbackMonotonicNs"] = readbackNs;
    payload["passes"] = nlohmann::json::array();
    for (uint32_t index = 0; index < count; ++index)
        payload["passes"].push_back({{"name", names.at(index)},
            {"ms", durations.at(index) ? nlohmann::json(*durations[index]) : nlohmann::json(nullptr)}});
    return payload;
}
}
