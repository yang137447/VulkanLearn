// R16 截图停顿修复的 CPU 契约测试。
//
// 背景：R15 正式性能的 CPU p99 失败里，12 个 >72 ms 停顿有 10 个来自截图路径
// （rendererBackendVulkan.cpp CompleteSwapchainScreenshot）：device-wide WaitIdle +
// 3.7 MiB 非 cached 主机可见读 + 逐像素 BGR 转换写盘，实测约 210 ms。
// 修复后只等承载 copy 的那次提交自己的 frame fence，staging 改成 HOST_CACHED。
// 本测试固定修复里“不能漂移”的 CPU 契约：
// 1) staging 内存必须优先 HOST_CACHED，缺失时安全退回（与质量回读同一约定）；
// 2) 输出仍是 1280x720 top-down 24bpp BMP（2,764,854 B，与 R15 现有截图逐字节同布局）；
// 3) BGRA/RGBA 的通道顺序与 4 字节行对齐保持不变；
// 4) fence 等待是有界的，不允许退回无限等待。
#include "commonFunction.h"
#include "render/backend/screenshotReadback.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace
{
namespace Shot = VL::ScreenshotReadback;

vk::PhysicalDeviceMemoryProperties DeviceMemory(bool withCached)
{
    vk::PhysicalDeviceMemoryProperties properties{};
    properties.memoryTypeCount = 2;
    properties.memoryTypes[0].propertyFlags = vk::MemoryPropertyFlagBits::eDeviceLocal;
    properties.memoryTypes[1].propertyFlags = vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent |
        (withCached ? vk::MemoryPropertyFlagBits::eHostCached : vk::MemoryPropertyFlags{});
    return properties;
}

uint32_t ReadU32(const std::array<uint8_t, 54>& header, size_t offset)
{
    return static_cast<uint32_t>(header[offset]) |
        (static_cast<uint32_t>(header[offset + 1]) << 8u) |
        (static_cast<uint32_t>(header[offset + 2]) << 16u) |
        (static_cast<uint32_t>(header[offset + 3]) << 24u);
}

int32_t ReadI32(const std::array<uint8_t, 54>& header, size_t offset)
{
    return static_cast<int32_t>(ReadU32(header, offset));
}
}

// 3.7 MiB staging 用 cached host memory：R15 实测非 cached 读约 180 ms。
TEST(TerrainScreenshotReadback, PrefersHostCachedStagingMemory)
{
    EXPECT_EQ(Shot::StagingMemoryFlags & vk::MemoryPropertyFlagBits::eHostCached,
              vk::MemoryPropertyFlagBits::eHostCached);
    vk::PhysicalDeviceMemoryProperties properties = DeviceMemory(true);
    vk::MemoryPropertyFlags flags = Shot::StagingMemoryFlags;
    EXPECT_EQ(CommonFunction::FindMemoryType(properties, 0b10u, flags), 1u);
}

// 没有 cached 型时 FindMemoryType 必须退回旧的 HOST_VISIBLE|HOST_COHERENT，
// 否则为了性能标志会直接启动失败。
TEST(TerrainScreenshotReadback, FallsBackWhenCachedTypeIsAbsent)
{
    vk::PhysicalDeviceMemoryProperties properties = DeviceMemory(false);
    vk::MemoryPropertyFlags flags = Shot::StagingMemoryFlags;
    EXPECT_EQ(CommonFunction::FindMemoryType(properties, 0b10u, flags), 1u);
}

// 1280x720 就是正式性能的渲染分辨率；BMP 文件总长与 R15 落盘的 2,764,854 B 一致。
TEST(TerrainScreenshotReadback, LayoutMatchesExistingBmpSizeAt720p)
{
    const std::optional<Shot::BmpLayout> layout = Shot::MakeBmpLayout(1280, 720);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->rowBytes, 3840u);
    EXPECT_EQ(layout->paddedRowBytes, 3840u);
    EXPECT_EQ(layout->pixelBytes, 2764800u);
    EXPECT_EQ(layout->fileBytes, 2764854u);
}

