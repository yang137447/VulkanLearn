#include "terrain/terrainHeightmapIO.h"

#include <array>
#include <cctype>
#include <exception>
#include <fstream>
#include <limits>
#include <stb_image.h>

#include "resource/image/textureIO.h"

namespace VL::TerrainHeightmapIO
{
namespace
{

std::string LowerExtension(const std::filesystem::path& path)
{
    std::string extension = path.extension().string();
    for (char& character : extension)
    {
        character = static_cast<char>(
            std::tolower(static_cast<unsigned char>(character)));
    }
    return extension;
}

bool HasPngSignature(const std::filesystem::path& path)
{
    static constexpr std::array<uint8_t, 8> Signature = {
        0x89u, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
        return false;
    std::array<uint8_t, Signature.size()> bytes{};
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return stream.gcount() == static_cast<std::streamsize>(bytes.size()) &&
        bytes == Signature;
}

void FreePixels(stbi_us*& pixels) noexcept
{
    if (pixels != nullptr)
    {
        stbi_image_free(pixels);
        pixels = nullptr;
    }
}

std::optional<HostImage> LoadSixteenBitPng(
    const std::filesystem::path& path,
    std::string& error)
{
    const std::string pathString = path.string();
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_us* pixels = stbi_load_16(
        pathString.c_str(), &width, &height, &channels, 1);
    if (pixels == nullptr)
    {
        error = "Failed to decode 16-bit terrain heightmap PNG: " + pathString;
        return std::nullopt;
    }

    if (channels != 1)
    {
        FreePixels(pixels);
        error = "16-bit terrain heightmap PNG must be grayscale/single-channel "
            "(found " + std::to_string(channels) + " channels): " + pathString;
        return std::nullopt;
    }
    if (width <= 0 || height <= 0)
    {
        FreePixels(pixels);
        error = "16-bit terrain heightmap PNG has invalid dimensions " +
            std::to_string(width) + "x" + std::to_string(height) + ": " +
            pathString;
        return std::nullopt;
    }

    const size_t widthSize = static_cast<size_t>(width);
    const size_t heightSize = static_cast<size_t>(height);
    if (widthSize > std::numeric_limits<size_t>::max() / heightSize ||
        widthSize * heightSize >
            std::numeric_limits<size_t>::max() / sizeof(float))
    {
        FreePixels(pixels);
        error = "16-bit terrain heightmap PNG dimensions overflow: " + pathString;
        return std::nullopt;
    }

    HostImage image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.channels = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.format = HostImage::PixelFormat::R32_FLOAT;
    image.semantic = HostImage::TextureSemantic::Data;
    image.rowStrideBytes = image.width * static_cast<uint32_t>(sizeof(float));
    const size_t sampleCount = widthSize * heightSize;
    image.data.resize(sampleCount * sizeof(float));
    float* samples = reinterpret_cast<float*>(image.data.data());
    for (size_t index = 0; index < sampleCount; ++index)
        samples[index] = static_cast<float>(pixels[index]) / 65535.0f;
    FreePixels(pixels);
    return image;
}

} // namespace

std::optional<HostImage> Load(
    const std::filesystem::path& path,
    std::string& error)
{
    error.clear();
    if (path.empty())
    {
        error = "Terrain heightmap path is empty";
        return std::nullopt;
    }

    const std::string extension = LowerExtension(path);
    const std::string pathString = path.string();
    // 只在 PNG 签名与 IHDR 都确认是 16-bit 时才进入专用通路；其他格式
    // 继续走 TextureIO，避免改变 8-bit/EXR/HDR 已有的格式与数值语义。
    if (extension == ".png" &&
        HasPngSignature(path) &&
        stbi_is_16_bit(pathString.c_str()) != 0)
    {
        return LoadSixteenBitPng(path, error);
    }

    TextureIO::LoadOptions options;
    options.semantic = HostImage::TextureSemantic::Data;
    options.flipY = TextureIO::LoadOptions::FlipYMode::ForceOff;
    options.transfer = TextureIO::LoadOptions::Transfer::Linear;
    options.forceChannels = 4;
    try
    {
        std::optional<HostImage> image = TextureIO::Load(path, options);
        if (!image)
        {
            error = "Failed to decode terrain heightmap: " + pathString;
            return std::nullopt;
        }
        if (image->width == 0 || image->height == 0)
        {
            error = "Terrain heightmap has zero dimensions: " + pathString;
            return std::nullopt;
        }
        return image;
    }
    catch (const std::exception& exception)
    {
        error = "Failed to decode terrain heightmap: " + pathString + ": " +
            exception.what();
        return std::nullopt;
    }
}

} // namespace VL::TerrainHeightmapIO
