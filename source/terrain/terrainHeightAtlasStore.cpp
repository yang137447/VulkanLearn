#include "terrain/terrainHeightAtlasStore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include "core/tracingIfstream.h"
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

#include "shader/build/atomicFile.h"
#include "shader/build/contentHash.h"

namespace VL
{
namespace
{
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Layout = TerrainHeightAtlasLayout;
constexpr std::array<uint8_t, 8> Magic{'V', 'L', 'T', 'H', 'A', '0', '0', '1'};
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

void Require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error("Terrain tiling height atlas: " + message);
}

uint64_t Integer(const Json& value, uint64_t maximum)
{
    Require(value.is_number_integer() &&
        (value.is_number_unsigned() || value.get<int64_t>() >= 0), "invalid integer");
    const auto result = value.get<uint64_t>();
    Require(result <= maximum, "integer exceeds limit");
    return result;
}

void Fields(const Json& object, std::initializer_list<const char*> names)
{
    Require(object.is_object() && object.size() == names.size(), "invalid manifest fields");
    for (const auto* name : names)
        Require(object.contains(name), std::string("missing manifest field: ") + name);
}

void RejectSymlink(const fs::path& path)
{
    Require(!fs::is_symlink(fs::symlink_status(path)), "symlink file rejected: " + path.string());
}

std::vector<uint8_t> Read(const fs::path& path, uint64_t expected, bool exact)
{
    RejectSymlink(path);
    Require(fs::is_regular_file(path), "missing regular file: " + path.string());
    const auto size = fs::file_size(path);
    Require(exact ? size == expected : size <= expected, "file size mismatch: " + path.string());
    RuntimeFileTrace::TracingIfstream input(
        path, std::ios::binary, "TerrainHeightAtlasStore.Read");
    Require(bool(input), "cannot open file: " + path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!bytes.empty())
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(bool(input) && input.peek() == std::char_traits<char>::eof(),
        "file changed or unreadable: " + path.string());
    return bytes;
}

Json Parse(const std::vector<uint8_t>& bytes)
{
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes.begin(), bytes.end(),
        [&](int depth, Json::parse_event_t event, Json& value)
        {
            Require(depth <= 4, "manifest nesting limit exceeded");
            if (event == Json::parse_event_t::object_start)
                keys.emplace_back();
            else if (event == Json::parse_event_t::key)
                Require(!keys.empty() && keys.back().insert(value.get<std::string>()).second,
                    "duplicate manifest field");
            else if (event == Json::parse_event_t::object_end)
                keys.pop_back();
            return true;
        });
}

void Put(std::vector<uint8_t>& bytes, size_t offset, uint64_t value, uint32_t count)
{
    for (uint32_t index = 0; index < count; ++index)
        bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8));
}

uint64_t Get(const std::vector<uint8_t>& bytes, size_t offset, uint32_t count)
{
    uint64_t value = 0;
    for (uint32_t index = 0; index < count; ++index)
        value |= uint64_t(bytes[offset + index]) << (index * 8);
    return value;
}

fs::path PackagePath(const fs::path& manifestPath, const std::string& filename,
    const ContentDigest& hash)
{
    Require(filename == hash.ToHex() + ".height.bin", "invalid content-addressed package name");
    const auto root = fs::canonical(manifestPath.has_parent_path() ?
        manifestPath.parent_path() : fs::path("."));
    const auto path = root / filename;
    RejectSymlink(path);
    Require(fs::weakly_canonical(path).parent_path() == root &&
        fs::absolute(manifestPath).lexically_normal() != path, "package escapes manifest directory");
    return path;
}

uint16_t Pixel(const TerrainHeightAtlasStoreData& data, uint32_t slot,
    uint32_t localX, uint32_t localY)
{
    const auto columns = data.desc.width / Layout::PageExtent;
    const auto pixelX = (slot % columns) * Layout::PageExtent + localX;
    const auto pixelY = (slot / columns) * Layout::PageExtent + localY;
    const auto offset = (uint64_t(pixelY) * data.desc.width + pixelX) * 2;
    return uint16_t(data.atlas[offset]) | uint16_t(data.atlas[offset + 1]) << 8;
}
}

