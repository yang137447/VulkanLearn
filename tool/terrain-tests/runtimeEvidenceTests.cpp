#include "core/runtimeEvidence.h"
#include "render/frame/terrainCompositedCaptureLifecycle.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>
#include <io.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace VL
{
namespace RuntimeEvidence
{
#if defined(VL_FILE_IO_TEST_HOOKS)
struct TestAccess
{
    static void ReplaceWithReadOnlyFile(Sink& sink, const std::filesystem::path& path)
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        if (sink.file)
            std::fclose(sink.file);
#if defined(_WIN32)
        sink.file = _wfopen(path.c_str(), L"rb");
#else
        sink.file = std::fopen(path.c_str(), "rb");
#endif
        if (!sink.file)
            throw std::runtime_error("test read-only stream open failed");
    }

    static void BufferOnReadOnlyDescriptor(Sink& sink, const std::filesystem::path& path)
    {
        std::lock_guard<std::mutex> lock(sink.mutex);
        if (sink.file)
            std::fclose(sink.file);
#if defined(_WIN32)
        sink.file = _wfopen(path.c_str(), L"ab");
        FILE* reader = _wfopen(path.c_str(), L"rb");
#else
        sink.file = std::fopen(path.c_str(), "ab");
        FILE* reader = std::fopen(path.c_str(), "rb");
#endif
        if (!sink.file || !reader)
            throw std::runtime_error("test descriptor setup failed");
        if (std::setvbuf(sink.file, nullptr, _IOFBF, 4096) != 0 ||
            std::fwrite("pending", 1, 7, sink.file) != 7)
        {
            std::fclose(reader);
            throw std::runtime_error("test buffered write setup failed");
        }
#if defined(_WIN32)
        const bool replaced = _dup2(_fileno(reader), _fileno(sink.file)) == 0;
#else
        const bool replaced = dup2(fileno(reader), fileno(sink.file)) >= 0;
#endif
        std::fclose(reader);
        if (!replaced)
            throw std::runtime_error("test descriptor replacement failed");
    }
};
#endif
namespace
{
using Json = nlohmann::json;

void SetEnvironment(const std::string& value)
{
#if defined(_WIN32)
    ASSERT_EQ(_putenv_s("VULKANLEARN_RUNTIME_EVIDENCE", value.c_str()), 0);
#else
    if (value.empty())
        ASSERT_EQ(unsetenv("VULKANLEARN_RUNTIME_EVIDENCE"), 0);
    else
        ASSERT_EQ(setenv("VULKANLEARN_RUNTIME_EVIDENCE", value.c_str(), 1), 0);
#endif
}

class RuntimeEvidenceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        const char* previous = std::getenv("VULKANLEARN_RUNTIME_EVIDENCE");
        previousEnvironment = previous ? previous : "";
        static std::atomic<uint64_t> counter{0};
        const auto token = std::chrono::steady_clock::now().time_since_epoch().count();
        root = std::filesystem::temp_directory_path() /
            ("vl-runtime-evidence-" + std::to_string(token) + "-" + std::to_string(++counter));
        ASSERT_TRUE(std::filesystem::create_directory(root));
        package = root / "package";
        ASSERT_TRUE(std::filesystem::create_directory(package));
        output = root / "evidence.jsonl";
        SetEnvironment(output.u8string());
    }

    void TearDown() override
    {
        SetEnvironment(previousEnvironment);
        const auto resolved = std::filesystem::weakly_canonical(root);
        const auto temporary = std::filesystem::canonical(std::filesystem::temp_directory_path());
        ASSERT_EQ(resolved.parent_path(), temporary);
        ASSERT_EQ(resolved.filename().u8string().find("vl-runtime-evidence-"), 0u);
        std::filesystem::remove_all(resolved);
    }

    std::vector<Json> Read() const
    {
        std::ifstream stream(output, std::ios::binary);
        if (!stream)
            throw std::runtime_error("missing evidence file");
        std::vector<Json> records;
        std::string line;
        uint64_t previousTime = 0;
        while (std::getline(stream, line))
        {
            EXPECT_FALSE(line.empty());
            EXPECT_LE(line.size() + 1, Sink::LineCapacity);
            auto record = Json::parse(line);
            EXPECT_EQ(record.at("sequence").get<uint64_t>(), records.size() + 1);
            const auto currentTime = record.at("steady_ns").get<uint64_t>();
            EXPECT_GE(currentTime, previousTime);
            previousTime = currentTime;
            EXPECT_GT(record.at("threadTag").get<uint64_t>(), 0u);
            EXPECT_EQ(record.at("outputCategory"), "explicitoutput");
            records.push_back(std::move(record));
        }
        EXPECT_TRUE(stream.eof());
        std::ifstream tail(output, std::ios::binary | std::ios::ate);
        if (tail.tellg() > 0)
        {
            tail.seekg(-1, std::ios::end);
            EXPECT_EQ(tail.get(), '\n');
        }
        return records;
    }

    std::filesystem::path root;
    std::filesystem::path package;
    std::filesystem::path output;
    std::string previousEnvironment;
};

