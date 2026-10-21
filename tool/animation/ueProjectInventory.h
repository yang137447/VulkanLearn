#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace VL::AnimationPreflight
{
// 只读盘点指定 UE 项目的作者输入，供 P0 固定版本和差异比较；文件名提示不等于资产类型。
// 输出清单和 BLAKE3 身份，不加载 UE、不保存原项目，也不推断许可、骨架映射或 GASP 身份。
nlohmann::json InventoryUnrealProject(const std::filesystem::path& project);

// 输出路径在任何文件创建前校验；按文件系统身份识别项目内路径，包含 Windows 大小写别名。
void ValidateUnrealInventoryOutput(
    const std::filesystem::path& project,
    const std::filesystem::path& output);
}
