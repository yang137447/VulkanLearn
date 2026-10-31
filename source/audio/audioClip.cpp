#include "audio/audioClip.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>
#include <vector>

namespace VL::Audio
{
namespace
{

constexpr std::uint16_t FormatPcm = 0x0001;
constexpr std::uint16_t FormatFloat = 0x0003;
constexpr std::uint16_t FormatExtensible = 0xFFFE;
constexpr std::size_t MaxWavBytes = 256ull * 1024ull * 1024ull;
constexpr std::size_t MaxDecodedFrames = 48000ull * 60ull;
constexpr std::uint16_t MaxChannels = 8;

std::uint16_t ReadU16(const std::uint8_t* data)
{
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(data[0]) |
        (static_cast<std::uint16_t>(data[1]) << 8));
}

std::uint32_t ReadU32(const std::uint8_t* data)
{
    return static_cast<std::uint32_t>(data[0]) |
        (static_cast<std::uint32_t>(data[1]) << 8) |
        (static_cast<std::uint32_t>(data[2]) << 16) |
        (static_cast<std::uint32_t>(data[3]) << 24);
}

bool HasFourCc(const std::uint8_t* data, const char* id)
{
    return std::memcmp(data, id, 4) == 0;
}

float DecodePcmSample(
    const std::uint8_t* data, int bitsPerSample)
{
    switch (bitsPerSample)
    {
    case 8:
        // WAV 8-bit PCM 是无符号偏置格式。
        return (static_cast<float>(data[0]) - 128.0f) / 128.0f;
    case 16:
    {
        const std::int16_t value = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(data[0]) |
            (static_cast<std::uint16_t>(data[1]) << 8));
        return static_cast<float>(value) / 32768.0f;
    }
    case 24:
    {
        std::int32_t value =
            static_cast<std::int32_t>(data[0]) |
            (static_cast<std::int32_t>(data[1]) << 8) |
            (static_cast<std::int32_t>(data[2]) << 16);
        if (value & 0x00800000) value |= ~0x00FFFFFF;
        return static_cast<float>(value) / 8388608.0f;
    }
    case 32:
    {
        const std::int32_t value = static_cast<std::int32_t>(ReadU32(data));
        return static_cast<float>(
            static_cast<double>(value) / 2147483648.0);
    }
    default:
        return 0.0f;
    }
}

float DecodeFloatSample(
    const std::uint8_t* data, int bitsPerSample)
{
    if (bitsPerSample == 32)
    {
        float value = 0.0f;
        std::memcpy(&value, data, sizeof(value));
        return std::isfinite(value) ? value : 0.0f;
    }
    if (bitsPerSample == 64)
    {
        double value = 0.0;
        std::memcpy(&value, data, sizeof(value));
        return std::isfinite(value) ? static_cast<float>(value) : 0.0f;
    }
    return 0.0f;
}

bool ReadWholeFile(
    const std::filesystem::path& path,
    std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file)
    {
        error = "cannot open " + path.string();
        return false;
    }
    const std::streamoff size = file.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > MaxWavBytes)
    {
        error = "invalid WAV size: " + path.string();
        return false;
    }
    bytes.resize(static_cast<std::size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!bytes.empty() &&
        !file.read(reinterpret_cast<char*>(bytes.data()), size))
    {
        error = "cannot read " + path.string();
        return false;
    }
    return true;
}