// top-down（负高度）保证交换链第 0 行仍是图像顶行：修复不得翻转截图。
TEST(TerrainScreenshotReadback, BmpHeaderIsTopDown24Bit)
{
    const std::optional<Shot::BmpLayout> layout = Shot::MakeBmpLayout(1280, 720);
    ASSERT_TRUE(layout.has_value());
    const std::array<uint8_t, 54> header = Shot::MakeBmpHeader(*layout);
    EXPECT_EQ(header[0], 'B');
    EXPECT_EQ(header[1], 'M');
    EXPECT_EQ(ReadU32(header, 2), 2764854u);
    EXPECT_EQ(ReadU32(header, 10), 54u);
    EXPECT_EQ(ReadU32(header, 14), 40u);
    EXPECT_EQ(ReadI32(header, 18), 1280);
    EXPECT_EQ(ReadI32(header, 22), -720);
    EXPECT_EQ(header[26], 1);
    EXPECT_EQ(header[28], 24);
    EXPECT_EQ(ReadU32(header, 34), 2764800u);
}

// 4 字节行对齐：宽 2 的 BGR24 行只有 6 字节，BMP 行必须补到 8 字节。
TEST(TerrainScreenshotReadback, PadsRowsToFourByteBoundary)
{
    const std::optional<Shot::BmpLayout> layout = Shot::MakeBmpLayout(2, 2);
    ASSERT_TRUE(layout.has_value());
    EXPECT_EQ(layout->rowBytes, 6u);
    EXPECT_EQ(layout->paddedRowBytes, 8u);
    EXPECT_EQ(layout->pixelBytes, 16u);
    EXPECT_EQ(layout->fileBytes, 70u);
}

// BGRA 保持 BGR 顺序，RGBA 需要反转通道；alpha 不写进 BMP。
TEST(TerrainScreenshotReadback, ConvertsBgraAndRgbaToBgr)
{
    const uint8_t bgra[4] = {0x11, 0x22, 0x33, 0x44};
    uint8_t bgr[3] = {0, 0, 0};
    Shot::ConvertPixelToBgr24(bgra, true, bgr);
    EXPECT_EQ(bgr[0], 0x11);
    EXPECT_EQ(bgr[1], 0x22);
    EXPECT_EQ(bgr[2], 0x33);

    const uint8_t rgba[4] = {0x11, 0x22, 0x33, 0x44};
    Shot::ConvertPixelToBgr24(rgba, false, bgr);
    EXPECT_EQ(bgr[0], 0x33);
    EXPECT_EQ(bgr[1], 0x22);
    EXPECT_EQ(bgr[2], 0x11);
}

TEST(TerrainScreenshotReadback, RejectsEmptyExtent)
{
    EXPECT_FALSE(Shot::MakeBmpLayout(0, 720).has_value());
    EXPECT_FALSE(Shot::MakeBmpLayout(1280, 0).has_value());
}

// 只支持交换链实际使用的 8-bit RGBA/BGRA；其它格式必须明确失败而不是写坏图。
TEST(TerrainScreenshotReadback, SupportedFormatsAreTheSwapchainEightBitSet)
{
    EXPECT_TRUE(Shot::IsSupportedScreenshotFormat(vk::Format::eB8G8R8A8Unorm));
    EXPECT_TRUE(Shot::IsSupportedScreenshotFormat(vk::Format::eB8G8R8A8Srgb));
    EXPECT_TRUE(Shot::IsSupportedScreenshotFormat(vk::Format::eR8G8B8A8Unorm));
    EXPECT_TRUE(Shot::IsSupportedScreenshotFormat(vk::Format::eR8G8B8A8Srgb));
    EXPECT_FALSE(Shot::IsSupportedScreenshotFormat(vk::Format::eR16G16B16A16Sfloat));
    EXPECT_TRUE(Shot::IsBgraFormat(vk::Format::eB8G8R8A8Srgb));
    EXPECT_FALSE(Shot::IsBgraFormat(vk::Format::eR8G8B8A8Srgb));
}

// 等待上限必须有限且大于正常帧预算：一帧 @30Hz 约 33 ms，正常 GPU pass 约 2 ms。
TEST(TerrainScreenshotReadback, FenceWaitIsBoundedButGenerous)
{
    EXPECT_GE(Shot::FenceTimeoutNanoseconds, 100ull * 1000ull * 1000ull);
    EXPECT_LE(Shot::FenceTimeoutNanoseconds, 5000ull * 1000ull * 1000ull);
}
