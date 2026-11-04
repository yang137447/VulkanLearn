// TFX layout interpreted from AMD TressFXTFXFileHeader.h and TressFX_Exporter.py.
// Copyright (c) 2016-2025 Advanced Micro Devices, Inc. All rights reserved.
// MIT license and source revision: THIRD_PARTY_NOTICES.md.
#include "hair/tfxLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <new>

namespace VL::Hair
{
namespace
{
constexpr std::size_t HeaderBytes = 160;
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);

struct ParseFailure
{
    RuntimeError error;
};

[[noreturn]] void Reject(
    const char* code, const std::string& message,
    const std::string& path, const std::string& field)
{
    throw ParseFailure{MakeRuntimeError(code, message, path, field)};
}

std::uint32_t ReadU32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::uint32_t>(bytes[offset])
        | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8u)
        | (static_cast<std::uint32_t>(bytes[offset + 2]) << 16u)
        | (static_cast<std::uint32_t>(bytes[offset + 3]) << 24u);
}

float ReadFloat(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    const auto bits = ReadU32(bytes, offset);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

struct Block
{
    const char* name;
    std::size_t offset;
    std::size_t count;
    std::size_t components;
    bool required;

    std::size_t End() const { return offset + count * components * sizeof(float); }
};

void ValidateBlocks(
    const std::vector<std::uint8_t>& bytes, const std::array<Block, 5>& blocks,
    const std::string& path)
{
    // count 已受顶点预算约束；先证明所有字节范围有效，再分配或读取 payload。
    for (std::size_t i = 0; i < blocks.size(); ++i)
    {
        const auto& block = blocks[i];
        if (!block.offset)
        {
            if (block.required)
                Reject("Hair.TfxOffset", "Required position block is missing", path, block.name);
            continue;
        }
        if (block.offset < HeaderBytes || block.offset % 8u != 0u)
            Reject("Hair.TfxOffset", "Block offset must be after the header and aligned to 8 bytes",
                path, block.name);
        const auto byteCount = block.count * block.components * sizeof(float);
        if (block.offset > bytes.size() || byteCount > bytes.size() - block.offset)
            Reject("Hair.TfxTruncated", "Block exceeds the file size", path, block.name);
        for (std::size_t j = 0; j < i; ++j)
        {
            const auto& previous = blocks[j];
            if (previous.offset && block.offset < previous.End() && previous.offset < block.End())
                Reject("Hair.TfxOverlap", "Attribute blocks overlap", path, block.name);
        }
    }
}

template <std::size_t Components>
std::vector<std::array<float, Components>> ReadVectors(
    const std::vector<std::uint8_t>& bytes, const Block& block, const std::string& path)
{
    if (!block.offset) return {};
    std::vector<std::array<float, Components>> values(block.count);
    auto offset = block.offset;
    for (auto& value : values)
    {
        for (auto& component : value)
        {
            component = ReadFloat(bytes, offset);
            if (!std::isfinite(component))
                Reject("Hair.TfxNonFinite", "Attribute contains NaN or infinity", path, block.name);
            offset += sizeof(float);
        }
    }
    return values;
}

TfxAsset Parse(const std::vector<std::uint8_t>& bytes, const std::string& path)
{
    if (bytes.size() > MaxTfxFileBytes)
        Reject("Hair.TfxBudget", "TFX file exceeds the 512 MiB input budget", path, "file");
    if (bytes.size() < HeaderBytes)
        Reject("Hair.TfxTruncated", "TFX header requires 160 bytes", path, "header");

    TfxAsset asset;
    asset.version = ReadFloat(bytes, 0);
    // 5.0 Maya exporter 实际写 4.0；版本号不等于产品版本。保留原加载器的 >=4 策略，
    // 只解释已知字段并严格验证布局，不从未知版本推测任何新字段。
    if (!std::isfinite(asset.version) || asset.version < 4.0f)
        Reject("Hair.TfxVersion", "Expected a finite TFX layout version >= 4", path, "version");
    asset.guideCount = ReadU32(bytes, 4);
    asset.pointsPerStrand = ReadU32(bytes, 8);
    if (!asset.guideCount)
        Reject("Hair.TfxCount", "TFX must contain at least one guide", path, "guideCount");
    const auto points = asset.pointsPerStrand;
    if (points < 4 || points > 64 || (points & (points - 1u)) != 0u)
        Reject("Hair.TfxCount", "Points per strand must be 4, 8, 16, 32 or 64", path, "pointsPerStrand");
    if (asset.guideCount > MaxTfxVertexCount / points)
        Reject("Hair.TfxBudget", "TFX exceeds the 4194304 vertex budget", path, "guideCount");
    const auto vertexCount = static_cast<std::size_t>(asset.guideCount) * points;

    // 头文件的 position 注释误写成每根一个 FLOAT4；exporter/loader 都是每点一个。
    const std::array<Block, 5> blocks{{
        {"positions", ReadU32(bytes, 12), vertexCount, 4, true},
        {"strandUVs", ReadU32(bytes, 16), asset.guideCount, 2, false},
        {"vertexUVs", ReadU32(bytes, 20), vertexCount, 2, false},
        {"strandThickness", ReadU32(bytes, 24), asset.guideCount, 1, false},
        {"vertexColors", ReadU32(bytes, 28), vertexCount, 4, false}
    }};
    ValidateBlocks(bytes, blocks, path);
    asset.positions = ReadVectors<4>(bytes, blocks[0], path);
    asset.strandUVs = ReadVectors<2>(bytes, blocks[1], path);
    asset.vertexUVs = ReadVectors<2>(bytes, blocks[2], path);
    if (blocks[3].offset)
    {
        asset.strandThickness.resize(blocks[3].count);
        for (std::size_t i = 0; i < blocks[3].count; ++i)
        {
            const auto thickness = ReadFloat(bytes, blocks[3].offset + i * sizeof(float));
            if (!std::isfinite(thickness) || thickness < 0)
                Reject("Hair.TfxThickness", "Strand thickness must be finite and nonnegative",
                    path, blocks[3].name);
            asset.strandThickness[i] = thickness;
        }
    }
    asset.vertexColors = ReadVectors<4>(bytes, blocks[4], path);
    for (std::size_t axis = 0; axis < 3; ++axis)
        asset.boundsMin[axis] = asset.boundsMax[axis] = asset.positions.front()[axis];
    for (const auto& position : asset.positions)
    {
        if (position[3] < 0)
            Reject("Hair.TfxInverseMass", "Position w must be a nonnegative inverse mass", path, "positions");
        for (std::size_t axis = 0; axis < 3; ++axis)
        {
            asset.boundsMin[axis] = std::min(asset.boundsMin[axis], position[axis]);
            asset.boundsMax[axis] = std::max(asset.boundsMax[axis], position[axis]);
        }
    }
    return asset;
}
}