uint64_t TerrainHeightAtlasStorePayloadBytes(
    const TerrainHeightAtlasStoreDescription& desc, uint64_t budgetBytes)
{
    constexpr uint32_t MaxColumns = Layout::MaxExtent / Layout::PageExtent;
    Require(desc.width > 0 && desc.height > 0 &&
        desc.width <= Layout::MaxExtent && desc.height <= Layout::MaxExtent &&
        desc.width % Layout::PageExtent == 0 && desc.height % Layout::PageExtent == 0 &&
        desc.pageCount > 0 && desc.pageCount <= MaxColumns * MaxColumns &&
        desc.layerCount > 0 && desc.layerCount <= Layout::MaxExtent, "invalid atlas dimensions/counts");
    uint32_t columns = 1;
    while (columns * columns < desc.pageCount)
        ++columns;
    Require(desc.width == columns * Layout::PageExtent &&
        desc.height == ((desc.pageCount + columns - 1) / columns) * Layout::PageExtent,
        "noncanonical atlas allocation shape");
    const uint64_t payload = uint64_t(desc.width) * desc.height * 2 + uint64_t(desc.layerCount) * 16;
    Require(payload <= budgetBytes && payload <= TerrainHeightAtlasStoreMaxPayloadBytes,
        "resident payload exceeds budget");
    Require(payload * 3 + TerrainHeightAtlasStoreMaxManifestBytes + TerrainHeightAtlasStoreHeaderBytes <=
        TerrainHeightAtlasStoreMaxWorkingBytes, "CPU working memory exceeds budget");
    return payload;
}

void ValidateTerrainHeightAtlasStoreData(
    const TerrainHeightAtlasStoreData& data, uint64_t budgetBytes)
{
    TerrainHeightAtlasStorePayloadBytes(data.desc, budgetBytes);
    Require(data.atlas.size() == uint64_t(data.desc.width) * data.desc.height * 2 &&
        data.layerMap.size() == data.desc.layerCount, "atlas/LUT byte count mismatch");
    std::vector<int32_t> owners(data.desc.pageCount, -1);
    uint32_t mappingIndex = 0;
    for (const auto& mapping : data.layerMap)
    {
        bool empty = true;
        for (const float component : mapping)
        {
            Require(std::isfinite(component) && component >= 0 &&
                component <= Layout::MaxExtent && component == std::floor(component),
                "noninteger/nonfinite/out-of-range LUT entry");
            empty &= component == 0;
        }
        if (empty)
        {
            ++mappingIndex;
            continue;
        }
        const auto base = static_cast<uint32_t>(mapping[0]);
        const auto pagesX = static_cast<uint32_t>(mapping[1]);
        const auto width = static_cast<uint32_t>(mapping[2]);
        const auto height = static_cast<uint32_t>(mapping[3]);
        Require(width > 0 && height > 0 && pagesX == (width + Layout::Interior - 1) / Layout::Interior,
            "invalid height source LUT dimensions");
        const auto pagesY = (height + Layout::Interior - 1) / Layout::Interior;
        const uint64_t count = uint64_t(pagesX) * pagesY;
        Require(base < data.desc.pageCount && count <= data.desc.pageCount - base,
            "LUT page range exceeds atlas");
        if (owners[base] >= 0)
        {
            Require(data.layerMap[owners[base]] == mapping, "overlapping height source ranges");
            ++mappingIndex;
            continue;
        }
        for (uint32_t page = 0; page < count; ++page)
        {
            Require(owners[base + page] == -1, "overlapping height source pages");
            owners[base + page] = static_cast<int32_t>(mappingIndex);
            const auto pageX = page % pagesX;
            const auto pageY = page / pagesX;
            for (uint32_t localY = 0; localY < Layout::PageExtent; ++localY)
                for (uint32_t localX = 0; localX < Layout::PageExtent; ++localX)
                {
                    const auto sourceX = (pageX * Layout::Interior + localX +
                        width - Layout::Border % width) % width;
                    const auto sourceY = (pageY * Layout::Interior + localY +
                        height - Layout::Border % height) % height;
                    const auto sourceSlot = base + (sourceY / Layout::Interior) * pagesX +
                        sourceX / Layout::Interior;
                    if (Pixel(data, base + page, localX, localY) !=
                        Pixel(data, sourceSlot, sourceX % Layout::Interior + Layout::Border,
                            sourceY % Layout::Interior + Layout::Border))
                        Require(false, "repeat border/partial page differs from source interior");
                }
        }
        ++mappingIndex;
    }
    Require(std::all_of(owners.begin(), owners.end(), [](int32_t owner) { return owner >= 0; }),
        "unmapped height atlas pages");
}

