#pragma once

// 文件职责：MI 文档服务的资源根发现、相对路径规范化与
// M_*/MI_*/T_* JSON 资源解析。错误码与规范化结果保持不变。

#include "editor/service/material/materialInstanceDocumentInternal.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace VL::Editor::MaterialDocumentDetail
{

std::string NormalizeSeparators(std::string_view path);
bool IsRegularFile(const std::filesystem::path& path);
bool ContainsParentComponent(const std::filesystem::path& path);
bool HasAssetFileName(std::string_view path, std::string_view prefix);

} // namespace VL::Editor::MaterialDocumentDetail
