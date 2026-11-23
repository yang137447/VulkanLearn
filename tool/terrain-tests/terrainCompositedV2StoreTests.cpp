#include <gtest/gtest.h>
#include "terrainCompositedProductionFixture.h"

#include <atomic>
#include <chrono>
#include <fstream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace VL::TerrainCompositedVTStoreTesting
{
extern thread_local std::function<void(const std::filesystem::path&, bool)> StagingObserver;
extern thread_local std::function<void(const std::filesystem::path&, bool)> InputObserver;
}

namespace
{
using namespace VL;

class ObserveStaging
{
public:
    explicit ObserveStaging(std::function<void(const std::filesystem::path&, bool)> observer)
    {
        previous = std::move(TerrainCompositedVTStoreTesting::StagingObserver);
        TerrainCompositedVTStoreTesting::StagingObserver = std::move(observer);
    }
    ~ObserveStaging()
    {
        TerrainCompositedVTStoreTesting::StagingObserver = std::move(previous);
    }
    ObserveStaging(const ObserveStaging&) = delete;
    ObserveStaging& operator=(const ObserveStaging&) = delete;
private:
    std::function<void(const std::filesystem::path&, bool)> previous;
};

void NoStagingDirectories(const std::filesystem::path& root)
{
    for (const auto& entry : std::filesystem::directory_iterator(root))
        EXPECT_NE(entry.path().filename().string().find(".vlcvt-publish-"), 0u);
}

bool SameFileIdentity(const std::filesystem::path& first, const std::filesystem::path& second)
{
#ifdef _WIN32
    const HANDLE firstHandle = CreateFileW(first.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const HANDLE secondHandle = CreateFileW(second.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    BY_HANDLE_FILE_INFORMATION firstInfo{}, secondInfo{};
    const bool same = firstHandle != INVALID_HANDLE_VALUE && secondHandle != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(firstHandle, &firstInfo) &&
        GetFileInformationByHandle(secondHandle, &secondInfo) &&
        firstInfo.dwVolumeSerialNumber == secondInfo.dwVolumeSerialNumber &&
        firstInfo.nFileIndexHigh == secondInfo.nFileIndexHigh &&
        firstInfo.nFileIndexLow == secondInfo.nFileIndexLow;
    if (firstHandle != INVALID_HANDLE_VALUE)
        CloseHandle(firstHandle);
    if (secondHandle != INVALID_HANDLE_VALUE)
        CloseHandle(secondHandle);
    return same;
#else
    return std::filesystem::equivalent(first, second);
#endif
}

#ifdef _WIN32
bool RemoveFixtureTree(const std::filesystem::path& path)
{
    WIN32_FIND_DATAW entry{};
    const HANDLE search = FindFirstFileW((path / L"*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE)
    {
        const auto error = GetLastError();
        return error == ERROR_PATH_NOT_FOUND ||
            (error == ERROR_FILE_NOT_FOUND && RemoveDirectoryW(path.c_str()) != 0);
    }
    bool removed = true;
    do
    {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..")
            continue;
        const auto child = path / name;
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (entry.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                removed = (RemoveDirectoryW(child.c_str()) != 0) && removed;
            else
                removed = RemoveFixtureTree(child) && removed;
        }
        else
            removed = (DeleteFileW(child.c_str()) != 0) && removed;
    } while (FindNextFileW(search, &entry));
    removed = (GetLastError() == ERROR_NO_MORE_FILES) && removed;
    FindClose(search);
    return (RemoveDirectoryW(path.c_str()) != 0) && removed;
}

bool Junction(const std::filesystem::path& link, const std::filesystem::path& destination)
{
    if (!CreateDirectoryW(link.c_str(), nullptr))
        return false;
    const auto handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        RemoveDirectoryW(link.c_str());
        return false;
    }
    const std::wstring substitute = L"\\??\\" + std::filesystem::absolute(destination).native();
    const std::wstring printable = std::filesystem::absolute(destination).native();
    const uint16_t substituteBytes = uint16_t(substitute.size() * sizeof(wchar_t));
    const uint16_t printableBytes = uint16_t(printable.size() * sizeof(wchar_t));
    const uint16_t pathBytes = substituteBytes + printableBytes + 2 * sizeof(wchar_t);
    std::vector<uint8_t> buffer(16 + pathBytes, 0);
    const uint32_t tag = IO_REPARSE_TAG_MOUNT_POINT;
    const uint16_t dataBytes = 8 + pathBytes;
    const uint16_t printOffset = substituteBytes + sizeof(wchar_t);
    std::memcpy(buffer.data(), &tag, 4);
    std::memcpy(buffer.data() + 4, &dataBytes, 2);
    std::memcpy(buffer.data() + 10, &substituteBytes, 2);
    std::memcpy(buffer.data() + 12, &printOffset, 2);
    std::memcpy(buffer.data() + 14, &printableBytes, 2);
    std::memcpy(buffer.data() + 16, substitute.c_str(), substituteBytes);
    std::memcpy(buffer.data() + 16 + printOffset, printable.c_str(), printableBytes);
    DWORD written = 0;
    const bool success = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
        DWORD(buffer.size()), nullptr, 0, &written, nullptr) != 0;
    CloseHandle(handle);
    if (!success)
        RemoveDirectoryW(link.c_str());
    return success;
}
#endif
class TerrainCompositedV2StoreTest : public testing::Test
{
protected:
    void SetUp() override
    {
        static std::atomic<uint64_t> sequence{0};
        root = std::filesystem::temp_directory_path() / ("vl-v2-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
            "-" + std::to_string(sequence++));
        manifest = root / "world.json";
        for (const auto* id : {"coverage", "local"})
        {
            TerrainCompositedVTBuildDesc desc;
            desc.surfaceId = id;
            desc.width = desc.height = 8192;
            desc.generatorVersion = TerrainCompositedGeneratorVersion;
            desc.encoding = TerrainCompositedEncoding;
            desc.channels = {TerrainCompositedVTFormat::RGBA8Srgb,
                TerrainCompositedVTFormat::RGBA8UNorm, TerrainCompositedVTFormat::RGBA8UNorm};
            publication.surfaces.push_back(desc);
            pages.push_back({{id, desc.Layout().MipCount() - 1, 0, 0},
                std::vector<uint8_t>(static_cast<size_t>(desc.PageBytes()), 19)});
        }
        publication = TerrainProductionFixture::Publication(root, publication.surfaces, pages);
        auto local = TerrainProductionFixture::Closure(root / "local-inputs", {"local"},
            TerrainCompositedWeightSource::LocalLayerArray);
        auto weights = local.Object("local-weights");
        weights.relativePath = "local-inputs/" + weights.relativePath;
        publication.production.objects.push_back(weights);
        publication.production.surfaces[1].weightSource = TerrainCompositedWeightSource::LocalLayerArray;
        publication.production.surfaces[1].inputs.localLayerWeights = weights.objectId;
        for (auto& surface : publication.surfaces)
            surface.dependencyHash = publication.production.SurfaceContentHash(surface.surfaceId);
    }
    void TearDown() override
    {
#ifdef _WIN32
        auto native = root.native();
        if (native.compare(0, 4, L"\\\\?\\") != 0)
            native = L"\\\\?\\" + std::filesystem::absolute(root).native();
        EXPECT_TRUE(RemoveFixtureTree(std::filesystem::path(native)));
#else
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
#endif
    }
    void Publish()
    {
        TerrainCompositedVTStore::Publish(manifest, publication, pages);
    }
    nlohmann::json Document()
    {
        const auto bytes = ReadBinaryFile(manifest);
        return nlohmann::json::parse(bytes.begin(), bytes.end());
    }
    void Write(const nlohmann::json& document)
    {
        WriteTextFileAtomically(manifest, document.dump());
    }
    std::filesystem::path root;
    std::filesystem::path manifest;
    TerrainCompositedVTPublication publication;
    std::vector<TerrainCompositedVTPageData> pages;
};
}

TEST_F(TerrainCompositedV2StoreTest, SparseWorldPreservesBothModesWithoutAuthorFiles)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    EXPECT_EQ(store.Surfaces().size(), 2u);
    EXPECT_EQ(store.Pages().size(), 2u);
    EXPECT_FALSE(store.HasPackagedPage({"local", 0, 61, 61}));
    EXPECT_TRUE(store.HasPackagedPage(pages.front().key));
    EXPECT_THROW(store.HasPackagedPage({"missing", 0, 0, 0}), std::exception);
    EXPECT_THROW(store.HasPackagedPage({"local", 14, 0, 0}), std::exception);
    EXPECT_THROW(store.ReadPage({"local", 0, 0, 0}), std::exception);
    for (const auto& object : store.Production().objects)
        EXPECT_EQ(store.ReadInput(object.objectId).size(), object.bytes);
    EXPECT_NO_THROW(store.ValidateAll());
}

TEST_F(TerrainCompositedV2StoreTest, InputCorruptionIsRejectedBeforeConsumptionAndAtInstallation)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto& object = store.Production().Object("local-weights");
    auto bytes = store.ReadInput(object.objectId);
    bytes[0] ^= 1;
    WriteBinaryFileAtomically(root / object.relativePath, bytes);
    EXPECT_NO_THROW(TerrainCompositedVTStore::Open(manifest));
    EXPECT_THROW(store.ReadInput(object.objectId), std::exception);
    EXPECT_THROW(store.ValidateAll(), std::exception);
    EXPECT_THROW(Publish(), std::exception);
}

