#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "audio/audioClip.h"

namespace
{
using namespace VL::Audio;

void AppendU16(std::vector<std::uint8_t>& bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
}

void AppendU32(std::vector<std::uint8_t>& bytes, std::uint32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void AppendFourCc(std::vector<std::uint8_t>& bytes, const char* id)
{
    bytes.insert(bytes.end(), id, id + 4);
}

std::vector<std::uint8_t> MakeWav(
    std::uint16_t format, std::uint16_t channels,
    std::uint32_t sampleRate, std::uint16_t bitsPerSample,
    const std::vector<std::uint8_t>& payload)
{
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(
        channels * (bitsPerSample / 8));
    std::vector<std::uint8_t> bytes;
    AppendFourCc(bytes, "RIFF");
    AppendU32(bytes, static_cast<std::uint32_t>(
        4 + 8 + 16 + 8 + payload.size() + (payload.size() & 1u)));
    AppendFourCc(bytes, "WAVE");
    AppendFourCc(bytes, "fmt ");
    AppendU32(bytes, 16);
    AppendU16(bytes, format);
    AppendU16(bytes, channels);
    AppendU32(bytes, sampleRate);
    AppendU32(bytes, sampleRate * blockAlign);
    AppendU16(bytes, blockAlign);
    AppendU16(bytes, bitsPerSample);
    AppendFourCc(bytes, "data");
    AppendU32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    if (payload.size() & 1u) bytes.push_back(0);
    return bytes;
}

std::filesystem::path WriteTempWav(
    const std::string& name, const std::vector<std::uint8_t>& bytes)
{
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "vl_audio_clip_tests";
    std::filesystem::create_directories(directory);
    const std::filesystem::path path = directory / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return path;
}

void AppendPcm16(std::vector<std::uint8_t>& bytes, std::int16_t value)
{
    AppendU16(bytes, static_cast<std::uint16_t>(value));
}

void AppendPcm24(std::vector<std::uint8_t>& bytes, std::int32_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
}

} // namespace

TEST(AudioClipLoading, Pcm16StereoIsDownmixedToMono)
{
    std::vector<std::uint8_t> payload;
    AppendPcm16(payload, 16384);
    AppendPcm16(payload, -16384);
    AppendPcm16(payload, 8192);
    AppendPcm16(payload, 8192);
    AppendPcm16(payload, -32768);
    AppendPcm16(payload, -32768);
    AppendPcm16(payload, 32767);
    AppendPcm16(payload, 32767);

    const std::filesystem::path path = WriteTempWav(
        "pcm16_stereo.wav",
        MakeWav(1, 2, 44100, 16, payload));
    AudioClip clip;
    std::string error;
    ASSERT_TRUE(LoadWavFile(path, clip, error)) << error;
    EXPECT_EQ(clip.sampleRate, 44100);
    ASSERT_EQ(clip.samples.size(), 4u);
    EXPECT_NEAR(clip.samples[0], 0.0f, 1.0e-5f);
    EXPECT_NEAR(clip.samples[1], 0.25f, 1.0e-5f);
    EXPECT_NEAR(clip.samples[2], -1.0f, 1.0e-5f);
    EXPECT_NEAR(clip.samples[3], 32767.0f / 32768.0f, 1.0e-5f);
    std::filesystem::remove(path);
}

TEST(AudioClipLoading, Pcm24AndFloat32AreDecodedAsSignedSamples)
{
    std::vector<std::uint8_t> pcm24;
    AppendPcm24(pcm24, 0x400000);
    AppendPcm24(pcm24, -0x400000);
    AppendPcm24(pcm24, 0x7FFFFF);
    AppendPcm24(pcm24, -0x800000);
    const std::filesystem::path pcm24Path = WriteTempWav(
        "pcm24_mono.wav", MakeWav(1, 1, 48000, 24, pcm24));

    AudioClip clip;
    std::string error;
    ASSERT_TRUE(LoadWavFile(pcm24Path, clip, error)) << error;
    ASSERT_EQ(clip.samples.size(), 4u);
    EXPECT_NEAR(clip.samples[0], 0.5f, 1.0e-5f);
    EXPECT_NEAR(clip.samples[1], -0.5f, 1.0e-5f);
    EXPECT_NEAR(clip.samples[2], 1.0f, 1.0e-5f);
    EXPECT_NEAR(clip.samples[3], -1.0f, 1.0e-5f);
    std::filesystem::remove(pcm24Path);

    std::vector<std::uint8_t> floatPayload;
    for (float value : {0.25f, -0.75f})
    {
        std::uint8_t bytes[4] = {};
        std::memcpy(bytes, &value, sizeof(value));
        floatPayload.insert(floatPayload.end(), bytes, bytes + 4);
    }
    const std::filesystem::path floatPath = WriteTempWav(
        "float32_mono.wav", MakeWav(3, 1, 48000, 32, floatPayload));
    ASSERT_TRUE(LoadWavFile(floatPath, clip, error)) << error;
    ASSERT_EQ(clip.samples.size(), 2u);
    EXPECT_NEAR(clip.samples[0], 0.25f, 1.0e-6f);
    EXPECT_NEAR(clip.samples[1], -0.75f, 1.0e-6f);
    std::filesystem::remove(floatPath);
}

TEST(AudioClipLoading, RejectsTruncatedAndCompressedWaveFiles)
{
    std::vector<std::uint8_t> payload(8, 0);
    std::vector<std::uint8_t> bytes =
        MakeWav(1, 1, 48000, 16, payload);
    bytes.resize(bytes.size() - 4);
    const std::filesystem::path truncatedPath = WriteTempWav(
        "truncated.wav", bytes);

    AudioClip clip;
    std::string error;
    EXPECT_FALSE(LoadWavFile(truncatedPath, clip, error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove(truncatedPath);

    const std::filesystem::path compressedPath = WriteTempWav(
        "compressed.wav", MakeWav(0x0011, 1, 48000, 4, payload));
    EXPECT_FALSE(LoadWavFile(compressedPath, clip, error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove(compressedPath);

    const std::filesystem::path tooManyChannelsPath = WriteTempWav(
        "too_many_channels.wav",
        MakeWav(1, 9, 48000, 16, std::vector<std::uint8_t>(36, 0)));
    EXPECT_FALSE(LoadWavFile(tooManyChannelsPath, clip, error));
    EXPECT_FALSE(error.empty());
    std::filesystem::remove(tooManyChannelsPath);
}

TEST(AudioClipProcessing, ResamplingProducesExpectedLengthAndEndpoints)
{
    std::vector<float> ramp(1000);
    for (std::size_t index = 0; index < ramp.size(); ++index)
        ramp[index] = static_cast<float>(index);

    const std::vector<float> resampled =
        ResampleLinear(ramp, 8000, 12000);
    ASSERT_EQ(resampled.size(), 1500u);
    EXPECT_NEAR(resampled.front(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(resampled.back(), 999.0f, 1.0e-3f);
    for (std::size_t index = 1; index < resampled.size(); ++index)
        EXPECT_GE(resampled[index], resampled[index - 1]);
}

TEST(AudioClipProcessing, SegmentClampsToSourceAndAppliesFades)
{
    std::vector<float> samples(100, 1.0f);
    const std::vector<float> segment = MakeSegment(
        samples, 1000, 0.01, 0.05, 0.01, 0.01);
    ASSERT_EQ(segment.size(), 50u);
    EXPECT_NEAR(segment.front(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(segment.back(), 0.0f, 1.0e-5f);
    EXPECT_NEAR(segment[25], 1.0f, 1.0e-5f);

    const std::vector<float> clamped =
        MakeSegment(samples, 1000, 0.09, 0.20, 0.0, 0.0);
    EXPECT_EQ(clamped.size(), 10u);
}

TEST(AudioClipProcessing, PeakAndGainClampSampledValues)
{
    std::vector<float> samples{-2.0f, 0.25f, 1.5f};
    EXPECT_NEAR(PeakAmplitude(samples), 2.0f, 1.0e-6f);
    ApplyGain(samples, 2.0f);
    EXPECT_NEAR(samples[0], -1.0f, 1.0e-6f);
    EXPECT_NEAR(samples[1], 0.5f, 1.0e-6f);
    EXPECT_NEAR(samples[2], 1.0f, 1.0e-6f);
}
