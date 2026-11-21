#include "shader/build/atomicFile.h"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <stdexcept>

#include <gtest/gtest.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace VL::AtomicFileTesting
{
extern thread_local std::function<void(const std::filesystem::path&)> BeforeCommit;
extern thread_local std::function<void(size_t)> AfterWrite;
}

namespace
{
uint64_t ProcessId()
{
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void Seed(const std::filesystem::path& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    Check(static_cast<bool>(output), "Failed to seed fixture");
}

std::string Read(const std::filesystem::path& path)
{
    const auto bytes = VL::ReadBinaryFile(path);
    return {bytes.begin(), bytes.end()};
}

std::filesystem::path TemporaryPath(const std::filesystem::path& root, uint64_t sequence)
{
    return root / (".vl-atomic.tmp." + std::to_string(ProcessId()) + "." +
        std::to_string(sequence));
}

void Reject(const std::function<void()>& action)
{
    bool rejected = false;
    try
    {
        action();
    }
    catch (const std::runtime_error&)
    {
        rejected = true;
    }
    Check(rejected, "Atomic write unexpectedly succeeded");
}

void NoTemporaryFiles(const std::filesystem::path& root)
{
    for (const auto& entry : std::filesystem::directory_iterator(root))
        Check(entry.path().filename().string().rfind(".vl-atomic.tmp.", 0) != 0,
            "Owned temporary file leaked");
}

void InFreshProcess(const std::function<void(const std::filesystem::path&)>& scenario)
{
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    EXPECT_EXIT(
        {
            const auto root = std::filesystem::temp_directory_path() /
                ("vl-atomic-exclusive-" + std::to_string(ProcessId()));
            bool owned = false;
            try
            {
                owned = std::filesystem::create_directory(root);
                Check(owned, "Fixture already exists");
                scenario(root);
                std::filesystem::remove_all(root);
                std::exit(0);
            }
            catch (const std::exception& error)
            {
                std::cerr << error.what();
                std::error_code ignored;
                if (owned)
                    std::filesystem::remove_all(root, ignored);
                std::exit(1);
            }
        },
        ::testing::ExitedWithCode(0), "");
}

void PrecreateHardlink(const std::filesystem::path& root, uint64_t sequence)
{
    const auto input = root / "previous-input.bin";
    if (!std::filesystem::exists(input))
        Seed(input, "previous immutable input");
    std::filesystem::create_hard_link(input, TemporaryPath(root, sequence));
    Check(std::filesystem::hard_link_count(input) >= 2, "Hardlink was not created");
}

void CheckHardlink(const std::filesystem::path& root, uint64_t sequence)
{
    Check(Read(root / "previous-input.bin") == "previous immutable input", "Old input damaged");
    Check(std::filesystem::equivalent(root / "previous-input.bin", TemporaryPath(root, sequence)),
        "Pre-existing hardlink removed");
}

TEST(AtomicFileExclusive, TextRejectsPrecreatedHardlink)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "world.json", "previous manifest");
        PrecreateHardlink(root, 0);
        Reject([&] { VL::WriteTextFileAtomically(root / "world.json", "new manifest"); });
        Check(Read(root / "world.json") == "previous manifest", "Old manifest damaged");
        CheckHardlink(root, 0);
    });
}

TEST(AtomicFileExclusive, BinaryRejectsPrecreatedHardlink)
{
    InFreshProcess([](const auto& root) {
        PrecreateHardlink(root, 0);
        Reject([&] { VL::WriteBinaryFileAtomically(root / "output.bin", std::vector<uint8_t>{1, 0, 2}); });
        Check(!std::filesystem::exists(root / "output.bin"), "Unexpected output");
        CheckHardlink(root, 0);
    });
}

TEST(AtomicFileExclusive, ChangedTextRejectsPrecreatedHardlink)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "world.json", "previous manifest");
        PrecreateHardlink(root, 0);
        Reject([&] { VL::WriteTextFileIfChangedAtomically(root / "world.json", "new manifest"); });
        Check(Read(root / "world.json") == "previous manifest", "Old manifest damaged");
        CheckHardlink(root, 0);
    });
}