TEST_F(TerrainCompositedV2StoreTest, CorruptRootIsDiagnosableAndDoesNotMasqueradeAsAbsent)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto key = pages.front().key;
    auto bytes = ReadBinaryFile(store.PackagePath());
    bytes[static_cast<size_t>(store.Pages().at(key).offset)] ^= 1;
    WriteBinaryFileAtomically(store.PackagePath(), bytes);
    const auto reopened = TerrainCompositedVTStore::Open(manifest);
    EXPECT_TRUE(reopened.HasPackagedPage(key));
    EXPECT_THROW(reopened.ReadPage(key), std::exception);
    EXPECT_THROW(reopened.ValidateAll(), std::exception);
}

TEST_F(TerrainCompositedV2StoreTest, MissingDuplicateOrWrongRootAndV1AreRejected)
{
    Publish();
    const auto original = Document();
    for (uint32_t mutation = 0; mutation < 5; ++mutation)
    {
        auto document = original;
        if (mutation == 0) document["roots"].erase(0);
        if (mutation == 1) document["roots"].push_back(document["roots"][0]);
        if (mutation == 2) document["roots"][0]["mip"] = 0;
        if (mutation == 3) document["roots"][0]["surfaceId"] = "unknown";
        if (mutation == 4) document["version"] = 1;
        Write(document);
        EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    }
}

