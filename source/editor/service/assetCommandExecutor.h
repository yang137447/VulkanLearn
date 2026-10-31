#pragma once

#include "editor/service/sceneDocumentService.h"

namespace VL::Editor
{
// Runtime 与 headless host 共用的资产命令分派；各 kind 独占草稿，TressFX 不借用 Mesh owner。
// 调用方负责命令队列与线程边界；这里不操作 World 或 Vulkan。
class AssetCommandExecutor
{
public:
    AssetCommandExecutor(const std::filesystem::path& root, SceneDocumentService& scenes)
        : scenes(scenes), meshes(root, EditorAssetDocumentKind::Mesh),
          textures(root, EditorAssetDocumentKind::Texture), blueprints(root, EditorAssetDocumentKind::Blueprint),
          tressfx(root, EditorAssetDocumentKind::Tressfx) {}
    EditorCommandResult Execute(const EditorCommandEnvelope& command);
    std::vector<EditorUi::EditorModifiedDocumentSnapshot> ModifiedDocuments() const;
    AssetDocumentService& BlueprintDocuments() { return blueprints; }
    // 毛发视口需要读取独立 TressFX 草稿与 revision；只提供只读快照入口，
    // 仍由 AssetDocumentService 独占草稿与保存边界。
    AssetDocumentService& TressfxDocuments() { return tressfx; }
private:
    SceneDocumentService& scenes;
    AssetDocumentService meshes, textures, blueprints, tressfx;
};
} // namespace VL::Editor
