#include "terrain/terrainHeightmapWriter.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <half.h>

#include "resource/image/textureIO.h"
#include "shader/build/atomicFile.h"

namespace VL::TerrainHeightmapWriter
{
namespace
{

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0xffffffffu)
{
    static std::array<uint32_t, 256> table = []
    {
        std::array<uint32_t, 256> values{};
        for (uint32_t index = 0; index < 256; ++index)
        {
            uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1u) ? 0xedb88320u ^ (value >> 1) : value >> 1;
            values[index] = value;
        }
        return values;
    }();
    for (size_t index = 0; index < size; ++index)
        crc = table[(crc ^ data[index]) & 0xffu] ^ (crc >> 8);
    return crc;
}

void AppendU32(std::vector<uint8_t>& bytes, uint32_t value)
{
    bytes.push_back(static_cast<uint8_t>(value >> 24));
    bytes.push_back(static_cast<uint8_t>(value >> 16));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
    bytes.push_back(static_cast<uint8_t>(value));
}

void AppendChunk(std::vector<uint8_t>& png, const char* type,
    const std::vector<uint8_t>& data)
{
    AppendU32(png, static_cast<uint32_t>(data.size()));
    const size_t typeOffset = png.size();
    png.insert(png.end(), type, type + 4);
    png.insert(png.end(), data.begin(), data.end());
    const uint32_t crc = Crc32(png.data() + typeOffset, 4 + data.size()) ^ 0xffffffffu;
    AppendU32(png, crc);
}

uint32_t Adler32(const std::vector<uint8_t>& data)
{
    uint32_t a = 1, b = 0;
    for (uint8_t value : data)
    {
        a = (a + value) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

void AppendStoredZlib(
    std::vector<uint8_t>& output,
    const std::vector<uint8_t>& raw)
{
    output.push_back(0x78);
    output.push_back(0x01);
    size_t offset = 0;
    do
    {
        const size_t count = std::min<size_t>(65535, raw.size() - offset);
        const bool final = offset + count == raw.size();
        output.push_back(final ? 1 : 0);
        output.push_back(static_cast<uint8_t>(count));
        output.push_back(static_cast<uint8_t>(count >> 8));
        const uint16_t inverse = static_cast<uint16_t>(~count);
        output.push_back(static_cast<uint8_t>(inverse));
        output.push_back(static_cast<uint8_t>(inverse >> 8));
        output.insert(
            output.end(), raw.begin() + offset, raw.begin() + offset + count);
        offset += count;
    } while (offset < raw.size());
    const uint32_t adler = Adler32(raw);
    output.push_back(static_cast<uint8_t>(adler >> 24));
    output.push_back(static_cast<uint8_t>(adler >> 16));
    output.push_back(static_cast<uint8_t>(adler >> 8));
    output.push_back(static_cast<uint8_t>(adler));
}

std::vector<uint8_t> BuildGrayscalePng(
    const std::vector<uint8_t>& raw,
    uint32_t width,
    uint32_t height,
    uint8_t bitDepth)
{
    std::vector<uint8_t> zlib;
    AppendStoredZlib(zlib, raw);

    std::vector<uint8_t> png{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::vector<uint8_t> header;
    AppendU32(header, width);
    AppendU32(header, height);
    header.insert(header.end(), {bitDepth, 0, 0, 0, 0});
    AppendChunk(png, "IHDR", header);
    AppendChunk(png, "IDAT", zlib);
    AppendChunk(png, "IEND", {});
    return png;
}

std::vector<uint8_t> EncodePng8(
    const HostImage& image,
    uint32_t width,
    uint32_t height)
{
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + width));
    const float* samples = reinterpret_cast<const float*>(image.data.data());
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x)
        {
            raw.push_back(static_cast<uint8_t>(std::clamp(
                std::lround(samples[static_cast<size_t>(y) * width + x] * 255.0f),
                0l, 255l)));
        }
    }
    return BuildGrayscalePng(raw, width, height, 8);
}