TEST_F(TerrainCompositedV2StoreTest, FailedPublishRetainsPreviousWorldAndExistingReader)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto store = TerrainCompositedVTStore::Open(manifest);
    uint32_t calls = 0;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, publication,
        TerrainCompositedVTStore::PageProducer([&](const auto&)
        {
            if (++calls == 2)
                throw std::runtime_error("injected producer failure");
            return pages.front().bytes;
        })), std::exception);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_EQ(store.ReadPage(pages.front().key), pages.front().bytes);
}

TEST_F(TerrainCompositedV2StoreTest, InputBudgetsAndForgedProjectionRejectBeforePublication)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    TerrainCompositedVTStoreLimits limits;
    limits.maxInputObjectBytes = 1;
    limits.maxInputBytes = 1;
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest, limits), std::exception);
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, publication, pages, limits), std::exception);
    for (auto& object : publication.production.objects)
        if (object.objectId == "catalog")
            object.heightBlendCatalogDigest = ContentHasher::HashString("forged actual bytes");
    for (auto& surface : publication.surfaces)
        surface.dependencyHash = publication.production.SurfaceContentHash(surface.surfaceId);
    EXPECT_THROW(Publish(), std::exception);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
}

TEST_F(TerrainCompositedV2StoreTest, MissingInputAndDirectoryLinkReplacementAreRejected)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto path = root / store.Production().Object("local-weights").relativePath;
    const auto saved = root / "saved-weights.bin";
    std::filesystem::rename(path, saved);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    std::filesystem::rename(saved, path);
    const auto originals = root / "objects";
    const auto preserved = root / "preserved-objects";
    std::filesystem::rename(originals, preserved);
    std::error_code error;
    std::filesystem::create_directory_symlink(preserved, originals, error);
    if (error)
    {
        std::filesystem::rename(preserved, originals);
        GTEST_SKIP() << "Directory symlink creation unavailable: " << error.message();
    }
    EXPECT_THROW(store.ReadInput("catalog"), std::exception);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    std::filesystem::remove(originals);
    std::filesystem::rename(preserved, originals);
}

