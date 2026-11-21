#include "terrain/terrainCompositedVTStore.h"

#include "shader/build/atomicFile.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>
#include <random>
#include <stdexcept>
#include <string>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace VL
{
#if defined(VL_FILE_IO_TEST_HOOKS)
namespace TerrainCompositedVTStoreTesting
{
// 仅供确定性负例在目录创建后/句柄创建后注入；线程局部，不改变 Publish 接口。
thread_local std::function<void(const std::filesystem::path&, bool)> StagingObserver;
thread_local std::function<void(const std::filesystem::path&, bool)> InputObserver;
}
#endif

namespace
{

using Json = nlohmann::json;
constexpr std::array<uint8_t, 16> PackageHeader{
    'V', 'L', 'C', 'V', 'T', '0', '0', '2', 2, 0, 0, 0, 136, 0, 0, 0};

class StagedPackage
{
public:
    explicit StagedPackage(const std::filesystem::path& root)
        : parent(root)
    {
        static std::atomic<uint64_t> sequence{0};
        std::random_device random;
        for (uint32_t attempt = 0; attempt < 128; ++attempt)
        {
            const auto candidate = parent.Path() / (".vlcvt-publish-" +
                std::to_string(random()) + "-" + std::to_string(sequence.fetch_add(1)));
            if (std::filesystem::create_directory(candidate))
            {
                directory = candidate;
                path = directory / "package.tmp";
                try
                {
                    Observe(false);
#ifdef _WIN32
                    handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE,
                        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (handle == INVALID_HANDLE_VALUE)
                        throw std::runtime_error("Failed to exclusively create staged VT package");
#else
                    directoryDescriptor = open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
                    if (directoryDescriptor < 0)
                        throw std::runtime_error("Failed to open VT staging directory");
                    descriptor = openat(directoryDescriptor, "package.tmp",
                        O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
                    if (descriptor < 0)
                        throw std::runtime_error("Failed to exclusively create staged VT package");
#endif
                    Observe(true);
                }
                catch (...)
                {
                    Cleanup();
                    throw;
                }
                return;
            }
        }
        throw std::runtime_error("Failed to allocate VT package staging directory");
    }
    ~StagedPackage()
    {
        Cleanup();
    }
    StagedPackage(const StagedPackage&) = delete;
    StagedPackage& operator=(const StagedPackage&) = delete;
    std::filesystem::path path;

    void Write(const void* data, size_t size)
    {
        const auto* bytes = static_cast<const uint8_t*>(data);
        size_t offset = 0;
        while (offset < size)
        {
            const size_t count = std::min(size - offset, size_t{1024 * 1024});
#ifdef _WIN32
            DWORD written = 0;
            if (!WriteFile(handle, bytes + offset, static_cast<DWORD>(count), &written, nullptr) ||
                written == 0)
#else
            const ssize_t written = write(descriptor, bytes + offset, count);
            if (written < 0 && errno == EINTR)
                continue;
            if (written <= 0)
#endif
                throw std::runtime_error("Failed to write staged VT package");
            offset += static_cast<size_t>(written);
        }
    }

    void FlushAndRewind()
    {
#ifdef _WIN32
        LARGE_INTEGER start{};
        if (!FlushFileBuffers(handle) || !SetFilePointerEx(handle, start, nullptr, FILE_BEGIN))
#else
        if (fsync(descriptor) != 0 || lseek(descriptor, 0, SEEK_SET) < 0)
#endif
            throw std::runtime_error("Failed to flush/rewind staged VT package");
    }

    uint64_t Size() const
    {
#ifdef _WIN32
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0)
            throw std::runtime_error("Failed to measure staged VT package");
        return static_cast<uint64_t>(size.QuadPart);
#else
        struct stat status{};
        if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0)
            throw std::runtime_error("Failed to measure staged VT package");
        return static_cast<uint64_t>(status.st_size);
#endif
    }

    size_t Read(void* data, size_t size)
    {
#ifdef _WIN32
        DWORD count = 0;
        if (!ReadFile(handle, data, static_cast<DWORD>(size), &count, nullptr))
            throw std::runtime_error("Failed to read staged VT package");
        return count;
#else
        ssize_t count;
        do
        {
            count = read(descriptor, data, size);
        } while (count < 0 && errno == EINTR);
        if (count < 0)
            throw std::runtime_error("Failed to read staged VT package");
        return static_cast<size_t>(count);
#endif
    }

    void ReadExact(void* data, size_t size)
    {
        auto* bytes = static_cast<uint8_t*>(data);
        size_t offset = 0;
        while (offset < size)
        {
            const size_t count = Read(bytes + offset, size - offset);
            if (count == 0)
                throw std::runtime_error("Truncated staged VT package");
            offset += count;
        }
    }

    std::error_code Publish(const std::filesystem::path& target)
    {
        parent.ValidateIdentity();
        // 发布仍使用已验证的文件句柄；已有目标只校验，绝不覆盖。
#ifdef _WIN32
        const auto absolute = (parent.Path() / target.filename()).native();
        std::vector<uint8_t> buffer(sizeof(FILE_RENAME_INFO) + absolute.size() * sizeof(wchar_t));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(absolute.size() * sizeof(wchar_t));
        std::memcpy(info->FileName, absolute.data(), info->FileNameLength);
        if (!SetFileInformationByHandle(handle, FileRenameInfo, info, static_cast<DWORD>(buffer.size())))
            return std::error_code(GetLastError(), std::system_category());
        published = true;
        // 包完成发布后先释放独占句柄，再提交 manifest，读者才能立即打开新包。
        const HANDLE publishedHandle = handle;
        handle = INVALID_HANDLE_VALUE;
        if (!CloseHandle(publishedHandle))
            return std::error_code(GetLastError(), std::system_category());
#else
        // fd 链接不会重新解析 package.tmp；不支持描述符链接的平台安全失败。
#if defined(__linux__)
        const auto source = "/proc/self/fd/" + std::to_string(descriptor);
#else
        const auto source = "/dev/fd/" + std::to_string(descriptor);
#endif
        if (linkat(AT_FDCWD, source.c_str(), AT_FDCWD, target.c_str(), AT_SYMLINK_FOLLOW) != 0)
            return std::error_code(errno, std::generic_category());
#endif
        return {};
    }

private:
    void Observe(bool opened)
    {
#if defined(VL_FILE_IO_TEST_HOOKS)
        if (TerrainCompositedVTStoreTesting::StagingObserver)
            TerrainCompositedVTStoreTesting::StagingObserver(path, opened);
#endif
    }

    void Cleanup() noexcept
    {
        // 创建失败不拥有 package.tmp；清理不跟随路径，也不递归删除竞入条目。
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE)
        {
            if (!published)
            {
                FILE_DISPOSITION_INFO disposition{TRUE};
                SetFileInformationByHandle(handle, FileDispositionInfo, &disposition, sizeof(disposition));
            }
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
        }
#else
        if (descriptor >= 0)
        {
            struct stat owned{}, named{};
            if (fstat(descriptor, &owned) == 0 &&
                fstatat(directoryDescriptor, "package.tmp", &named, AT_SYMLINK_NOFOLLOW) == 0 &&
                owned.st_dev == named.st_dev && owned.st_ino == named.st_ino)
                unlinkat(directoryDescriptor, "package.tmp", 0);
            close(descriptor);
            descriptor = -1;
        }
        if (directoryDescriptor >= 0)
        {
            close(directoryDescriptor);
            directoryDescriptor = -1;
        }
#endif
        std::error_code ignored;
        std::filesystem::remove(directory, ignored);
    }

    std::filesystem::path directory;
    PinnedFileDirectory parent;
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool published = false;
#else
    int directoryDescriptor = -1;
    int descriptor = -1;
#endif
};

