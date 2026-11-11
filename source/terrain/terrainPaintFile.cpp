#include "terrain/terrainPaintFile.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>
#include <system_error>
#include <utility>
#include <vector>

#include "shader/build/atomicFile.h"

namespace VL
{
namespace
{

constexpr char PaintMagic[4] = {'V', 'L', 'P', '1'};
constexpr uint32_t PaintHeaderBytes = 24;
constexpr uint32_t PaintMinDimension = 2;
constexpr uint32_t PaintMaxDimension = 4096;
constexpr uint32_t PaintMinLayerCount = 1;
constexpr uint32_t PaintMaxLayerCount = 256;
// 编辑器实际使用的 256²×256 也远小于该上限；限制总量是为了让损坏/恶意头部
// 在分配前失败，而不是尝试申请数十 GiB。
constexpr uint64_t PaintMaxFileBytes = 512ull * 1024ull * 1024ull;

static_assert(sizeof(float) == 4, "VLP1 stores 32-bit IEEE floats");

void AppendU32Le(std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 24));
}

uint32_t ReadU32Le(const uint8_t* data)
{
    return static_cast<uint32_t>(data[0]) |
        (static_cast<uint32_t>(data[1]) << 8) |
        (static_cast<uint32_t>(data[2]) << 16) |
        (static_cast<uint32_t>(data[3]) << 24);
}

