#include "hair/hairTraceFormat.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <system_error>

namespace
{
constexpr size_t SampleFloats = 12;
constexpr size_t SampleBytes = SampleFloats * sizeof(float);

bool IsValidHeader(const VL::Hair::HairTraceHeader& header, std::string& error)
{
    if (header.version != VL::Hair::HairTraceVersion)
    {
        error = "unsupported hair trace version " + std::to_string(header.version) +
            " (expected " + std::to_string(VL::Hair::HairTraceVersion) + ")";
        return false;
    }
    if (header.totalGuideCount == 0 || header.stride == 0)
    {
        error = "hair trace header needs a positive guide count and stride";
        return false;
    }
    if (header.selectedCount == 0 ||
        header.selectedCount > VL::Hair::HairTraceMaxSelectedGuides)
    {
        error = "hair trace selectedCount must be in 1.." +
            std::to_string(VL::Hair::HairTraceMaxSelectedGuides);
        return false;
    }
    if (header.pointsPerGuide < 2 ||
        header.pointsPerGuide > VL::Hair::HairTraceMaxPointsPerGuide)
    {
        error = "hair trace pointsPerGuide must be in 2.." +
            std::to_string(VL::Hair::HairTraceMaxPointsPerGuide);
        return false;
    }
    if (header.selectedGuides.size() != header.selectedCount)
    {
        error = "hair trace selectedGuides size does not match selectedCount";
        return false;
    }
    for (size_t index = 0; index < header.selectedGuides.size(); ++index)
    {
        if (header.selectedGuides[index] >= header.totalGuideCount)
        {
            error = "hair trace selected guide index is outside the object";
            return false;
        }
        if (index > 0 && header.selectedGuides[index] <= header.selectedGuides[index - 1])
        {
            error = "hair trace selected guide indices must be strictly increasing";
            return false;
        }
    }
    const size_t expectedRest =
        static_cast<size_t>(header.selectedCount) * header.pointsPerGuide;
    if (header.restLengths.size() != expectedRest)
    {
        error = "hair trace restLengths size does not match selectedCount * pointsPerGuide";
        return false;
    }
    for (float rest : header.restLengths)
    {
        if (!std::isfinite(rest) || rest < 0.0f)
        {
            error = "hair trace restLengths must be finite and non-negative";
            return false;
        }
    }
    return true;
}
} // namespace

