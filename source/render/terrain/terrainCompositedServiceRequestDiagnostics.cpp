#include "render/terrain/terrainCompositedServiceRequestDiagnostics.h"
#include "core/runtimeEvidence.h"
#include "core/tracingIfstream.h"
#include "terrain/terrainCompositedFeedback.h"
#include <cstdlib>
#include <sstream>
#include <stdexcept>

namespace VL
{
std::unique_ptr<TerrainCompositedServiceRequestDiagnostics>
TerrainCompositedServiceRequestDiagnostics::Create(TerrainRvtBudgetOwner& owner)
{
    const char* path = std::getenv("VULKANLEARN_RVT_REQUEST_PROBE_PLAN");
    if (!path || !*path)
        return {};
    if (!RuntimeEvidence::IsEnabled())
        throw std::runtime_error("Explicit request probe requires the dedicated runtime evidence sink");
    auto admission = owner.TryReserve({0, 65536 + sizeof(TerrainCompositedServiceRequestDiagnostics), 0, 0},
        TerrainRvtCostClass::Workspace, TerrainRvtResourcePhase::Active);
    if (!admission)
        throw std::runtime_error("Explicit request probe configuration exceeds Workspace CPU admission");
    RuntimeFileTrace::TracingIfstream input(path, std::ios::binary | std::ios::ate);
    if (!input || input.tellg() <= 0 || input.tellg() > 8192)
        throw std::runtime_error("Explicit request probe plan must contain 1..8192 bytes");
    const auto bytes = static_cast<size_t>(input.tellg());
    input.seekg(0);
    std::string text(bytes, '\0');
    input.read(text.data(), static_cast<std::streamsize>(bytes));
    if (!input)
        throw std::runtime_error("Explicit request probe plan read failed");
    auto result = std::make_unique<TerrainCompositedServiceRequestDiagnostics>();
    result->configurationLease = std::move(admission);
    result->plan = nlohmann::json::parse(text);
    result->activationPose = result->plan.at("activationPose").get<uint64_t>();
    result->requestId = result->plan.at("requestId").get<std::string>();
    result->expectedWorld = result->plan.at("worldGeneration").get<uint64_t>();
    if (!result->activationPose || !result->expectedWorld || result->requestId.empty() ||
        result->requestId.size() > 96 || result->plan.value("schema", "") != "rvt-explicit-page-probe-v1")
        throw std::runtime_error("Explicit request probe requires bounded requestId and exact World/activationPose");
    return result;
}

void TerrainCompositedServiceRequestDiagnostics::Cancel() noexcept
{
    collector.Cancel();
    plan.clear();
    terminal = true;
    armed = false;
}

void TerrainCompositedServiceRequestDiagnostics::ObserveFeedback(
    const TerrainCompositedRuntime& runtime, const std::vector<TerrainCompositedVTTaskToken>& requests)
{
    if (terminal)
        return;
    ++feedbackBatch;
    feedbackRequestCount = requests.size();
    matchingFeedbackRequests.fill(0);
    size_t index = 0;
    for (const auto& page : plan.at("pages"))
    {
        if (index >= matchingFeedbackRequests.size())
            break;
        const auto token = runtime.Token({page.at("surface").get<std::string>(),
            page.at("mip").get<uint32_t>(), page.at("x").get<uint32_t>(), page.at("y").get<uint32_t>()});
        witnesses[index].selectedAfter = TerrainCompositedRequestProbe::Inspect(
            runtime, token, feedbackCompletedEpoch).selected;
        matchingFeedbackRequests[index++] = std::count_if(requests.begin(), requests.end(),
            [&](const auto& requested) { return requested.Matches(token); });
    }
}

void TerrainCompositedServiceRequestDiagnostics::ObserveReadback(
    const TerrainCompositedRuntime& runtime,
    const std::array<std::string_view, 256>& surfaces, size_t surfaceCount,
    const uint8_t* bytes, size_t byteCount, uint8_t frameTag,
    uint64_t copiedEpoch, uint64_t completedEpoch)
{
    if (terminal || !armed)
        return;
    if (!bytes || byteCount != TerrainCompositedFeedbackBufferBytes ||
        !frameTag || copiedEpoch > completedEpoch)
        throw std::runtime_error("Explicit feedback witness requires completed native readback");
    feedbackCopiedEpoch = copiedEpoch;
    feedbackCompletedEpoch = completedEpoch;
    feedbackFrameTag = frameTag;
    witnesses.fill({});
    size_t index = 0;
    for (const auto& page : plan.at("pages"))
    {
        if (index >= witnesses.size())
            break;
        const auto logical = page.at("surface").get<std::string>();
        size_t surfaceIndex = 0;
        while (surfaceIndex < surfaceCount && surfaces[surfaceIndex] != logical)
            ++surfaceIndex;
        if (surfaceIndex == surfaceCount)
            throw std::runtime_error("Explicit feedback witness surface is absent");
        const auto mip = page.at("mip").get<uint32_t>();
        const auto x = page.at("x").get<uint32_t>();
        const auto y = page.at("y").get<uint32_t>();
        auto& witness = witnesses[index++];
        witness.selectedBefore = TerrainCompositedRequestProbe::Inspect(
            runtime, runtime.Token({logical, mip, x, y}), completedEpoch).selected;
        for (uint32_t pair = 0; pair < 2; ++pair)
            for (size_t offset = 0; offset < TerrainCompositedFeedbackImageBytes; offset += 4)
            {
                const auto* address = bytes + pair * 2 * TerrainCompositedFeedbackImageBytes + offset;
                const auto* metadata = address + TerrainCompositedFeedbackImageBytes;
                if (address[0] != surfaceIndex || (address[3] & 127u) != mip)
                    continue;
                ++witness.sameSurfaceMipTexels;
                const bool exact = address[1] == x && address[2] == y && (address[3] & 128u);
                if (!witness.present || exact)
                {
                    std::copy_n(address, 4, witness.address.begin());
                    std::copy_n(metadata, 4, witness.metadata.begin());
                    witness.present = true;
                }
                if (!exact)
                    continue;
                ++witness.exactAddressTexels;
                const auto decoded = DecodeTerrainCompositedFeedbackTexel(address, metadata);
                witness.decodedMatches += decoded.valid && decoded.frameTag == frameTag;
            }
    }
}

void TerrainCompositedServiceRequestDiagnostics::Capture(
    const TerrainCompositedRuntime& runtime, TerrainRvtBudgetOwner& owner,
    uint64_t world, uint64_t pose, double poseStartedMs, uint64_t renderFrame,
    uint64_t completedEpoch, double nowMs)
{
    if (terminal)
        return;
    if (world != expectedWorld || (armed && (world != activeWorld || pose != activationPose)))
    {
        Cancel();
        return;
    }
    if (pose != activationPose)
        return;
    if (!armed)
    {
        if (!collector.ArmPlan(runtime, plan, owner))
            throw std::runtime_error("Explicit request probe token/Workspace admission failed");
        activeWorld = world;
        armed = true;
    }
    if (!collector.MatchesCurrent(runtime))
    {
        Cancel();
        return;
    }
    const double elapsed = nowMs - poseStartedMs;
    if (elapsed < double(captures) * 1000)
        return;
    if (elapsed > 12000 || captures >= TerrainCompositedExplicitRequestCollector::MaxCaptures)
    {
        Cancel();
        return;
    }
    std::ostringstream output;
    if (!collector.Capture(runtime, renderFrame, completedEpoch, output) || !output.good())
        throw std::runtime_error("Explicit request probe capture/output failed");
    const auto text = output.str();
    if (text.size() > 32768)
        throw std::runtime_error("Explicit request probe output exceeds charged bound");
    std::istringstream rows(text);
    std::string line;
    size_t pageIndex = 0;
    while (std::getline(rows, line))
    {
        auto row = nlohmann::json::parse(line);
        row["logicalSurfaceId"] = runtime.LogicalSurfaceId(row.at("namespace").get<std::string>());
        row["stage"] = "post-complete/pre-record";
        row["poseId"] = pose;
        row["requestId"] = requestId;
        row["poseElapsedMs"] = elapsed;
        row["captureIndex"] = captures;
        row["lastDecodedFeedbackBatch"] = feedbackBatch;
        row["decodedFeedbackRequestCount"] = feedbackRequestCount;
        row["observedFeedbackRequested"] = feedbackBatch ?
            nlohmann::json(matchingFeedbackRequests.at(pageIndex) != 0) : nlohmann::json();
        row["observedFeedbackMatchingRequests"] = matchingFeedbackRequests.at(pageIndex++);
        const auto& witness = witnesses.at(pageIndex - 1);
        row["feedbackReadback"] = {
            {"copiedEpoch", feedbackCopiedEpoch}, {"completedEpoch", feedbackCompletedEpoch},
            {"expectedFrameTag", feedbackFrameTag}, {"bytesPresent", witness.present},
            {"addressRGBA8", witness.address}, {"metadataRGBA8", witness.metadata},
            {"sameSurfaceMipTexels", witness.sameSurfaceMipTexels},
            {"exactAddressTexels", witness.exactAddressTexels}, {"decodedMatches", witness.decodedMatches},
            {"selectedBeforeFeedback", witness.selectedBefore}, {"selectedAfterSelector", witness.selectedAfter}};
        row["feedbackSourceRenderFrame"] = nullptr;
        RuntimeEvidence::Record("RVT_REQUEST_PROBE", row);
    }
    ++captures;
    if (captures == TerrainCompositedExplicitRequestCollector::MaxCaptures)
        Cancel();
}
}