RuntimeResult<TfxAsset> ParseTfx(
    const std::vector<std::uint8_t>& bytes, const std::string& sourcePath)
{
    try { return RuntimeResult<TfxAsset>::Success(Parse(bytes, sourcePath)); }
    catch (const ParseFailure& failure) { return RuntimeResult<TfxAsset>::Failure(failure.error); }
    catch (const std::bad_alloc&)
    {
        return RuntimeResult<TfxAsset>::Failure(MakeRuntimeError(
            "Hair.TfxBudget", "Cannot allocate TFX asset data", sourcePath));
    }
}

RuntimeResult<TfxAsset> LoadTfx(const std::filesystem::path& path)
{
    const auto source = path.u8string();
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input)
        return RuntimeResult<TfxAsset>::Failure(MakeRuntimeError(
            "Hair.TfxRead", "Cannot open TFX file", source));
    const auto size = input.tellg();
    if (size < 0)
        return RuntimeResult<TfxAsset>::Failure(MakeRuntimeError(
            "Hair.TfxRead", "Cannot determine TFX file size", source));
    if (size > static_cast<std::streamoff>(MaxTfxFileBytes))
        return RuntimeResult<TfxAsset>::Failure(MakeRuntimeError(
            "Hair.TfxBudget", "TFX file exceeds the 512 MiB input budget", source));
    try
    {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        input.seekg(0);
        if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), size))
            return RuntimeResult<TfxAsset>::Failure(MakeRuntimeError(
                "Hair.TfxRead", "Cannot read complete TFX file", source));
        // 不把目录流或读取失败形成的空数据交给正式资产消费者。
        return ParseTfx(bytes, source);
    }
    catch (const std::bad_alloc&)
    {
        return RuntimeResult<TfxAsset>::Failure(MakeRuntimeError(
            "Hair.TfxBudget", "Cannot allocate TFX input bytes", source));
    }
}
}