bool DecodeWaveData(
    const std::vector<std::uint8_t>& bytes,
    std::size_t dataOffset, std::size_t dataSize,
    std::uint16_t format, std::uint16_t channels,
    std::uint16_t bitsPerSample, int sampleRate,
    AudioClip& clip, std::string& error)
{
    if (channels == 0 || channels > MaxChannels)
    {
        error = "WAV has unsupported channel count";
        return false;
    }
    if (bitsPerSample == 0 || bitsPerSample % 8 != 0)
    {
        error = "WAV has unsupported bit depth";
        return false;
    }
    if (format != FormatPcm && format != FormatFloat)
    {
        error = "WAV uses an unsupported compression format";
        return false;
    }
    if (format == FormatPcm &&
        bitsPerSample != 8 && bitsPerSample != 16 &&
        bitsPerSample != 24 && bitsPerSample != 32)
    {
        error = "WAV has unsupported PCM bit depth";
        return false;
    }
    if (format == FormatFloat &&
        bitsPerSample != 32 && bitsPerSample != 64)
    {
        error = "WAV has unsupported float bit depth";
        return false;
    }

    const std::size_t bytesPerSample =
        static_cast<std::size_t>(bitsPerSample / 8);
    const std::size_t blockSize = bytesPerSample * channels;
    const std::size_t frameCount = dataSize / blockSize;
    if (frameCount == 0)
    {
        error = "WAV data chunk has no complete frame";
        return false;
    }
    if (frameCount > MaxDecodedFrames)
    {
        error = "WAV exceeds decoded frame limit";
        return false;
    }

    clip.sampleRate = sampleRate;
    clip.samples.assign(frameCount, 0.0f);
    for (std::size_t frame = 0; frame < frameCount; ++frame)
    {
        const std::uint8_t* frameData =
            bytes.data() + dataOffset + frame * blockSize;
        double sum = 0.0;
        for (std::uint16_t channel = 0; channel < channels; ++channel)
        {
            const std::uint8_t* sampleData =
                frameData + static_cast<std::size_t>(channel) * bytesPerSample;
            const float value = format == FormatFloat
                ? DecodeFloatSample(sampleData, bitsPerSample)
                : DecodePcmSample(sampleData, bitsPerSample);
            sum += std::isfinite(value) ? value : 0.0;
        }
        clip.samples[frame] = std::clamp(
            static_cast<float>(sum / channels), -1.0f, 1.0f);
    }
    return true;
}

} // namespace

bool LoadWavFile(
    const std::filesystem::path& path, AudioClip& clip, std::string& error)
{
    clip = AudioClip{};
    std::vector<std::uint8_t> bytes;
    if (!ReadWholeFile(path, bytes, error)) return false;
    if (bytes.size() < 12 ||
        !HasFourCc(bytes.data(), "RIFF") ||
        !HasFourCc(bytes.data() + 8, "WAVE"))
    {
        error = "not a RIFF/WAVE file: " + path.string();
        return false;
    }

    std::size_t formatOffset = 0;
    std::size_t formatSize = 0;
    std::size_t dataOffset = 0;
    std::size_t dataSize = 0;
    std::size_t cursor = 12;
    while (cursor + 8 <= bytes.size())
    {
        const std::uint8_t* chunk = bytes.data() + cursor;
        const std::uint32_t chunkSize = ReadU32(chunk + 4);
        const std::size_t payload = cursor + 8;
        if (chunkSize > bytes.size() - payload)
        {
            error = "truncated WAV chunk: " + path.string();
            return false;
        }
        if (HasFourCc(chunk, "fmt "))
        {
            formatOffset = payload;
            formatSize = chunkSize;
        }
        else if (HasFourCc(chunk, "data"))
        {
            dataOffset = payload;
            dataSize = chunkSize;
        }
        // RIFF chunk 按偶数字节对齐；奇数 chunk 后有一个 padding byte。
        cursor = payload + chunkSize + (chunkSize & 1u);
    }

    if (formatSize < 16 || formatOffset == 0 || dataOffset == 0)
    {
        error = "WAV is missing fmt or data chunk: " + path.string();
        return false;
    }

    const std::uint8_t* formatData = bytes.data() + formatOffset;
    std::uint16_t format = ReadU16(formatData);
    const std::uint16_t channels = ReadU16(formatData + 2);
    const std::uint32_t sampleRate = ReadU32(formatData + 4);
    const std::uint16_t blockAlign = ReadU16(formatData + 12);
    const std::uint16_t bitsPerSample = ReadU16(formatData + 14);
    if (format == FormatExtensible)
    {
        // WAVE_FORMAT_EXTENSIBLE 的 subformat GUID 前两字节仍是
        // PCM(1)/IEEE float(3)，这里不依赖 GUID 其余字节。
        if (formatSize < 40)
        {
            error = "truncated WAVE_FORMAT_EXTENSIBLE header";
            return false;
        }
        format = ReadU16(formatData + 24);
    }
    if (sampleRate == 0 || sampleRate > 384000)
    {
        error = "WAV has invalid sample rate";
        return false;
    }
    const std::size_t expectedBlock =
        static_cast<std::size_t>(channels) * (bitsPerSample / 8);
    if (blockAlign != 0 && expectedBlock != 0 &&
        static_cast<std::size_t>(blockAlign) != expectedBlock)
    {
        // blockAlign 必须等于一帧字节数；否则按 bits/channels 连续解码
        // 会错位或越界，直接拒绝而不是猜测文件布局。
        error = "WAV block alignment does not match one sample frame";
        return false;
    }
    return DecodeWaveData(
        bytes, dataOffset, dataSize, format, channels, bitsPerSample,
        static_cast<int>(sampleRate), clip, error);
}

