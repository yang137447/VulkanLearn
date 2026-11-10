#pragma once

#include <filesystem>
#include <string>

#include "resource/image/hostImage.h"

namespace VL::TerrainHeightmapWriter
{

struct WriteOptions
{
    enum class PngBitDepth
    {
        // 目标已是 16-bit PNG 时保留 16-bit；新文件/8-bit 目标写 8-bit。
        Auto,
        Eight,
        Sixteen,
    };

    PngBitDepth pngBitDepth = PngBitDepth::Auto;
};

// 把归一化高度快照写回真实像素文件。当前支持：
//   .png -> grayscale，默认 8-bit；目标已是 16-bit PNG 或显式 Sixteen 时
//           写 16-bit（PNG stored deflate block，不依赖 zlib）
//   .bmp -> 24-bit grayscale
//   .exr -> R32_FLOAT 扩成 RGBA16_FLOAT 后复用 TextureIO 的半精度 EXR 输出
//           （R/G/B 同值、A=1，不 clamp 越界高度）
// 其他扩展名返回 false，不猜测格式。
bool Write(
    const std::filesystem::path& path,
    const HostImage& normalizedHeightmap,
    std::string& error);
bool Write(
    const std::filesystem::path& path,
    const HostImage& normalizedHeightmap,
    const WriteOptions& options,
    std::string& error);

} // namespace VL::TerrainHeightmapWriter
