#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "hair/hairTraceFormat.h"

namespace
{
using VL::Hair::HairTraceFrameInfo;
using VL::Hair::HairTraceHeader;
using VL::Hair::HairTraceReadResult;
using VL::Hair::HairTraceReader;
using VL::Hair::HairTraceSample;
using VL::Hair::HairTraceWriter;

std::filesystem::path TempPath(const char* name)
{
    return std::filesystem::temp_directory_path() /
        (std::string("vl_hair_trace_") + name + ".bin");
}

HairTraceHeader MakeHeader()
{
    HairTraceHeader header;
    header.version = VL::Hair::HairTraceVersion;
    header.totalGuideCount = 4096;
    header.pointsPerGuide = 4;
    header.stride = 1024;
    header.selectedCount = 3;
    header.startFrame = 900;
    header.selectedGuides = {0, 1024, 2048};
    header.restLengths = {
        1.0f, 2.0f, 3.0f, 0.0f,
        1.5f, 2.5f, 3.5f, 0.0f,
        0.5f, 1.25f, 2.25f, 0.0f,
    };
    return header;
}

std::vector<float> MakePayload(size_t sampleCount, float offset)
{
    // Writer 的每个状态数组是 sampleCount*4（每点一个 float4），不是 *12；
    // 文件的 48-byte 交错由 WriteFrame 内部完成。
    std::vector<float> payload(sampleCount * 4);
    for (size_t sample = 0; sample < sampleCount; ++sample)
    {
        for (size_t element = 0; element < 4; ++element)
            payload[sample * 4 + element] =
                offset + static_cast<float>(sample * 4 + element) * 0.25f;
    }
    return payload;
}

void WriteValidFile(const std::filesystem::path& path, uint32_t frameCount)
{
    HairTraceWriter writer;
    std::string error;
    ASSERT_TRUE(writer.Open(path.string(), MakeHeader(), error)) << error;
    for (uint32_t frame = 0; frame < frameCount; ++frame)
    {
        HairTraceFrameInfo info;
        info.frameIndex = 1000 + frame;
        info.worldGeneration = 7;
        info.substeps = 1 + frame % 2;
        info.timeStep = 0.0125f;
        info.resetReason = frame == 0 ? 2u : 0u;
        info.dispatchCount = 7 + frame;
        info.hairModel = Eigen::Matrix4f::Identity();
        info.hairModel(0, 3) = static_cast<float>(frame) * 10.0f;
        info.sdfPenetrationMaxCm = 0.25f + static_cast<float>(frame);
        info.capsulePenetrationMaxCm = 0.5f + static_cast<float>(frame);
        const size_t sampleCount = 3 * 4;
        const std::vector<float> current = MakePayload(sampleCount, 1.0f + frame);
        const std::vector<float> previous = MakePayload(sampleCount, 2.0f + frame);
        const std::vector<float> previousPrevious = MakePayload(sampleCount, 3.0f + frame);
        ASSERT_TRUE(writer.WriteFrame(info, current.data(), previous.data(),
            previousPrevious.data(), error)) << error;
    }
    ASSERT_TRUE(writer.Close(error)) << error;
}

void ExpectSample(const HairTraceSample& sample, size_t sampleIndex, float offset)
{
    float values[12];
    std::memcpy(values, sample.current.data(), sizeof(float) * 4);
    std::memcpy(values + 4, sample.previous.data(), sizeof(float) * 4);
    std::memcpy(values + 8, sample.previousPrevious.data(), sizeof(float) * 4);
    for (size_t element = 0; element < 12; ++element)
    {
        // MakePayload 给 current/previous/prevPrevious 的基值依次 +1。
        const float blockOffset = offset + static_cast<float>(element / 4);
        EXPECT_FLOAT_EQ(values[element],
            blockOffset +
                static_cast<float>(sampleIndex * 4 + element % 4) * 0.25f)
            << "sample " << sampleIndex << " element " << element;
    }
}
} // namespace