TEST_F(TerrainCompositedV2StoreTest, WindowsJunctionReplacementCannotRedirectAnOpenedInputOwner)
{
#ifdef _WIN32
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto originals = root / "objects";
    const auto preserved = root / "preserved-objects";
    std::filesystem::rename(originals, preserved);
    if (!Junction(originals, preserved))
    {
        std::filesystem::rename(preserved, originals);
        GTEST_SKIP() << "Filesystem does not permit a junction";
    }
    EXPECT_THROW(store.ReadInput("catalog"), std::exception);
    EXPECT_THROW(TerrainCompositedVTStore::Open(manifest), std::exception);
    EXPECT_TRUE(RemoveDirectoryW(originals.c_str()));
    std::filesystem::rename(preserved, originals);
    EXPECT_NO_THROW(store.ValidateAll());
#else
    GTEST_SKIP() << "Windows junction";
#endif
}

TEST_F(TerrainCompositedV2StoreTest, StagingHardlinkCollisionRetainsPreviousInputManifestAndIntruder)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto input = root / publication.production.Object("catalog").relativePath;
    const auto originalInput = ReadBinaryFile(input);
    std::filesystem::path collision;
    uint32_t producerCalls = 0;
    ObserveStaging observer([&](const auto& path, bool opened) {
        ASSERT_FALSE(opened);
        collision = path;
#ifdef _WIN32
        ASSERT_TRUE(CreateHardLinkW(path.c_str(), input.c_str(), nullptr)) << GetLastError();
#else
        ASSERT_NO_THROW(std::filesystem::create_hard_link(input, path));
#endif
    });
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, publication,
        TerrainCompositedVTStore::PageProducer([&](const auto&) {
            ++producerCalls;
            return pages.front().bytes;
        })), std::exception);
    EXPECT_EQ(producerCalls, 0u);
    ASSERT_FALSE(collision.empty());
    EXPECT_TRUE(ReadBinaryFile(input) == originalInput) << "Previous input was overwritten";
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_TRUE(SameFileIdentity(collision, input));
    EXPECT_NO_THROW(store.ValidateAll());
    EXPECT_TRUE(std::filesystem::remove(collision));
    EXPECT_TRUE(std::filesystem::remove(collision.parent_path()));
}

TEST_F(TerrainCompositedV2StoreTest, StagingExistingRegularFileIsNeitherTruncatedNorRemoved)
{
    ASSERT_NO_THROW(Publish());
    const auto original = ReadBinaryFile(manifest);
    std::filesystem::path collision;
    ObserveStaging observer([&](const auto& path, bool opened) {
        ASSERT_FALSE(opened);
        collision = path;
        WriteTextFileAtomically(path, "intruder sentinel");
    });
    EXPECT_THROW(Publish(), std::exception);
    ASSERT_FALSE(collision.empty());
    const auto bytes = ReadBinaryFile(collision);
    EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "intruder sentinel");
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_TRUE(std::filesystem::remove(collision));
    EXPECT_TRUE(std::filesystem::remove(collision.parent_path()));
}

TEST_F(TerrainCompositedV2StoreTest, StagingDirectoryCollisionDoesNotRecursivelyRemoveForeignChildren)
{
    ASSERT_NO_THROW(Publish());
    std::filesystem::path collision;
    ObserveStaging observer([&](const auto& path, bool opened) {
        ASSERT_FALSE(opened);
        collision = path;
        ASSERT_TRUE(std::filesystem::create_directory(path));
        WriteTextFileAtomically(path / "sentinel", "keep");
    });
    EXPECT_THROW(Publish(), std::exception);
    ASSERT_FALSE(collision.empty());
    EXPECT_EQ(ReadBinaryFile(collision / "sentinel"), (std::vector<uint8_t>{'k', 'e', 'e', 'p'}));
    EXPECT_TRUE(std::filesystem::remove(collision / "sentinel"));
    EXPECT_TRUE(std::filesystem::remove(collision));
    EXPECT_TRUE(std::filesystem::remove(collision.parent_path()));
}

