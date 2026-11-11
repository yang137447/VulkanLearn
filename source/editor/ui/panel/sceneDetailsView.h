#pragma once

#include <functional>
#include <string>
#include <unordered_set>

#include "editor/ui/materialInstanceEditorView.h"
#include "render/camera/cameraExposure.h"
#include "ui/uiRenderSnapshot.h"
#include "ui/widgets/widgetDocument.h"
#include "world/actorProperty.h"

namespace VL::EditorUi
{

struct EditorSceneActorSnapshot;

namespace Panel
{

// Details 的已构建状态仍由面板持有；本模块只借用引用，不复制缓存。
struct DetailsViewContext
{
    UiWidgets::WidgetDocument* widgetDocument = nullptr;
    const MaterialInstanceEditorSnapshot* snapshot = nullptr;
    const UiViewModelSnapshot* runtimeView = nullptr;
    const std::unordered_set<std::string>* collapsedSections = nullptr;
    std::string* detailsIdentity = nullptr;
    std::string* actorDetailsIdentity = nullptr;
    bool* textureOptionsDirty = nullptr;
    bool* refreshingControls = nullptr;
    bool viewingJson = false;
    bool resourceEditorVisible = false;
    const std::string* localStatus = nullptr;
    std::function<std::string(const EditorSceneActorSnapshot&)> sceneLogicComponentsMarkup;
    std::function<void(const EditorSceneActorSnapshot*)> refreshActorValues;
    std::function<void()> refreshControls;
};

void RefreshDetailsView(const DetailsViewContext& context);

// Details 强度以 lux 表示，保留物理光照迁移后的作者范围；不再受旧调试页滑条约束。
inline constexpr float SunIntensityMin = 0.0f;
inline constexpr float SunIntensityMax = 150000.0f;

bool CameraExposureFieldEditable(EditorActorProperty property, ExposureMode mode);
std::string DetailsSectionId(const std::string& key);
std::string ActorSectionKey(const std::string& groupKey);

std::string PrimaryDirectionalLightControls(const EditorSceneActorSnapshot& actor,
    const UiViewModelSnapshot& runtimeView);

std::string BuildActorSectionMarkup(const EditorSceneActorSnapshot& actor,
    const std::unordered_set<std::string>& collapsedSections,
    const UiViewModelSnapshot& runtimeView,
    const std::function<std::string(const EditorSceneActorSnapshot&)>& sceneLogicComponentsMarkup);

void ApplySectionStateView(UiWidgets::WidgetDocument& widgetDocument,
    const std::unordered_set<std::string>& collapsedSections);

} // namespace Panel
} // namespace VL::EditorUi
