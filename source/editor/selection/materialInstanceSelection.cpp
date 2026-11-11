#include "editor/selection/materialInstanceSelection.h"

#include <utility>

#include "editor/command/editorCommand.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"

namespace VL::Editor::Selection
{

MaterialInstanceEditorRuntimeSelectionTarget::
    MaterialInstanceEditorRuntimeSelectionTarget(
        MaterialInstanceEditorRuntime& runtime)
    : runtime(&runtime)
{
}

bool MaterialInstanceEditorRuntimeSelectionTarget::OpenMaterialInstance(
    const MaterialInstanceSelection& selection)
{
    if (runtime == nullptr || !runtime->IsInitialized() ||
        selection.worldGeneration == 0 || selection.scenePath.empty() ||
        selection.objectIdentity.empty() ||
        selection.materialInstancePath.empty())
    {
        return false;
    }

    EditorCommandEnvelope command;
    command.source = EditorCommandSource::RmlUi;
    command.type = EditorCommandType::OpenMaterialInstanceAsset;
    command.payload = OpenMaterialInstanceAssetPayload{
        selection.materialInstancePath,
        EditorNavigationOrigin{
            selection.scenePath,
            selection.objectIdentity,
            selection.materialSlotIndex,
            selection.materialSlotName}};
    runtime->Submit(std::move(command));
    return true;
}

bool MaterialInstanceSelectionRouter::Route(
    const MaterialInstanceSelection& selection) const
{
    return target != nullptr && target->OpenMaterialInstance(selection);
}

} // namespace VL::Editor::Selection