void Require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void Fields(const Json& object, std::initializer_list<const char*> fields)
{
    Require(object.is_object() && object.size() == fields.size(), "Invalid VT manifest fields");
    for (const char* field : fields)
    {
        Require(object.contains(field), "Missing VT manifest field");
    }
}

uint64_t Integer(const Json& value, uint64_t maximum = UINT64_MAX)
{
    Require(value.is_number_integer(), "VT manifest integer required");
    if (!value.is_number_unsigned())
    {
        Require(value.get<int64_t>() >= 0, "Negative VT manifest integer");
    }
    const uint64_t result = value.get<uint64_t>();
    Require(result <= maximum, "VT manifest integer out of range");
    return result;
}

std::string Text(const Json& value)
{
    Require(value.is_string(), "VT manifest string required");
    const auto result = value.get<std::string>();
    Require(!result.empty() && result.size() <= 1024 &&
        result.find('\0') == std::string::npos, "Invalid VT manifest string");
    return result;
}

ContentDigest Digest(const Json& value)
{
    const auto text = Text(value);
    const auto digest = ContentDigest::FromHex(text);
    Require(digest.ToHex() == text, "Noncanonical VT digest");
    return digest;
}

TerrainCompositedVTFormat ParseFormat(const Json& value)
{
    const auto name = Text(value);
    for (const auto format : {TerrainCompositedVTFormat::RGBA8UNorm,
        TerrainCompositedVTFormat::RGBA8Srgb, TerrainCompositedVTFormat::RG8UNorm,
        TerrainCompositedVTFormat::R16UNorm, TerrainCompositedVTFormat::BC5UNorm,
        TerrainCompositedVTFormat::BC7UNorm, TerrainCompositedVTFormat::BC7Srgb})
    {
        if (TerrainCompositedVTFormatName(format) == name)
        {
            return format;
        }
    }
    throw std::runtime_error("Unsupported VT page format");
}