TEST_F(RuntimeEvidenceTest, DefaultDisabledHasNoFileOrConsoleSideEffect)
{
    SetEnvironment("");
    ::testing::internal::CaptureStdout();
    ::testing::internal::CaptureStderr();
    {
        Sink sink({}, {0, 0, 0});
        EXPECT_FALSE(sink.IsEnabled());
        EXPECT_EQ(sink.Record("", Json::binary({1, 2, 3})), Status::Disabled);
        EXPECT_EQ(sink.Flush(), Status::Disabled);
        EXPECT_EQ(sink.Close(), Status::Disabled);
        EXPECT_EQ(sink.GetStatistics().records, 0u);
        EXPECT_EQ(sink.GetStatistics().cpuNanoseconds, 0u);
    }
    EXPECT_TRUE(::testing::internal::GetCapturedStdout().empty());
    EXPECT_TRUE(::testing::internal::GetCapturedStderr().empty());
    EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(RuntimeEvidenceTest, ConcurrentTenThousandRecordsAreCompleteAndSequenced)
{
    Sink sink(package);
    std::atomic<uint64_t> failures{0};
    std::vector<std::thread> writers;
    for (uint64_t worker = 0; worker < 8; ++worker)
        writers.emplace_back([&, worker]
        {
            try
            {
                for (uint64_t index = 0; index < 1250; ++index)
                    if (sink.Record("terrainRvtQuality", {
                        {"eventClass", "original"}, {"worker", worker}, {"index", index},
                        {"frameEpoch", 17}, {"ready", false}, {"text", "中文\n\"\\\t"}}) != Status::Recorded)
                        ++failures;
            }
            catch (...) { ++failures; }
        });
    for (auto& writer : writers)
        writer.join();
    ASSERT_EQ(failures.load(), 0u);
    EXPECT_EQ(sink.Flush(), Status::Recorded);
    EXPECT_EQ(sink.Close(), Status::Recorded);
    const auto records = Read();
    ASSERT_EQ(records.size(), 10001u);
    std::set<uint64_t> identities;
    std::set<uint64_t> threadTags;
    for (size_t index = 0; index < 10000; ++index)
    {
        const auto& record = records[index];
        const auto& payload = record.at("payload");
        EXPECT_EQ(record.at("eventClass"), "terrainRvtQuality");
        EXPECT_EQ(payload.at("eventClass"), "original");
        EXPECT_EQ(payload.at("frameEpoch"), 17);
        EXPECT_EQ(payload.at("ready"), false);
        EXPECT_EQ(payload.at("text"), "中文\n\"\\\t");
        identities.insert(payload.at("worker").get<uint64_t>() * 1250 +
            payload.at("index").get<uint64_t>());
        threadTags.insert(record.at("threadTag").get<uint64_t>());
    }
    EXPECT_EQ(identities.size(), 10000u);
    EXPECT_EQ(threadTags.size(), 8u);
    EXPECT_EQ(records.back().at("eventClass"), "runtimeEvidenceEnd");
    EXPECT_EQ(records.back().at("payload").at("incomplete"), false);
    EXPECT_EQ(records.back().at("payload").at("recordsBeforeTerminal"), 10000);
    const auto stats = sink.GetStatistics();
    EXPECT_FALSE(stats.incomplete);
    EXPECT_TRUE(stats.closed);
    EXPECT_EQ(stats.records, records.size());
    EXPECT_EQ(stats.bytes, std::filesystem::file_size(output));
    EXPECT_GT(stats.cpuNanoseconds, 0u);
    EXPECT_GT(stats.peakCallNanoseconds, 0u);
    EXPECT_EQ(stats.fixedMetadataBytes, Sink::CostQuote().fixedMetadataBytes);
    EXPECT_EQ(Sink::CostQuote().gpuBytes, 0u);
    EXPECT_EQ(Sink::CostQuote().cpuBytes,
        Sink::CostQuote().fixedMetadataBytes + Sink::CostQuote().workspaceBytes);
    EXPECT_EQ(stats.fixedMetadataBytes, sizeof(Sink));
}

TEST_F(RuntimeEvidenceTest, InterleavedConsoleEchoCannotPolluteDedicatedSink)
{
    Sink sink(package);
    ::testing::internal::CaptureStdout();
    std::thread console([] {
        for (size_t index = 0; index < 1000; ++index)
            std::cout << "consoleecho {\"broken\":" << index << "\nhuman stdout\n";
    });
    for (size_t index = 0; index < 1000; ++index)
        ASSERT_EQ(sink.Record("frame", {{"value", index}}), Status::Recorded);
    console.join();
    const auto text = ::testing::internal::GetCapturedStdout();
    EXPECT_NE(text.find("consoleecho"), std::string::npos);
    ASSERT_EQ(sink.Close(), Status::Recorded);
    const auto records = Read();
    ASSERT_EQ(records.size(), 1001u);
    for (size_t index = 0; index < 1000; ++index)
        EXPECT_EQ(records[index].at("payload").at("value"), index);
}

TEST_F(RuntimeEvidenceTest, EachRecordIsVisibleBeforeCloseAndDestructorWritesFooter)
{
    {
        Sink sink(package);
        EXPECT_EQ(sink.Record("first", Json::array({1, "two", nullptr})), Status::Recorded);
        const auto records = Read();
        ASSERT_EQ(records.size(), 1u);
        EXPECT_EQ(records[0].at("payload"), Json::array({1, "two", nullptr}));
    }
    const auto records = Read();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records.back().at("eventClass"), "runtimeEvidenceEnd");
    EXPECT_FALSE(records.back().at("payload").at("incomplete").get<bool>());
}

