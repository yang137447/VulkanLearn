#pragma once

#include <string>

namespace VL
{

class World;
struct EditorActorPropertyEdit;
namespace Editor { class SceneDocumentService; }

// Runtime-side seam for editor scene documents. Implemented by the editor module,
// so the command executor does not include editor service headers or editor UI
// snapshot types. The editor host replaces this seam in P3.
bool PrepareEditorActorEdit(Editor::SceneDocumentService* sceneDocuments,
    const std::string& scenePath, const std::string& identity, std::string& error);
void RecordEditorActorEdit(Editor::SceneDocumentService* sceneDocuments,
    World& world, const EditorActorPropertyEdit& edit);

} // namespace VL