void CheckLimits(const TerrainCompositedVTStoreLimits& limits)
{
    Require(limits.maxManifestBytes > 0 && limits.maxPackageBytes >= PackageHeader.size() &&
        limits.maxPages > 0 &&
        limits.maxInputObjectBytes > 0 && limits.maxInputBytes >= limits.maxInputObjectBytes &&
        limits.maxInputObjectBytes <= uint64_t(std::numeric_limits<std::streamsize>::max()) &&
        limits.maxManifestBytes <= uint64_t(std::numeric_limits<size_t>::max()) &&
        limits.maxManifestBytes <= uint64_t(std::numeric_limits<std::streamsize>::max()) &&
        limits.maxPackageBytes <= uint64_t(std::numeric_limits<std::streamoff>::max()),
        "Invalid VT store limits");
}

void RejectSymlink(const std::filesystem::path& path)
{
    Require(!std::filesystem::is_symlink(std::filesystem::symlink_status(path)),
        "VT store file must not be a symlink");
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    Require(attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_REPARSE_POINT),
        "VT store path must not be a reparse point");
#endif
}

void RejectLinkedAncestors(const std::filesystem::path& requested)
{
    PinnedFileDirectory parent(requested.has_parent_path() ?
        requested.parent_path() : std::filesystem::path("."));
    RejectSymlink(parent.Path() / requested.filename());
}

std::filesystem::path PackagePath(
    const std::filesystem::path& manifestPath, const std::string& filename,
    const ContentDigest& hash)
{
    Require(filename == hash.ToHex() + ".vlcvt", "Invalid VT package path");
    Require(manifestPath.filename() != std::filesystem::path(filename),
        "VT manifest must not replace its package");
    RejectLinkedAncestors(manifestPath);
    const PinnedFileDirectory parent(
        manifestPath.has_parent_path() ? manifestPath.parent_path() : std::filesystem::path("."));
    const auto path = parent.Path() / filename;
    RejectSymlink(path);
    return path;
}

struct ParsedManifest
{
    TerrainCompositedProductionClosure production;
    std::vector<TerrainCompositedVTBuildDesc> surfaces;
    std::map<TerrainCompositedVTPageKey, TerrainCompositedVTPageRecord> pages;
    std::filesystem::path packagePath;
    uint64_t packageSize = 0;
    ContentDigest packageHash;
};

