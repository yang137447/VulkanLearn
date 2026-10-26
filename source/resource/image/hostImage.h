#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// 保存解码后的 CPU 像素，供纹理准备与上传消费；不拥有 GPU 资源。
// 当前只存 mip 0，数组按 layer 顺序连续排列，每层保留相同的行跨度。
struct HostImage
{
    enum class PixelFormat
    {
        R8_UNORM,
        RG8_UNORM,
        RGBA8_UNORM,
        R8_SRGB,
        RG8_SRGB,
        RGBA8_SRGB,
        R16_FLOAT,
        RG16_FLOAT,
        RGBA16_FLOAT,
        R32_FLOAT,
        RGBA32_FLOAT,
    };

    enum class TextureSemantic
    {
        Color,
        Normal,
        Data,
        Lut,
        EnvHdr,
    };

    struct ChannelSwizzle
    {
        uint8_t r = 0;
        uint8_t g = 1;
        uint8_t b = 2;
        uint8_t a = 3;
    };

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    PixelFormat format = PixelFormat::RGBA8_UNORM;
    TextureSemantic semantic = TextureSemantic::Color;
    ChannelSwizzle swizzle;
    std::vector<uint8_t> data;
    uint32_t rowStrideBytes = 0;

    size_t GetSize() const;
    size_t GetPixelSize() const;
    void* GetMipData(uint32_t mip, uint32_t layer = 0);
    const void* GetMipData(uint32_t mip, uint32_t layer = 0) const;

    // 接受同尺寸/格式的单层解码结果，在 CPU 资产边界校验后组装；不生成 mip。
    static HostImage CombineArrayLayers(std::vector<HostImage> layers);
};
