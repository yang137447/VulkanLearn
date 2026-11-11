#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// 保存解码后的 CPU 像素，供纹理准备与上传消费；不拥有 GPU 资源。
// 存储布局固定为 layer-major、层内 mip-major：第 layer 层的全部 mip 连续存放，
// 每个 mip 内部按行连续。非块格式一行是 height 行像素；BC7/BC5 一行是一个
// 4×4 纹素块行，行跨度按块行计算（ceil(width/4) * 16 字节）。
// 压缩块本身无法在运行时有损地行翻转或重新生成 mip，容器必须提供完整 mip 链。
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
        // 16-bit unsigned single channel: terrain heights keep native precision
        // instead of being truncated to 8 bits by the PNG decode path.
        R16_UNORM,
        R16_FLOAT,
        RG16_FLOAT,
        RGBA16_FLOAT,
        R32_FLOAT,
        RGBA32_FLOAT,
        // 块压缩格式：4×4 纹素 = 16 字节/块；mip 由容器离线提供。
        BC7_UNORM,
        BC7_SRGB,
        BC5_UNORM,
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

    // 块压缩格式统一的纹素块尺寸与字节数（BC7/BC5 都是 128 bit/block）。
    static constexpr uint32_t TexelBlockSize = 4;
    static constexpr uint32_t TexelBlockBytes = 16;

    static bool IsBlockCompressed(PixelFormat format);
    static bool IsSRGBFormat(PixelFormat format);
    // 非块格式返回每像素字节数；块格式返回 16（每块字节数）。
    // 块布局不能写成 width * 该值，必须使用 GetMipRowStride/GetMipRowCount。
    static uint32_t BytesPerTexelOrBlock(PixelFormat format);

    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 4;
    uint32_t mipLevels = 1;
    uint32_t arrayLayers = 1;
    PixelFormat format = PixelFormat::RGBA8_UNORM;
    TextureSemantic semantic = TextureSemantic::Color;
    ChannelSwizzle swizzle;
    std::vector<uint8_t> data;
    // mip 0 的行跨度；0 表示紧凑排列。mip > 0 一律紧凑（块格式按块行）。
    // 现有非块单 mip 资产依赖该字段承载解码器的行填充。
    uint32_t rowStrideBytes = 0;

    size_t GetSize() const;
    // 仅对非块格式有效；块格式请用 BytesPerTexelOrBlock/GetMipRowStride。
    size_t GetPixelSize() const;

    uint32_t GetMipWidth(uint32_t mip) const;
    uint32_t GetMipHeight(uint32_t mip) const;
    // 单个 mip 的一行字节数：块格式为 ceil(width/4)*16，非块格式为 width*像素字节。
    uint32_t GetMipRowStride(uint32_t mip) const;
    // 单个 mip 的行数：块格式为块行数 ceil(height/4)。
    size_t GetMipRowCount(uint32_t mip) const;
    size_t GetMipLayerBytes(uint32_t mip) const;
    // 一层（含全部 mip）与整张图的字节数；用于校验容器长度与计算 staging 大小。
    size_t GetLayerBytes() const;
    size_t GetTotalBytes() const;
    // 层内 mip 顺序的起始偏移，配合 GetMipLayerBytes 得到每个 subresource 的数据窗口。
    size_t GetMipOffset(uint32_t mip, uint32_t layer) const;
    // 校验尺寸、mip/层数量与 data.size() 是否与布局一致；data 为空时跳过（沿用旧行为）。
    void ValidateStorage() const;

    void* GetMipData(uint32_t mip, uint32_t layer = 0);
    const void* GetMipData(uint32_t mip, uint32_t layer = 0) const;

    // 接受同尺寸/格式/同 mip 链的单层解码结果，在 CPU 资产边界校验后按层拼接；
    // 不生成 mip，也不允许把嵌套数组当成一层。
    static HostImage CombineArrayLayers(std::vector<HostImage> layers);
};