TEST_F(TerrainCompositedV2StoreTest, StreamingHashAndProducerFailureCleanOwnedStaging)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto expectedPackage = ReadBinaryFile(TerrainCompositedVTStore::Open(manifest).PackagePath());
    size_t calls = 0;
    TerrainCompositedVTStore::Publish(manifest, publication,
        TerrainCompositedVTStore::PageProducer([&](const auto& key) {
            ++calls;
            for (const auto& page : pages)
                if (page.key == key)
                    return page.bytes;
            throw std::runtime_error("Unknown fixture key");
        }));
    EXPECT_EQ(calls, pages.size());
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    const auto store = TerrainCompositedVTStore::Open(manifest);
    EXPECT_EQ(ReadBinaryFile(store.PackagePath()), expectedPackage);
    EXPECT_EQ(ContentHasher::HashFile(store.PackagePath()).ToHex(), Document()["packageHash"]);
    EXPECT_NO_THROW(store.ValidateAll());
    NoStagingDirectories(root);
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, publication,
        TerrainCompositedVTStore::PageProducer([](const auto&) -> std::vector<uint8_t> {
            throw std::runtime_error("injected page failure");
        })), std::exception);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    NoStagingDirectories(root);
}

TEST_F(TerrainCompositedV2StoreTest, IncompleteProducerAfterFirstPageRetainsPublication)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto store = TerrainCompositedVTStore::Open(manifest);
    size_t calls = 0;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, publication,
        TerrainCompositedVTStore::PageProducer([&](const auto& key) {
            if (++calls == 2)
                return std::vector<uint8_t>{1};
            for (const auto& page : pages)
                if (page.key == key)
                    return page.bytes;
            throw std::runtime_error("Unknown key");
        })), std::exception);
    EXPECT_EQ(calls, 2u);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_NO_THROW(store.ValidateAll());
    NoStagingDirectories(root);
    EXPECT_NO_THROW(Publish());
}

TEST_F(TerrainCompositedV2StoreTest, InputChangeDuringProductionRejectsAndRetainsPreviousManifest)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto input = root / publication.production.Object("compute").relativePath;
    const auto previousInput = ReadBinaryFile(input);
    bool changed = false;
    EXPECT_THROW(TerrainCompositedVTStore::Publish(manifest, publication,
        TerrainCompositedVTStore::PageProducer([&](const auto& key) {
            if (!changed)
            {
                auto modified = previousInput;
                modified.front() ^= 1;
                TerrainProductionFixture::WriteFixtureBytes(input, modified);
                changed = true;
            }
            for (const auto& page : pages)
                if (page.key == key)
                    return page.bytes;
            throw std::runtime_error("Unknown key");
        })), std::exception);
    EXPECT_TRUE(changed);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    NoStagingDirectories(root);
    TerrainProductionFixture::WriteFixtureBytes(input, previousInput);
    EXPECT_NO_THROW(TerrainCompositedVTStore::Open(manifest).ValidateAll());
}

TEST_F(TerrainCompositedV2StoreTest, ExceptionAfterStagingOpenReleasesFileAndDirectory)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    bool observed = false;
    {
        ObserveStaging observer([&](const auto&, bool opened) {
            if (opened)
            {
                observed = true;
                throw std::runtime_error("injected post-open failure");
            }
        });
        EXPECT_THROW(Publish(), std::exception);
    }
    EXPECT_TRUE(observed);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    NoStagingDirectories(root);
    EXPECT_NO_THROW(Publish());
}

#ifdef _WIN32
TEST_F(TerrainCompositedV2StoreTest, WindowsUnicodeDirectoryAndManifestRoundTrip)
{
    const auto destination = root / std::filesystem::path(L"\u5730\u5f62-\u6750\u8d28");
    ASSERT_TRUE(std::filesystem::create_directory(destination));
    std::filesystem::rename(root / "objects", destination / "objects");
    std::filesystem::rename(root / "local-inputs", destination / "local-inputs");
    manifest = destination / std::filesystem::path(L"\u4e16\u754c.json");
    ASSERT_NO_THROW(Publish());
    auto store = TerrainCompositedVTStore::Open(manifest);
    EXPECT_NO_THROW(store.ValidateAll());
    EXPECT_EQ(store.ReadPage(pages.front().key), pages.front().bytes);
    EXPECT_FALSE(store.ReadInput("catalog").empty());
    EXPECT_NO_THROW(Publish());
    NoStagingDirectories(destination);
}

