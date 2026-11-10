#pragma once

#include "hostImage.h"

#include <filesystem>
#include <optional>
#include <vector>

class TextureIO
{
public:
    enum class FileFormat
    {
        Png,
        Jpg,
        Exr,
        Hdr,
        Tga,
        Bmp,
        Ktx2,
        Dds
    };

    struct LoadOptions
    {
        enum class FlipYMode
        {
            ForceOn,
            ForceOff
        };

        enum class Transfer
        {
            Linear,
            SRGB
        };

        HostImage::TextureSemantic semantic = HostImage::TextureSemantic::Color;
        FlipYMode flipY = FlipYMode::ForceOff;
        Transfer transfer = Transfer::Linear;
        // 内存加载没有扩展名时，用它显式提示走 LDR/HDR/EXR 哪条解码路径。
        std::optional<FileFormat> formatHint = std::nullopt;
        bool genMipmaps = false;
        int forceChannels = 0;
        // Decode 16-bit PNGs without the 8-bit truncation stbi_load applies.
        // Used by terrain height arrays; ignored for HDR/EXR/container sources.
        bool forceSixteenBit = false;
    };

    struct SaveOptions
    {
        HostImage::TextureSemantic semantic = HostImage::TextureSemantic::Data;
        FileFormat format = FileFormat::Png;
        float quality = 0.9f;
        bool exrWriteRGOnly = false;
    };

    static std::optional<HostImage> Load(const std::filesystem::path& path);
    static std::optional<HostImage> Load(const std::filesystem::path& path, const LoadOptions& options);
    // 复用单图解码和方向合同，按 sources 顺序组装 CPU 数组；任一层失败则抛出，不返回半成品。
    // 只准备 mip 0；forceChannels 仅接受默认 RGBA 或显式 4，不接受 CPU mip 生成。
    static HostImage LoadArray(
        const std::vector<std::filesystem::path>& sources,
        const LoadOptions& options);
    static bool Save(const std::filesystem::path& path, const HostImage& image);
    static bool Save(const std::filesystem::path& path, const HostImage& image, const SaveOptions& options);

    static std::optional<HostImage> LoadFromMemory(const void* data, size_t size);
    static std::optional<HostImage> LoadFromMemory(const void* data, size_t size, const LoadOptions& options);
    static std::vector<uint8_t> SaveToMemory(const HostImage& image);
    static std::vector<uint8_t> SaveToMemory(const HostImage& image, const SaveOptions& options);
};
