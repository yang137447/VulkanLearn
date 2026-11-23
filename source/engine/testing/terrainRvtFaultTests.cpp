#include "engine/testing/terrainRvtFaultTests.h"

#include <limits>
#include <set>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include "core/runtimeEvidence.h"

namespace VL
{
namespace
{
using Json = nlohmann::json;

Json ControlPayload(const TerrainFaultRequest& request, const TerrainFaultQueueResult& result)
{
    return {{"protocol", 1}, {"faultId", request.faultId},
        {"kind", TerrainFaultKindName(request.kind)}, {"worldGeneration", request.worldGeneration},
        {"selector", {{"surfaceId", request.page.surfaceId}, {"mip", request.page.mip},
            {"x", request.page.x}, {"y", request.page.y}, {"revision", request.page.revision}}},
        {"requestedStatus", request.status}, {"requestedSubmitResult", request.submitResult},
        {"replacementRevision", request.replacementRevision},
        {"testAdmissionLimitBytes", request.testAdmissionLimitBytes},
        {"result", TerrainFaultQueueCodeName(result.code)}, {"diagnostic", result.diagnostic}};
}

uint64_t Unsigned(const Json& document, const char* key, uint64_t maximum = UINT64_MAX)
{
    const auto& value = document.at(key);
    if ((!value.is_number_unsigned() && !value.is_number_integer()) ||
        (value.is_number_integer() && !value.is_number_unsigned() && value.get<int64_t>() < 0))
        throw std::invalid_argument(std::string(key) + " must be an unsigned integer");
    const auto result = value.get<uint64_t>();
    if (result > maximum)
        throw std::invalid_argument(std::string(key) + " exceeds its integer bound");
    return result;
}
}

TerrainFaultRequest ParseTerrainRvtFaultCommand(const std::string& payload)
{
    if (payload.empty() || payload.size() > 4096)
        throw std::invalid_argument("rvtfault requires a JSON object of at most 4096 bytes");
    std::set<std::string> seenKeys;
    const Json document = Json::parse(payload, [&](int, Json::parse_event_t event, Json& parsed) {
        if (event == Json::parse_event_t::key && !seenKeys.insert(parsed.get<std::string>()).second)
            throw std::invalid_argument("Duplicate rvtfault JSON key");
        return true;
    });
    if (!document.is_object())
        throw std::invalid_argument("rvtfault requires an object");
    TerrainFaultRequest request;
    const auto kind = document.at("kind").get<std::string>();
    for (const auto candidate : {TerrainFaultKind::StatusNonzero, TerrainFaultKind::SubmitFail,
        TerrainFaultKind::StaleRevision, TerrainFaultKind::BudgetReject, TerrainFaultKind::SlotReuse,
        TerrainFaultKind::WorldCandidateBadInput, TerrainFaultKind::Cancel})
        if (kind == TerrainFaultKindName(candidate))
            request.kind = candidate;
    if (request.kind == TerrainFaultKind::None)
        throw std::invalid_argument("Unknown rvtfault kind");
    request.faultId = document.at("faultId").get<std::string>();
    std::set<std::string> fields{"kind", "faultId"};
    if (request.kind != TerrainFaultKind::Cancel)
    {
        fields.insert("worldGeneration");
        request.worldGeneration = Unsigned(document, "worldGeneration");
    }
    if (request.kind != TerrainFaultKind::Cancel && request.kind != TerrainFaultKind::BudgetReject &&
        request.kind != TerrainFaultKind::WorldCandidateBadInput)
    {
        fields.insert({"surfaceId", "mip", "x", "y", "revision"});
        request.page.surfaceId = document.at("surfaceId").get<std::string>();
        request.page.mip = static_cast<uint32_t>(Unsigned(document, "mip", 31));
        request.page.x = static_cast<uint32_t>(Unsigned(document, "x", 131071));
        request.page.y = static_cast<uint32_t>(Unsigned(document, "y", 131071));
        request.page.revision = Unsigned(document, "revision");
    }
    switch (request.kind)
    {
    case TerrainFaultKind::StatusNonzero:
        fields.insert("status");
        request.status = static_cast<uint32_t>(Unsigned(document, "status", UINT32_MAX));
        break;
    case TerrainFaultKind::SubmitFail:
    {
        fields.insert("submitResult");
        const auto& value = document.at("submitResult");
        if (!value.is_number_integer() || value.is_number_unsigned())
            throw std::invalid_argument("submitResult must be a negative int32 VkResult");
        const auto result = value.get<int64_t>();
        if (result < INT32_MIN || result >= 0)
            throw std::invalid_argument("submitResult must be a negative int32 VkResult");
        request.submitResult = static_cast<int32_t>(result);
        break;
    }
    case TerrainFaultKind::StaleRevision:
        fields.insert("replacementRevision");
        request.replacementRevision = Unsigned(document, "replacementRevision");
        break;
    case TerrainFaultKind::BudgetReject:
        fields.insert("testAdmissionLimitBytes");
        request.testAdmissionLimitBytes = Unsigned(document, "testAdmissionLimitBytes");
        break;
    case TerrainFaultKind::SlotReuse:
        fields.insert({"slot", "writeEpoch"});
        request.slot = static_cast<uint32_t>(Unsigned(document, "slot", UINT32_MAX));
        request.writeEpoch = Unsigned(document, "writeEpoch");
        break;
    case TerrainFaultKind::WorldCandidateBadInput:
        fields.insert({"candidateScene", "objectId", "byteOffset", "xorMask"});
        request.candidateScene = document.at("candidateScene").get<std::string>();
        request.objectId = document.at("objectId").get<std::string>();
        request.byteOffset = Unsigned(document, "byteOffset");
        request.xorMask = static_cast<uint32_t>(Unsigned(document, "xorMask", 255));
        break;
    default:
        break;
    }
    for (const auto& entry : document.items())
        if (!fields.count(entry.key()))
            throw std::invalid_argument("Unexpected rvtfault field: " + entry.key());
    const auto invalid = ValidateTerrainFaultRequest(request);
    if (!invalid.empty())
        throw std::invalid_argument(invalid);
    return request;
}

TerrainFaultQueueResult TerrainRvtFaultTestingService::Submit(const TerrainFaultRequest& request)
{
    const auto result = TerrainFaultInjection().Queue(request);
    RuntimeEvidence::Record("RVT_FAULT_CONTROL", ControlPayload(request, result));
    return result;
}

std::string TerrainRvtFaultTestingService::ControlDiagnostic(
    const TerrainFaultRequest& request, const TerrainFaultQueueResult& result)
{
    return "[RVT_FAULT_CONTROL] " + ControlPayload(request, result).dump();
}
}