TEST_F(TerrainCompositedV2StoreTest, WindowsExtendedLongDirectoryRoundTrip)
{
    root = std::filesystem::path(L"\\\\?\\" + std::filesystem::absolute(root).native());
    auto destination = root;
    for (size_t index = 0; index < 5; ++index)
    {
        destination /= std::filesystem::path(std::wstring(55, L'a') + std::to_wstring(index));
        ASSERT_TRUE(CreateDirectoryW(destination.c_str(), nullptr)) << GetLastError();
    }
    ASSERT_GT(destination.native().size(), size_t(MAX_PATH));
    std::filesystem::rename(root / "objects", destination / "objects");
    std::filesystem::rename(root / "local-inputs", destination / "local-inputs");
    manifest = destination / L"world.json";
    ASSERT_NO_THROW(Publish());
    auto store = TerrainCompositedVTStore::Open(manifest);
    EXPECT_NO_THROW(store.ValidateAll());
    EXPECT_EQ(store.ReadPage(pages.front().key), pages.front().bytes);
    EXPECT_FALSE(store.ReadInput("catalog").empty());
    EXPECT_NO_THROW(Publish());
    NoStagingDirectories(destination);
}

TEST_F(TerrainCompositedV2StoreTest, WindowsTransientJunctionRestoredAfterInputOpenIsRejected)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto originals = root / "objects";
    const auto preserved = root / "saved-objects";
    const auto outside = root.parent_path() / (root.filename().string() + "-outside");
    ASSERT_TRUE(std::filesystem::create_directory(outside));
    for (const auto& entry : std::filesystem::directory_iterator(originals))
        std::filesystem::copy_file(entry.path(), outside / entry.path().filename());
    bool switched = false;
    bool restored = false;
    const auto restore = [&] {
        if (switched && !restored)
        {
            EXPECT_TRUE(RemoveDirectoryW(originals.c_str()));
            std::filesystem::rename(preserved, originals);
            restored = true;
        }
    };
    TerrainCompositedVTStoreTesting::InputObserver = [&](const auto&, bool opened) {
        if (!opened)
        {
            std::filesystem::rename(originals, preserved);
            ASSERT_TRUE(Junction(originals, outside));
            switched = true;
        }
        else
            restore();
    };
    EXPECT_THROW(store.ReadInput("catalog"), std::exception);
    TerrainCompositedVTStoreTesting::InputObserver = {};
    restore();
    EXPECT_TRUE(switched);
    EXPECT_TRUE(restored);
    EXPECT_NO_THROW(store.ValidateAll());
    for (const auto& entry : std::filesystem::directory_iterator(outside))
        EXPECT_TRUE(std::filesystem::remove(entry.path()));
    EXPECT_TRUE(std::filesystem::remove(outside));
}

