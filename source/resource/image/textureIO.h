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
    static bool Save(const std::filesystem::path& path, const HostImage& image);
    static bool Save(const std::filesystem::path& path, const HostImage& image, const SaveOptions& options);

    static std::optional<HostImage> LoadFromMemory(const void* data, size_t size);
    static std::optional<HostImage> LoadFromMemory(const void* data, size_t size, const LoadOptions& options);
    static std::vector<uint8_t> SaveToMemory(const HostImage& image);
    static std::vector<uint8_t> SaveToMemory(const HostImage& image, const SaveOptions& options);
};
