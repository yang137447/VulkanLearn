#include "editor/service/assetCommandExecutor.h"

namespace VL::Editor
{
EditorCommandResult AssetCommandExecutor::Execute(const EditorCommandEnvelope& command)
{
    const auto kind = GetEditorAssetDocumentKind(command.type);
    if (kind == EditorAssetDocumentKind::Scene) return scenes.Execute(command);
    if (kind == EditorAssetDocumentKind::Mesh) return meshes.Execute(command);
    if (kind == EditorAssetDocumentKind::Texture) return textures.Execute(command);
    if (kind == EditorAssetDocumentKind::Blueprint) return blueprints.Execute(command);
    EditorCommandResult result;
    result.commandId = command.commandId;
    result.status = EditorCommandStatus::Rejected;
    result.errorCode = EditorErrorCode::InvalidCommandType;
    result.message = "Unsupported asset document command";
    return result;
}

std::vector<EditorUi::EditorModifiedDocumentSnapshot> AssetCommandExecutor::ModifiedDocuments() const
{
    auto result = scenes.ModifiedDocuments();
    const auto meshDocuments = meshes.ModifiedDocuments(), textureDocuments = textures.ModifiedDocuments();
    result.insert(result.end(), meshDocuments.begin(), meshDocuments.end());
    result.insert(result.end(), textureDocuments.begin(), textureDocuments.end());
    const auto blueprintDocuments = blueprints.ModifiedDocuments();
    result.insert(result.end(), blueprintDocuments.begin(), blueprintDocuments.end());
    return result;
}
} // namespace VL::Editor
