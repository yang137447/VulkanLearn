#pragma once

#include "editor/command/editorCommand.h"

namespace VL::Editor
{
// 字段定义同时约束协议输入、隔离候选与能力投影；不访问文件、World 或 renderer。
nlohmann::json EncodeAssetChanges(const std::vector<EditorAssetFieldChange>& changes,
    EditorAssetDocumentKind kind);
std::vector<EditorAssetFieldChange> DecodeAssetChanges(const nlohmann::json& changes,
    EditorAssetDocumentKind kind);
nlohmann::json ApplyAssetChanges(nlohmann::json& candidate, EditorAssetDocumentKind kind,
    const std::vector<EditorAssetFieldChange>& changes);
nlohmann::json DescribeAssetCapabilities(EditorAssetDocumentKind kind, const nlohmann::json& document);
nlohmann::json ProjectEditableAsset(EditorAssetDocumentKind kind, const nlohmann::json& document);
} // namespace VL::Editor
