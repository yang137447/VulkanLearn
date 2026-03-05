#include "textureIO.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

namespace
{
    bool ShouldFlipY(TextureIO::LoadOptions::FlipYMode mode)
    {
        if (mode == TextureIO::LoadOptions::FlipYMode::ForceOn)
        {
            return true;
        }
        return false;
    }

    void FlipImageY(HostImage& image)
    {
        if (image.height <= 1)
        {
            return;
        }
        const size_t rowBytes = image.rowStrideBytes > 0 ? image.rowStrideBytes : image.width * image.GetPixelSize();
        std::vector<uint8_t> rowBuffer(rowBytes);
        uint8_t* raw = image.data.data();
        for (uint32_t y = 0; y < image.height / 2; ++y)
        {
            uint8_t* rowTop = raw + static_cast<size_t>(y) * rowBytes;
            uint8_t* rowBottom = raw + static_cast<size_t>(image.height - 1 - y) * rowBytes;
            std::memcpy(rowBuffer.data(), rowTop, rowBytes);
            std::memcpy(rowTop, rowBottom, rowBytes);
            std::memcpy(rowBottom, rowBuffer.data(), rowBytes);
        }
    }

    TextureIO::FileFormat FileFormatFromPath(const std::filesystem::path& path)
    {
        std::string ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".png") return TextureIO::FileFormat::Png;
        if (ext == ".jpg" || ext == ".jpeg") return TextureIO::FileFormat::Jpg;
        if (ext == ".tga") return TextureIO::FileFormat::Tga;
        if (ext == ".bmp") return TextureIO::FileFormat::Bmp;
        if (ext == ".hdr") return TextureIO::FileFormat::Hdr;
        if (ext == ".exr") return TextureIO::FileFormat::Exr;
        if (ext == ".ktx2") return TextureIO::FileFormat::Ktx2;
        if (ext == ".dds") return TextureIO::FileFormat::Dds;
        throw std::runtime_error("Unsupported image file extension: " + path.string());
    }

    HostImage::PixelFormat ResolvePixelFormat(TextureIO::LoadOptions::Transfer transfer)
    {
        const bool useSrgb = transfer == TextureIO::LoadOptions::Transfer::SRGB;
        return useSrgb ? HostImage::PixelFormat::RGBA8_SRGB : HostImage::PixelFormat::RGBA8_UNORM;
    }

    template<typename T>
    void WriteValue(std::ofstream& stream, const T& value)
    {
        stream.write(reinterpret_cast<const char*>(&value), sizeof(T));
    }

    void WriteCString(std::ofstream& stream, const char* value)
    {
        stream.write(value, std::strlen(value) + 1);
    }

    void WriteAttribute(std::ofstream& stream, const char* name, const char* type, const std::vector<char>& data)
    {
        WriteCString(stream, name);
        WriteCString(stream, type);
        int32_t size = static_cast<int32_t>(data.size());
        WriteValue(stream, size);
        if (!data.empty())
        {
            stream.write(data.data(), data.size());
        }
    }

    std::vector<char> MakeChannelsAttributeData(bool rgOnly)
    {
        std::vector<char> data;
        if (rgOnly)
        {
            const std::array<const char*, 2> channelNames = { "G", "R" };
            for (const char* channelName : channelNames)
            {
                const size_t nameLength = std::strlen(channelName) + 1;
                data.insert(data.end(), channelName, channelName + nameLength);
                int32_t pixelType = 1;
                data.insert(data.end(), reinterpret_cast<char*>(&pixelType), reinterpret_cast<char*>(&pixelType) + sizeof(pixelType));
                data.insert(data.end(), 4, 0);
                int32_t sampling = 1;
                data.insert(data.end(), reinterpret_cast<char*>(&sampling), reinterpret_cast<char*>(&sampling) + sizeof(sampling));
                data.insert(data.end(), reinterpret_cast<char*>(&sampling), reinterpret_cast<char*>(&sampling) + sizeof(sampling));
            }
        }
        else
        {
            const std::array<const char*, 4> channelNames = { "A", "B", "G", "R" };
            for (const char* channelName : channelNames)
            {
                const size_t nameLength = std::strlen(channelName) + 1;
                data.insert(data.end(), channelName, channelName + nameLength);
                int32_t pixelType = 1;
                data.insert(data.end(), reinterpret_cast<char*>(&pixelType), reinterpret_cast<char*>(&pixelType) + sizeof(pixelType));
                data.insert(data.end(), 4, 0);
                int32_t sampling = 1;
                data.insert(data.end(), reinterpret_cast<char*>(&sampling), reinterpret_cast<char*>(&sampling) + sizeof(sampling));
                data.insert(data.end(), reinterpret_cast<char*>(&sampling), reinterpret_cast<char*>(&sampling) + sizeof(sampling));
            }
        }
        data.push_back(0);
        return data;
    }

    std::vector<char> MakeBox2iAttributeData(int32_t width, int32_t height)
    {
        std::vector<char> data;
        int32_t minX = 0;
        int32_t minY = 0;
        int32_t maxX = width - 1;
        int32_t maxY = height - 1;
        data.insert(data.end(), reinterpret_cast<char*>(&minX), reinterpret_cast<char*>(&minX) + sizeof(minX));
        data.insert(data.end(), reinterpret_cast<char*>(&minY), reinterpret_cast<char*>(&minY) + sizeof(minY));
        data.insert(data.end(), reinterpret_cast<char*>(&maxX), reinterpret_cast<char*>(&maxX) + sizeof(maxX));
        data.insert(data.end(), reinterpret_cast<char*>(&maxY), reinterpret_cast<char*>(&maxY) + sizeof(maxY));
        return data;
    }

    bool SaveExrHalf(const std::filesystem::path& outputPath, const HostImage& image, bool rgOnly)
    {
        if (image.format != HostImage::PixelFormat::RGBA16_FLOAT)
        {
            return false;
        }

        std::filesystem::path parentPath = outputPath.parent_path();
        if (!parentPath.empty())
        {
            std::filesystem::create_directories(parentPath);
        }

        std::ofstream stream(outputPath, std::ios::binary);
        if (!stream.is_open())
        {
            return false;
        }

        const uint32_t width = image.width;
        const uint32_t height = image.height;
        const uint16_t* pixels = reinterpret_cast<const uint16_t*>(image.data.data());

        uint32_t magic = 20000630u;
        uint32_t version = 2u;
        WriteValue(stream, magic);
        WriteValue(stream, version);
        WriteAttribute(stream, "channels", "chlist", MakeChannelsAttributeData(rgOnly));
        WriteAttribute(stream, "compression", "compression", std::vector<char>{0});
        WriteAttribute(stream, "dataWindow", "box2i", MakeBox2iAttributeData(static_cast<int32_t>(width), static_cast<int32_t>(height)));
        WriteAttribute(stream, "displayWindow", "box2i", MakeBox2iAttributeData(static_cast<int32_t>(width), static_cast<int32_t>(height)));
        WriteAttribute(stream, "lineOrder", "lineOrder", std::vector<char>{0});
        float pixelAspectRatio = 1.0f;
        WriteAttribute(stream, "pixelAspectRatio", "float", std::vector<char>(reinterpret_cast<char*>(&pixelAspectRatio), reinterpret_cast<char*>(&pixelAspectRatio) + sizeof(pixelAspectRatio)));
        float screenCenter[2] = { 0.0f, 0.0f };
        WriteAttribute(stream, "screenWindowCenter", "v2f", std::vector<char>(reinterpret_cast<char*>(screenCenter), reinterpret_cast<char*>(screenCenter) + sizeof(screenCenter)));
        float screenWidth = 1.0f;
        WriteAttribute(stream, "screenWindowWidth", "float", std::vector<char>(reinterpret_cast<char*>(&screenWidth), reinterpret_cast<char*>(&screenWidth) + sizeof(screenWidth)));
        stream.put('\0');

        const uint32_t channelCount = rgOnly ? 2 : 4;
        const uint64_t lineOffsetTablePosition = static_cast<uint64_t>(stream.tellp());
        const uint32_t scanlineDataSize = width * channelCount * sizeof(uint16_t);
        const uint32_t chunkSize = sizeof(int32_t) + sizeof(uint32_t) + scanlineDataSize;
        const uint64_t firstChunkOffset = lineOffsetTablePosition + static_cast<uint64_t>(height) * sizeof(uint64_t);
        for (uint32_t y = 0; y < height; ++y)
        {
            uint64_t offset = firstChunkOffset + static_cast<uint64_t>(y) * chunkSize;
            WriteValue(stream, offset);
        }

        std::vector<uint16_t> scanline(scanlineDataSize / sizeof(uint16_t));
        for (uint32_t y = 0; y < height; ++y)
        {
            int32_t scanlineY = static_cast<int32_t>(y);
            WriteValue(stream, scanlineY);
            WriteValue(stream, scanlineDataSize);

            const uint16_t* srcRow = pixels + static_cast<size_t>(y) * width * 4;
            if (rgOnly)
            {
                for (uint32_t x = 0; x < width; ++x)
                {
                    scanline[x] = srcRow[x * 4 + 1];
                    scanline[width + x] = srcRow[x * 4 + 0];
                }
            }
            else
            {
                const std::array<uint32_t, 4> srcChannelIndex = { 3u, 2u, 1u, 0u };
                for (uint32_t c = 0; c < 4; ++c)
                {
                    uint32_t src = srcChannelIndex[c];
                    for (uint32_t x = 0; x < width; ++x)
                    {
                        scanline[c * width + x] = srcRow[x * 4 + src];
                    }
                }
            }

            stream.write(reinterpret_cast<const char*>(scanline.data()), scanlineDataSize);
        }

        return true;
    }
}