uint32_t FloatToBits(float value)
{
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

float BitsToFloat(uint32_t bits)
{
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

bool CheckedMultiply(size_t left, size_t right, size_t& out)
{
    if (left != 0 && right > std::numeric_limits<size_t>::max() / left)
        return false;
    out = left * right;
    return true;
}

bool CheckedAdd(size_t left, size_t right, size_t& out)
{
    if (right > std::numeric_limits<size_t>::max() - left)
        return false;
    out = left + right;
    return true;
}

std::string PaintError(
    const std::filesystem::path& path,
    const std::string& message)
{
    return "Terrain paint file " + path.string() + ": " + message;
}

bool ValidateShape(
    uint32_t width,
    uint32_t height,
    uint32_t layerCount,
    const std::filesystem::path& path,
    std::string& error)
{
    if (width != height)
    {
        error = PaintError(path, "width and height must match (non-square paint maps are not supported)");
        return false;
    }
    if (width < PaintMinDimension || width > PaintMaxDimension)
    {
        error = PaintError(path,
            "dimensions must be in [" + std::to_string(PaintMinDimension) + ", " +
            std::to_string(PaintMaxDimension) + "]");
        return false;
    }
    if (layerCount < PaintMinLayerCount || layerCount > PaintMaxLayerCount)
    {
        error = PaintError(path,
            "layerCount must be in [" + std::to_string(PaintMinLayerCount) + ", " +
            std::to_string(PaintMaxLayerCount) + "]");
        return false;
    }
    return true;
}

} // namespace

bool ResolveTerrainPaintPath(
    const std::filesystem::path& resourceRoot,
    const std::string& relative,
    std::filesystem::path& out,
    std::string& error)
{
    error.clear();
    out.clear();
    if (relative.empty())
    {
        error = "Terrain paintPath is empty";
        return false;
    }
    const std::filesystem::path relativePath(relative);
    if (relativePath.is_absolute() || relativePath.has_root_name() ||
        relativePath.has_root_directory())
    {
        error = "Terrain paintPath must be resource-root relative: " + relative;
        return false;
    }
    std::error_code ec;
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(resourceRoot, ec);
    if (ec)
    {
        error = "Terrain paint root cannot be resolved: " + resourceRoot.string();
        return false;
    }
    const std::filesystem::path target =
        std::filesystem::weakly_canonical(root / relativePath, ec);
    if (ec)
    {
        error = "Terrain paintPath cannot be resolved: " + relative;
        return false;
    }
    const std::filesystem::path relativeToRoot =
        target.lexically_relative(root);
    if (relativeToRoot.empty() || relativeToRoot.is_absolute())
    {
        error = "Terrain paintPath escapes the resource root: " + relative;
        return false;
    }
    for (const std::filesystem::path& part : relativeToRoot)
    {
        if (part == "..")
        {
            error = "Terrain paintPath escapes the resource root: " + relative;
            return false;
        }
    }
    out = target;
    return true;
}

bool SaveTerrainLayerPaintFile(
    const std::filesystem::path& path,
    const TerrainLayerPaintData& paint,
    std::string& error)
{
    error.clear();
    if (paint.Empty())
    {
        error = PaintError(path, "paint data is empty or its array sizes do not match its dimensions");
        return false;
    }
    if (!ValidateShape(paint.width, paint.height, paint.layerCount, path, error))
        return false;

    size_t sampleCount = 0;
    if (!CheckedMultiply(static_cast<size_t>(paint.width), paint.height, sampleCount) ||
        !CheckedMultiply(sampleCount, paint.layerCount, sampleCount))
    {
        error = PaintError(path, "paint sample count overflows addressable memory");
        return false;
    }

    for (size_t index = 0; index < paint.weights.size(); ++index)
    {
        const float weight = paint.weights[index];
        if (!std::isfinite(weight) || weight < 0.0f || weight > 1.0f)
        {
            error = PaintError(path,
                "weight[" + std::to_string(index) + "] must be finite and in [0, 1]");
            return false;
        }
    }

    size_t weightBytes = 0;
    size_t bodyBytes = 0;
    size_t totalBytes = 0;
    if (!CheckedMultiply(sampleCount, sizeof(float), weightBytes) ||
        !CheckedAdd(weightBytes, sampleCount, bodyBytes) ||
        !CheckedAdd(PaintHeaderBytes, bodyBytes, totalBytes))
    {
        error = PaintError(path, "encoded file size overflows addressable memory");
        return false;
    }
    if (totalBytes > PaintMaxFileBytes)
    {
        error = PaintError(path, "encoded file exceeds the 512 MiB paint budget");
        return false;
    }

    try
    {
        std::vector<uint8_t> bytes;
        bytes.reserve(totalBytes);
        bytes.insert(bytes.end(), PaintMagic, PaintMagic + 4);
        AppendU32Le(bytes, paint.width);
        AppendU32Le(bytes, paint.height);
        AppendU32Le(bytes, paint.layerCount);
        AppendU32Le(bytes, 0); // flags
        AppendU32Le(bytes, 0); // reserved
        for (float weight : paint.weights)
            AppendU32Le(bytes, FloatToBits(weight));
        bytes.insert(bytes.end(), paint.opacity.begin(), paint.opacity.end());
        WriteBinaryFileAtomically(path, bytes);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = PaintError(path, std::string("write failed: ") + exception.what());
        return false;
    }
}

bool LoadTerrainLayerPaintFile(
    const std::filesystem::path& path,
    uint32_t expectedLayerCount,
    TerrainLayerPaintData& out,
    std::string& error)
{
    error.clear();

    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input.is_open())
    {
        error = PaintError(path, "cannot open file");
        return false;
    }
    const std::streamoff endPosition = input.tellg();
    if (endPosition < 0)
    {
        error = PaintError(path, "cannot measure file size");
        return false;
    }
    const uint64_t fileSize = static_cast<uint64_t>(endPosition);
    if (fileSize < PaintHeaderBytes)
    {
        error = PaintError(path, "file is truncated (missing VLP1 header)");
        return false;
    }
    input.seekg(0, std::ios::beg);

    uint8_t header[PaintHeaderBytes] = {};
    input.read(reinterpret_cast<char*>(header), sizeof(header));
    if (!input)
    {
        error = PaintError(path, "failed to read VLP1 header");
        return false;
    }
    if (std::memcmp(header, PaintMagic, 4) != 0)
    {
        error = PaintError(path, "invalid magic (expected VLP1)");
        return false;
    }

    const uint32_t width = ReadU32Le(header + 4);
    const uint32_t height = ReadU32Le(header + 8);
    const uint32_t layerCount = ReadU32Le(header + 12);
    if (ReadU32Le(header + 16) != 0 || ReadU32Le(header + 20) != 0)
    {
        error = PaintError(path, "unsupported flags/reserved bits (v1 requires zero)");
        return false;
    }
    if (!ValidateShape(width, height, layerCount, path, error))
        return false;
    if (layerCount != expectedLayerCount)
    {
        error = PaintError(path,
            "layerCount " + std::to_string(layerCount) +
            " does not match the terrain layer table (" +
            std::to_string(expectedLayerCount) + ")");
        return false;
    }

    size_t sampleCount = 0;
    if (!CheckedMultiply(static_cast<size_t>(width), height, sampleCount) ||
        !CheckedMultiply(sampleCount, layerCount, sampleCount))
    {
        error = PaintError(path, "paint sample count overflows addressable memory");
        return false;
    }
    size_t weightBytes = 0;
    size_t bodyBytes = 0;
    size_t expectedSize = 0;
    if (!CheckedMultiply(sampleCount, sizeof(float), weightBytes) ||
        !CheckedAdd(weightBytes, sampleCount, bodyBytes) ||
        !CheckedAdd(PaintHeaderBytes, bodyBytes, expectedSize))
    {
        error = PaintError(path, "decoded file size overflows addressable memory");
        return false;
    }
    if (expectedSize > PaintMaxFileBytes)
    {
        error = PaintError(path, "decoded file exceeds the 512 MiB paint budget");
        return false;
    }
    if (fileSize != static_cast<uint64_t>(expectedSize))
    {
        error = PaintError(path,
            "size mismatch: expected " + std::to_string(expectedSize) +
            " bytes, found " + std::to_string(fileSize));
        return false;
    }

    std::vector<float> weights;
    std::vector<uint8_t> opacity;
    try
    {
        weights.resize(sampleCount);
        opacity.resize(sampleCount);
        std::vector<uint8_t> rawWeights(weightBytes);
        input.read(reinterpret_cast<char*>(rawWeights.data()),
            static_cast<std::streamsize>(rawWeights.size()));
        if (!input)
        {
            error = PaintError(path, "file is truncated while reading weights");
            return false;
        }
        for (size_t index = 0; index < sampleCount; ++index)
        {
            const float weight = BitsToFloat(ReadU32Le(rawWeights.data() + index * 4));
            if (!std::isfinite(weight) || weight < 0.0f || weight > 1.0f)
            {
                error = PaintError(path,
                    "weight[" + std::to_string(index) +
                    "] must be finite and in [0, 1]");
                return false;
            }
            weights[index] = weight;
        }
        input.read(reinterpret_cast<char*>(opacity.data()),
            static_cast<std::streamsize>(opacity.size()));
        if (!input)
        {
            error = PaintError(path, "file is truncated while reading opacity");
            return false;
        }
    }
    catch (const std::bad_alloc&)
    {
        error = PaintError(path, "allocation failed for the decoded paint snapshot");
        return false;
    }

    out.width = width;
    out.height = height;
    out.layerCount = layerCount;
    out.weights = std::move(weights);
    out.opacity = std::move(opacity);
    return true;
}

} // namespace VL