TEST(AtomicFileExclusive, BatchStageFailureCleansOnlyOwnedTemporaryFiles)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "first", "old first");
        Seed(root / "second", "old second");
        PrecreateHardlink(root, 1);
        Reject([&] { VL::WriteFileBatchAtomically({
            {root / "first", {1, 2}}, {root / "second", {3, 4}}}); });
        Check(Read(root / "first") == "old first", "First destination changed before staging completed");
        Check(Read(root / "second") == "old second", "Second destination damaged");
        Check(!std::filesystem::exists(TemporaryPath(root, 0)), "First staged file leaked");
        CheckHardlink(root, 1);
    });
}

TEST(AtomicFileExclusive, ExistingTemporaryDirectoryIsRetained)
{
    InFreshProcess([](const auto& root) {
        const auto temporary = TemporaryPath(root, 0);
        std::filesystem::create_directory(temporary);
        Seed(temporary / "sentinel", "keep");
        Reject([&] { VL::WriteTextFileAtomically(root / "world.json", "new"); });
        Check(Read(temporary / "sentinel") == "keep", "Pre-existing directory changed");
    });
}

TEST(AtomicFileExclusive, NormalBinaryTextAndBatchWritesLeaveNoTemporaryFiles)
{
    InFreshProcess([](const auto& root) {
        const std::vector<uint8_t> binary{0, 1, 255, 0, 42};
        VL::WriteBinaryFileAtomically(root / "binary", binary);
        Check(VL::ReadBinaryFile(root / "binary") == binary, "Binary content mismatch");
        VL::WriteBinaryFileAtomically(root / "empty", nullptr, 0);
        Check(VL::ReadBinaryFile(root / "empty").empty(), "Empty content mismatch");
        VL::WriteTextFileAtomically(root / "text", "old");
        Check(VL::WriteTextFileIfChangedAtomically(root / "text", "new"), "Changed write ignored");
        Check(!VL::WriteTextFileIfChangedAtomically(root / "text", "new"), "Identical write changed");
        VL::WriteFileBatchAtomically({{root / "text", {'o', 'k'}}, {root / "other", {0, 255}}});
        Check(Read(root / "text") == "ok", "Batch text mismatch");
        Check(VL::ReadBinaryFile(root / "other") == std::vector<uint8_t>({0, 255}), "Batch binary mismatch");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, BinaryWriteSpansMultipleNativeWriteChunks)
{
    InFreshProcess([](const auto& root) {
        std::vector<uint8_t> bytes(1024 * 1024 + 17, 0xa5);
        bytes.front() = 0;
        bytes.back() = 0xff;
        VL::WriteBinaryFileAtomically(root / "binary", bytes.data(), bytes.size());
        Check(VL::ReadBinaryFile(root / "binary") == bytes, "Multi-chunk binary content mismatch");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, IncompleteNativeWritePreservesDestinationAndReleasesResources)
{
    InFreshProcess([](const auto& root) {
        const auto target = root / "binary";
        Seed(target, "previous");
        size_t written = 0;
        VL::AtomicFileTesting::AfterWrite = [&](size_t offset) {
            written = offset;
            throw std::runtime_error("injected incomplete write");
        };
        Reject([&] { VL::WriteBinaryFileAtomically(target,
            std::vector<uint8_t>(1024 * 1024 + 17, 42)); });
        VL::AtomicFileTesting::AfterWrite = {};
        Check(written == 1024 * 1024, "Expected a partial native write");
        Check(Read(target) == "previous", "Incomplete write replaced destination");
        NoTemporaryFiles(root);
        VL::WriteTextFileAtomically(target, "retry");
        Check(Read(target) == "retry", "Failure retained a resource lock");
    });
}

TEST(AtomicFileExclusive, ExistingRegularTemporaryFileIsRetained)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "world.json", "previous");
        Seed(TemporaryPath(root, 0), "pre-existing temporary file");
        Reject([&] { VL::WriteTextFileAtomically(root / "world.json", "new"); });
        Check(Read(root / "world.json") == "previous", "Destination changed");
        Check(Read(TemporaryPath(root, 0)) == "pre-existing temporary file", "Collision file changed");
    });
}