std::optional<HostImage> TextureIO::Load(const std::filesystem::path& path, const LoadOptions& options)
{
    const std::string pathString = path.string();
    int width = 0;
    int height = 0;
    int channels = 0;
    const int desiredChannels = options.forceChannels > 0 ? options.forceChannels : STBI_rgb_alpha;

    stbi_uc* pixels = stbi_load(pathString.c_str(), &width, &height, &channels, desiredChannels);
    if (!pixels)
    {
        return std::nullopt;
    }

    HostImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.channels = static_cast<uint32_t>(desiredChannels);
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.semantic = options.semantic;
    image.format = ResolvePixelFormat(options.transfer);
    image.rowStrideBytes = image.width * image.GetPixelSize();
    const size_t dataSize = static_cast<size_t>(image.rowStrideBytes) * image.height;
    image.data.resize(dataSize);
    std::memcpy(image.data.data(), pixels, dataSize);
    stbi_image_free(pixels);

    if (ShouldFlipY(options.flipY))
    {
        FlipImageY(image);
    }

    return image;
}

std::optional<HostImage> TextureIO::Load(const std::filesystem::path& path)
{
    return Load(path, LoadOptions());
}

bool TextureIO::Save(const std::filesystem::path& path, const HostImage& image, const SaveOptions& options)
{
    const FileFormat format = options.format == FileFormat::Png &&
        path.has_extension() ? FileFormatFromPath(path) : options.format;
    if (format == FileFormat::Exr)
    {
        return SaveExrHalf(path, image, options.exrWriteRGOnly);
    }
    return false;
}