TEST_F(RuntimeEvidenceTest, RejectsRelativeMissingParentAndInsidePackagePaths)
{
    SetEnvironment("relative-evidence.jsonl");
    EXPECT_THROW(Sink sink(package), std::runtime_error);
    SetEnvironment((root / "missing" / "evidence.jsonl").u8string());
    EXPECT_THROW(Sink sink(package), std::runtime_error);
    SetEnvironment((package / "evidence.jsonl").u8string());
    EXPECT_THROW(Sink sink(package), std::runtime_error);
    EXPECT_THROW(Sink sink({}), std::runtime_error);
    EXPECT_TRUE(std::filesystem::is_empty(package));
}

TEST_F(RuntimeEvidenceTest, ExistingEvidenceIsNeverTruncated)
{
    {
        std::ofstream stream(output);
        stream << "existing evidence\n";
    }
    Sink sink(package);
    EXPECT_THROW(sink.Record("event", {}), std::runtime_error);
    EXPECT_TRUE(sink.GetStatistics().incomplete);
    EXPECT_THROW(sink.Flush(), std::runtime_error);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    std::ifstream stream(output);
    std::string line;
    std::getline(stream, line);
    EXPECT_EQ(line, "existing evidence");
}

TEST_F(RuntimeEvidenceTest, ReadOnlyPrivateRootFailsWithoutSilentDrop)
{
    const auto directory = root / "readonly";
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    SetEnvironment((directory / "evidence.jsonl").u8string());
#if defined(_WIN32)
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(D;;0x2;;;WD)(A;;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr));
    const bool protectedRoot = SetFileSecurityW(directory.c_str(),
        DACL_SECURITY_INFORMATION, descriptor) != FALSE;
    LocalFree(descriptor);
    ASSERT_TRUE(protectedRoot);
#else
    ASSERT_EQ(chmod(directory.c_str(), 0500), 0);
#endif
    {
        Sink sink(package);
        EXPECT_THROW(sink.Record("event", {{"value", 1}}), std::runtime_error);
        EXPECT_TRUE(sink.GetStatistics().incomplete);
        EXPECT_THROW(sink.Close(), std::runtime_error);
    }
#if defined(_WIN32)
    descriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr));
    EXPECT_TRUE(SetFileSecurityW(directory.c_str(), DACL_SECURITY_INFORMATION, descriptor));
    LocalFree(descriptor);
