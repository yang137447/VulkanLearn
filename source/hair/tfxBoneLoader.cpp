// .tfxbone layout interpreted from AMD TressFXAsset.cpp LoadBoneData
// (Copyright (c) 2016-2025 Advanced Micro Devices, Inc., MIT license).
#include "hair/tfxBoneLoader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace VL::Hair
{
namespace
{
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

std::int32_t ReadI32(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    return static_cast<std::int32_t>(ReadU32(bytes, offset));
}

float ReadFloat(const std::vector<std::uint8_t>& bytes, std::size_t offset)
{
    const std::uint32_t bits = ReadU32(bytes, offset);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void RequireBytes(const std::vector<std::uint8_t>& bytes, std::size_t offset,
    std::size_t count, const std::string& path, const std::string& field)
{
    if (offset > bytes.size() || count > bytes.size() - offset)
        Reject("Hair.TfxBoneTruncated",
            "tfxbone payload is truncated while reading " + field, path, field);
}
} // namespace

RuntimeResult<TfxBoneAsset> ParseTfxBone(
    const std::vector<std::uint8_t>& bytes, const std::string& sourcePath)
{
    try
    {
        TfxBoneAsset asset;
        std::size_t offset = 0;
        RequireBytes(bytes, offset, sizeof(std::int32_t), sourcePath, "numBones");
        const std::int32_t numBones = ReadI32(bytes, offset);
        offset += sizeof(std::int32_t);
        if (numBones < 0 || static_cast<std::uint32_t>(numBones) > TfxBoneMaxBones)
            Reject("Hair.TfxBoneBoneCount",
                "tfxbone bone count is outside 0.." + std::to_string(TfxBoneMaxBones),
                sourcePath, "numBones");
        asset.boneNames.reserve(static_cast<std::size_t>(numBones));

        for (std::int32_t bone = 0; bone < numBones; ++bone)
        {
            const std::string field = "boneNames[" + std::to_string(bone) + "]";
            RequireBytes(bytes, offset, 2 * sizeof(std::int32_t), sourcePath, field);
            // index 在源实现里未使用；仍读取以保持字节流对齐。
            offset += sizeof(std::int32_t);
            const std::int32_t charLen = ReadI32(bytes, offset);
            offset += sizeof(std::int32_t);
            if (charLen < 0 || static_cast<std::uint32_t>(charLen) > TfxBoneMaxNameBytes)
                Reject("Hair.TfxBoneNameLength",
                    "tfxbone bone name length is invalid: " + std::to_string(charLen),
                    sourcePath, field);
            RequireBytes(bytes, offset, static_cast<std::size_t>(charLen), sourcePath, field);
            // charLen 包含源实现写入的 null；字符串在第一个 null 处结束。
            const char* text = reinterpret_cast<const char*>(bytes.data() + offset);
            std::size_t length = 0;
            while (length < static_cast<std::size_t>(charLen) && text[length] != '\0')
                ++length;
            asset.boneNames.emplace_back(text, length);
            offset += static_cast<std::size_t>(charLen);
        }

        RequireBytes(bytes, offset, sizeof(std::int32_t), sourcePath, "numStrands");
        const std::int32_t numStrands = ReadI32(bytes, offset);
        offset += sizeof(std::int32_t);
        if (numStrands < 0 || static_cast<std::uint32_t>(numStrands) > TfxBoneMaxStrands)
            Reject("Hair.TfxBoneStrandCount",
                "tfxbone strand count is outside 0.." + std::to_string(TfxBoneMaxStrands),
                sourcePath, "numStrands");
        asset.strands.reserve(static_cast<std::size_t>(numStrands));

        for (std::int32_t strand = 0; strand < numStrands; ++strand)
        {
            const std::string field = "strands[" + std::to_string(strand) + "]";
            RequireBytes(bytes, offset, 2 * sizeof(std::int32_t), sourcePath, field);
            // 每 strand 的 index 在源实现里未使用。
            offset += sizeof(std::int32_t);
            const std::int32_t count = ReadI32(bytes, offset);
            offset += sizeof(std::int32_t);
            if (count < 0 ||
                static_cast<std::uint32_t>(count) > TfxBoneMaxInfluencesPerStrand)
                Reject("Hair.TfxBoneInfluenceCount",
                    "tfxbone influence count is invalid: " + std::to_string(count),
                    sourcePath, field);
            RequireBytes(bytes, offset,
                static_cast<std::size_t>(count) * (sizeof(std::int32_t) + sizeof(float)),
                sourcePath, field);

            TfxBoneStrand parsed;
            parsed.influences.reserve(static_cast<std::size_t>(count));
            for (std::int32_t influence = 0; influence < count; ++influence)
            {
                const std::int32_t boneIndex = ReadI32(bytes, offset);
                offset += sizeof(std::int32_t);
                const float weight = ReadFloat(bytes, offset);
                offset += sizeof(float);
                if (boneIndex >= numBones)
                    Reject("Hair.TfxBoneIndex",
                        "tfxbone bone index " + std::to_string(boneIndex) +
                        " is outside 0.." + std::to_string(numBones - 1),
                        sourcePath, field);
                if (!std::isfinite(weight) || weight < 0.0f)
                    Reject("Hair.TfxBoneWeight",
                        "tfxbone influence weight is not finite/non-negative",
                        sourcePath, field);
                parsed.influences.push_back({boneIndex, weight});
            }
            asset.strands.push_back(std::move(parsed));
        }

        // 源实现允许尾部保留未知字段；这里同样不要求精确耗尽。
        return RuntimeResult<TfxBoneAsset>::Success(std::move(asset));
    }
    catch (const ParseFailure& failure)
    {
        return RuntimeResult<TfxBoneAsset>::Failure(failure.error);
    }
}

RuntimeResult<TfxBoneAsset> LoadTfxBone(const std::filesystem::path& path)
{
    const std::string display = path.string();
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
            return RuntimeResult<TfxBoneAsset>::Failure(MakeRuntimeError(
                "Hair.TfxBoneOpen", "failed to open tfxbone file", display, {}));
        stream.seekg(0, std::ios::end);
        const std::streamoff length = stream.tellg();
        if (length < 0 || static_cast<std::uint64_t>(length) > (1ull << 30))
            return RuntimeResult<TfxBoneAsset>::Failure(MakeRuntimeError(
                "Hair.TfxBoneSize", "tfxbone file size is invalid/too large", display, {}));
        stream.seekg(0, std::ios::beg);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
        if (!bytes.empty())
            stream.read(reinterpret_cast<char*>(bytes.data()), length);
        if (!stream)
            return RuntimeResult<TfxBoneAsset>::Failure(MakeRuntimeError(
                "Hair.TfxBoneRead", "failed to read tfxbone file", display, {}));
        return ParseTfxBone(bytes, display);
    }
    catch (const std::exception& error)
    {
        return RuntimeResult<TfxBoneAsset>::Failure(MakeRuntimeError(
            "Hair.TfxBoneRead", error.what(), display, {}));
    }
}

RuntimeResult<std::vector<TressfxStrandBoneWeights>> MapTfxBoneToSkeleton(
    const TfxBoneAsset& asset, const std::vector<std::string>& jointNames,
    std::uint32_t guideCount, std::vector<std::string>& unmappedBones,
    const std::string& sourcePath)
{
    try
    {
        if (asset.strands.size() > guideCount)
            Reject("Hair.TfxBoneGuideMismatch",
                "tfxbone has " + std::to_string(asset.strands.size()) +
                " strands but the guide asset has " + std::to_string(guideCount),
                sourcePath, "numStrands");

        std::unordered_map<std::string, std::uint32_t> jointByName;
        jointByName.reserve(jointNames.size());
        for (std::uint32_t joint = 0; joint < jointNames.size(); ++joint)
            jointByName.emplace(jointNames[joint], joint);

        std::unordered_set<std::string> reportedUnmapped;
        std::vector<TressfxStrandBoneWeights> result(guideCount);
        for (std::size_t strand = 0; strand < asset.strands.size(); ++strand)
        {
            std::unordered_map<std::uint32_t, float> accumulated;
            for (const TfxBoneInfluence& influence : asset.strands[strand].influences)
            {
                // 源 LoadBoneData 对 boneIndex < 0 或 weight == 0 直接 continue。
                if (influence.boneIndex < 0 || influence.weight == 0.0f)
                    continue;
                const std::size_t index = static_cast<std::size_t>(influence.boneIndex);
                if (index >= asset.boneNames.size())
                    Reject("Hair.TfxBoneIndex",
                        "tfxbone influence references a missing bone name", sourcePath,
                        "strands[" + std::to_string(strand) + "]");
                const std::string& name = asset.boneNames[index];
                const auto joint = jointByName.find(name);
                if (joint == jointByName.end())
                {
                    // 源把未知骨映射成 INDEX_NONE 后跳过；记录而不是静默丢弃。
                    if (reportedUnmapped.insert(name).second)
                        unmappedBones.push_back(name);
                    continue;
                }
                accumulated[joint->second] += influence.weight;
            }

            float sum = 0.0f;
            for (const auto& [joint, weight] : accumulated)
                sum += weight;
            if (sum <= 0.0f)
                continue; // 空权重：调用方回退到根骨/rest，保持源 BoneCount=0 语义。

            // 按 joint 排序，保证文件/运行结果的确定性；权重归一化等价于源
            // shader 的 BoneMatrix /= WeightSum。
            std::vector<std::pair<std::uint32_t, float>> ordered(
                accumulated.begin(), accumulated.end());
            std::sort(ordered.begin(), ordered.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
            TressfxStrandBoneWeights& target = result[strand];
            target.joints.reserve(ordered.size());
            target.weights.reserve(ordered.size());
            for (const auto& [joint, weight] : ordered)
            {
                target.joints.push_back(joint);
                target.weights.push_back(weight / sum);
            }
        }
        return RuntimeResult<std::vector<TressfxStrandBoneWeights>>::Success(
            std::move(result));
    }
    catch (const ParseFailure& failure)
    {
        return RuntimeResult<std::vector<TressfxStrandBoneWeights>>::Failure(
            failure.error);
    }
}
} // namespace VL::Hair