bool TextureIO::Save(const std::filesystem::path& path, const HostImage& image)
{
    return Save(path, image, SaveOptions());
}

std::optional<HostImage> TextureIO::LoadFromMemory(const void* data, size_t size, const LoadOptions& options)
{
    int width = 0;
    int height = 0;
    int channels = 0;
    const int desiredChannels = options.forceChannels > 0 ? options.forceChannels : STBI_rgb_alpha;
    const stbi_uc* memory = reinterpret_cast<const stbi_uc*>(data);
    stbi_uc* pixels = stbi_load_from_memory(memory, static_cast<int>(size), &width, &height, &channels, desiredChannels);
    if (!pixels)
    {
        return std::nullopt;
    }

    HostImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.channels = static_cast<uint32_t>(desiredChannels);
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.semantic = options.semantic;
    image.format = ResolvePixelFormat(options.transfer);
    image.rowStrideBytes = image.width * image.GetPixelSize();
    const size_t dataSize = static_cast<size_t>(image.rowStrideBytes) * image.height;
    image.data.resize(dataSize);
    std::memcpy(image.data.data(), pixels, dataSize);
    stbi_image_free(pixels);

    if (ShouldFlipY(options.flipY))
    {
        FlipImageY(image);
    }
    return image;
}

std::optional<HostImage> TextureIO::LoadFromMemory(const void* data, size_t size)
{
    return LoadFromMemory(data, size, LoadOptions());
}

std::vector<uint8_t> TextureIO::SaveToMemory(const HostImage& image, const SaveOptions& options)
{
    std::vector<uint8_t> output;
    const std::filesystem::path tempPath = std::filesystem::temp_directory_path() / "textureio_temp_output.exr";
    if (Save(tempPath, image, options))
    {
        std::ifstream stream(tempPath, std::ios::binary);
        output.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
    }
    return output;
}

std::vector<uint8_t> TextureIO::SaveToMemory(const HostImage& image)
{
    return SaveToMemory(image, SaveOptions());
}