#else
    EXPECT_EQ(chmod(directory.c_str(), 0700), 0);
#endif
    EXPECT_FALSE(std::filesystem::exists(directory / "evidence.jsonl"));
}

TEST_F(RuntimeEvidenceTest, RecordCapReservesIncompleteTerminalAndRejectsLaterWrites)
{
    Sink sink(package, {128 * 1024, 3, Sink::LineCapacity});
    EXPECT_EQ(sink.Record("event", {{"value", 1}}), Status::Recorded);
    EXPECT_EQ(sink.Record("event", {{"value", 2}}), Status::Recorded);
    EXPECT_THROW(sink.Record("event", {{"value", 3}}), std::runtime_error);
    EXPECT_THROW(sink.Record("event", {{"value", 4}}), std::runtime_error);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    const auto records = Read();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_TRUE(records.back().at("payload").at("incomplete").get<bool>());
    EXPECT_TRUE(sink.GetStatistics().closed);
    EXPECT_TRUE(HasIncompleteSession());
}

TEST_F(RuntimeEvidenceTest, ByteCapDoesNotWritePartialRecordAndRetainsTerminal)
{
    Sink sink(package, {4096, 100, Sink::LineCapacity});
    EXPECT_EQ(sink.Record("event", {{"text", std::string(1600, 'a')}}), Status::Recorded);
    const auto before = std::filesystem::file_size(output);
    EXPECT_THROW(sink.Record("event", {{"text", std::string(1600, 'b')}}), std::runtime_error);
    EXPECT_EQ(std::filesystem::file_size(output), before);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    const auto records = Read();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_TRUE(records.back().at("payload").at("incomplete").get<bool>());
    EXPECT_LE(std::filesystem::file_size(output), 4096u);
}

TEST_F(RuntimeEvidenceTest, LineLimitRejectsEscapedOversizeWithoutTruncation)
{
    Sink sink(package);
    EXPECT_EQ(sink.Record("valid", {{"text", std::string(63000, 'a')}}), Status::Recorded);
    const auto before = std::filesystem::file_size(output);
    EXPECT_THROW(sink.Record("oversize", {{"text", std::string(40000, '\n')}}), std::runtime_error);
    EXPECT_EQ(std::filesystem::file_size(output), before);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    const auto records = Read();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_EQ(records[0].at("payload").at("text").get<std::string>().size(), 63000u);
    EXPECT_TRUE(records.back().at("payload").at("incomplete").get<bool>());
}

TEST_F(RuntimeEvidenceTest, InvalidUtf8AndNonFinitePayloadFailClosed)
{
    {
        Sink sink(package);
        EXPECT_THROW(sink.Record("badUtf8", {{"text", std::string(1, char(0xff))}}), std::exception);
        EXPECT_TRUE(sink.GetStatistics().incomplete);
        EXPECT_THROW(sink.Close(), std::runtime_error);
    }
    EXPECT_TRUE(Read().back().at("payload").at("incomplete").get<bool>());
    output = root / "nonfinite.jsonl";
    SetEnvironment(output.u8string());
    Sink sink(package);
    EXPECT_THROW(sink.Record("nonfinite", std::numeric_limits<double>::infinity()), std::runtime_error);
    EXPECT_TRUE(sink.GetStatistics().incomplete);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    EXPECT_TRUE(Read().back().at("payload").at("incomplete").get<bool>());
}

TEST_F(RuntimeEvidenceTest, ExplicitCloseIsIdempotentAndRejectsLateRecords)
{
    Sink sink(package);
    EXPECT_EQ(sink.Record("event", Json::object()), Status::Recorded);
    EXPECT_EQ(sink.Close(), Status::Recorded);
    const auto before = std::filesystem::file_size(output);
    EXPECT_EQ(sink.Close(), Status::Recorded);
    EXPECT_THROW(sink.Record("late", {}), std::runtime_error);
    EXPECT_EQ(std::filesystem::file_size(output), before);
}

