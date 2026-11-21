#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>
#include "core/runtimeEvidence.h"
#include "engine/testing/terrainRvtFaultTests.h"
#include "render/backend/frameTimingEvidence.h"
#include "render/frame/frameSubmitFault.h"

TEST(FrameEvidencePayload, NativeIdentityAndUnclampedIntervalStayLossless)
{
    const auto identity = VL::RenderFrameIdentity::FromFrame(315, (uint64_t{1} << 60) + 11);
    const double wallMs = 5432.123456789;
    const auto payload = VL::CpuFrameTimingPayload(&identity, 1, true, 313,
        "planned", wallMs, 1234567890123456ll);
    EXPECT_DOUBLE_EQ(payload.at("renderFrameIntervalMs").get<double>(), wallMs);
    EXPECT_EQ(payload.at("worldGeneration").get<uint64_t>(), (uint64_t{1} << 60) + 11);
    EXPECT_EQ(payload.at("renderFrame"), 315);
    EXPECT_EQ(payload.at("submitEpoch"), 316);
    EXPECT_EQ(payload.at("frameSlot"), 1);
}

TEST(FrameEvidencePayload, SkippedAndLegacySamplesCannotInventReadyOrFrame)
{
    const auto identity = VL::RenderFrameIdentity::FromFrame(17, 93);
    const auto skipped = VL::CpuFrameTimingPayload(&identity, {}, false, 0, "none", {}, 99, true);
    EXPECT_EQ(skipped.at("event"), "frame-skipped");
    EXPECT_TRUE(skipped.at("renderFrameIntervalMs").is_null());
    EXPECT_TRUE(skipped.at("frameSlot").is_null());
    EXPECT_EQ(skipped.at("acquiredReady"), false);
    const auto legacy = VL::CpuFrameTimingPayload(nullptr, {}, {}, 0, "identity-unavailable", 33.3, 100);
    EXPECT_TRUE(legacy.at("frame").is_null());
    EXPECT_TRUE(legacy.at("worldGeneration").is_null());
}

TEST(FrameEvidencePayload, GpuReadbackSequenceCannotReplaceOriginalRecordedFrame)
{
    const auto identity = VL::RenderFrameIdentity::FromFrame(1007, 88);
    const auto payload = VL::GpuFrameTimingPayload(identity, 0, 3, 1008, 987654321,
        {"geometry", "terrainQualityAfterFeedback", "bloom"}, {7.25, 0.125, {}}, 3);
    EXPECT_EQ(payload.at("frame"), 1007);
    EXPECT_EQ(payload.at("readbackSequence"), 3);
    EXPECT_EQ(payload.at("fenceCompletedEpoch"), 1008);
    EXPECT_EQ(payload.at("passes").at(1).at("name"), "terrainQualityAfterFeedback");
    EXPECT_TRUE(payload.at("passes").at(2).at("ms").is_null());
}