namespace VL::Hair
{
HairTraceWriter::~HairTraceWriter()
{
    if (stream.is_open())
    {
        stream.close();
    }
}

bool HairTraceWriter::Open(std::string filePath, const HairTraceHeader& traceHeader,
    std::string& error)
{
    error.clear();
    if (stream.is_open())
    {
        error = "hair trace writer is already open";
        return false;
    }
    if (!IsValidHeader(traceHeader, error))
    {
        return false;
    }

    // 与 Console 命令一致：允许由调用方给出尚不存在的父目录，但只在显式 trace
    // 时创建；普通渲染路径不会触碰文件系统。
    const std::filesystem::path fsPath(filePath);
    if (fsPath.has_parent_path())
    {
        std::error_code directoryError;
        std::filesystem::create_directories(fsPath.parent_path(), directoryError);
        if (directoryError)
        {
            error = "cannot create hair trace directory: " + directoryError.message();
            return false;
        }
    }

    stream.open(fsPath, std::ios::binary | std::ios::trunc);
    if (!stream.is_open())
    {
        error = "cannot open hair trace file for writing: " + filePath;
        return false;
    }
    path = std::move(filePath);
    header = traceHeader;
    bytesWritten = 0;
    failed = false;
    frameScratch.assign(
        static_cast<size_t>(header.SampleCount()) * SampleFloats, 0.0f);

    if (!WriteRaw(HairTraceMagic.data(), HairTraceMagic.size(), error) ||
        !WriteRaw(&header.version, sizeof(header.version), error) ||
        !WriteRaw(&header.totalGuideCount, sizeof(header.totalGuideCount), error) ||
        !WriteRaw(&header.pointsPerGuide, sizeof(header.pointsPerGuide), error) ||
        !WriteRaw(&header.stride, sizeof(header.stride), error) ||
        !WriteRaw(&header.selectedCount, sizeof(header.selectedCount), error) ||
        !WriteRaw(&header.startFrame, sizeof(header.startFrame), error) ||
        !WriteRaw(header.selectedGuides.data(),
            header.selectedGuides.size() * sizeof(uint32_t), error) ||
        !WriteRaw(header.restLengths.data(),
            header.restLengths.size() * sizeof(float), error))
    {
        stream.close();
        return false;
    }
    return true;
}

bool HairTraceWriter::WriteFrame(const HairTraceFrameInfo& info,
    const float* current,
    const float* previous,
    const float* previousPrevious,
    std::string& error)
{
    error.clear();
    if (!stream.is_open() || failed)
    {
        error = "hair trace writer is not open";
        return false;
    }
    if (current == nullptr || previous == nullptr || previousPrevious == nullptr)
    {
        error = "hair trace frame payload is null";
        return false;
    }
    if (!std::isfinite(info.timeStep) || info.timeStep < 0.0f)
    {
        error = "hair trace timeStep must be finite and non-negative";
        return false;
    }

    // 文件顺序固定为 guide -> point -> (current, previous, prevPrevious)；
    // readback buffer 是 state -> guide -> point，这里做一次交错整理。
    const size_t sampleCount = static_cast<size_t>(header.SampleCount());
    for (size_t sample = 0; sample < sampleCount; ++sample)
    {
        float* destination = frameScratch.data() + sample * SampleFloats;
        std::memcpy(destination, current + sample * 4, 4 * sizeof(float));
        std::memcpy(destination + 4, previous + sample * 4, 4 * sizeof(float));
        std::memcpy(destination + 8, previousPrevious + sample * 4, 4 * sizeof(float));
    }

    if (!WriteRaw(&info.frameIndex, sizeof(info.frameIndex), error) ||
        !WriteRaw(&info.worldGeneration, sizeof(info.worldGeneration), error) ||
        !WriteRaw(&info.substeps, sizeof(info.substeps), error) ||
        !WriteRaw(&info.timeStep, sizeof(info.timeStep), error) ||
        !WriteRaw(&info.resetReason, sizeof(info.resetReason), error) ||
        !WriteRaw(&info.dispatchCount, sizeof(info.dispatchCount), error) ||
        !WriteRaw(info.hairModel.data(), 16 * sizeof(float), error) ||
        !WriteRaw(&info.sdfPenetrationMaxCm,
            sizeof(info.sdfPenetrationMaxCm), error) ||
        !WriteRaw(&info.capsulePenetrationMaxCm,
            sizeof(info.capsulePenetrationMaxCm), error) ||
        !WriteRaw(frameScratch.data(),
            frameScratch.size() * sizeof(float), error))
    {
        failed = true;
        return false;
    }
    return true;
}

bool HairTraceWriter::Close(std::string& error)
{
    error.clear();
    if (!stream.is_open())
    {
        return true;
    }
    stream.flush();
    if (!stream.good())
    {
        error = "failed to flush hair trace file: " + path;
        stream.close();
        return false;
    }
    stream.close();
    return true;
}

bool HairTraceWriter::WriteRaw(const void* data, size_t bytes, std::string& error)
{
    if (bytes == 0)
    {
        return true;
    }
    stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    if (!stream.good())
    {
        error = "failed to write hair trace file: " + path;
        failed = true;
        return false;
    }
    bytesWritten += bytes;
    return true;
}

HairTraceReader::~HairTraceReader()
{
    if (stream.is_open())
    {
        stream.close();
    }
}

HairTraceReadResult HairTraceReader::Open(std::string filePath,
    HairTraceHeader& outHeader,
    std::string& error)
{
    error.clear();
    stream.open(std::filesystem::path(filePath), std::ios::binary);
    if (!stream.is_open())
    {
        error = "cannot open hair trace file for reading: " + filePath;
        return HairTraceReadResult::Error;
    }

    std::array<uint8_t, 4> magic{};
    HairTraceHeader header;
    if (!stream.read(reinterpret_cast<char*>(magic.data()), magic.size()) ||
        magic != HairTraceMagic)
    {
        error = "invalid hair trace magic (expected \"VLHT\")";
        return HairTraceReadResult::Error;
    }
    if (!stream.read(reinterpret_cast<char*>(&header.version), sizeof(header.version)) ||
        !stream.read(reinterpret_cast<char*>(&header.totalGuideCount),
            sizeof(header.totalGuideCount)) ||
        !stream.read(reinterpret_cast<char*>(&header.pointsPerGuide),
            sizeof(header.pointsPerGuide)) ||
        !stream.read(reinterpret_cast<char*>(&header.stride),
            sizeof(header.stride)) ||
        !stream.read(reinterpret_cast<char*>(&header.selectedCount),
            sizeof(header.selectedCount)) ||
        !stream.read(reinterpret_cast<char*>(&header.startFrame),
            sizeof(header.startFrame)))
    {
        error = "truncated hair trace header";
        return HairTraceReadResult::Error;
    }
    // 固定字段先做范围校验，再读取变长列表；此时列表大小尚为 0，不能直接调用
    // IsValidHeader（它要求 selectedGuides/restLengths 已与 selectedCount 对齐）。
    if (header.version != HairTraceVersion ||
        header.totalGuideCount == 0 || header.stride == 0 ||
        header.selectedCount == 0 ||
        header.selectedCount > HairTraceMaxSelectedGuides ||
        header.pointsPerGuide < 2 ||
        header.pointsPerGuide > HairTraceMaxPointsPerGuide)
    {
        error = "invalid hair trace header fields";
        return HairTraceReadResult::Error;
    }

    header.selectedGuides.resize(header.selectedCount);
    if (!stream.read(reinterpret_cast<char*>(header.selectedGuides.data()),
            static_cast<std::streamsize>(
                header.selectedGuides.size() * sizeof(uint32_t))))
    {
        error = "truncated hair trace selected guide list";
        return HairTraceReadResult::Error;
    }
    header.restLengths.resize(
        static_cast<size_t>(header.selectedCount) * header.pointsPerGuide);
    if (!stream.read(reinterpret_cast<char*>(header.restLengths.data()),
            static_cast<std::streamsize>(
                header.restLengths.size() * sizeof(float))))
    {
        error = "truncated hair trace rest length list";
        return HairTraceReadResult::Error;
    }
    // 头已经读完，再做一次完整校验（selectedGuides/restLengths 的大小/取值）。
    if (!IsValidHeader(header, error))
    {
        return HairTraceReadResult::Error;
    }

    this->header = std::move(header);
    outHeader = this->header;
    opened = true;
    return HairTraceReadResult::Frame;
}

HairTraceReadResult HairTraceReader::ReadFrame(HairTraceFrameInfo& info,
    std::vector<HairTraceSample>& samples,
    std::string& error)
{
    error.clear();
    if (!opened)
    {
        error = "hair trace reader is not open";
        return HairTraceReadResult::Error;
    }
    if (stream.peek() == std::char_traits<char>::eof())
    {
        return HairTraceReadResult::EndOfFile;
    }

    if (ReadExact(&info.frameIndex, sizeof(info.frameIndex), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(&info.worldGeneration, sizeof(info.worldGeneration), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(&info.substeps, sizeof(info.substeps), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(&info.timeStep, sizeof(info.timeStep), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(&info.resetReason, sizeof(info.resetReason), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(&info.dispatchCount, sizeof(info.dispatchCount), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(info.hairModel.data(), 16 * sizeof(float), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(&info.sdfPenetrationMaxCm,
            sizeof(info.sdfPenetrationMaxCm), error) !=
            HairTraceReadResult::Frame ||
        ReadExact(&info.capsulePenetrationMaxCm,
            sizeof(info.capsulePenetrationMaxCm), error) !=
            HairTraceReadResult::Frame)
    {
        return HairTraceReadResult::Error;
    }

    const size_t sampleCount = static_cast<size_t>(header.SampleCount());
    std::vector<float> payload(sampleCount * SampleFloats);
    if (ReadExact(payload.data(), payload.size() * sizeof(float), error) !=
        HairTraceReadResult::Frame)
    {
        return HairTraceReadResult::Error;
    }

    samples.resize(sampleCount);
    for (size_t sample = 0; sample < sampleCount; ++sample)
    {
        const float* source = payload.data() + sample * SampleFloats;
        samples[sample].current = Eigen::Vector4f(source[0], source[1], source[2], source[3]);
        samples[sample].previous =
            Eigen::Vector4f(source[4], source[5], source[6], source[7]);
        samples[sample].previousPrevious =
            Eigen::Vector4f(source[8], source[9], source[10], source[11]);
    }
    return HairTraceReadResult::Frame;
}

HairTraceReadResult HairTraceReader::ReadExact(void* destination, size_t bytes,
    std::string& error)
{
    stream.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
    if (static_cast<size_t>(stream.gcount()) == bytes)
    {
        return HairTraceReadResult::Frame;
    }
    error = "truncated hair trace frame payload";
    return HairTraceReadResult::Error;
}

const char* HairTraceResetReasonName(uint32_t reason)
{
    switch (reason)
    {
    case 0: return "none";
    case 1: return "initial-state";
    case 2: return "first-pose";
    case 3: return "playback-changed";
    case 4: return "pose-discontinuity";
    case 5: return "pose-recovered";
    default: return "unknown";
    }
}
} // namespace VL::Hair