TEST_F(RuntimeEvidenceTest, GlobalApiRequiresProcessInitializationAndOneProcessOwner)
{
    RecordProperty("processCpuBytes", std::to_string(CostQuote().cpuBytes));
    RecordProperty("processFixedMetadataBytes", std::to_string(CostQuote().fixedMetadataBytes));
    RecordProperty("processWorkspaceBytes", std::to_string(CostQuote().workspaceBytes));
    SetEnvironment("");
    EXPECT_EQ(Initialize({}), Status::Disabled);
    EXPECT_FALSE(IsEnabled());
    EXPECT_EQ(Record("off", {}), Status::Disabled);
    EXPECT_EQ(Flush(), Status::Disabled);
    EXPECT_EQ(Close(), Status::Disabled);
    EXPECT_FALSE(std::filesystem::exists(output));
    SetEnvironment(output.u8string());
    EXPECT_EQ(Close(), Status::Disabled);
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_THROW(Record("beforeRootResolved", {}), std::runtime_error);
    ASSERT_EQ(Initialize(package), Status::Recorded);
    EXPECT_TRUE(IsEnabled());
    EXPECT_THROW(Initialize(package), std::runtime_error);
    EXPECT_EQ(Record("worldOne", {{"frameEpoch", 11}}), Status::Recorded);
    EXPECT_EQ(Record("worldTwo", {{"frameEpoch", 23}}), Status::Recorded);
    EXPECT_EQ(Flush(), Status::Recorded);
    EXPECT_EQ(Close(), Status::Recorded);
    EXPECT_EQ(GetStatistics().records, 3u);
    EXPECT_GT(CostQuote().fixedMetadataBytes, Sink::CostQuote().fixedMetadataBytes);
    EXPECT_EQ(CostQuote().cpuBytes, CostQuote().fixedMetadataBytes + CostQuote().workspaceBytes);
    EXPECT_EQ(CostQuote().gpuBytes, 0u);
    const auto records = Read();
    ASSERT_EQ(records.size(), 3u);
    EXPECT_EQ(records[0].at("payload").at("frameEpoch"), 11);
    EXPECT_EQ(records[1].at("payload").at("frameEpoch"), 23);
}

TEST_F(RuntimeEvidenceTest, ApplicationScopeClosesSuccessfulReturn)
{
    Sink sink(package);
    {
        ApplicationScope application(&sink);
        ASSERT_EQ(sink.Record("native", {{"ready", false}}), Status::Recorded);
        EXPECT_EQ(application.Finish(0), 0);
    }
    const auto records = Read();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_FALSE(records.back().at("payload").at("incomplete").get<bool>());
    EXPECT_TRUE(sink.GetStatistics().closed);
}

TEST_F(RuntimeEvidenceTest, ApplicationScopeClosesNonzeroEarlyReturnAsIncomplete)
{
    Sink sink(package);
    {
        ApplicationScope application(&sink);
        ASSERT_EQ(sink.Record("native", {}), Status::Recorded);
        EXPECT_EQ(application.Finish(17), 17);
    }
    const auto records = Read();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_TRUE(records.back().at("payload").at("incomplete").get<bool>());
    EXPECT_TRUE(sink.GetStatistics().closed);
}

TEST_F(RuntimeEvidenceTest, ApplicationScopeClosesDuringExceptionUnwinding)
{
    Sink sink(package);
    EXPECT_THROW({
        ApplicationScope application(&sink);
        sink.Record("native", {{"submissionEpoch", 123}});
        throw std::runtime_error("application failure");
    }, std::runtime_error);
    const auto records = Read();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_TRUE(records.back().at("payload").at("incomplete").get<bool>());
    EXPECT_TRUE(sink.GetStatistics().closed);
}

TEST_F(RuntimeEvidenceTest, ApplicationScopeClosesDuringNonstandardException)
{
    Sink sink(package);
    EXPECT_THROW({
        ApplicationScope application(&sink);
        sink.Record("native", {});
        throw 7;
    }, int);
    EXPECT_TRUE(Read().back().at("payload").at("incomplete").get<bool>());
    EXPECT_TRUE(sink.GetStatistics().closed);
}

TEST_F(RuntimeEvidenceTest, ApplicationScopeTurnsSwallowedSinkErrorIntoFailure)
{
    Sink sink(package, {128 * 1024, 2, Sink::LineCapacity});
    ApplicationScope application(&sink);
    ASSERT_EQ(sink.Record("native", {}), Status::Recorded);
    EXPECT_THROW(sink.Record("overCap", {}), std::runtime_error);
    EXPECT_EQ(application.Finish(0), 1);
    EXPECT_TRUE(Read().back().at("payload").at("incomplete").get<bool>());
}

