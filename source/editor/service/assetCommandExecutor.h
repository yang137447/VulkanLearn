#pragma once

#include "editor/service/sceneDocumentService.h"

namespace VL::Editor
{
// Runtime 与 headless host 共用的资产命令分派；借用唯一 Scene owner，独占 Mesh/Texture 草稿。
// 调用方负责命令队列与线程边界；这里不操作 World 或 Vulkan。
class AssetCommandExecutor
{
public:
    AssetCommandExecutor(const std::filesystem::path& root, SceneDocumentService& scenes)
        : scenes(scenes), meshes(root, EditorAssetDocumentKind::Mesh),
          textures(root, EditorAssetDocumentKind::Texture), blueprints(root, EditorAssetDocumentKind::Blueprint) {}
    EditorCommandResult Execute(const EditorCommandEnvelope& command);
    std::vector<EditorUi::EditorModifiedDocumentSnapshot> ModifiedDocuments() const;
    AssetDocumentService& BlueprintDocuments() { return blueprints; }
private:
    SceneDocumentService& scenes;
    AssetDocumentService meshes, textures, blueprints;
};
} // namespace VL::Editor
