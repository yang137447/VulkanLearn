#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace VL::AnimationPreflight
{
// 只读 Assimp 数据审计；输出原始层级、权重统计与动画关键帧，不做 VL 空间转换或资源发布。
nlohmann::json InspectFbx(const std::filesystem::path& path);
}