ParsedManifest ParseManifest(
    const Json& document, const std::filesystem::path& path,
    const TerrainCompositedVTStoreLimits& limits)
{
    Fields(document, {"kind", "version", "innerExtent", "border", "storedExtent",
        "mipRule", "filterRule", "package", "packageBytes", "packageHash", "surfaces", "pages",
        "production", "roots"});
    Require(Text(document.at("kind")) == "terrain-composited-vt" &&
        Integer(document.at("version")) == TerrainCompositedVTStore::Version &&
        Integer(document.at("innerExtent")) == TerrainCompositedVTLayout::InnerExtent &&
        Integer(document.at("border")) == TerrainCompositedVTLayout::Border &&
        Integer(document.at("storedExtent")) == TerrainCompositedVTLayout::StoredExtent &&
        Text(document.at("mipRule")) == "floor-half-to-one" &&
        Text(document.at("filterRule")) == TerrainCompositedFilterRule,
        "Unsupported VT manifest version/layout/filter");
    ParsedManifest result;
    result.production = TerrainCompositedProductionFromJson(document.at("production"));
    uint64_t inputBytes = 0;
    for (const auto& object : result.production.objects)
    {
        Require(object.bytes <= limits.maxInputObjectBytes &&
            object.bytes <= limits.maxInputBytes - inputBytes, "VT input byte limit exceeded");
        inputBytes += object.bytes;
    }
    result.packageSize = Integer(document.at("packageBytes"), limits.maxPackageBytes);
    Require(result.packageSize >= PackageHeader.size(), "VT package too short");
    result.packageHash = Digest(document.at("packageHash"));
    result.packagePath = PackagePath(path, Text(document.at("package")), result.packageHash);
    const auto& surfaceArray = document.at("surfaces");
    const auto& pageArray = document.at("pages");
    Require(surfaceArray.is_array() && !surfaceArray.empty() &&
        surfaceArray.size() <= limits.maxPages && pageArray.is_array() &&
        pageArray.size() <= limits.maxPages, "Invalid VT surface/page count");
    std::map<std::string, size_t> surfaceIndices;
    for (const auto& surface : surfaceArray)
    {
        Fields(surface, {"surfaceId", "width", "height", "addressMode", "dependencyHash",
            "generatorVersion", "encoding", "channels", "contentHash", "tailMip"});
        TerrainCompositedVTBuildDesc desc;
        desc.surfaceId = Text(surface.at("surfaceId"));
        desc.width = static_cast<uint32_t>(Integer(surface.at("width"), TerrainCompositedVTLayout::MaxDimension));
        desc.height = static_cast<uint32_t>(Integer(surface.at("height"), TerrainCompositedVTLayout::MaxDimension));
        const auto address = Text(surface.at("addressMode"));
        Require(address == "clamp" || address == "repeat", "Invalid VT address mode");
        desc.addressMode = address == "clamp" ? TerrainCompositedVTAddressMode::Clamp
                                             : TerrainCompositedVTAddressMode::Repeat;
        desc.dependencyHash = Digest(surface.at("dependencyHash"));
        desc.generatorVersion = static_cast<uint32_t>(Integer(surface.at("generatorVersion"), UINT32_MAX));
        desc.encoding = Text(surface.at("encoding"));
        const auto& channels = surface.at("channels");
        Require(channels.is_array() && !channels.empty() && channels.size() <= 8,
            "Invalid VT channel count");
        for (const auto& channel : channels)
        {
            desc.channels.push_back(ParseFormat(channel));
        }
        Require(desc.ContentHash() == Digest(surface.at("contentHash")),
            "VT content identity mismatch");
        const auto layout = desc.Layout();
        Require(layout.Mip(0).pagesX <= 64 && layout.Mip(0).pagesY <= 64 &&
            layout.MipCount() <= 14, "VT surface exceeds supported feedback domain");
        Require(Integer(surface.at("tailMip")) == layout.TailMip(), "Invalid VT tail mip");
        Require(desc.dependencyHash == result.production.SurfaceContentHash(desc.surfaceId),
            "VT production/surface dependency mismatch");
        Require(surfaceIndices.emplace(desc.surfaceId, result.surfaces.size()).second,
            "Duplicate VT surface");
        result.surfaces.push_back(std::move(desc));
    }
    Require(result.surfaces.size() == result.production.surfaces.size(), "VT production surface set mismatch");
    uint64_t nextOffset = PackageHeader.size();
    for (const auto& page : pageArray)
    {
        Fields(page, {"surfaceId", "mip", "x", "y", "offset", "size", "hash"});
        TerrainCompositedVTPageKey key{Text(page.at("surfaceId")),
            static_cast<uint32_t>(Integer(page.at("mip"), UINT32_MAX)),
            static_cast<uint32_t>(Integer(page.at("x"), UINT32_MAX)),
            static_cast<uint32_t>(Integer(page.at("y"), UINT32_MAX))};
        const auto surface = surfaceIndices.find(key.surfaceId);
        Require(surface != surfaceIndices.end(), "Unknown VT page surface");
        const auto& desc = result.surfaces[surface->second];
        Require(desc.Layout().Contains(key), "Out-of-range VT page");
        TerrainCompositedVTPageRecord record;
        record.offset = Integer(page.at("offset"), result.packageSize);
        record.size = Integer(page.at("size"), result.packageSize);
        record.hash = Digest(page.at("hash"));
        Require(record.offset == nextOffset && record.size == desc.PageBytes() &&
            record.size <= result.packageSize - record.offset,
            "Invalid VT page offset/size/overlap");
        nextOffset += record.size;
        Require(result.pages.emplace(std::move(key), record).second, "Duplicate VT page");
    }
    Require(nextOffset == result.packageSize, "VT package trailing bytes/gaps");
    const auto& roots = document.at("roots");
    Require(roots.is_array() && roots.size() == result.surfaces.size(), "Missing/duplicate VT roots");
    std::set<std::string> rooted;
    for (const auto& root : roots)
    {
        Fields(root, {"surfaceId", "mip", "x", "y"});
        const auto id = Text(root.at("surfaceId"));
        const auto found = surfaceIndices.find(id);
        Require(found != surfaceIndices.end() && rooted.insert(id).second, "Unknown/duplicate root surface");
        const auto mip = result.surfaces[found->second].Layout().MipCount() - 1;
        Require(Integer(root.at("mip")) == mip && Integer(root.at("x")) == 0 &&
            Integer(root.at("y")) == 0 && result.pages.count({id, mip, 0, 0}) == 1,
            "VT final root page missing/invalid");
    }
    return result;
}

void ReadExact(RootedFileReader& input, void* data, size_t size)
{
    auto* bytes = static_cast<uint8_t*>(data);
    size_t offset = 0;
    while (offset < size)
    {
        const auto count = input.Read(bytes + offset, size - offset);
        Require(count != 0, "Truncated VT file");
        offset += count;
    }
}

std::vector<uint8_t> ReadRange(
    RootedFileReader& input, const TerrainCompositedVTPageRecord& record)
{
    input.Seek(record.offset);
    std::vector<uint8_t> bytes(static_cast<size_t>(record.size));
    ReadExact(input, bytes.data(), bytes.size());
    Require(ContentHasher::HashBytes(bytes.data(), bytes.size()) == record.hash,
        "VT page integrity mismatch");
    input.ValidateIdentity();
    return bytes;
}

