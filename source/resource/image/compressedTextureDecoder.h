#pragma once

#include "hostImage.h"

#include <cstddef>
#include <filesystem>

// DDS/KTX2 只接受预编码的 BC7/BC5 块，不包含编码器，也不解码 Basis/Zstd/ZLIB
// 超压缩。容器必须自带 mip 链；运行时不会为块格式生成 mip。
namespace CompressedTextureDecoder
{
    bool MatchesDds(const void* data, size_t size);
    bool MatchesKtx2(const void* data, size_t size);
    bool IsCompressedContainerPath(const std::filesystem::path& path);

    // srgb 只决定 BC7 选择 BC7_SRGB 还是 BC7_UNORM；BC5 固定 UNORM。
    // 返回的 HostImage 采用 layer-major / 层内 mip-major 布局。
    HostImage DecodeDds(
        const void* data,
        size_t size,
        HostImage::TextureSemantic semantic,
        bool srgb);
    HostImage DecodeKtx2(
        const void* data,
        size_t size,
        HostImage::TextureSemantic semantic,
        bool srgb);
}
