#pragma once

#include "core/runtimeResult.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace VL::Hair
{
// TressFX .tfxbone 的作者数据。源 LoadBoneData（TressFXAsset.cpp:417-501）按
// 小端 int32/float 读取：numBones、每骨 index/charLen/ANSI 名字、numStrands、
// 每 strand index/count + count×(boneIndex, weight)。这里保留原始名字与权重，
// 不静默丢未知骨；映射到目标骨架由 MapTfxBoneToSkeleton 负责。
struct TfxBoneInfluence
{
    // 文件内 boneIndex 指向 boneNames；-1 是源实现的“无骨”标记，解析时保留为
    // 负索引，映射阶段按源行为跳过。
    std::int32_t boneIndex = -1;
    float weight = 0.0f;
};

struct TfxBoneStrand
{
    std::vector<TfxBoneInfluence> influences;
};

struct TfxBoneAsset
{
    std::vector<std::string> boneNames;
    // strands.size() 可以小于 guideCount；源把剩余 strand 的 BoneCount 置 0，
    // 运行时由调用方回退到根骨/rest。
    std::vector<TfxBoneStrand> strands;
};

// 目标骨架上的逐 strand 权重。joints 是调用方给出的 jointNames 下标；
// weights 已归一化到 1（源 shader 的 WeightSum 除法在导入期完成）。
// 空 weights 表示该 strand 没有可用骨，调用方必须回退到根骨。
struct TressfxStrandBoneWeights
{
    std::vector<std::uint32_t> joints;
    std::vector<float> weights;
};

inline constexpr std::uint32_t TfxBoneMaxBones = 4096;
inline constexpr std::uint32_t TfxBoneMaxInfluencesPerStrand = 64;
inline constexpr std::uint32_t TfxBoneMaxStrands = 1u << 20u;
inline constexpr std::uint32_t TfxBoneMaxNameBytes = 4096;

// 只解析 byte 缓冲，失败返回带 path/field 的结构化错误；不访问磁盘。
RuntimeResult<TfxBoneAsset> ParseTfxBone(
    const std::vector<std::uint8_t>& bytes, const std::string& sourcePath = {});
RuntimeResult<TfxBoneAsset> LoadTfxBone(const std::filesystem::path& path);

// 把 .tfxbone 的骨名映射到目标骨架 jointNames，并归一化权重。
// - boneIndex < 0 或 weight == 0：按源 LoadBoneData 的行为跳过。
// - 骨名不在 jointNames：按源行为跳过该影响，但记入 unmappedBones（去重）。
// - 正索引越界、权重非有限/负值、strand 数超过 guideCount：硬错误。
// 返回数组长度 == guideCount；缺失的 strand 是空权重。
RuntimeResult<std::vector<TressfxStrandBoneWeights>> MapTfxBoneToSkeleton(
    const TfxBoneAsset& asset, const std::vector<std::string>& jointNames,
    std::uint32_t guideCount, std::vector<std::string>& unmappedBones,
    const std::string& sourcePath = {});
}