TEST_F(TerrainCompositedV2StoreTest, WindowsInputHandlePinsAncestorsAndFileDuringConsumption)
{
    Publish();
    const auto store = TerrainCompositedVTStore::Open(manifest);
    bool observed = false;
    TerrainCompositedVTStoreTesting::InputObserver = [&](const auto& path, bool opened) {
        if (!opened)
            return;
        observed = true;
        const auto movedObjects = root / "moved-objects";
        const bool objectsMoved = MoveFileExW(path.parent_path().c_str(), movedObjects.c_str(), 0) != 0;
        EXPECT_FALSE(objectsMoved);
        if (objectsMoved)
            EXPECT_TRUE(MoveFileExW(movedObjects.c_str(), path.parent_path().c_str(), 0));
        const auto movedRoot = root.parent_path() / (root.filename().string() + "-moved");
        const bool rootMoved = MoveFileExW(root.c_str(), movedRoot.c_str(), 0) != 0;
        EXPECT_FALSE(rootMoved);
        if (rootMoved)
            EXPECT_TRUE(MoveFileExW(movedRoot.c_str(), root.c_str(), 0));
        EXPECT_FALSE(DeleteFileW(path.c_str()));
        const HANDLE writing = CreateFileW(path.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        EXPECT_EQ(writing, INVALID_HANDLE_VALUE);
        if (writing != INVALID_HANDLE_VALUE)
            CloseHandle(writing);
    };
    const auto bytes = store.ReadInput("catalog");
    TerrainCompositedVTStoreTesting::InputObserver = {};
    EXPECT_TRUE(observed);
    EXPECT_EQ(ContentHasher::HashBytes(bytes.data(), bytes.size()), publication.production.Object("catalog").digest);
}

TEST_F(TerrainCompositedV2StoreTest, WindowsStagingReparseCollisionRetainsForeignDirectory)
{
    ASSERT_NO_THROW(Publish());
    const auto foreign = root / "foreign";
    std::filesystem::create_directory(foreign);
    WriteTextFileAtomically(foreign / "sentinel", "keep");
    std::filesystem::path collision;
    ObserveStaging observer([&](const auto& path, bool opened) {
        ASSERT_FALSE(opened);
        collision = path;
        ASSERT_TRUE(Junction(path, foreign)) << GetLastError();
    });
    EXPECT_THROW(Publish(), std::exception);
    ASSERT_FALSE(collision.empty());
    EXPECT_EQ(ReadBinaryFile(foreign / "sentinel"), (std::vector<uint8_t>{'k', 'e', 'e', 'p'}));
    EXPECT_NE(GetFileAttributesW(collision.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT, 0u);
    EXPECT_TRUE(RemoveDirectoryW(collision.c_str()));
    EXPECT_TRUE(std::filesystem::remove(collision.parent_path()));
}

TEST_F(TerrainCompositedV2StoreTest, WindowsStagingHardlinkCollisionWithLockedManifestPreservesOldWorld)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto input = root / publication.production.Object("catalog").relativePath;
    const auto originalInput = ReadBinaryFile(input);
    const HANDLE locked = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    std::filesystem::path collision;
    {
        ObserveStaging observer([&](const auto& path, bool opened) {
            ASSERT_FALSE(opened);
            collision = path;
            ASSERT_TRUE(CreateHardLinkW(path.c_str(), input.c_str(), nullptr)) << GetLastError();
        });
        EXPECT_THROW(Publish(), std::exception);
        EXPECT_TRUE(ReadBinaryFile(input) == originalInput) << "Previous input was overwritten";
        EXPECT_EQ(ReadBinaryFile(manifest), original);
        EXPECT_TRUE(SameFileIdentity(collision, input));
    }
    EXPECT_TRUE(CloseHandle(locked));
    EXPECT_TRUE(std::filesystem::remove(collision));
    EXPECT_TRUE(std::filesystem::remove(collision.parent_path()));
    EXPECT_NO_THROW(TerrainCompositedVTStore::Open(manifest).ValidateAll());
}

TEST_F(TerrainCompositedV2StoreTest, WindowsLockedManifestAfterStreamingPreservesInputsAndCleansStaging)
{
    Publish();
    const auto original = ReadBinaryFile(manifest);
    const auto store = TerrainCompositedVTStore::Open(manifest);
    const auto input = root / publication.production.Object("catalog").relativePath;
    const auto originalInput = ReadBinaryFile(input);
    for (auto& page : pages)
        page.bytes.front() ^= 1;
    const HANDLE locked = CreateFileW(manifest.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(locked, INVALID_HANDLE_VALUE);
    EXPECT_THROW(Publish(), std::exception);
    EXPECT_EQ(ReadBinaryFile(manifest), original);
    EXPECT_EQ(ReadBinaryFile(input), originalInput);
    EXPECT_TRUE(CloseHandle(locked));
    EXPECT_NO_THROW(store.ValidateAll());
    NoStagingDirectories(root);
}

TEST_F(TerrainCompositedV2StoreTest, WindowsOpenStagingHandleBlocksReplacementReopenAndDirectoryMove)
{
    const auto input = root / publication.production.Object("catalog").relativePath;
    const auto originalInput = ReadBinaryFile(input);
    bool observed = false;
    ObserveStaging observer([&](const auto& path, bool opened) {
        if (!opened)
            return;
        observed = true;
        EXPECT_FALSE(DeleteFileW(path.c_str()));
        EXPECT_FALSE(MoveFileExW(path.c_str(), (root / "stolen").c_str(), MOVEFILE_REPLACE_EXISTING));
        EXPECT_FALSE(MoveFileExW(path.parent_path().c_str(), (root / "moved-dir").c_str(), 0));
        const HANDLE hostile = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ |
            FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        EXPECT_EQ(hostile, INVALID_HANDLE_VALUE);
        if (hostile != INVALID_HANDLE_VALUE)
            CloseHandle(hostile);
        EXPECT_FALSE(CreateHardLinkW(path.c_str(), input.c_str(), nullptr));
    });
    EXPECT_NO_THROW(Publish());
    EXPECT_TRUE(observed);
    EXPECT_EQ(ReadBinaryFile(input), originalInput);
    EXPECT_NO_THROW(TerrainCompositedVTStore::Open(manifest).ValidateAll());
    NoStagingDirectories(root);
}
#endif
