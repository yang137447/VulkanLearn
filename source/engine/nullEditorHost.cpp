#include "engine/nullEditorHost.h"

namespace VL
{

RuntimeResult<void> NullEditorHost::Initialize(const EditorHostInit&)
{
    return RuntimeResult<void>::Success();
}

void NullEditorHost::Shutdown() {}
void NullEditorHost::ReleaseRendererResources() {}
void NullEditorHost::BeginUiActionBatch() {}

bool NullEditorHost::ApplyUiAction(
    const UiAction&,
    World*,
    RuntimeCommandExecutionResult&)
{
    return false;
}

void NullEditorHost::EndUiActionBatch(World*) {}
bool NullEditorHost::ApplyTerrainEditUpdate(World&) { return false; }
void NullEditorHost::Tick(float, World*) {}
void NullEditorHost::TickThumbnails(const std::vector<PlatformEvent>&) {}

bool NullEditorHost::CanUndo(const std::string&, bool) const
{
    return false;
}

nlohmann::json NullEditorHost::DraftSceneDocument(
    const std::string&,
    std::string&)
{
    return nullptr;
}

bool NullEditorHost::PrepareActorEdit(
    const std::string&,
    const std::string&,
    std::string&)
{
    return false;
}

void NullEditorHost::RecordActorEdit(World&, const ActorPropertyEdit&) {}
void NullEditorHost::ReplaySceneEditsAfterWorldChange(
    const std::string&,
    uint64_t,
    World&)
{
}

void NullEditorHost::ResetAuthoringSessions() {}

std::shared_ptr<const void> NullEditorHost::BuildSceneActorSnapshots(
    const World&) const
{
    return {};
}

std::shared_ptr<const void> NullEditorHost::BuildSceneModelSnapshots() const
{
    return {};
}

void NullEditorHost::BuildUiSections(const World*) {}

bool NullEditorHost::SelectSceneObjectFromPick(
    const MaterialInstanceSelection&)
{
    return false;
}

void NullEditorHost::ClearMaterialInstanceSceneSelection() {}
IEditorConsoleSink* NullEditorHost::ConsoleSink() { return nullptr; }
IEditorViewportOverlay* NullEditorHost::ViewportOverlay() { return nullptr; }
bool NullEditorHost::IsEditorFlavor() const { return false; }
bool NullEditorHost::IsEnabled() const { return false; }
bool NullEditorHost::IsVisible() const { return false; }

} // namespace VL