void WriteTerrainHeightAtlasStore(const fs::path& manifestPath, const TerrainHeightAtlasStoreData& data)
{
    ValidateTerrainHeightAtlasStoreData(data, TerrainHeightAtlasStoreMaxPayloadBytes);
    RejectSymlink(manifestPath);
    const auto& desc = data.desc;
    const auto atlasBytes = data.atlas.size();
    const auto lutBytes = uint64_t(desc.layerCount) * 16;
    std::vector<uint8_t> package(TerrainHeightAtlasStoreHeaderBytes + atlasBytes + lutBytes, 0);
    std::copy(Magic.begin(), Magic.end(), package.begin());
    Put(package, 8, 1, 4);
    Put(package, 12, Layout::SourceMip, 4);
    Put(package, 16, desc.width, 4);
    Put(package, 20, desc.height, 4);
    Put(package, 24, desc.pageCount, 4);
    Put(package, 28, desc.layerCount, 4);
    Put(package, 32, Layout::Interior, 4);
    Put(package, 36, Layout::Border, 4);
    Put(package, 40, atlasBytes, 8);
    Put(package, 48, lutBytes, 8);
    std::copy(data.atlas.begin(), data.atlas.end(),
        package.begin() + TerrainHeightAtlasStoreHeaderBytes);
    size_t offset = TerrainHeightAtlasStoreHeaderBytes + atlasBytes;
    for (const auto& mapping : data.layerMap)
        for (const float component : mapping)
        {
            uint32_t bits = 0;
            std::memcpy(&bits, &component, 4);
            Put(package, offset, bits, 4);
            offset += 4;
        }
    const auto hash = ContentHasher::HashBytes(package.data(), package.size());
    const auto filename = hash.ToHex() + ".height.bin";
    Json manifest{
        {"kind", "terrain-tiling-height-atlas"}, {"version", 1}, {"sourceMip", Layout::SourceMip},
        {"innerExtent", Layout::Interior}, {"border", Layout::Border}, {"storedExtent", Layout::PageExtent},
        {"addressMode", "repeat"}, {"filter", "linear-lod0"},
        {"width", desc.width}, {"height", desc.height}, {"pageCount", desc.pageCount},
        {"layerCount", desc.layerCount}, {"atlasFormat", "r16-unorm"}, {"lutFormat", "rgba32-float"},
        {"atlasBytes", atlasBytes}, {"lutBytes", lutBytes}, {"package", filename},
        {"packageBytes", package.size()}, {"packageHash", hash.ToHex()}};
    const auto text = manifest.dump(2);
    Require(text.size() <= TerrainHeightAtlasStoreMaxManifestBytes, "manifest exceeds byte budget");
    fs::create_directories(manifestPath.has_parent_path() ? manifestPath.parent_path() : fs::path("."));
    const auto packagePath = PackagePath(manifestPath, filename, hash);
    if (fs::exists(packagePath))
        Require(fs::is_regular_file(packagePath) && fs::file_size(packagePath) == package.size() &&
            ContentHasher::HashFile(packagePath) == hash, "existing content-addressed binary is corrupt");
    else
        WriteBinaryFileAtomically(packagePath, package);
    Require(ContentHasher::HashFile(packagePath) == hash, "published binary hash mismatch");
    WriteTextFileAtomically(manifestPath, text);
}