template<typename Read, typename AtEnd>
void ValidatePackageContents(const ParsedManifest& manifest, const Read& read, const AtEnd& atEnd)
{
    std::array<uint8_t, PackageHeader.size()> header{};
    read(header.data(), header.size());
    Require(header == PackageHeader, "Invalid VT package header");
    ContentHasher packageHasher;
    packageHasher.Update(header.data(), header.size());
    std::vector<const TerrainCompositedVTPageRecord*> physicalPages;
    physicalPages.reserve(manifest.pages.size());
    for (const auto& entry : manifest.pages)
        physicalPages.push_back(&entry.second);
    std::sort(physicalPages.begin(), physicalPages.end(), [](const auto* first, const auto* second)
        { return first->offset < second->offset; });
    std::array<uint8_t, 64 * 1024> buffer{};
    uint64_t offset = header.size();
    for (const auto* page : physicalPages)
    {
        Require(page->offset == offset && page->size <= manifest.packageSize - offset,
            "Invalid VT page offset/size/overlap");
        ContentHasher pageHasher;
        uint64_t remaining = page->size;
        while (remaining != 0)
        {
            const auto bytes = static_cast<size_t>(std::min<uint64_t>(remaining, buffer.size()));
            read(buffer.data(), bytes);
            pageHasher.Update(buffer.data(), bytes);
            packageHasher.Update(buffer.data(), bytes);
            remaining -= bytes;
        }
        Require(pageHasher.Finalize() == page->hash, "VT page integrity mismatch");
        offset += page->size;
    }
    Require(offset == manifest.packageSize && atEnd(),
        "VT package trailing bytes/gaps");
    Require(packageHasher.Finalize() == manifest.packageHash, "VT package integrity mismatch");
}

void ValidatePackage(const ParsedManifest& manifest)
{
    RootedFileReader input(manifest.packagePath.parent_path(), manifest.packagePath.filename());
    Require(input.Size() == manifest.packageSize, "VT package size mismatch");
    ValidatePackageContents(manifest, [&](void* data, size_t size) {
        ReadExact(input, data, size);
    }, [&] {
        uint8_t extra{};
        return input.Read(&extra, 1) == 0;
    });
    input.ValidateIdentity();
}

std::filesystem::path InputPath(const std::filesystem::path& root,
    const TerrainCompositedProductionObject& object)
{
    PinnedFileDirectory parent(root);
    RootedFileReader input(parent.Path(), object.relativePath);
    Require(input.Size() == object.bytes, "VT production input size changed");
    return parent.Path() / object.relativePath;
}

std::vector<uint8_t> ConsumeInput(const std::filesystem::path& root,
    const TerrainCompositedProductionObject& object, bool capture, bool observe = false)
{
    const auto path = InputPath(root, object);
#if defined(VL_FILE_IO_TEST_HOOKS)
    if (observe && TerrainCompositedVTStoreTesting::InputObserver)
        TerrainCompositedVTStoreTesting::InputObserver(path, false);
#endif
    RootedFileReader input(root, object.relativePath);
#if defined(VL_FILE_IO_TEST_HOOKS)
    if (observe && TerrainCompositedVTStoreTesting::InputObserver)
        TerrainCompositedVTStoreTesting::InputObserver(path, true);
#endif
    Require(input.Size() == object.bytes, "VT production input handle size mismatch");
    Require(!capture || object.bytes <= SIZE_MAX, "VT production input read budget exceeded");
    std::vector<uint8_t> bytes;
    if (capture)
        bytes.resize(static_cast<size_t>(object.bytes));
    std::array<uint8_t, 64 * 1024> buffer{};
    ContentHasher hasher;
    uint64_t offset = 0;
    while (offset < object.bytes)
    {
        const auto count = static_cast<size_t>(std::min<uint64_t>(object.bytes - offset, buffer.size()));
        auto* destination = capture ? bytes.data() + static_cast<size_t>(offset) : buffer.data();
        const auto read = input.Read(destination, count);
        Require(read > 0, "Truncated VT production input");
        hasher.Update(destination, read);
        offset += read;
    }
    uint8_t extra = 0;
    Require(input.Read(&extra, 1) == 0 && hasher.Finalize() == object.digest,
        "VT production input integrity mismatch");
    input.ValidateIdentity();
    if (capture)
        ValidateTerrainCompositedProductionPayload(object, bytes);
    // 保留原路径规则，同时由仍存活的文件/祖先句柄绑定实际读取来源。
    InputPath(root, object);
    return bytes;
}

void ValidateInputs(const std::filesystem::path& root, const TerrainCompositedProductionClosure& production)
{
    for (const auto& object : production.objects)
    {
        ConsumeInput(root, object,
            object.baseCatalogDigest != ContentDigest{} || object.heightBlendCatalogDigest != ContentDigest{});
    }
}