std::vector<uint8_t> EncodePng16(
    const HostImage& image,
    uint32_t width,
    uint32_t height)
{
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(height) * (1 + static_cast<size_t>(width) * 2));
    const float* samples = reinterpret_cast<const float*>(image.data.data());
    for (uint32_t y = 0; y < height; ++y)
    {
        raw.push_back(0);
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint16_t value = static_cast<uint16_t>(std::clamp(
                std::lround(
                    samples[static_cast<size_t>(y) * width + x] * 65535.0f),
                0l, 65535l));
            // PNG 16-bit 样本固定大端；这里显式拆字节，不依赖宿主字节序。
            raw.push_back(static_cast<uint8_t>(value >> 8));
            raw.push_back(static_cast<uint8_t>(value & 0xffu));
        }
    }
    return BuildGrayscalePng(raw, width, height, 16);
}

bool ReadPngBitDepth(
    const std::filesystem::path& path,
    uint8_t& bitDepth)
{
    static constexpr std::array<uint8_t, 8> Signature = {
        0x89u, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return false;
    std::array<uint8_t, Signature.size()> signature{};
    stream.read(
        reinterpret_cast<char*>(signature.data()),
        static_cast<std::streamsize>(signature.size()));
    if (stream.gcount() != static_cast<std::streamsize>(signature.size()) ||
        signature != Signature)
    {
        return false;
    }

    std::array<uint8_t, 4> length{};
    stream.read(
        reinterpret_cast<char*>(length.data()),
        static_cast<std::streamsize>(length.size()));
    if (stream.gcount() != static_cast<std::streamsize>(length.size()))
        return false;
    const uint32_t chunkLength =
        (static_cast<uint32_t>(length[0]) << 24u) |
        (static_cast<uint32_t>(length[1]) << 16u) |
        (static_cast<uint32_t>(length[2]) << 8u) |
        static_cast<uint32_t>(length[3]);
    std::array<uint8_t, 4> type{};
    stream.read(
        reinterpret_cast<char*>(type.data()),
        static_cast<std::streamsize>(type.size()));
    if (stream.gcount() != static_cast<std::streamsize>(type.size()) ||
        chunkLength != 13 ||
        type[0] != 'I' || type[1] != 'H' || type[2] != 'D' || type[3] != 'R')
    {
        return false;
    }
    std::array<uint8_t, 13> header{};
    stream.read(
        reinterpret_cast<char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    if (stream.gcount() != static_cast<std::streamsize>(header.size()))
        return false;
    bitDepth = header[8];
    return true;
}

std::vector<uint8_t> EncodeBmp24(
    const HostImage& image,
    uint32_t width,
    uint32_t height)
{
    const uint32_t rowBytes = (width * 3u + 3u) & ~3u;
    const uint32_t pixelBytes = rowBytes * height;
    std::vector<uint8_t> bmp(54 + pixelBytes, 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    const uint32_t fileSize = static_cast<uint32_t>(bmp.size());
    std::memcpy(bmp.data() + 2, &fileSize, 4);
    const uint32_t offset = 54;
    std::memcpy(bmp.data() + 10, &offset, 4);
    const uint32_t infoSize = 40;
    std::memcpy(bmp.data() + 14, &infoSize, 4);
    std::memcpy(bmp.data() + 18, &width, 4);
    const int32_t signedHeight = static_cast<int32_t>(height);
    std::memcpy(bmp.data() + 22, &signedHeight, 4);
    const uint16_t planes = 1, bits = 24;
    std::memcpy(bmp.data() + 26, &planes, 2);
    std::memcpy(bmp.data() + 28, &bits, 2);
    std::memcpy(bmp.data() + 34, &pixelBytes, 4);
    const float* samples = reinterpret_cast<const float*>(image.data.data());
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t* row = bmp.data() + 54 + static_cast<size_t>(height - 1 - y) * rowBytes;
        for (uint32_t x = 0; x < width; ++x)
        {
            const uint8_t value = static_cast<uint8_t>(std::clamp(
                std::lround(samples[static_cast<size_t>(y) * width + x] * 255.0f),
                0l, 255l));
            row[x * 3 + 0] = value;
            row[x * 3 + 1] = value;
            row[x * 3 + 2] = value;
        }
    }
    return bmp;
}

HostImage MakeExrHeightImage(const HostImage& normalizedHeightmap)
{
    // TextureIO 的 EXR 写入器只接受 RGBA16_FLOAT；高度图输入是 R32_FLOAT。
    // 这里显式扩成四个 half 通道，R/G/B 都携带同一个高度样本，A=1。
    // EXR 分支不做 [0,1] clamp，保留编辑器中已有的 HDR/EXR 越界高度值。
    HostImage image;
    image.width = normalizedHeightmap.width;
    image.height = normalizedHeightmap.height;
    image.channels = 4;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.format = HostImage::PixelFormat::RGBA16_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = image.width * 8u;
    image.data.resize(
        static_cast<size_t>(image.rowStrideBytes) * image.height);

    const float* samples = reinterpret_cast<const float*>(
        normalizedHeightmap.data.data());
    uint16_t* destination = reinterpret_cast<uint16_t*>(image.data.data());
    const size_t sampleCount =
        static_cast<size_t>(image.width) * image.height;
    const uint16_t one = Imath::half(1.0f).bits();
    for (size_t index = 0; index < sampleCount; ++index)
    {
        const uint16_t value = Imath::half(samples[index]).bits();
        destination[index * 4 + 0] = value;
        destination[index * 4 + 1] = value;
        destination[index * 4 + 2] = value;
        destination[index * 4 + 3] = one;
    }
    return image;
}

} // namespace

bool Write(
    const std::filesystem::path& path,
    const HostImage& normalizedHeightmap,
    std::string& error)
{
    return Write(path, normalizedHeightmap, WriteOptions{}, error);
}

bool Write(
    const std::filesystem::path& path,
    const HostImage& normalizedHeightmap,
    const WriteOptions& options,
    std::string& error)
{
    error.clear();
    if (normalizedHeightmap.width == 0 || normalizedHeightmap.height == 0 ||
        normalizedHeightmap.format != HostImage::PixelFormat::R32_FLOAT ||
        normalizedHeightmap.channels != 1 ||
        normalizedHeightmap.mipLevels != 1 || normalizedHeightmap.arrayLayers != 1 ||
        normalizedHeightmap.data.size() !=
            static_cast<size_t>(normalizedHeightmap.width) *
            normalizedHeightmap.height * sizeof(float))
    {
        error = "Heightmap writer requires a single-layer R32_FLOAT image";
        return false;
    }
    try
    {
        std::string extension = path.extension().string();
        for (char& character : extension)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (extension == ".exr")
        {
            const HostImage exrImage =
                MakeExrHeightImage(normalizedHeightmap);
            TextureIO::SaveOptions saveOptions;
            saveOptions.format = TextureIO::FileFormat::Exr;
            if (!TextureIO::Save(path, exrImage, saveOptions))
            {
                error = "EXR heightmap write failed: " + path.string();
                return false;
            }
            return true;
        }
        std::vector<uint8_t> bytes;
        if (extension == ".png")
        {
            bool sixteenBit = options.pngBitDepth ==
                WriteOptions::PngBitDepth::Sixteen;
            if (options.pngBitDepth == WriteOptions::PngBitDepth::Auto)
            {
                uint8_t destinationBitDepth = 0;
                sixteenBit = ReadPngBitDepth(path, destinationBitDepth) &&
                    destinationBitDepth == 16;
            }
            bytes = sixteenBit
                ? EncodePng16(
                    normalizedHeightmap,
                    normalizedHeightmap.width,
                    normalizedHeightmap.height)
                : EncodePng8(
                    normalizedHeightmap,
                    normalizedHeightmap.width,
                    normalizedHeightmap.height);
        }
        else if (extension == ".bmp")
            bytes = EncodeBmp24(normalizedHeightmap,
                normalizedHeightmap.width, normalizedHeightmap.height);
        else
        {
            error = "Unsupported heightmap format (expected .png/.bmp/.exr): " +
                path.string();
            return false;
        }
        WriteBinaryFileAtomically(path, bytes);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = std::string("Heightmap write failed: ") + exception.what();
        return false;
    }
}

} // namespace VL::TerrainHeightmapWriter