TEST(FrameEvidenceSink, RealDedicatedFileSurvivesSuppressedStdoutAndRejectsPostCloseWrites)
{
    namespace fs = std::filesystem;
    using Json = nlohmann::json;
    const auto root = fs::temp_directory_path() /
        ("vl-frame-sink-cpu-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root / "package");
    const auto path = root / "cpu-fixture.jsonl";
    const char* old = std::getenv("VULKANLEARN_RUNTIME_EVIDENCE");
    const std::string previous = old ? old : "";
#if defined(_WIN32)
    ASSERT_EQ(_putenv_s("VULKANLEARN_RUNTIME_EVIDENCE", path.u8string().c_str()), 0);
#else
    ASSERT_EQ(setenv("VULKANLEARN_RUNTIME_EVIDENCE", path.c_str(), 1), 0);
#endif
    ASSERT_EQ(VL::RuntimeEvidence::Initialize(root / "package"), VL::RuntimeEvidence::Status::Recorded);
    VL::RuntimeEvidence::Record("CPU_TEST_SCOPE", {{"scope", "synthetic CPU metadata, no GPU execution"}});
    std::ostringstream hidden;
    auto* saved = std::cout.rdbuf(hidden.rdbuf());
    struct RestoreStdout
    {
        std::streambuf* saved;
        ~RestoreStdout() { std::cout.rdbuf(saved); }
    } restore{saved};
    const auto identity = VL::RenderFrameIdentity::FromFrame(20, 7);
    VL::RuntimeEvidence::Record("CPU_RAW", VL::CpuFrameTimingPayload(
        &identity, 0, true, 19, "planned", 1234.567890123, 777777777));
    VL::RuntimeEvidence::Record("GPU_RAW", VL::GpuFrameTimingPayload(identity, 0, 3, 21, 888888888,
        {"geometry", "terrainQualityAfterFeedback"}, {1.5, 0.25}, 2));
    VL::TerrainFaultRequest missing;
    missing.kind = VL::TerrainFaultKind::StatusNonzero;
    missing.faultId = "cpu-fixture-not-native";
    missing.worldGeneration = 7;
    missing.page = {"terrain|cpu-fixture", 1, 0, 0, 0};
    missing.status = 1;
    const auto control = VL::TerrainRvtFaultTestingService::Submit(missing);
    EXPECT_EQ(control.code, VL::TerrainFaultQueueCode::PendingHookMissing);
    VL::ObserveRecoveredDescriptorFrame(identity, {21, 0, 1, false}, {7, 88}, "cpu-fixture", 0);
    VL::ObservePostAbortCompletedFrame(999);
    EXPECT_TRUE(VL::NeedsDescriptorRecoveryWitness());
    const auto submitted = VL::RenderFrameIdentity::FromFrame(24, 7);
    const VL::FrameBindingIdentity actual{7, 89};
    VL::ObserveDescriptorRecoverySubmittedFrame(submitted, &actual);
    VL::ObservePostAbortCompletedFrame(24);
    EXPECT_TRUE(VL::NeedsDescriptorRecoveryWitness());
    VL::ObservePostAbortCompletedFrame(25);
    EXPECT_FALSE(VL::NeedsDescriptorRecoveryWitness());
    std::cout.rdbuf(saved);
    ASSERT_EQ(VL::RuntimeEvidence::Close(), VL::RuntimeEvidence::Status::Recorded);
    std::ifstream input(path);
    std::vector<Json> records;
    std::string line;
    while (std::getline(input, line)) records.push_back(Json::parse(line));
    ASSERT_FALSE(records.empty());
    uint64_t sequence = 0;
    bool cpu = false, gpu = false, fault = false, complete = false;
    for (const auto& record : records)
    {
        EXPECT_EQ(record.at("sequence").get<uint64_t>(), ++sequence);
        EXPECT_EQ(record.at("outputCategory"), "explicitoutput");
        const auto& data = record.at("payload");
        if (record.at("eventClass") == "CPU_RAW" && data.value("event", "") == "frame-timing")
        {
            cpu = true;
            EXPECT_DOUBLE_EQ(data.at("renderFrameIntervalMs").get<double>(), 1234.567890123);
            EXPECT_EQ(data.at("frame"), 20);
        }
        if (record.at("eventClass") == "GPU_RAW")
        {
            gpu = true;
            EXPECT_EQ(data.at("renderFrame"), 20);
            EXPECT_EQ(data.at("readbackSequence"), 3);
        }
        if (record.at("eventClass") == "RVT_FAULT_CONTROL")
        {
            fault = true;
            EXPECT_EQ(data.at("result"), "pendinghookmissing");
        }
        if (data.value("event", "") == "descriptor-retry-frame-completed")
        {
            complete = true;
            EXPECT_EQ(data.at("renderFrame"), 24);
            EXPECT_EQ(data.at("submitEpoch"), 25);
            EXPECT_EQ(data.at("skippedRenderFrame"), 20);
            EXPECT_EQ(data.at("requestedBindingRevision"), 88);
            EXPECT_EQ(data.at("renderedBindingRevision"), 89);
        }
    }
    EXPECT_TRUE(cpu && gpu && fault && complete);
    EXPECT_EQ(records.back().at("eventClass"), "runtimeEvidenceEnd");
    EXPECT_THROW(VL::RuntimeEvidence::Record("CPU_RAW", {{"fixture", "post-close"}}), std::runtime_error);
    EXPECT_TRUE(VL::RuntimeEvidence::HasIncompleteSession());
#if defined(_WIN32)
    _putenv_s("VULKANLEARN_RUNTIME_EVIDENCE", previous.c_str());
#else
    if (previous.empty()) unsetenv("VULKANLEARN_RUNTIME_EVIDENCE");
    else setenv("VULKANLEARN_RUNTIME_EVIDENCE", previous.c_str(), 1);
#endif
    input.close();
    ASSERT_EQ(fs::weakly_canonical(root).parent_path(), fs::canonical(fs::temp_directory_path()));
    ASSERT_EQ(root.filename().u8string().find("vl-frame-sink-cpu-"), 0u);
    fs::remove_all(root);
}