std::vector<float> ResampleLinear(
    const std::vector<float>& samples, int sourceRate, int targetRate)
{
    if (samples.empty() || sourceRate <= 0 || targetRate <= 0)
        return {};
    if (sourceRate == targetRate) return samples;

    const double ratio = static_cast<double>(targetRate) /
        static_cast<double>(sourceRate);
    const std::size_t outputCount = static_cast<std::size_t>(
        std::max<long long>(
            1, std::llround(static_cast<double>(samples.size()) * ratio)));
    std::vector<float> output(outputCount, 0.0f);
    const double step = static_cast<double>(sourceRate) /
        static_cast<double>(targetRate);
    const std::size_t last = samples.size() - 1;
    for (std::size_t index = 0; index < outputCount; ++index)
    {
        const double position = static_cast<double>(index) * step;
        const std::size_t base = static_cast<std::size_t>(position);
        if (base >= last)
        {
            output[index] = samples[last];
            continue;
        }
        const float fraction = static_cast<float>(position - base);
        output[index] = samples[base] +
            (samples[base + 1] - samples[base]) * fraction;
    }
    return output;
}

std::vector<float> MakeSegment(
    const std::vector<float>& samples, int sampleRate,
    double startSeconds, double durationSeconds,
    double fadeInSeconds, double fadeOutSeconds)
{
    if (samples.empty() || sampleRate <= 0 || durationSeconds <= 0.0)
        return {};
    const std::size_t start = std::min(
        samples.size(),
        static_cast<std::size_t>(std::max<long long>(
            0, std::llround(startSeconds * sampleRate))));
    const std::size_t requested = static_cast<std::size_t>(
        std::max<long long>(
            0, std::llround(durationSeconds * sampleRate)));
    const std::size_t length = std::min(requested, samples.size() - start);
    if (length == 0) return {};

    std::vector<float> segment(
        samples.begin() + static_cast<std::ptrdiff_t>(start),
        samples.begin() + static_cast<std::ptrdiff_t>(start + length));
    const std::size_t fadeIn = std::min(
        length, static_cast<std::size_t>(std::max<long long>(
            0, std::llround(fadeInSeconds * sampleRate))));
    const std::size_t fadeOut = std::min(
        length, static_cast<std::size_t>(std::max<long long>(
            0, std::llround(fadeOutSeconds * sampleRate))));
    for (std::size_t index = 0; index < fadeIn; ++index)
    {
        const float t = static_cast<float>(index) /
            static_cast<float>(fadeIn);
        segment[index] *= 0.5f - 0.5f * std::cos(
            3.14159265358979323846f * t);
    }
    for (std::size_t index = 0; index < fadeOut; ++index)
    {
        const std::size_t position = length - 1 - index;
        const float t = static_cast<float>(index) /
            static_cast<float>(fadeOut);
        segment[position] *= 0.5f - 0.5f * std::cos(
            3.14159265358979323846f * t);
    }
    return segment;
}

float PeakAmplitude(const std::vector<float>& samples)
{
    float peak = 0.0f;
    for (float sample : samples)
    {
        if (std::isfinite(sample)) peak = std::max(peak, std::abs(sample));
    }
    return peak;
}

void ApplyGain(std::vector<float>& samples, float gain)
{
    for (float& sample : samples)
    {
        sample = std::clamp(
            std::isfinite(sample) ? sample * gain : 0.0f, -1.0f, 1.0f);
    }
}

} // namespace VL::Audio
