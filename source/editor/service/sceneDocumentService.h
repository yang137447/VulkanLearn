#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "editor/persistence/materialInstancePersistence.h"
#include "editor/scene/editorActorProperty.h"
#include "editor/ui/materialInstanceEditorView.h"

namespace VL::Editor
{
// GT 拥有按资产路径索引的关卡草稿；只消费已接受的 actor 值快照，不持有 World。
// 保存补丁保留未编辑 JSON，预览/观察相机与临时可见性不进入文档。
class SceneDocumentService
{
public:
    explicit SceneDocumentService(std::filesystem::path resourceRoot);
    bool Open(const std::string& path, std::string& error);
    bool PrepareActorEdit(const std::string& path, const std::string& identity,
        std::string& error);
    void RecordActorEdit(const EditorUi::EditorSceneActorSnapshot& actor,
        const EditorActorPropertyEdit& edit);
    EditorCommandResult Save(const EditorCommandEnvelope& command);
    std::vector<EditorUi::EditorModifiedDocumentSnapshot> ModifiedDocuments() const;
    std::vector<EditorActorPropertyEdit> ReplayEdits(const std::string& path,
        uint64_t generation, std::string& error);
    static bool IsPreviewScene(const std::string& path);
    void Clear() { documents.clear(); }

private:
    struct Document
    {
        std::filesystem::path absolutePath;
        nlohmann::json baseline;
        nlohmann::json draft;
        ContentDigest digest{};
        uint64_t revision = 1;
        EditorUi::EditorDocumentStatus status = EditorUi::EditorDocumentStatus::Clean;
        std::string message;
        // 按接受顺序重放：法线分量编辑有归一化语义，不能按字段排序或简单去重。
        std::vector<EditorActorPropertyEdit> edits;
    };
    std::string Normalize(const std::string& path) const;
    std::filesystem::path Resolve(const std::string& path) const;
    static nlohmann::json* FindActor(nlohmann::json& json, const std::string& identity);
    std::filesystem::path resourceRoot;
    std::map<std::string, Document> documents;
};
} // namespace VL::Editor
