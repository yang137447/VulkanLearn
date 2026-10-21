#pragma once

#include <filesystem>
#include <nlohmann/json.hpp>

namespace VL::AnimationPreflight
{
// 固定 P0 导出集经原生 CPU 导入后生成验收数据；只读隔离输入，不生成正式资源或推进播放。
nlohmann::json BuildGaspImportReport(const std::filesystem::path& auditDirectory);
// 固定 Walk 的 CPU 变形几何，供普通静态渲染与 GPU 蒙皮进行外部像素对照。
nlohmann::json BuildGaspSkinReference(const std::filesystem::path& auditDirectory, float time);
}
