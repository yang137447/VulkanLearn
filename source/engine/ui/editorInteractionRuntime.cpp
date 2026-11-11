#include "engine/ui/editorInteractionRuntime.h"

#include <filesystem>
#include <memory>
#include <utility>

#include "controller.h"
#include "engine/subsystemCollection.h"
#include "renderSystem.h"
#include "sceneNode.h"
#include "script/scriptRuntime.h"
#include "ui/uiSubsystem.h"
#include "world/world.h"

namespace VL
{

bool IsEditorSceneInteractionEnabled(
    const SubsystemCollection& subsystems,
    const UiSubsystem* uiSubsystem)
{
    if (uiSubsystem == nullptr || !uiSubsystem->IsDeveloperUiVisible() ||
        RenderSystem::GetInstance().IsOfflineMode())
    {
        return false;
    }
    const auto& world = subsystems.GetWorldManager().GetActiveWorld();
    if (!world || !subsystems.GetScriptRuntime().IsEditing())
    {
        return false;
    }
    // 资产预览有自己的 orbit 相机，不参与关卡编辑交互。
    return std::filesystem::path(world->GetScenePath()).filename() !=
        "SC_model_preview.json";
}

bool SynchronizeEditorInteractionState(
    const SubsystemCollection& subsystems,
    UiSubsystem* uiSubsystem,
    Controller* controller,
    bool& wasEnabled)
{
    const bool enabled = IsEditorSceneInteractionEnabled(subsystems, uiSubsystem);
    const auto& world = subsystems.GetWorldManager().GetActiveWorld();
    if (world && world->GetCamera())
    {
        // Play/Pause 期间镜头只属于蓝图运行时；编辑器观察相机必须完全退出。
        world->SetEditorViewEnabled(enabled);
        if (controller != nullptr)
        {
            std::shared_ptr<SceneNode> viewTarget;
            if (enabled)
                viewTarget = world->GetRenderCamera();
            else
                viewTarget = world->GetViewTarget().lock();
            controller->SetViewTarget(
                std::move(viewTarget), world->GetGeneration());
        }
    }
    RenderSystem::GetInstance().SetEditorOverlaysVisible(enabled);
    if (wasEnabled && !enabled)
    {
        // 进入 Play/Pause 时必须清掉编辑器选中态；否则蓝图相机画面里会继续
        // 显示上一帧的选择描边。
        RenderSystem::GetInstance().ClearSelectedMaterialInstance();
        if (uiSubsystem != nullptr)
            uiSubsystem->ClearMaterialInstanceSceneSelection();
    }
    wasEnabled = enabled;
    return enabled;
}

} // namespace VL
