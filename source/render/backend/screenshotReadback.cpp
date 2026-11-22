#include "render/backend/screenshotReadback.h"

namespace VL
{
namespace ScreenshotReadback
{
namespace
{

void WriteLittleEndianU32(
    std::array<uint8_t, 54>& header,
    size_t offset,
    uint32_t value)
{
    header[offset + 0] = static_cast<uint8_t>(value & 0xffu);
    header[offset + 1] = static_cast<uint8_t>((value >> 8u) & 0xffu);
    header[offset + 2] = static_cast<uint8_t>((value >> 16u) & 0xffu);
    header[offset + 3] = static_cast<uint8_t>((value >> 24u) & 0xffu);
}

void WriteLittleEndianI32(
    std::array<uint8_t, 54>& header,
    size_t offset,
    int32_t value)
{
    WriteLittleEndianU32(header, offset, static_cast<uint32_t>(value));
}

} // namespace

bool IsSupportedScreenshotFormat(vk::Format format)
{
    return format == vk::Format::eB8G8R8A8Unorm ||
        format == vk::Format::eB8G8R8A8Srgb ||
        format == vk::Format::eR8G8B8A8Unorm ||
        format == vk::Format::eR8G8B8A8Srgb;
}

std::optional<BmpLayout> MakeBmpLayout(uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0)
        return std::nullopt;

    BmpLayout layout;
    layout.width = width;
    layout.height = height;
    layout.rowBytes = width * 3u;
    layout.paddedRowBytes = (layout.rowBytes + 3u) & ~3u;
    const uint64_t pixelBytes =
        static_cast<uint64_t>(layout.paddedRowBytes) * height;
    // BMP 的 bfSize/biSizeImage 是 32 位字段；溢出时宁可拒绝，也不写一个坏文件头。
    if (pixelBytes > 0xffffffffull - 54ull)
        return std::nullopt;
    layout.pixelBytes = static_cast<uint32_t>(pixelBytes);
    layout.fileBytes = layout.pixelBytes + 54u;
    return layout;
}

std::array<uint8_t, 54> MakeBmpHeader(const BmpLayout& layout)
{
    std::array<uint8_t, 54> header{};
    header[0] = 'B';
    header[1] = 'M';
    WriteLittleEndianU32(header, 2, layout.fileBytes);
    WriteLittleEndianU32(header, 10, 54u);
    WriteLittleEndianU32(header, 14, 40u);
    WriteLittleEndianI32(header, 18, static_cast<int32_t>(layout.width));
    // 负高度 = top-down：交换链图像第 0 行就是屏幕顶行，写盘时不做翻转。
    WriteLittleEndianI32(header, 22, -static_cast<int32_t>(layout.height));
    header[26] = 1;
    header[28] = 24;
    WriteLittleEndianU32(header, 34, layout.pixelBytes);
    return header;
}

bool IsBgraFormat(vk::Format format)
{
    return format == vk::Format::eB8G8R8A8Unorm ||
        format == vk::Format::eB8G8R8A8Srgb;
}

void ConvertPixelToBgr24(
    const uint8_t* source,
    bool isBgra,
    uint8_t* destination)
{
    if (isBgra)
    {
        destination[0] = source[0];
        destination[1] = source[1];
        destination[2] = source[2];
    }
    else
    {
        destination[0] = source[2];
        destination[1] = source[1];
        destination[2] = source[0];
    }
}

} // namespace ScreenshotReadback
} // namespace VL
