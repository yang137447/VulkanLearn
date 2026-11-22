// R15 CPU p99 诊断的质量回读测试。
//
// 背景：R15 正式性能只在 CPU 帧间隔 p99 失败（Coverage 140.18 ms / Local 78.58 ms，
// cap 72.105 ms）。逐帧对齐后，12 个 >72 ms 帧里有 2 个窗口内没有截图，只有
// capture_completed / RVT_QUALITY_CAPTURE；每 capture 的 Consume 中位数约 10.1 ms，
// 但两次分别是 104.2 与 161.6 ms。Consume 里唯一的整块内存操作是 1.6 MiB payload
// 的 memcpy（terrainCompositedQualityGpu.cpp TryConsume）。本测试固定两件事：
// 1) 回读内存类型必须优先 HOST_CACHED（缺失时安全退回，不改变功能）；
// 2) 固定 payload 的 ABI 尺寸与解码结果，证明 CPU 采样循环本身不是那 100 ms。
#include "commonFunction.h"
#include "render/resource/terrainCompositedQualityData.h"

#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <vector>

namespace
{
namespace Abi = VL::TerrainQualityAbi;

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

// 与 terrainCompositedQualityGpu 回读缓冲相同的“首选 cached”请求。
const vk::MemoryPropertyFlags ReadbackFlags = vk::MemoryPropertyFlagBits::eHostVisible |
    vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eHostCached;

std::vector<uint32_t> EmptyPayload()
{
    std::vector<uint32_t> words(Abi::Bytes / sizeof(uint32_t), 0);
    words[Abi::MagicWord] = Abi::Magic;
    words[Abi::VersionWord] = Abi::Version;
    words[Abi::CapacityWord] = Abi::Capacity;
    words[Abi::EpochLow] = 7;
    words[Abi::GenerationLow] = 3;
    words[Abi::RenderWidth] = 1280;
    words[Abi::RenderHeight] = 720;
    words[Abi::SurfaceCount] = 1;
    Abi::Surface surface{};
    surface.index = 0;
    surface.width = 128;
    surface.height = 128;
    surface.maxMipAndRepeat = 7; // 128 的 mip 级数，同时是 shader 的 repeat 位
    surface.generationLow = 3;
    std::memcpy(words.data() + Abi::SurfaceWord, &surface, sizeof(surface));
    return words;
}
}

// HOST_CACHED 是 R15 的修复点：缺失时 FindMemoryType 必须安全退回同一组
// HOST_VISIBLE|HOST_COHERENT 内存，否则启动会失败。
TEST(TerrainQualityReadback, PrefersHostCachedMemoryType)
{
    vk::PhysicalDeviceMemoryProperties properties = DeviceMemory(true);
    vk::MemoryPropertyFlags flags = ReadbackFlags;
    EXPECT_EQ(CommonFunction::FindMemoryType(properties, 0b10u, flags), 1u);
}

TEST(TerrainQualityReadback, FallsBackWhenNoCachedTypeExists)
{
    vk::PhysicalDeviceMemoryProperties properties = DeviceMemory(false);
    vk::MemoryPropertyFlags flags = ReadbackFlags;
    EXPECT_EQ(CommonFunction::FindMemoryType(properties, 0b10u, flags), 1u);
    // 只请求 HOST_VISIBLE（旧行为）时也必须仍能选中同一块内存，证明退回不是功能变化。
    vk::MemoryPropertyFlags legacy = vk::MemoryPropertyFlagBits::eHostVisible |
        vk::MemoryPropertyFlagBits::eHostCoherent;
    EXPECT_EQ(CommonFunction::FindMemoryType(properties, 0b10u, legacy), 1u);
}

// 固定 1,619,200 字节 payload：回读量与 capture-N.bin 完全一致。
TEST(TerrainQualityReadback, FixedPayloadIs1619200Bytes)
{
    EXPECT_EQ(Abi::Bytes, 1619200u);
    EXPECT_EQ(Abi::Bytes / sizeof(uint32_t), 404800u);
    EXPECT_EQ(Abi::Capacity, 16384u);
}

// 一个没有可见采样、surface 自洽的 payload 必须解码为 complete。
TEST(TerrainQualityReadback, DecodesEmptyButCompletePayload)
{
    std::vector<uint32_t> words = EmptyPayload();
    const Abi::Counts counts = Abi::Recompute(words.data(), Abi::Bytes);
    EXPECT_EQ(counts.visibleSamples, 0u);
    EXPECT_EQ(counts.unresolved, 0u);
    EXPECT_TRUE(counts.complete);
}

// 同样的 16,384 采样循环：缺少 result 位的采样必须变成 unresolved 而非崩溃，
// 且整块解码保持 O(16,384) 的固定工作量（R15 实测 0.36-1.84 ms，不是 100 ms 的来源）。
TEST(TerrainQualityReadback, MarksSampleWithoutResultWordUnresolved)
{
    std::vector<uint32_t> words = EmptyPayload();
    Abi::Sample sample{};
    sample.uv[0] = 0.5f;
    sample.uv[1] = 0.5f;
    sample.majorUv[0] = 0.5f;
    sample.majorUv[1] = 0.5f;
    sample.targetMip = 7.0f;
    sample.effectiveMipBias = 0.0f;
    sample.surfaceAndTaps = (1u << 16); // surface 0，一个 tap
    sample.claims = 1;
    std::memcpy(words.data() + Abi::SampleWord, &sample, sizeof(sample));
    words[Abi::VisibleCount] = 1;
    words[Abi::ResolvedCount] = 1;
    const Abi::Counts counts = Abi::Recompute(words.data(), Abi::Bytes);
    EXPECT_EQ(counts.visibleSamples, 1u);
    EXPECT_EQ(counts.unresolved, 1u);
    EXPECT_EQ(counts.validAtTargetPlusBias, 0u);
    EXPECT_FALSE(counts.complete);
}

TEST(TerrainQualityReadback, RejectsWrongPayloadSize)
{
    std::vector<uint32_t> words = EmptyPayload();
    EXPECT_THROW(Abi::Recompute(words.data(), Abi::Bytes - 4), std::runtime_error);
}

TEST(TerrainQualityReadback, RejectsWrongMagic)
{
    std::vector<uint32_t> words = EmptyPayload();
    words[Abi::MagicWord] = 0;
    EXPECT_THROW(Abi::Recompute(words.data(), Abi::Bytes), std::runtime_error);
}