TerrainHeightAtlasStoreData ReadTerrainHeightAtlasStore(const fs::path& manifestPath, uint64_t budgetBytes)
{
    try
    {
        const auto manifest = Parse(Read(manifestPath, TerrainHeightAtlasStoreMaxManifestBytes, false));
        Fields(manifest, {"kind", "version", "sourceMip", "innerExtent", "border", "storedExtent",
            "addressMode", "filter", "width", "height", "pageCount", "layerCount",
            "atlasFormat", "lutFormat", "atlasBytes", "lutBytes", "package", "packageBytes", "packageHash"});
        Require(manifest.at("kind") == "terrain-tiling-height-atlas" &&
            Integer(manifest.at("version"), 1) == 1 &&
            Integer(manifest.at("sourceMip"), Layout::SourceMip) == Layout::SourceMip &&
            Integer(manifest.at("innerExtent"), Layout::Interior) == Layout::Interior &&
            Integer(manifest.at("border"), Layout::Border) == Layout::Border &&
            Integer(manifest.at("storedExtent"), Layout::PageExtent) == Layout::PageExtent &&
            manifest.at("addressMode") == "repeat" && manifest.at("filter") == "linear-lod0" &&
            manifest.at("atlasFormat") == "r16-unorm" && manifest.at("lutFormat") == "rgba32-float",
            "unsupported height atlas protocol/layout/format");
        TerrainHeightAtlasStoreData result;
        auto& desc = result.desc;
        desc.width = static_cast<uint32_t>(Integer(manifest.at("width"), Layout::MaxExtent));
        desc.height = static_cast<uint32_t>(Integer(manifest.at("height"), Layout::MaxExtent));
        desc.pageCount = static_cast<uint32_t>(Integer(manifest.at("pageCount"), 900));
        desc.layerCount = static_cast<uint32_t>(Integer(manifest.at("layerCount"), Layout::MaxExtent));
        const auto payloadBytes = TerrainHeightAtlasStorePayloadBytes(desc, budgetBytes);
        const uint64_t atlasBytes = uint64_t(desc.width) * desc.height * 2;
        const uint64_t lutBytes = uint64_t(desc.layerCount) * 16;
        Require(Integer(manifest.at("atlasBytes"), TerrainHeightAtlasStoreMaxPayloadBytes) == atlasBytes &&
            Integer(manifest.at("lutBytes"), TerrainHeightAtlasStoreMaxPayloadBytes) == lutBytes &&
            Integer(manifest.at("packageBytes"), TerrainHeightAtlasStoreMaxPayloadBytes +
                TerrainHeightAtlasStoreHeaderBytes) == payloadBytes + TerrainHeightAtlasStoreHeaderBytes,
            "manifest payload length mismatch");
        const auto textHash = manifest.at("packageHash").get<std::string>();
        const auto hash = ContentDigest::FromHex(textHash);
        Require(hash.ToHex() == textHash, "noncanonical binary hash");
        result.packagePath = PackagePath(manifestPath, manifest.at("package").get<std::string>(), hash);
        const auto package = Read(result.packagePath, payloadBytes + TerrainHeightAtlasStoreHeaderBytes, true);
        Require(ContentHasher::HashBytes(package.data(), package.size()) == hash, "binary hash mismatch");
        Require(std::equal(Magic.begin(), Magic.end(), package.begin()) &&
            Get(package, 8, 4) == 1 && Get(package, 12, 4) == Layout::SourceMip &&
            Get(package, 16, 4) == desc.width && Get(package, 20, 4) == desc.height &&
            Get(package, 24, 4) == desc.pageCount && Get(package, 28, 4) == desc.layerCount &&
            Get(package, 32, 4) == Layout::Interior && Get(package, 36, 4) == Layout::Border &&
            Get(package, 40, 8) == atlasBytes && Get(package, 48, 8) == lutBytes &&
            Get(package, 56, 8) == 0, "binary header disagrees with manifest");
        result.atlas.assign(package.begin() + TerrainHeightAtlasStoreHeaderBytes,
            package.begin() + TerrainHeightAtlasStoreHeaderBytes + atlasBytes);
        result.layerMap.resize(desc.layerCount);
        size_t offset = TerrainHeightAtlasStoreHeaderBytes + atlasBytes;
        for (auto& mapping : result.layerMap)
            for (float& component : mapping)
            {
                const auto bits = static_cast<uint32_t>(Get(package, offset, 4));
                std::memcpy(&component, &bits, 4);
                offset += 4;
            }
        ValidateTerrainHeightAtlasStoreData(result, budgetBytes);
        return result;
    }
    catch (const std::exception& error)
    {
        throw std::runtime_error("Height atlas manifest " + manifestPath.string() + ": " + error.what());
    }
}

fs::path TerrainHeightAtlasStorePackagePath(const fs::path& manifestPath)
{
    return ReadTerrainHeightAtlasStore(manifestPath, TerrainHeightAtlasStoreMaxPayloadBytes).packagePath;
}
}
