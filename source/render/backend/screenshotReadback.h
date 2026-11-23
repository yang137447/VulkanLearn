#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vulkan/vulkan.hpp>

namespace VL
{
// 截图回读里与 Vulkan 设备无关的纯 CPU 布局/策略。
//
// R15 正式性能的 CPU p99 失败（Coverage 140.182 ms / Local 78.582 ms，cap 72.105 ms）
// 中 10/12 个停顿是截图路径：device-wide WaitIdle + 3.7 MiB 非 cached 主机可见读 +
// 逐像素 BGR 转换写盘。这里的常量和布局函数固定“修复后仍必须保持不变”的部分：
// 截图内容、朝向、格式支持范围，以及那次提交 fence 的有限等待上限。
namespace ScreenshotReadback
{
// 只等承载 copyImageToBuffer 的那次帧提交自己的 fence。一帧 @30Hz 约 33 ms，
// 实测 GPU pass 合计约 2 ms；1 s 已经超过 30 帧预算。超时说明 GPU 已挂起，
// 继续无限等待只会把渲染线程拖死，因此按有界失败处理，而不是退回 device WaitIdle。
inline constexpr uint64_t FenceTimeoutNanoseconds = 1000ull * 1000ull * 1000ull;

// staging buffer 必须优先 cached host memory：R15 诊断实测 3.7 MiB 非 cached
// 映射读是 180 ms 量级；cached 后只剩内存拷贝 + BGR 转换的十几 ms。
// CommonFunction::FindMemoryType 在没有 cached 型时会退回同一组
// HOST_VISIBLE|HOST_COHERENT，所以这是性能提示，不会让启动失败。
inline const vk::MemoryPropertyFlags StagingMemoryFlags =
    vk::MemoryPropertyFlagBits::eHostVisible |
    vk::MemoryPropertyFlagBits::eHostCoherent |
    vk::MemoryPropertyFlagBits::eHostCached;

bool IsSupportedScreenshotFormat(vk::Format format);

// 24bpp BMP 的字节布局。BMP 行必须 4 字节对齐，文件大小字段是 32 位。
struct BmpLayout
{
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t rowBytes = 0;
    uint32_t paddedRowBytes = 0;
    uint32_t pixelBytes = 0;
    uint32_t fileBytes = 0;
};

// width/height 为 0 或布局超过 BMP 32 位大小字段时返回 nullopt。
std::optional<BmpLayout> MakeBmpLayout(uint32_t width, uint32_t height);

// 54 字节 BITMAPFILEHEADER + BITMAPINFOHEADER。高度取负表示 top-down 扫描行，
// 与交换链图像的内存顺序一致，保证截图不会上下翻转。
std::array<uint8_t, 54> MakeBmpHeader(const BmpLayout& layout);

bool IsBgraFormat(vk::Format format);

// 交换链像素是 8-bit RGBA/BGRA；BMP 只要 BGR 三通道。
void ConvertPixelToBgr24(const uint8_t* source, bool isBgra, uint8_t* destination);
} // namespace ScreenshotReadback
} // namespace VL