void ValidateHeader(const std::filesystem::path& path, uint64_t bytes)
{
    RootedFileReader stream(path.parent_path(), path.filename());
    Require(stream.Size() == bytes, "VT package size mismatch");
    std::array<uint8_t, PackageHeader.size()> header{};
    ReadExact(stream, header.data(), header.size());
    Require(header == PackageHeader, "Invalid VT package header");
    stream.ValidateIdentity();
}

Json StrictParse(const std::vector<uint8_t>& bytes)
{
    std::vector<std::set<std::string>> objectKeys;
    return Json::parse(bytes.begin(), bytes.end(),
        [&objectKeys](int depth, Json::parse_event_t event, Json& value)
        {
            Require(depth <= 32, "VT manifest nesting limit exceeded");
            if (event == Json::parse_event_t::object_start)
            {
                objectKeys.emplace_back();
            }
            else if (event == Json::parse_event_t::key)
            {
                Require(!objectKeys.empty() &&
                    objectKeys.back().insert(value.get<std::string>()).second,
                    "Duplicate VT manifest field");
            }
            else if (event == Json::parse_event_t::object_end)
            {
                objectKeys.pop_back();
            }
            return true;
        });
}

}

void TerrainCompositedVTStore::Publish(
    const std::filesystem::path& manifestPath,
    const TerrainCompositedVTPublication& publication,
    const std::vector<TerrainCompositedVTPageData>& pages,
    const TerrainCompositedVTStoreLimits& limits)
{
    CheckLimits(limits);
    Require(pages.size() <= limits.maxPages, "VT page limit exceeded");
    std::map<std::string, const TerrainCompositedVTBuildDesc*> descriptions;
    for (const auto& desc : publication.surfaces)
    {
        desc.ContentHash();
        Require(descriptions.emplace(desc.surfaceId, &desc).second, "Duplicate VT surface");
    }
    Require(pages.size() == publication.packagedPages.size(), "VT supplied page set mismatch");
    std::map<TerrainCompositedVTPageKey, const std::vector<uint8_t>*> supplied;
    for (const auto& page : pages)
    {
        const auto desc = descriptions.find(page.key.surfaceId);
        Require(desc != descriptions.end() && desc->second->Layout().Contains(page.key),
            "Invalid VT publish page key");
        Require(page.bytes.size() == desc->second->PageBytes(), "Invalid VT publish page size");
        Require(supplied.emplace(page.key, &page.bytes).second, "Duplicate VT publish page");
    }
    for (const auto& key : publication.packagedPages)
        Require(supplied.count(key) == 1, "VT supplied page set mismatch");
    Publish(manifestPath, publication, PageProducer([&](const TerrainCompositedVTPageKey& key)
        { return *supplied.at(key); }), limits);
}

