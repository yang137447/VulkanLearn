#pragma once
#include <filesystem>
#include <nlohmann/json.hpp>

namespace VL::AnimationPreflight
{
// 正式源描述/模型/schema/FBX 的只读构建入口；仅向资源 Generated 发布派生缓存。
// 报告含稳定输入摘要与缓存命中原因，不改变源资产或运行时播放器。
nlohmann::json BuildMotionDatabaseReport(
    const std::filesystem::path& resourceRoot, const std::string& sourcePath);
nlohmann::json CaptureMotionDatabaseInputs(
    const std::filesystem::path& resourceRoot, const std::string& sourcePath);
}
