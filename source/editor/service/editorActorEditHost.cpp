#include "engine/editorActorEditHost.h"

#include "editor/scene/editorSceneActorBuilder.h"
#include "editor/service/sceneDocumentService.h"

namespace VL
{

bool PrepareEditorActorEdit(Editor::SceneDocumentService* sceneDocuments,
    const std::string& scenePath, const std::string& identity, std::string& error)
{
    return sceneDocuments != nullptr &&
        sceneDocuments->PrepareActorEdit(scenePath, identity, error);
}

void RecordEditorActorEdit(Editor::SceneDocumentService* sceneDocuments,
    World& world, const EditorActorPropertyEdit& edit)
{
    if (sceneDocuments == nullptr) return;
    // actor 快照的构建仍完全留在编辑器侧；运行时执行器只调用本转发函数。
    for (const auto& actor : BuildEditorSceneActors(world))
        if (actor.identity == edit.identity)
            sceneDocuments->RecordActorEdit(actor, edit);
}

} // namespace VL
