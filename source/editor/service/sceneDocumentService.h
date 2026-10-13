#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

#include "editor/service/assetDocumentService.h"
#include "editor/scene/editorActorProperty.h"
#include "editor/ui/materialInstanceEditorView.h"

namespace VL::Editor
{
// GT 拥有按资产路径索引的关卡草稿；只消费已接受的 actor 值快照，不持有 World。
// 保存补丁保留未编辑 JSON，预览/观察相机与临时可见性不进入文档。
class SceneDocumentService : public AssetDocumentService
{
public:
    explicit SceneDocumentService(std::filesystem::path resourceRoot);
    bool Open(const std::string& path, std::string& error);
    bool PrepareActorEdit(const std::string& path, const std::string& identity,
        std::string& error);
    void RecordActorEdit(const EditorUi::EditorSceneActorSnapshot& actor,
        const EditorActorPropertyEdit& edit);
    EditorCommandResult Save(const EditorCommandEnvelope& command);
    std::vector<EditorActorPropertyEdit> ReplayEdits(const std::string& path,
        uint64_t generation, std::string& error);
    static bool IsPreviewScene(const std::string& path);
    // 标准修改以完整草稿参与下一次 World 事务；外部冲突时拒绝使用旧基线。
    nlohmann::json DraftForWorldLoad(const std::string& path, std::string& error);
    void NotifyWorldLoaded(const std::string& path);

private:
    std::string Normalize(const std::string& path) const;
    std::filesystem::path Resolve(const std::string& path) const;
    static nlohmann::json* FindActor(nlohmann::json& json, const std::string& identity);
};
} // namespace VL::Editor
