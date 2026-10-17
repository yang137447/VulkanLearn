#pragma once

#include <filesystem>
#include <map>
#include "editor/command/assetDocumentFields.h"
#include "editor/persistence/materialInstancePersistence.h"
#include "editor/scene/editorActorProperty.h"
#include "editor/ui/materialInstanceEditorView.h"

namespace VL::Editor
{
// GT/headless executor 拥有资产 baseline/draft；producer 只消费投影，不触碰文件或 World。
// Scene 派生类复用同一状态保存已接受的 UI 编辑；Blueprint 另有有界作者历史。
class AssetDocumentService
{
public:
    AssetDocumentService(std::filesystem::path root, EditorAssetDocumentKind kind);
    bool Open(const std::string& path, std::string& error);
    EditorCommandResult Execute(const EditorCommandEnvelope& command);
    std::vector<EditorUi::EditorModifiedDocumentSnapshot> ModifiedDocuments() const;
    std::vector<EditorAssetDocumentResultPayload> DocumentSnapshots() const;
    void SetBlueprintClasses(nlohmann::json value) { blueprintClasses = std::move(value); }
    void SetBlueprintComponentClasses(nlohmann::json value) { blueprintComponentClasses = std::move(value); }
    void Clear() { documents.clear(); }

protected:
    struct BlueprintHistoryEntry
    {
        nlohmann::json value;
        size_t bytes = 0;
    };
    struct BlueprintHistory
    {
        std::vector<BlueprintHistoryEntry> undo, redo;
        size_t bytes = 0;
    };
    struct Document
    {
        std::filesystem::path absolutePath;
        nlohmann::json baseline, draft;
        ContentDigest digest{};
        uint64_t revision = 1;
        EditorUi::EditorDocumentStatus status = EditorUi::EditorDocumentStatus::Clean;
        std::string message;
        std::vector<EditorActorPropertyEdit> edits;
        bool replayViaDraft = false;
        bool standardEdited = false;
        bool awaitingWorldLoad = false;
        BlueprintHistory blueprintHistory;
    };
    std::string Normalize(const std::string& path) const;
    std::filesystem::path Resolve(const std::string& path) const;
    void Validate(const nlohmann::json& candidate, const std::string& path) const;
    static void PublishDraft(Document& document, nlohmann::json candidate);
    std::filesystem::path resourceRoot;
    std::map<std::string, Document> documents;
    EditorAssetDocumentKind kind;
    nlohmann::json blueprintClasses = nullptr;
    nlohmann::json blueprintComponentClasses = nullptr;

private:
    EditorAssetDocumentResultPayload Describe(const std::string& path, const Document& document) const;
    void DescribeFailure(const std::string& path, const Document& document,
        const EditorCommandEnvelope& command, EditorCommandResult& result) const;
    std::filesystem::path ResolveReference(const std::string& path) const;
    static void TrimBlueprintHistory(BlueprintHistory& history, bool redo);
    static void PublishBlueprintDraft(Document& document, nlohmann::json candidate);
    void MoveBlueprintHistory(Document& document, const std::string& path, bool redo);
};
} // namespace VL::Editor