TEST(HairTraceFormat, RoundTripsHeaderAndFrames)
{
    const std::filesystem::path path = TempPath("roundtrip");
    std::filesystem::remove(path);
    WriteValidFile(path, 2);
    ASSERT_TRUE(std::filesystem::is_regular_file(path));

    // Header 固定 32 B + selectedCount*4 + selectedCount*pointsPerGuide*4；
    // 每帧 32 B + 64 B model + 8 B 穿透统计 + selectedCount*pointsPerGuide*48 B。
    const uint64_t headerBytes = 32u + 3u * 4u + 3u * 4u * 4u;
    const uint64_t frameBytes = 32u + 64u + 8u + 3u * 4u * 48u;
    EXPECT_EQ(std::filesystem::file_size(path), headerBytes + 2u * frameBytes);

    {
        HairTraceReader reader;
        HairTraceHeader header;
        std::string error;
        ASSERT_EQ(reader.Open(path.string(), header, error),
            HairTraceReadResult::Frame) << error;
        EXPECT_EQ(header.version, VL::Hair::HairTraceVersion);
        EXPECT_EQ(header.totalGuideCount, 4096u);
        EXPECT_EQ(header.pointsPerGuide, 4u);
        EXPECT_EQ(header.stride, 1024u);
        EXPECT_EQ(header.selectedCount, 3u);
        EXPECT_EQ(header.startFrame, 900u);
        EXPECT_EQ(header.selectedGuides,
            (std::vector<uint32_t>{0, 1024, 2048}));
        ASSERT_EQ(header.restLengths.size(), 12u);
        EXPECT_FLOAT_EQ(header.restLengths[1], 2.0f);

        for (uint32_t frame = 0; frame < 2; ++frame)
        {
            HairTraceFrameInfo info;
            std::vector<HairTraceSample> samples;
            ASSERT_EQ(reader.ReadFrame(info, samples, error),
                HairTraceReadResult::Frame) << error;
            EXPECT_EQ(info.frameIndex, 1000u + frame);
            EXPECT_EQ(info.worldGeneration, 7u);
            EXPECT_EQ(info.substeps, 1u + frame % 2);
            EXPECT_FLOAT_EQ(info.timeStep, 0.0125f);
            EXPECT_EQ(info.resetReason, frame == 0 ? 2u : 0u);
            EXPECT_EQ(info.dispatchCount, 7u + frame);
            EXPECT_FLOAT_EQ(info.hairModel(0, 3), static_cast<float>(frame) * 10.0f);
            EXPECT_FLOAT_EQ(info.sdfPenetrationMaxCm, 0.25f + static_cast<float>(frame));
            EXPECT_FLOAT_EQ(info.capsulePenetrationMaxCm, 0.5f + static_cast<float>(frame));
            ASSERT_EQ(samples.size(), 12u);
            ExpectSample(samples[0], 0, 1.0f + frame);
            ExpectSample(samples[11], 11, 1.0f + frame);
            EXPECT_FLOAT_EQ(samples[0].previous.x(), 2.0f + frame);
            EXPECT_FLOAT_EQ(samples[0].previousPrevious.x(), 3.0f + frame);
        }
        HairTraceFrameInfo info;
        std::vector<HairTraceSample> samples;
        EXPECT_EQ(reader.ReadFrame(info, samples, error),
            HairTraceReadResult::EndOfFile);
    }
    std::filesystem::remove(path);
}

TEST(HairTraceFormat, RejectsBadMagic)
{
    const std::filesystem::path path = TempPath("bad_magic");
    std::filesystem::remove(path);
    WriteValidFile(path, 1);
    {
        std::fstream stream(path, std::ios::binary | std::ios::in | std::ios::out);
        ASSERT_TRUE(stream.is_open());
        const char badMagic[4] = {'X', 'X', 'X', 'X'};
        stream.write(badMagic, 4);
    }
    {
        HairTraceReader reader;
        HairTraceHeader header;
        std::string error;
        EXPECT_EQ(reader.Open(path.string(), header, error),
            HairTraceReadResult::Error);
        EXPECT_NE(error.find("magic"), std::string::npos) << error;
    }
    std::filesystem::remove(path);
}

TEST(HairTraceFormat, RejectsTruncatedHeader)
{
    const std::filesystem::path path = TempPath("truncated_header");
    std::filesystem::remove(path);
    {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(stream.is_open());
        const char partial[10] = {'V', 'L', 'H', 'T', 1, 0, 0, 0, 0, 0};
        stream.write(partial, sizeof(partial));
    }
    {
        HairTraceReader reader;
        HairTraceHeader header;
        std::string error;
        EXPECT_EQ(reader.Open(path.string(), header, error),
            HairTraceReadResult::Error);
        EXPECT_FALSE(error.empty());
    }
    std::filesystem::remove(path);
}

TEST(HairTraceFormat, RejectsTruncatedFramePayload)
{
    const std::filesystem::path path = TempPath("truncated_frame");
    std::filesystem::remove(path);
    WriteValidFile(path, 1);
    const uint64_t size = std::filesystem::file_size(path);
    ASSERT_GT(size, 8u);
    std::filesystem::resize_file(path, size - 8u);

    {
        HairTraceReader reader;
        HairTraceHeader header;
        std::string error;
        ASSERT_EQ(reader.Open(path.string(), header, error),
            HairTraceReadResult::Frame) << error;
        HairTraceFrameInfo info;
        std::vector<HairTraceSample> samples;
        EXPECT_EQ(reader.ReadFrame(info, samples, error),
            HairTraceReadResult::Error);
        EXPECT_NE(error.find("truncated"), std::string::npos) << error;
    }
    std::filesystem::remove(path);
}

TEST(HairTraceFormat, RejectsInvalidSelectionHeader)
{
    HairTraceHeader header = MakeHeader();
    header.selectedGuides[2] = 4096; // 越界
    HairTraceWriter writer;
    std::string error;
    EXPECT_FALSE(writer.Open(TempPath("invalid_header").string(), header, error));
    EXPECT_FALSE(error.empty());
}