TEST_F(RuntimeEvidenceTest, ApplicationScopeDisabledDoesNotCreateEvidence)
{
    SetEnvironment("");
    Sink sink({});
    {
        ApplicationScope application(&sink);
        EXPECT_EQ(application.Finish(0), 0);
    }
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(sink.GetStatistics().records, 0u);
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleClosesLastSubmittedCaptureBeforeFooter)
{
    Sink sink(package);
    ApplicationScope application(&sink);
    TerrainCompositedCaptureLifecycle lifecycle;
    auto emit = [&](Json event) { sink.Record("RVT_QUALITY_EVENT", event); };
    for (uint64_t index = 1; index <= 8; ++index)
    {
        lifecycle.Recorded(index, 1, index, index * 40, index * 40 + 1);
        lifecycle.Submitted(index, index * 40 + 1, emit);
        lifecycle.Completed(index, 1, index, index * 40, index * 40 + 1, index * 40 + 2, emit);
    }
    lifecycle.Recorded(9, 1, 9, 427, 428);
    lifecycle.Submitted(9, 428, emit);
    EXPECT_TRUE(lifecycle.Close("shutdown", emit));
    EXPECT_EQ(application.Finish(0), 0);
    const auto records = Read();
    ASSERT_EQ(records.size(), 19u);
    size_t completed = 0;
    for (const auto& record : records)
        if (record.at("payload").contains("event") &&
            record.at("payload").at("event") == "capture_completed")
            ++completed;
    EXPECT_EQ(completed, 8u);
    const auto& aborted = records[17].at("payload");
    EXPECT_EQ(aborted.at("event"), "capture_aborted");
    EXPECT_EQ(aborted.at("reason"), "shutdown");
    EXPECT_EQ(aborted.at("worldGeneration"), 1);
    EXPECT_EQ(aborted.at("poseId"), 9);
    EXPECT_EQ(aborted.at("frame"), 427);
    EXPECT_EQ(aborted.at("submissionEpoch"), 428);
    EXPECT_EQ(aborted.at("captureHandle"), 9);
    EXPECT_EQ(aborted.at("measurementProvided"), false);
    EXPECT_EQ(aborted.at("submitted"), true);
    EXPECT_FALSE(aborted.contains("completedEpoch"));
    EXPECT_EQ(records.back().at("eventClass"), "runtimeEvidenceEnd");
    EXPECT_EQ(records.back().at("payload").at("incomplete"), false);
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleEmptyOrConsumedOwnerEmitsNoClosure)
{
    TerrainCompositedCaptureLifecycle lifecycle;
    std::vector<Json> events;
    auto emit = [&](Json event) { events.push_back(std::move(event)); };
    EXPECT_FALSE(lifecycle.Close("shutdown", emit));
    lifecycle.Recorded(11, 7, 2, 0, 1);
    lifecycle.Submitted(11, 1, emit);
    lifecycle.Completed(11, 7, 2, 0, 1, 1, emit);
    EXPECT_FALSE(lifecycle.Close("shutdown", emit));
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].at("event"), "capture_completed");
    EXPECT_EQ(events[1].at("measurementProvided"), true);
    EXPECT_EQ(events[1].at("completedEpoch"), 1);
    EXPECT_FALSE(lifecycle.Pending());
    EXPECT_LE(sizeof(lifecycle), 128u);
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleUnsubmittedAbortPreservesRecordedIdentity)
{
    TerrainCompositedCaptureLifecycle lifecycle;
    std::vector<Json> events;
    lifecycle.Recorded(12, 3, 4, 100, 101);
    EXPECT_TRUE(lifecycle.Close("frame_aborted",
        [&](Json event) { events.push_back(std::move(event)); }));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].at("captureHandle"), 12);
    EXPECT_EQ(events[0].at("worldGeneration"), 3);
    EXPECT_EQ(events[0].at("poseId"), 4);
    EXPECT_EQ(events[0].at("frame"), 100);
    EXPECT_EQ(events[0].at("drawSubmissionEpoch"), 101);
    EXPECT_EQ(events[0].at("submissionEpoch"), 0);
    EXPECT_EQ(events[0].at("submitted"), false);
    EXPECT_EQ(events[0].at("measurementProvided"), false);
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleWorldReplacementCannotCompleteOldIdentity)
{
    TerrainCompositedCaptureLifecycle lifecycle;
    std::vector<Json> events;
    auto emit = [&](Json event) { events.push_back(std::move(event)); };
    lifecycle.Recorded(1, 1, 9, 427, 428);
    lifecycle.Submitted(1, 428, emit);
    EXPECT_TRUE(lifecycle.Close("world_replaced", emit));
    lifecycle.Recorded(2, 2, 1, 431, 432);
    lifecycle.Submitted(2, 432, emit);
    EXPECT_THROW(lifecycle.Completed(1, 1, 9, 427, 428, 440, emit), std::runtime_error);
    EXPECT_EQ(lifecycle.Pending()->world, 2u);
    lifecycle.Completed(2, 2, 1, 431, 432, 440, emit);
    ASSERT_EQ(events.size(), 4u);
    EXPECT_EQ(events[1].at("reason"), "world_replaced");
    EXPECT_EQ(events[1].at("measurementProvided"), false);
    EXPECT_EQ(events[3].at("worldGeneration"), 2);
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleSwapchainClosureIsSingleAndHasNoMeasurement)
{
    TerrainCompositedCaptureLifecycle lifecycle;
    std::vector<Json> events;
    auto emit = [&](Json event) { events.push_back(std::move(event)); };
    lifecycle.Recorded(3, 1, 7, 58, 59);
    lifecycle.Submitted(3, 59, emit);
    EXPECT_TRUE(lifecycle.Close("swapchain_teardown", emit));
    EXPECT_FALSE(lifecycle.Close("shutdown", emit));
    EXPECT_THROW(lifecycle.Completed(3, 1, 7, 58, 59, 60, emit), std::runtime_error);
    ASSERT_EQ(events.size(), 2u);
    EXPECT_EQ(events[1].at("reason"), "swapchain_teardown");
    EXPECT_EQ(events[1].at("measurementProvided"), false);
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleRejectsUnknownDuplicateAndUnfencedIdentities)
{
    TerrainCompositedCaptureLifecycle lifecycle;
    size_t emitted = 0;
    auto emit = [&](Json) { ++emitted; };
    EXPECT_THROW(lifecycle.Submitted(1, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(1, 1, 1, 1, 2, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Recorded(0, 1, 1, 1, 2), std::runtime_error);
    EXPECT_THROW(lifecycle.Recorded(1, 0, 1, 1, 2), std::runtime_error);
    EXPECT_THROW(lifecycle.Recorded(1, 1, 0, 1, 2), std::runtime_error);
    EXPECT_THROW(lifecycle.Recorded(1, 1, 1, 1, 0), std::runtime_error);
    lifecycle.Recorded(1, 1, 1, 1, 2);
    EXPECT_THROW(lifecycle.Recorded(2, 2, 2, 2, 3), std::runtime_error);
    EXPECT_THROW(lifecycle.Submitted(2, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Submitted(1, 3, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(1, 1, 1, 1, 2, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Close("", emit), std::invalid_argument);
    EXPECT_EQ(emitted, 0u);
    lifecycle.Submitted(1, 2, emit);
    EXPECT_THROW(lifecycle.Submitted(1, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(2, 1, 1, 1, 2, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(1, 2, 1, 1, 2, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(1, 1, 2, 1, 2, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(1, 1, 1, 2, 2, 2, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(1, 1, 1, 1, 3, 3, emit), std::runtime_error);
    EXPECT_THROW(lifecycle.Completed(1, 1, 1, 1, 2, 1, emit), std::runtime_error);
    EXPECT_EQ(emitted, 1u);
    EXPECT_TRUE(lifecycle.Pending());
    EXPECT_TRUE(lifecycle.Close("shutdown", emit));
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleEmissionFailureCannotLosePendingIdentity)
{
    TerrainCompositedCaptureLifecycle lifecycle;
    lifecycle.Recorded(5, 1, 9, 427, 428);
    auto failure = [](Json) { throw std::runtime_error("evidence failure"); };
    EXPECT_THROW(lifecycle.Submitted(5, 428, failure), std::runtime_error);
    ASSERT_TRUE(lifecycle.Pending());
    EXPECT_EQ(lifecycle.Pending()->submissionEpoch, 428u);
    EXPECT_THROW(lifecycle.Completed(5, 1, 9, 427, 428, 429, failure), std::runtime_error);
    EXPECT_TRUE(lifecycle.Pending());
    EXPECT_THROW(lifecycle.Close("shutdown", failure), std::runtime_error);
    EXPECT_TRUE(lifecycle.Pending());
    std::vector<Json> events;
    EXPECT_TRUE(lifecycle.Close("shutdown", [&](Json event) { events.push_back(std::move(event)); }));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(events[0].at("event"), "capture_aborted");
    EXPECT_EQ(events[0].at("measurementProvided"), false);
    EXPECT_FALSE(lifecycle.Pending());
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleClosureWriteFailureForcesIncompleteFooter)
{
    Sink sink(package, {128 * 1024, 2, Sink::LineCapacity});
    ApplicationScope application(&sink);
    TerrainCompositedCaptureLifecycle lifecycle;
    auto emit = [&](Json event) { sink.Record("RVT_QUALITY_EVENT", event); };
    lifecycle.Recorded(9, 1, 9, 427, 428);
    lifecycle.Submitted(9, 428, emit);
    EXPECT_THROW(lifecycle.Close("shutdown", emit), std::runtime_error);
    EXPECT_TRUE(lifecycle.Pending());
    EXPECT_EQ(application.Finish(0), 1);
    const auto records = Read();
    ASSERT_EQ(records.size(), 2u);
    EXPECT_TRUE(records.back().at("payload").at("incomplete").get<bool>());
}

TEST_F(RuntimeEvidenceTest, CaptureLifecycleDisabledSinkCreatesNoEvidence)
{
    SetEnvironment("");
    Sink sink(package);
    TerrainCompositedCaptureLifecycle lifecycle;
    auto emit = [&](Json event) { EXPECT_EQ(sink.Record("RVT_QUALITY_EVENT", event), Status::Disabled); };
    lifecycle.Recorded(9, 1, 9, 427, 428);
    lifecycle.Submitted(9, 428, emit);
    EXPECT_TRUE(lifecycle.Close("shutdown", emit));
    EXPECT_FALSE(std::filesystem::exists(output));
    EXPECT_EQ(sink.GetStatistics().records, 0u);
}

#if defined(VL_FILE_IO_TEST_HOOKS)
TEST_F(RuntimeEvidenceTest, ActualFileWriteErrorSetsStickyIncomplete)
{
    Sink sink(package);
    ASSERT_EQ(sink.Record("first", {}), Status::Recorded);
    TestAccess::ReplaceWithReadOnlyFile(sink, output);
    EXPECT_THROW(sink.Record("writeFailure", {}), std::runtime_error);
    EXPECT_TRUE(sink.GetStatistics().incomplete);
    EXPECT_THROW(sink.Flush(), std::runtime_error);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    EXPECT_EQ(Read().size(), 1u);
}

TEST_F(RuntimeEvidenceTest, TerminalFileErrorCannotReportSuccessfulClose)
{
    Sink sink(package);
    ASSERT_EQ(sink.Record("first", {}), Status::Recorded);
    TestAccess::ReplaceWithReadOnlyFile(sink, output);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    EXPECT_TRUE(sink.GetStatistics().incomplete);
    EXPECT_TRUE(sink.GetStatistics().closed);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    EXPECT_EQ(Read().size(), 1u);
}

TEST_F(RuntimeEvidenceTest, ActualFlushFailureSetsStickyIncomplete)
{
    Sink sink(package);
    ASSERT_EQ(sink.Record("first", {}), Status::Recorded);
    TestAccess::BufferOnReadOnlyDescriptor(sink, output);
    EXPECT_THROW(sink.Flush(), std::runtime_error);
    EXPECT_TRUE(sink.GetStatistics().incomplete);
    EXPECT_THROW(sink.Close(), std::runtime_error);
    EXPECT_TRUE(sink.GetStatistics().closed);
    EXPECT_EQ(Read().size(), 1u);
}

TEST_F(RuntimeEvidenceTest, ApplicationScopeCloseFailureCannotReturnSuccess)
{
    Sink sink(package);
    ApplicationScope application(&sink);
    ASSERT_EQ(sink.Record("native", {}), Status::Recorded);
    TestAccess::ReplaceWithReadOnlyFile(sink, output);
    EXPECT_EQ(application.Finish(0), 1);
    EXPECT_TRUE(sink.GetStatistics().incomplete);
    EXPECT_TRUE(sink.GetStatistics().closed);
}
#endif
}
}
}