void TerrainCompositedVTStore::Publish(
    const std::filesystem::path& manifestPath,
    const TerrainCompositedVTPublication& publication,
    const PageProducer& producePage,
    const TerrainCompositedVTStoreLimits& limits)
{
    const auto& surfaces = publication.surfaces;
    CheckLimits(limits);
    RejectSymlink(manifestPath);
    Require(!surfaces.empty() && surfaces.size() <= limits.maxPages &&
        bool(producePage), "Invalid VT publish count/producer");
    std::map<std::string, const TerrainCompositedVTBuildDesc*> surfaceMap;
    Json surfaceArray = Json::array();
    Json roots = Json::array();
    const auto productionJson = TerrainCompositedProductionToJson(publication.production);
    Require(publication.packagedPages.size() <= limits.maxPages, "VT page limit exceeded");
    std::set<TerrainCompositedVTPageKey> selected(
        publication.packagedPages.begin(), publication.packagedPages.end());
    Require(selected.size() == publication.packagedPages.size(), "Duplicate VT selected page");
    uint64_t expectedBytes = PackageHeader.size();
    for (const auto& desc : surfaces)
    {
        const auto contentHash = desc.ContentHash();
        const auto layout = desc.Layout();
        Require(layout.Mip(0).pagesX <= 64 && layout.Mip(0).pagesY <= 64 &&
            layout.MipCount() <= 14, "VT surface exceeds supported feedback domain");
        Require(desc.dependencyHash == publication.production.SurfaceContentHash(desc.surfaceId),
            "VT publication production identity mismatch");
        const TerrainCompositedVTPageKey rootKey{desc.surfaceId, layout.MipCount() - 1, 0, 0};
        Require(selected.count(rootKey) == 1, "VT publication missing final root");
        roots.push_back({{"surfaceId", rootKey.surfaceId}, {"mip", rootKey.mip}, {"x", 0}, {"y", 0}});
        Require(surfaceMap.emplace(desc.surfaceId, &desc).second, "Duplicate VT surface");
        Json channels = Json::array();
        for (auto format : desc.channels)
        {
            channels.push_back(TerrainCompositedVTFormatName(format));
        }
        surfaceArray.push_back({{"surfaceId", desc.surfaceId}, {"width", desc.width},
            {"height", desc.height},
            {"addressMode", desc.addressMode == TerrainCompositedVTAddressMode::Clamp ? "clamp" : "repeat"},
            {"dependencyHash", desc.dependencyHash.ToHex()}, {"generatorVersion", desc.generatorVersion},
            {"encoding", desc.encoding}, {"channels", channels}, {"contentHash", contentHash.ToHex()},
            {"tailMip", layout.TailMip()}});
    }
    Require(surfaces.size() == publication.production.surfaces.size(), "VT production surface set mismatch");
    for (const auto& key : selected)
    {
        const auto found = surfaceMap.find(key.surfaceId);
        Require(found != surfaceMap.end() && found->second->Layout().Contains(key),
            "Invalid VT selected page");
        Require(found->second->PageBytes() <= limits.maxPackageBytes - expectedBytes,
            "VT package byte limit exceeded");
        expectedBytes += found->second->PageBytes();
    }
    uint64_t manifestBytes = surfaceArray.dump(2).size() + productionJson.dump(2).size() +
        roots.dump(2).size() + 1024;
    Require(manifestBytes <= limits.maxManifestBytes, "VT manifest byte limit exceeded");
    const auto root = manifestPath.has_parent_path() ? manifestPath.parent_path() : std::filesystem::path(".");
    std::filesystem::create_directories(root);
    PinnedFileDirectory pinnedRoot(root);
    const auto& canonicalRoot = pinnedRoot.Path();
    uint64_t inputBytes = 0;
    for (const auto& object : publication.production.objects)
    {
        Require(object.bytes <= limits.maxInputObjectBytes && object.bytes <= limits.maxInputBytes - inputBytes,
            "VT input byte limit exceeded");
        inputBytes += object.bytes;
        Require((canonicalRoot / object.relativePath) != (canonicalRoot / manifestPath.filename()),
            "VT input must not replace manifest");
    }
    ValidateInputs(canonicalRoot, publication.production);
#ifdef _WIN32
    ULARGE_INTEGER available{};
    Require(GetDiskFreeSpaceExW(canonicalRoot.c_str(), &available, nullptr, nullptr) != 0,
        "Failed to query VT staging disk space");
    Require(expectedBytes <= available.QuadPart, "Insufficient disk space for staged VT package");
#else
    Require(expectedBytes <= std::filesystem::space(canonicalRoot).available,
        "Insufficient disk space for staged VT package");
#endif
    StagedPackage staged(canonicalRoot);
    ContentHasher packageHasher;
    staged.Write(PackageHeader.data(), PackageHeader.size());
    packageHasher.Update(PackageHeader.data(), PackageHeader.size());
    uint64_t offset = PackageHeader.size();
    Json pageArray = Json::array();
    for (const auto& key : selected)
    {
        const auto& desc = *surfaceMap.at(key.surfaceId);
        const auto bytes = producePage(key);
        Require(bytes.size() == desc.PageBytes(), "VT producer returned invalid page size");
        staged.Write(bytes.data(), bytes.size());
        packageHasher.Update(bytes.data(), bytes.size());
        Json record{{"surfaceId", key.surfaceId}, {"mip", key.mip},
            {"x", key.x}, {"y", key.y}, {"offset", offset}, {"size", bytes.size()},
            {"hash", ContentHasher::HashBytes(bytes.data(), bytes.size()).ToHex()}};
        const uint64_t recordBytes = record.dump(2).size() + 32;
        Require(recordBytes <= limits.maxManifestBytes - manifestBytes,
            "VT manifest byte limit exceeded");
        manifestBytes += recordBytes;
        pageArray.push_back(std::move(record));
        offset += bytes.size();
    }
    Require(offset == expectedBytes, "VT streamed package size mismatch");
    staged.FlushAndRewind();
    const auto packageHash = packageHasher.Finalize();
    const auto filename = packageHash.ToHex() + ".vlcvt";
    Json document{{"kind", "terrain-composited-vt"}, {"version", Version},
        {"innerExtent", TerrainCompositedVTLayout::InnerExtent},
        {"border", TerrainCompositedVTLayout::Border},
        {"storedExtent", TerrainCompositedVTLayout::StoredExtent},
        {"mipRule", "floor-half-to-one"}, {"filterRule", TerrainCompositedFilterRule},
        {"package", filename}, {"packageBytes", expectedBytes}, {"packageHash", packageHash.ToHex()},
        {"surfaces", surfaceArray}, {"pages", pageArray}, {"roots", roots}, {"production", productionJson}};
    const auto text = document.dump(2);
    Require(text.size() <= limits.maxManifestBytes, "VT manifest byte limit exceeded");
    const auto parsed = ParseManifest(document, manifestPath, limits);
    for (const auto& object : publication.production.objects)
        Require((canonicalRoot / object.relativePath) != parsed.packagePath,
            "VT input must not alias page package");
    auto stagedManifest = parsed;
    stagedManifest.packagePath = staged.path;
    Require(std::filesystem::is_regular_file(staged.path) &&
        std::filesystem::file_size(staged.path) == stagedManifest.packageSize &&
        staged.Size() == stagedManifest.packageSize, "VT package size mismatch");
    ValidatePackageContents(stagedManifest, [&](void* data, size_t size) {
        staged.ReadExact(data, size);
    }, [&] {
        uint8_t extra = 0;
        return staged.Read(&extra, 1) == 0;
    });
    std::vector<std::unique_ptr<RootedFileReader>> retainedInputs;
    retainedInputs.reserve(publication.production.objects.size());
    for (const auto& object : publication.production.objects)
        retainedInputs.push_back(std::make_unique<RootedFileReader>(canonicalRoot, object.relativePath));
    ValidateInputs(canonicalRoot, publication.production);
    pinnedRoot.ValidateIdentity();
    if (!std::filesystem::exists(parsed.packagePath))
    {
        const auto error = staged.Publish(parsed.packagePath);
        if (error)
        {
            Require(std::filesystem::exists(parsed.packagePath), "Failed to publish staged VT package");
        }
    }
    RootedFileReader publishedPackage(parsed.packagePath.parent_path(), parsed.packagePath.filename());
    ValidatePackage(parsed);
    for (const auto& input : retainedInputs)
        input->ValidateIdentity();
    WriteTextFileAtomically(canonicalRoot / manifestPath.filename(), text);
}