TEST(AtomicFileExclusive, DirectoryInputsAndDestinationsAreRejected)
{
    InFreshProcess([](const auto& root) {
        const auto directory = root / "directory";
        std::filesystem::create_directory(directory);
        Seed(directory / "sentinel", "keep");
        Reject([&] { VL::ReadBinaryFile(directory); });
        Reject([&] { VL::RootedFileReader reader(root, "directory"); });
        Reject([&] { VL::WriteTextFileAtomically(directory, "new"); });
        Check(Read(directory / "sentinel") == "keep", "Directory content changed");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, DestinationChangeDuringStagingIsRejected)
{
    InFreshProcess([](const auto& root) {
        const auto target = root / "world.json";
        Seed(target, "previous");
        VL::AtomicFileTesting::BeforeCommit = [&](const auto&) { Seed(target, "concurrent update"); };
        Reject([&] { VL::WriteTextFileAtomically(target, "stale replacement"); });
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(Read(target) == "concurrent update", "Concurrent version overwritten");
        NoTemporaryFiles(root);
        VL::WriteTextFileAtomically(target, "retry");
        Check(Read(target) == "retry", "Failure retained a resource lock");
    });
}

TEST(AtomicFileExclusive, SameContentDestinationReplacementIsRejectedByIdentity)
{
    InFreshProcess([](const auto& root) {
        const auto target = root / "world.json";
        Seed(target, "previous");
        Seed(root / "replacement", "previous");
        VL::AtomicFileTesting::BeforeCommit = [&](const auto&) {
            Check(std::filesystem::remove(target), "Failed to remove owned fixture");
            std::filesystem::rename(root / "replacement", target);
        };
        Reject([&] { VL::WriteTextFileAtomically(target, "stale replacement"); });
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(Read(target) == "previous", "Concurrent file overwritten");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, NewlyAppearingDestinationIsRetained)
{
    InFreshProcess([](const auto& root) {
        const auto target = root / "world.json";
        VL::AtomicFileTesting::BeforeCommit = [&](const auto&) { Seed(target, "concurrent"); };
        Reject([&] { VL::WriteTextFileAtomically(target, "stale replacement"); });
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(Read(target) == "concurrent", "Newly appearing destination overwritten");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, RootedReaderSeekBoundsEofAndRelease)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "input", "abcdef");
        {
            VL::RootedFileReader input(root, "input");
            input.Seek(3);
            std::array<char, 8> bytes{};
            Check(input.Read(bytes.data(), 3) == 3 && std::string(bytes.data(), 3) == "def",
                "Seek content mismatch");
            Check(input.Read(bytes.data(), 1) == 0, "Expected EOF");
            input.Seek(0);
            Check(input.Read(bytes.data(), 1) == 1 && bytes[0] == 'a', "Rewind mismatch");
            Reject([&] { input.Seek(7); });
            Reject([&] { input.Read(nullptr, 1); });
            Check(input.Read(nullptr, 0) == 0, "Empty read failed");
            input.ValidateIdentity();
        }
        VL::WriteTextFileAtomically(root / "input", "released");
        Check(Read(root / "input") == "released", "Reader leaked a file lock");
    });
}

#if defined(_WIN32)
TEST(AtomicFileExclusive, WindowsRepeatedReadAndFailurePathsReleaseNativeHandles)
{
    InFreshProcess([](const auto& root) {
        const auto target = root / "world.json";
        Seed(target, "previous");
        const auto exercise = [&] {
            Check(Read(target) == "previous", "Read mismatch");
            Reject([&] { VL::RootedFileReader input(root, "missing"); });
            VL::AtomicFileTesting::AfterWrite = [](size_t) {
                throw std::runtime_error("injected write failure");
            };
            Reject([&] { VL::WriteTextFileAtomically(target, "unfinished"); });
            VL::AtomicFileTesting::AfterWrite = {};
        };
        exercise();
        DWORD before = 0;
        Check(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "Handle count unavailable");
        for (size_t iteration = 0; iteration < 32; ++iteration)
            exercise();
        NoTemporaryFiles(root);
        DWORD after = 0;
        Check(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "Handle count unavailable");
        std::cout << "Native handles after warmup: before=" << before << " after=" << after << '\n';
        Check(after == before, "Native handles leaked during repeated reads/failures");
        VL::WriteTextFileAtomically(target, "retry");
        Check(Read(target) == "retry", "Failure retained a lock");
    });
}

TEST(AtomicFileExclusive, WindowsDriveUnicodeAndExplicitExtendedPrefixRoundTrip)
{
    InFreshProcess([](const auto& root) {
        const auto directory = root / std::filesystem::path(L"\u8def\u5f84-\u6750\u8d28");
        std::filesystem::create_directory(directory);
        const auto target = directory / std::filesystem::path(L"\u6e05\u5355.json");
        bool observed = false;
        VL::AtomicFileTesting::BeforeCommit = [&](const auto& path) {
            observed = true;
            Check(path.native().compare(0, 4, L"\\\\?\\") == 0,
                "Native extended prefix lost a leading slash");
        };
        VL::WriteTextFileAtomically(target, "drive-unicode");
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(observed, "Native path cutpoint not reached");
        Check(Read(target) == "drive-unicode", "Unicode write mismatch");
        const auto extended = std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(directory).native());
        VL::WriteTextFileAtomically(extended / target.filename(), "extended-unicode");
        VL::RootedFileReader reader(extended, target.filename());
        std::array<char, 32> bytes{};
        const auto count = reader.Read(bytes.data(), bytes.size());
        Check(std::string(bytes.data(), count) == "extended-unicode", "Extended rooted read mismatch");
        Check(reader.Size() == count && reader.Read(bytes.data(), 1) == 0, "Rooted size/EOF mismatch");
        reader.ValidateIdentity();
    });
}

TEST(AtomicFileExclusive, WindowsExtendedDirectoryBeyondMaxPathRoundTrip)
{
    InFreshProcess([](const auto& root) {
        std::wstring native = L"\\\\?\\" + std::filesystem::absolute(root).native();
        std::vector<std::wstring> created;
        for (size_t index = 0; index < 5; ++index)
        {
            native += L"\\" + std::wstring(55, L'a') + std::to_wstring(index);
            Check(CreateDirectoryW(native.c_str(), nullptr) != 0, "Failed to create native long directory");
            created.push_back(native);
        }
        Check(native.size() > MAX_PATH, "Long directory fixture too short");
        const auto directory = std::filesystem::path(native);
        const auto target = directory / L"payload.json";
        VL::WriteTextFileAtomically(target, "long native path");
        VL::WriteTextFileAtomically(target, "replacement");
        Check(Read(target) == "replacement", "Long ReadBinaryFile mismatch");
        {
            VL::RootedFileReader reader(directory, L"payload.json");
            std::array<char, 32> bytes{};
            const auto count = reader.Read(bytes.data(), bytes.size());
            Check(std::string(bytes.data(), count) == "replacement", "Long path content mismatch");
            reader.ValidateIdentity();
        }
        Check(DeleteFileW(target.c_str()) != 0, "Failed to remove owned long file");
        for (auto entry = created.rbegin(); entry != created.rend(); ++entry)
            Check(RemoveDirectoryW(entry->c_str()) != 0, "Failed to remove owned long directory");
    });
}

TEST(AtomicFileExclusive, WindowsLocalUncAndExtendedUncRoundTrip)
{
    const auto localTemp = std::filesystem::absolute(std::filesystem::temp_directory_path()).native();
    if (localTemp.size() < 3 || localTemp[1] != L':')
        GTEST_SKIP() << "Local drive temp directory required for optional SMB fixture";
    const auto share = L"\\\\localhost\\" + localTemp.substr(0, 1) + L"$" + localTemp.substr(2);
    if (GetFileAttributesW(share.c_str()) == INVALID_FILE_ATTRIBUTES)
        GTEST_SKIP() << "Local administrative SMB share unavailable: " << GetLastError();
    InFreshProcess([](const auto& root) {
        const auto local = std::filesystem::absolute(root).native();
        const auto name = L"\\\\localhost\\" + local.substr(0, 1) + L"$" + local.substr(2);
        const auto unc = std::filesystem::path(name);
        bool observed = false;
        VL::AtomicFileTesting::BeforeCommit = [&](const auto& path) {
            observed = true;
            Check(path.native().compare(0, 8, L"\\\\?\\UNC\\") == 0, "UNC native prefix was corrupted");
        };
        VL::WriteTextFileAtomically(unc / L"network.json", "ordinary UNC");
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(observed, "UNC commit cutpoint not reached");
        const auto extended = std::filesystem::path(L"\\\\?\\UNC\\" + name.substr(2));
        VL::WriteTextFileAtomically(extended / L"network.json", "extended UNC");
        {
            VL::RootedFileReader reader(extended, L"network.json");
            std::array<char, 32> bytes{};
            const auto count = reader.Read(bytes.data(), bytes.size());
            Check(std::string(bytes.data(), count) == "extended UNC", "UNC read mismatch");
            reader.ValidateIdentity();
        }
        Check(Read(root / "network.json") == "extended UNC", "UNC and local file identities diverged");
    });
}

TEST(AtomicFileExclusive, WindowsHeldTempRejectsSubstitutionBeforeCommit)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "world.json", "old manifest");
        Seed(root / "foreign", "foreign payload");
        bool observed = false;
        bool removed = false;
        VL::AtomicFileTesting::BeforeCommit = [&](const auto& path) {
            observed = true;
            removed = DeleteFileW(path.c_str()) != 0;
            if (removed)
                Check(CreateHardLinkW(path.c_str(), (root / "foreign").c_str(), nullptr) != 0,
                    "Failed to install substitute after temp deletion");
        };
        VL::WriteTextFileAtomically(root / "world.json", "new manifest");
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(observed, "Commit cutpoint not reached");
        Check(Read(root / "world.json") == "new manifest", "Substitute was published");
        Check(!removed, "Temporary entry was replaceable before commit");
        Check(Read(root / "foreign") == "foreign payload", "Foreign payload damaged");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, WindowsHeldTempRejectsSubstitutionBeforeLockedCommitCleanup)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "world.json", "old manifest");
        Seed(root / "foreign", "foreign payload");
        const HANDLE locked = CreateFileW((root / "world.json").c_str(), GENERIC_READ,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        Check(locked != INVALID_HANDLE_VALUE, "Failed to lock destination");
        bool removed = false;
        std::filesystem::path temporary;
        VL::AtomicFileTesting::BeforeCommit = [&](const auto& path) {
            temporary = path;
            removed = DeleteFileW(path.c_str()) != 0;
            if (removed)
                Check(CreateHardLinkW(path.c_str(), (root / "foreign").c_str(), nullptr) != 0,
                    "Failed to install cleanup substitute");
        };
        Reject([&] { VL::WriteTextFileAtomically(root / "world.json", "new manifest"); });
        VL::AtomicFileTesting::BeforeCommit = {};
        CloseHandle(locked);
        if (removed)
            Check(std::filesystem::exists(temporary), "Failure cleanup deleted a substitute not owned by writer");
        Check(!removed, "Closed temp permitted substitution before failure cleanup");
        Check(!temporary.empty(), "Commit cutpoint not reached");
        Check(Read(root / "world.json") == "old manifest", "Locked destination changed");
        Check(Read(root / "foreign") == "foreign payload", "Foreign payload damaged");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, WindowsBatchKeepsEveryStagedHandleUntilPublication)
{
    InFreshProcess([](const auto& root) {
        size_t commits = 0;
        VL::AtomicFileTesting::BeforeCommit = [&](const auto&) {
            if (++commits == 1)
                Check(DeleteFileW(TemporaryPath(root, 1).c_str()) == 0,
                    "Later staged file was closed before publication");
        };
        VL::WriteFileBatchAtomically({
            {root / "first", {'o', 'n', 'e'}}, {root / "second", {'t', 'w', 'o'}}});
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(commits == 2, "Batch commit cutpoint count mismatch");
        Check(Read(root / "first") == "one" && Read(root / "second") == "two", "Batch content mismatch");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, WindowsPinnedParentRejectsNamespaceReplacementBeforeCommit)
{
    InFreshProcess([](const auto& root) {
        const auto moved = root.parent_path() / (root.filename().string() + "-moved");
        bool observed = false;
        VL::AtomicFileTesting::BeforeCommit = [&](const auto&) {
            observed = true;
            const bool replaced = MoveFileExW(root.c_str(), moved.c_str(), 0) != 0;
            if (replaced)
                Check(MoveFileExW(moved.c_str(), root.c_str(), 0) != 0, "Failed to restore test-owned parent");
            Check(!replaced, "Atomic destination parent was replaceable");
        };
        VL::WriteTextFileAtomically(root / "world.json", "correct parent");
        VL::AtomicFileTesting::BeforeCommit = {};
        Check(observed && Read(root / "world.json") == "correct parent", "Parent binding mismatch");
        NoTemporaryFiles(root);
    });
}

class ReplacementLock
{
public:
    explicit ReplacementLock(const std::filesystem::path& path)
        : handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
    {
        Check(handle != INVALID_HANDLE_VALUE, "Failed to lock manifest");
    }
    ~ReplacementLock() { CloseHandle(handle); }
    ReplacementLock(const ReplacementLock&) = delete;
    ReplacementLock& operator=(const ReplacementLock&) = delete;
private:
    HANDLE handle;
};

TEST(AtomicFileExclusive, WindowsHardlinkAndReplacementLockRetainManifestAndInput)
{
    InFreshProcess([](const auto& root) {
        const auto manifest = root / "world.json";
        Seed(manifest, "previous manifest");
        PrecreateHardlink(root, 0);
        ReplacementLock lock(manifest);
        Reject([&] { VL::WriteTextFileAtomically(manifest, "new manifest bytes"); });
        CheckHardlink(root, 0);
        Check(Read(manifest) == "previous manifest", "Old manifest damaged");
        Reject([&] { VL::WriteTextFileAtomically(manifest, "new manifest bytes"); });
        Check(!std::filesystem::exists(TemporaryPath(root, 1)), "Replacement failure leaked temp");
        CheckHardlink(root, 0);
        Check(Read(manifest) == "previous manifest", "Locked manifest damaged");
    });
}

TEST(AtomicFileExclusive, WindowsBatchReplacementFailureCleansAllStagedFiles)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "world.json", "previous manifest");
        Seed(root / "input.bin", "previous input");
        ReplacementLock lock(root / "world.json");
        Reject([&] { VL::WriteFileBatchAtomically({
            {root / "world.json", {'n', 'e', 'w'}}, {root / "input.bin", {1, 2}}}); });
        Check(Read(root / "world.json") == "previous manifest", "Old manifest damaged");
        Check(Read(root / "input.bin") == "previous input", "Old input damaged");
        NoTemporaryFiles(root);
    });
}

TEST(AtomicFileExclusive, WindowsLaterBatchFailureRetainsPublishedFileAndCleansRemainingTemps)
{
    InFreshProcess([](const auto& root) {
        Seed(root / "published.bin", "old");
        Seed(root / "world.json", "previous manifest");
        Seed(root / "pending.bin", "previous input");
        ReplacementLock lock(root / "world.json");
        Reject([&] { VL::WriteFileBatchAtomically({
            {root / "published.bin", {'n', 'e', 'w'}},
            {root / "world.json", {'n', 'e', 'w'}},
            {root / "pending.bin", {1, 2}}}); });
        Check(Read(root / "published.bin") == "new", "Published file incorrectly cleaned");
        Check(Read(root / "world.json") == "previous manifest", "Old manifest damaged");
        Check(Read(root / "pending.bin") == "previous input", "Pending input damaged");
        NoTemporaryFiles(root);
    });
}
#endif
}
