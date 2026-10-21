#pragma once

#include "motionMatching.h"
#include <filesystem>

namespace VL::Animation
{
struct SkeletalAsset;

// 工具与运行时共用同一输入身份/缓存协议；运行时只在首次请求时读盘，
// 绑定后的不可变资源由 World 缓存，不把路径或可写统计交给 CLR/RT。
nlohmann::json BuildMotionDatabaseReport(
    const std::filesystem::path& resourceRoot, const std::string& sourcePath);
nlohmann::json CaptureMotionDatabaseInputs(
    const std::filesystem::path& resourceRoot, const std::string& sourcePath);

// 正式原根一个周期的模型空间水平平均速度；不适用于原地或周期转向的片段。
// 绑定时派生后冻结，不改变数据库名义倍率的特征和缓存身份。
struct MotionLoopVelocity
{
    Eigen::Vector3f velocity = Eigen::Vector3f::Zero();
    bool available = false;
};

MotionLoopVelocity ExtractLoopRootVelocity(const SkeletonMesh& mesh, const Clip& clip, bool loop);

struct MotionRuntimeDatabase
{
    std::string sourcePath;
    std::string identity;
    std::string payloadIdentity;
    std::string modelPath;
    std::shared_ptr<const MotionSearchDatabase> search;
    std::vector<size_t> assetClips;
    std::vector<MotionLoopVelocity> loopVelocities;
    std::vector<size_t> joints;
};

std::shared_ptr<const MotionRuntimeDatabase> LoadMotionRuntimeDatabase(
    const std::filesystem::path& resourceRoot, const std::string& sourcePath,
    const std::string& modelPath, const SkeletalAsset& asset);
}