TerrainCompositedVTStore TerrainCompositedVTStore::Open(
    const std::filesystem::path& manifestPath, const TerrainCompositedVTStoreLimits& limits)
{
    CheckLimits(limits);
    RejectSymlink(manifestPath);
    PinnedFileDirectory parent(manifestPath.has_parent_path() ?
        manifestPath.parent_path() : std::filesystem::path("."));
    std::vector<uint8_t> bytes;
    {
        RootedFileReader input(parent.Path(), manifestPath.filename());
        const auto size = input.Size();
        Require(size <= limits.maxManifestBytes, "VT manifest oversized");
        bytes.resize(static_cast<size_t>(size));
        ReadExact(input, bytes.data(), bytes.size());
        input.ValidateIdentity();
    }
    const auto parsed = ParseManifest(StrictParse(bytes), parent.Path() / manifestPath.filename(), limits);
    ValidateHeader(parsed.packagePath, parsed.packageSize);
    TerrainCompositedVTStore result;
    result.inputRoot = parent.Path();
    for (const auto& object : parsed.production.objects)
    {
        const auto path = result.inputRoot / object.relativePath;
        Require(path != (parent.Path() / manifestPath.filename()) &&
            path != parsed.packagePath, "VT input aliases manifest/package");
        InputPath(result.inputRoot, object);
    }
    result.packagePath = parsed.packagePath;
    result.packageSize = parsed.packageSize;
    result.packageHash = parsed.packageHash;
    result.production = parsed.production;
    result.limits = limits;
    result.surfaces = parsed.surfaces;
    result.pages = parsed.pages;
    return result;
}

std::vector<uint8_t> TerrainCompositedVTStore::ReadPage(
    const TerrainCompositedVTPageKey& key) const
{
    const auto found = pages.find(key);
    Require(found != pages.end(), "VT page not present in package");
    RejectLinkedAncestors(packagePath);
    RootedFileReader input(packagePath.parent_path(), packagePath.filename());
    Require(input.Size() == packageSize, "VT package changed after open");
    std::array<uint8_t, PackageHeader.size()> header{};
    ReadExact(input, header.data(), header.size());
    Require(header == PackageHeader, "VT package header changed after open");
    return ReadRange(input, found->second);
}

const TerrainCompositedVTBuildDesc& TerrainCompositedVTStore::Surface(const std::string& id) const
{
    for (const auto& surface : surfaces)
        if (surface.surfaceId == id)
            return surface;
    throw std::runtime_error("Unknown VT surface");
}

bool TerrainCompositedVTStore::HasPackagedPage(const TerrainCompositedVTPageKey& key) const
{
    Require(Surface(key.surfaceId).Layout().Contains(key), "Invalid VT page key");
    return pages.count(key) != 0;
}

std::vector<uint8_t> TerrainCompositedVTStore::ReadInput(const std::string& id) const
{
    const auto& object = production.Object(id);
    Require(object.bytes <= limits.maxInputObjectBytes && object.bytes <= SIZE_MAX,
        "VT input read budget exceeded");
    return ConsumeInput(inputRoot, object, true, true);
}

void TerrainCompositedVTStore::ValidateAll() const
{
    production.Validate();
    ValidateInputs(inputRoot, production);
    ParsedManifest parsed;
    parsed.packagePath = packagePath;
    parsed.packageSize = packageSize;
    parsed.packageHash = packageHash;
    parsed.pages = pages;
    RejectLinkedAncestors(packagePath);
    ValidateHeader(packagePath, packageSize);
    ValidatePackage(parsed);
}

}
