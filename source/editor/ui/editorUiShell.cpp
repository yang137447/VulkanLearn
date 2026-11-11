#include "editor/ui/editorUiShell.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string_view>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Elements/ElementFormControlSelect.h>

#include "Profiler.h"
#include "engine/diagnosticsSubsystem.h"
#include "engine/editorInteractionPolicy.h"
#include "editor/assets/editorJsonAssetLibrary.h"
#include "editor/assets/editorThumbnailLibrary.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "ui/editorPointerPolicy.h"
#include "ui/uiEditorViewModelSections.h"

namespace VL
{

struct EditorUiShell::Impl final : EditorUi::IMaterialInstanceSceneSelectionSink,
    EditorUi::IEditorRuntimeActionSink,
    EditorUi::IEditorJsonAssetRequestSink
{
    explicit Impl(EditorUiShell& shell) : owner(shell) {}

    bool RequestModelSelection(
        const MaterialInstanceModelContext& model) override
    {
        return owner.RequestModelSelection(model);
    }
    bool RequestMaterialSelection(
        const MaterialInstanceSelection& selection) override
    {
        return owner.RequestMaterialSelection(selection);
    }
    bool RequestActorSelection(const std::string& actorIdentity) override
    {
        return owner.RequestActorSelection(actorIdentity);
    }
    void RequestRuntimeAction(UiAction action) override
    {
        if (bridge != nullptr)
            bridge->RequestRuntimeAction(std::move(action));
    }
    void RequestOpenJsonAsset(const std::string& assetPath) override
    {
        owner.RequestOpenJsonAsset(assetPath);
    }
    void RequestRefreshAssetCatalog() override
    {
        owner.RequestRefreshAssetCatalog();
    }

    RuntimeResult<Rml::ElementDocument*> LoadEditorDocumentCandidate()
    {
        if (context == nullptr)
        {
            return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
                "EditorUiShell.ContextMissing",
                "Editor UI requires the runtime RmlUi context."));
        }
        const auto path = init.assetRoot / "editor-workspace.rml";
        bridge->BeginDocumentValidation();
        Rml::ElementDocument* candidate =
            context->LoadDocument(path.generic_string());
        std::string error;
        const bool parsed = bridge->EndDocumentValidation(error);
        const bool valid = candidate != nullptr && parsed &&
            EditorUi::MaterialInstanceAssetEditorPanel::ValidateDocument(
                *candidate, error);
        if (!valid)
        {
            if (candidate != nullptr)
                context->UnloadDocument(candidate);
            return RuntimeResult<Rml::ElementDocument*>::Failure(MakeRuntimeError(
                "EditorUiShell.DocumentInvalid",
                error.empty() ? "Editor workspace could not be loaded." : error,
                path.string()));
        }
        return RuntimeResult<Rml::ElementDocument*>::Success(candidate);
    }

    RuntimeResult<void> InitializeMaterialInstanceEditor()
    {
        materialInstanceEditorPanel.reset();
        materialInstanceEditorRuntime.reset();
        jsonAssetLibrary.reset();
        thumbnailLibrary.reset();

        if (!init.enabled || editorDocument == nullptr)
            return RuntimeResult<void>::Success();

        std::filesystem::path resourceRoot;
        if (!init.sceneRoot.empty())
            resourceRoot = init.sceneRoot.parent_path();

        std::filesystem::path projectRoot;
        if (!init.assetRoot.empty())
            projectRoot = init.assetRoot.parent_path();

        auto runtime = std::make_unique<Editor::MaterialInstanceEditorRuntime>();
        RuntimeResult<void> result = runtime->Initialize(
            Editor::MaterialInstanceEditorRuntime::Config{
                std::move(resourceRoot),
                std::move(projectRoot),
                256,
                static_cast<Editor::Preview::IMaterialInstancePreviewAdapter*>(
                    init.previewAdapter)});
        if (result.IsFailure())
            return result;

        materialInstanceEditorRuntime = std::move(runtime);
        materialInstanceEditorPanel =
            std::make_unique<EditorUi::MaterialInstanceAssetEditorPanel>(
                materialInstanceEditorRuntime.get());
        materialInstanceEditorPanel->SetSceneSelectionSink(this);
        materialInstanceEditorPanel->SetRuntimeActionSink(this);

        std::string error;
        jsonAssetLibrary = std::make_unique<Editor::EditorJsonAssetLibrary>();
        if (!jsonAssetLibrary->Initialize(init.sceneRoot.parent_path(), error))
        {
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EditorUiShell.AssetLibraryInvalid", error));
        }
        thumbnailLibrary = std::make_unique<Editor::EditorThumbnailLibrary>();
        if (!thumbnailLibrary->Initialize(
                init.sceneRoot.parent_path(),
                init.assetRoot.parent_path() / ".cache" / "editor-thumbnails",
                error))
        {
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EditorUiShell.ThumbnailLibraryInvalid", error));
        }
        materialInstanceEditorPanel->SetJsonAssetRequestSink(this);
        if (!materialInstanceEditorPanel->Attach(*editorDocument, error))
        {
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EditorUiShell.DocumentInvalid", error));
        }
        return RuntimeResult<void>::Success();
    }

    void PublishAssetsSection()
    {
        if (thumbnailLibrary != nullptr)
        {
            // 暂停/轮询与面板是否重建无关：缩略图必须在静止帧继续推进，
            // 只是不触发整份面板 DOM 刷新。
            thumbnailLibrary->SetPaused(!visible || sceneViewportInputActive ||
                editorPointerCaptured);
            thumbnailLibrary->Poll();
        }
        UiAssetsSection assets;
        assets.logs = ProcessLog().Freeze();
        if (jsonAssetLibrary != nullptr)
        {
            assets.assetCatalog = jsonAssetLibrary->GetCatalog();
            assets.jsonDocument = jsonAssetLibrary->GetDocument();
            assets.assetCatalogLoading = jsonAssetLibrary->IsCatalogLoading();
            assets.jsonDocumentLoading = jsonAssetLibrary->IsDocumentLoading();
        }
        if (thumbnailLibrary != nullptr)
            assets.thumbnails = thumbnailLibrary->GetSnapshot();
        if (sectionStore.PublishAssets(std::move(assets)))
        {
            bridge->RecordSectionPublish(UiSectionId::Assets);
            owner.PublishEditorTextures();
        }
    }

    void PublishSceneOutlinerSection()
    {
        static const std::shared_ptr<
            const std::vector<EditorUi::EditorSceneActorSnapshot>> EmptyActors =
            std::make_shared<
                const std::vector<EditorUi::EditorSceneActorSnapshot>>();
        static const std::shared_ptr<
            const std::vector<EditorUi::MaterialInstanceModelSnapshot>> EmptyModels =
            std::make_shared<
                const std::vector<EditorUi::MaterialInstanceModelSnapshot>>();
        UiSceneOutlinerSection payload;
        payload.actors = sceneActors != nullptr ? sceneActors : EmptyActors;
        payload.models = sceneModels != nullptr ? sceneModels : EmptyModels;
        payload.selectedActorIdentity = selectedActorIdentity;
        payload.selectedModel = selectedModelMaterials;
        payload.selectedMaterialSlotIndex = selectedMaterialSlotIndex;
        // 版本源是 outlinerRevision：只有指针替换或选中态变化才推进。
        if (sectionStore.PublishSceneOutliner(
                std::move(payload), outlinerRevision))
        {
            bridge->RecordSectionPublish(UiSectionId::SceneOutliner);
        }
    }

    void BuildDeveloperPanels(
        const UiViewModelSnapshot& view,
        const UiPerformanceSection& performance)
    {
        PROFILE_SCOPE("UI.BuildPanels");
        if (materialInstanceEditorPanel == nullptr ||
            materialInstanceEditorRuntime == nullptr)
        {
            return;
        }
        bridge->MarkPanelBuildMutation();
        static const UiSceneOutlinerSection EmptyOutliner;
        const UiSceneOutlinerSection& outliner =
            sectionStore.SceneOutliner().data != nullptr
            ? *sectionStore.SceneOutliner().data
            : EmptyOutliner;
        EditorUi::MaterialInstanceEditorSnapshot snapshot =
            materialInstanceEditorRuntime->GetSnapshot();
        snapshot.logs = appliedAssets.logs;
        snapshot.assetCatalog = appliedAssets.assetCatalog;
        snapshot.jsonDocument = appliedAssets.jsonDocument;
        snapshot.assetCatalogLoading = appliedAssets.assetCatalogLoading;
        snapshot.jsonDocumentLoading = appliedAssets.jsonDocumentLoading;
        snapshot.thumbnails = appliedAssets.thumbnails;
        snapshot.sceneModels = outliner.models != nullptr
            ? *outliner.models
            : std::vector<EditorUi::MaterialInstanceModelSnapshot>{};
        snapshot.sceneActors = outliner.actors;
        snapshot.selectedActorIdentity = outliner.selectedActorIdentity;
        snapshot.selectedModel = outliner.selectedModel;
        snapshot.selectedMaterialSlotIndex = outliner.selectedMaterialSlotIndex;
        snapshot.performance = performance.performance;
        (void)view;
        materialInstanceEditorPanel->Build(snapshot);
        if (thumbnailLibrary != nullptr)
        {
            thumbnailLibrary->RequestVisible(visible
                ? materialInstanceEditorPanel->VisibleThumbnailPaths()
                : std::vector<std::string>{});
        }
        bridge->RecordPanelBuild();
    }

    EditorUiShell& owner;
    UiEditorShellInit init;
    bool initialized = false;
    bool visible = false;
    Rml::Context* context = nullptr;
    RmlUiRenderInterface* renderInterface = nullptr;
    IEditorUiRuntimeBridge* bridge = nullptr;
    Rml::ElementDocument* editorDocument = nullptr;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
    std::unique_ptr<Editor::MaterialInstanceEditorRuntime> materialInstanceEditorRuntime;
    std::unique_ptr<EditorUi::MaterialInstanceAssetEditorPanel> materialInstanceEditorPanel;
    std::unique_ptr<Editor::EditorJsonAssetLibrary> jsonAssetLibrary;
    std::unique_ptr<Editor::EditorThumbnailLibrary> thumbnailLibrary;
    std::optional<EditorUi::MaterialInstanceModelSnapshot> selectedModelMaterials;
    std::optional<uint32_t> selectedMaterialSlotIndex;
    std::shared_ptr<const std::vector<EditorUi::MaterialInstanceModelSnapshot>> sceneModels;
    std::shared_ptr<const std::vector<EditorUi::EditorSceneActorSnapshot>> sceneActors;
    UiEditorSectionStore sectionStore;
    UiAssetsSection appliedAssets;
    UiSectionVersions appliedVersions;
    uint64_t outlinerRevision = 0;
    bool panelBuildRequested = true;
    std::string selectedActorIdentity;
    bool pendingSceneSelectionClear = false;
    bool sceneViewportInputActive = false;
    bool editorPointerCaptured = false;
    // 视图模型里的脚本状态是 Play/Pause 交互隔离的唯一权威输入。
    bool scriptsEditing = true;
    bool offlineMode = false;
    uint64_t blueprintClassesVersion = 0;
    std::optional<MaterialInstanceModelContext> pendingMaterialInstanceModelSelection;
    std::optional<MaterialInstanceSelection> pendingMaterialInstanceSelection;
};

EditorUiShell::EditorUiShell() : impl(std::make_unique<Impl>(*this)) {}
EditorUiShell::~EditorUiShell()
{
    if (impl != nullptr && impl->bridge != nullptr)
        impl->bridge->ClearEditorUiShell(this);
    Shutdown();
}

RuntimeResult<void> EditorUiShell::Initialize(const UiEditorShellInit& init)
{
    Shutdown();
    if (init.context == nullptr || init.renderInterface == nullptr ||
        init.bridge == nullptr)
    {
        return RuntimeResult<void>::Failure(MakeRuntimeError(
            "EditorUiShell.InvalidInit",
            "Editor UI shell requires the runtime context, render interface and bridge."));
    }

    impl->init = init;
    impl->context = init.context;
    impl->renderInterface = init.renderInterface;
    impl->bridge = init.bridge;
    impl->visible = init.enabled && init.visible;
    impl->viewportWidth = 0;
    impl->viewportHeight = 0;
    impl->initialized = true;

    if (!init.enabled)
        return RuntimeResult<void>::Success();

    auto documentResult = impl->LoadEditorDocumentCandidate();
    if (documentResult.IsFailure())
    {
        Shutdown();
        return RuntimeResult<void>::Failure(documentResult.Error());
    }
    impl->editorDocument = documentResult.Value();

    RuntimeResult<void> materialEditorResult =
        impl->InitializeMaterialInstanceEditor();
    if (materialEditorResult.IsFailure())
    {
        // 与拆分前一致：材质编辑器初始化失败只降级工作区，不阻断启动。
        if (init.diagnostics != nullptr)
        {
            init.diagnostics->ReportWarning(
                FormatRuntimeError(materialEditorResult.Error()));
        }
    }

    SetVisible(impl->visible);
    return RuntimeResult<void>::Success();
}

void EditorUiShell::Shutdown()
{
    if (impl == nullptr)
        return;
    if (impl->materialInstanceEditorPanel != nullptr)
        impl->materialInstanceEditorPanel->Detach();
    impl->materialInstanceEditorPanel.reset();
    impl->jsonAssetLibrary.reset();
    impl->thumbnailLibrary.reset();
    impl->materialInstanceEditorRuntime.reset();

    if (impl->context != nullptr && impl->editorDocument != nullptr)
    {
        impl->context->UnloadDocument(impl->editorDocument);
        impl->editorDocument = nullptr;
    }

    impl->initialized = false;
    impl->visible = false;
    impl->context = nullptr;
    impl->renderInterface = nullptr;
    impl->bridge = nullptr;
    impl->selectedModelMaterials.reset();
    impl->selectedMaterialSlotIndex.reset();
    impl->sceneModels.reset();
    impl->sceneActors.reset();
    impl->sectionStore = UiEditorSectionStore{};
    impl->appliedAssets = UiAssetsSection{};
    impl->appliedVersions = UiSectionVersions{};
    impl->outlinerRevision = 0;
    impl->panelBuildRequested = true;
    impl->selectedActorIdentity.clear();
    impl->pendingSceneSelectionClear = false;
    impl->sceneViewportInputActive = false;
    impl->editorPointerCaptured = false;
    impl->blueprintClassesVersion = 0;
    impl->pendingMaterialInstanceModelSelection.reset();
    impl->pendingMaterialInstanceSelection.reset();
}

RuntimeResult<void> EditorUiShell::ReloadDocument()
{
    if (!impl->initialized || !impl->init.enabled || impl->context == nullptr)
        return RuntimeResult<void>::Success();

    auto candidate = impl->LoadEditorDocumentCandidate();
    if (candidate.IsFailure())
        return RuntimeResult<void>::Failure(candidate.Error());

    Rml::ElementDocument* previous = impl->editorDocument;
    Rml::ElementDocument* next = candidate.Value();
    impl->editorPointerCaptured = false;
    impl->sceneViewportInputActive = false;
    if (impl->materialInstanceEditorPanel != nullptr)
    {
        impl->materialInstanceEditorPanel->CancelWorkspaceDrag();
        std::string error;
        if (!impl->materialInstanceEditorPanel->Attach(*next, error))
        {
            impl->context->UnloadDocument(next);
            // Attach() detaches the old document even on failure. Restore the
            // last valid workspace instead of publishing a live document with
            // no panel attached.
            if (previous != nullptr)
            {
                std::string restoreError;
                impl->materialInstanceEditorPanel->Attach(*previous, restoreError);
            }
            return RuntimeResult<void>::Failure(MakeRuntimeError(
                "EditorUiShell.DocumentAttachFailed",
                error.empty() ? "Editor workspace could not be attached." : error));
        }
    }
    if (previous != nullptr)
        impl->context->UnloadDocument(previous);
    impl->editorDocument = next;
    SetVisible(impl->visible);
    return RuntimeResult<void>::Success();
}

void EditorUiShell::SetVisible(bool visible)
{
    impl->visible = visible;
    if (impl->editorDocument == nullptr)
        return;
    if (visible)
    {
        impl->editorDocument->Show(Rml::ModalFlag::None, Rml::FocusFlag::None);
        // Hide 会将焦点交还运行时文档并将其置顶；无焦点 Show 不恢复层级。
        impl->editorDocument->PullToFront();
    }
    else
    {
        impl->editorDocument->Hide();
    }
}

bool EditorUiShell::IsVisible() const { return impl->visible; }

void EditorUiShell::Resize(uint32_t width, uint32_t height)
{
    impl->viewportWidth = width;
    impl->viewportHeight = height;
}

void EditorUiShell::Tick(
    float,
    const UiViewModelSnapshot& view,
    uint64_t scriptsSectionVersion)
{
    if (!impl->initialized || impl->materialInstanceEditorRuntime == nullptr)
        return;
    if (impl->blueprintClassesVersion != scriptsSectionVersion)
    {
        impl->blueprintClassesVersion = scriptsSectionVersion;
        impl->materialInstanceEditorRuntime->SetBlueprintClasses(
            view.scripts.classes);
        impl->materialInstanceEditorRuntime->SetBlueprintComponentClasses(
            view.scripts.componentClasses);
    }
    if (impl->materialInstanceEditorRuntime->Tick())
    {
        impl->bridge->RequestViewModelRefresh(
            UiSectionBit(UiSectionId::Scripts) |
            UiSectionBit(UiSectionId::Assets) |
            UiSectionBit(UiSectionId::SceneOutliner));
        impl->panelBuildRequested = true;
        impl->bridge->RequestPanelBuild();
    }
    if (impl->jsonAssetLibrary != nullptr)
        impl->jsonAssetLibrary->Poll();
}

void EditorUiShell::UpdateSections(
    const UiViewModelSnapshot& view,
    const UiPerformanceSection& performance,
    bool snapshotDue,
    UiSectionMask runtimeChanged)
{
    if (!impl->initialized)
        return;

    // Play/Pause 期间场景交互（选择、gizmo、观察相机、右键菜单、参考网格）
    // 全部归蓝图运行时；下降沿同时把键盘焦点从编辑器控件交还游戏视口。
    const bool wasEditing = impl->scriptsEditing;
    const bool scriptsEditing =
        !view.scripts.available || view.scripts.state == ScriptPlayState::Editing;
    impl->scriptsEditing = scriptsEditing;
    impl->offlineMode = view.offlineMode;
    if (wasEditing && !scriptsEditing)
    {
        impl->sceneViewportInputActive = true;
        if (impl->context != nullptr)
        {
            Rml::Element* focus = impl->context->GetFocusElement();
            if (focus != nullptr && IsEditorDocumentElement(focus))
                focus->Blur();
        }
    }

    if (snapshotDue)
    {
        impl->PublishAssetsSection();
        impl->PublishSceneOutlinerSection();
    }

    const UiSectionVersions versions = impl->sectionStore.Versions();
    const UiSectionMask editorChanged =
        versions.ChangedFrom(impl->appliedVersions);
    impl->appliedVersions = versions;
    if ((editorChanged & UiSectionBit(UiSectionId::Assets)) != 0)
        impl->appliedAssets = *impl->sectionStore.Assets().data;
    const UiSectionMask changed = runtimeChanged | editorChanged;

    const bool panelDue =
        (changed & UiDeveloperPanelSectionMask) != 0 ||
        (impl->panelBuildRequested && snapshotDue);
    const bool runtimeViewDue =
        (changed & UiRuntimeViewSectionMask) != 0 || panelDue;
    if (panelDue)
    {
        impl->panelBuildRequested = false;
        impl->BuildDeveloperPanels(view, performance);
    }
    if (runtimeViewDue && impl->materialInstanceEditorPanel != nullptr)
    {
        impl->materialInstanceEditorPanel->UpdateRuntimeView(view);
        impl->bridge->RecordRuntimeViewUpdate();
    }
    if (snapshotDue && impl->materialInstanceEditorPanel != nullptr)
    {
        impl->materialInstanceEditorPanel->UpdatePerformance(
            performance.performance);
        impl->materialInstanceEditorPanel->UpdateScriptPerformance(
            performance.scriptPerformance);
    }
}

void EditorUiShell::PublishEditorTextures()
{
    if (impl->renderInterface == nullptr)
        return;
    const auto& assets = impl->sectionStore.Assets();
    if (assets.data != nullptr)
    {
        impl->renderInterface->PublishThumbnails(assets.data->thumbnails);
        impl->renderInterface->PublishTexturePreview(assets.data->jsonDocument);
    }
}

UiSectionVersions EditorUiShell::Versions() const
{
    return impl->sectionStore.Versions();
}

bool EditorUiShell::HandlePlatformEvent(
    const PlatformEvent& event,
    bool& consumed)
{
    consumed = false;
    if (!impl->initialized || impl->editorDocument == nullptr ||
        impl->context == nullptr)
    {
        return false;
    }

    auto& panel = impl->materialInstanceEditorPanel;
    const bool modal = panel != nullptr && panel->HasModal();
    const bool resourceEditor = panel != nullptr &&
        panel->IsAssetEditorVisible();
    if (resourceEditor)
        impl->sceneViewportInputActive = false;
    const bool sceneInteractionEnabled =
        impl->scriptsEditing && !impl->offlineMode;
    const Rml::Element* focus = impl->context->GetFocusElement();
    const bool editorFocus = IsEditorDocumentElement(focus);
    // Play/Pause 下编辑器焦点不能再吞键；只有真正的文本输入仍需要键盘。
    // RmlUi 的 button/checkbox 也是 ElementFormControl，因此必须看 input type。
    bool textInputFocus = false;
    if (editorFocus)
    {
        const auto* input =
            dynamic_cast<const Rml::ElementFormControlInput*>(focus);
        if (input != nullptr)
        {
            const Rml::String type = input->GetAttribute<Rml::String>("type", "text");
            textInputFocus = type == "text" || type == "search" ||
                type == "password" || type == "number";
        }
        else
        {
            textInputFocus =
                dynamic_cast<const Rml::ElementFormControlSelect*>(focus) != nullptr;
        }
    }
    const bool editorOwnsKeys = modal ||
        (editorFocus && (sceneInteractionEnabled || textInputFocus));
    switch (event.type)
    {
    case PlatformEventType::MouseMove:
    case PlatformEventType::MouseButtonDown:
    case PlatformEventType::MouseButtonUp:
    case PlatformEventType::MouseWheel:
    {
        if (event.type != PlatformEventType::MouseMove)
        {
            impl->panelBuildRequested = true;
            impl->bridge->RequestPanelBuild();
        }
        impl->bridge->TrackPointer(event.mouseX, event.mouseY);
        const auto* hit = impl->context->GetElementAtPoint(
            Rml::Vector2f(event.mouseX, event.mouseY));
        const bool sceneHit = panel != nullptr &&
            panel->IsSceneViewportElement(hit);
        const bool editorHit = IsEditorDocumentElement(hit) && !sceneHit;
        if (ShouldReleaseScenePointerForEditor(event.type, editorHit))
            impl->sceneViewportInputActive = false;
        const bool captured = impl->editorPointerCaptured ||
            (panel != nullptr && panel->IsWorkspaceDragging());
        if ((sceneHit ||
             (impl->sceneViewportInputActive && !resourceEditor)) &&
            !captured && !modal)
        {
            // 右键在运行时输入里没有任何绑定：视口命中的右键交给编辑器文档，
            // 右键上下文菜单才能拿到 RmlUi 事件与坐标。
            if (event.mouseButton == PlatformMouseButton::Right &&
                sceneInteractionEnabled)
            {
                impl->bridge->ProcessRmlEvent(event);
                consumed = true;
                return true;
            }
            // 图像属于 editor DOM，但内部场景输入归游戏；点击不把焦点交给根节点。
            if (event.type == PlatformEventType::MouseButtonDown)
            {
                impl->sceneViewportInputActive = true;
                if (editorFocus)
                    impl->context->GetFocusElement()->Blur();
            }
            if (event.type == PlatformEventType::MouseMove)
                impl->bridge->ProcessRmlEvent(event);
            return false;
        }
        if (event.type == PlatformEventType::MouseButtonDown)
        {
            impl->sceneViewportInputActive = false;
            impl->editorPointerCaptured = captured || editorHit || modal;
            if (!editorHit && !modal && editorFocus)
                impl->context->GetFocusElement()->Blur();
        }
        consumed = impl->bridge->ProcessRmlEvent(event);
        if (event.type == PlatformEventType::MouseButtonUp &&
            !impl->bridge->HasPressedMouseButtons())
        {
            impl->editorPointerCaptured = false;
        }
        consumed = consumed || editorHit || captured || modal;
        return consumed;
    }
    case PlatformEventType::MouseLeave:
        if (panel != nullptr)
            panel->CancelModelPreviewDrag();
        consumed = impl->bridge->ProcessRmlEvent(event) ||
            impl->editorPointerCaptured;
        return consumed;
    case PlatformEventType::KeyUp:
        impl->panelBuildRequested = true;
        impl->bridge->RequestPanelBuild();
        if (impl->bridge->IsKeyboardKeyPressed(event.key))
            consumed = impl->bridge->ProcessRmlEvent(event) || editorOwnsKeys;
        return consumed;
    case PlatformEventType::KeyDown:
    case PlatformEventType::TextInput:
    case PlatformEventType::TextEditing:
    case PlatformEventType::GamepadButtonDown:
    case PlatformEventType::GamepadButtonUp:
        impl->panelBuildRequested = true;
        impl->bridge->RequestPanelBuild();
        if (editorOwnsKeys)
        {
            impl->bridge->ProcessRmlEvent(event);
            consumed = true;
        }
        return consumed;
    case PlatformEventType::GamepadAxisMotion:
        // 摇杆轴是连续输入：只转发给 RmlUi 做导航，不请求面板重建。
        if (editorOwnsKeys)
        {
            impl->bridge->ProcessRmlEvent(event);
            consumed = true;
        }
        return consumed;
    default:
        // 焦点与模态只独占输入；Quit 等生命周期事件必须继续交给 EngineLoop。
        return false;
    }
}

bool EditorUiShell::HandleGlobalShortcut(
    const PlatformEvent& event,
    bool& consumed)
{
    consumed = false;
    if (event.type == PlatformEventType::KeyDown && !event.repeat &&
        event.control && event.key == PlatformKey::S && impl->visible &&
        impl->materialInstanceEditorPanel != nullptr &&
        !impl->materialInstanceEditorPanel->HasModal())
    {
        impl->materialInstanceEditorPanel->RequestSave();
        consumed = true;
        return true;
    }
    return false;
}

bool EditorUiShell::ApplyAction(
    const UiAction&,
    World*,
    RuntimeCommandExecutionResult&,
    bool& consumed)
{
    // 当前编辑器动作仍由 RuntimeUi 的稳定点转发（ToggleDeveloperUi 需要
    // 运行时 immersive 状态）；该入口先固定 seam，后续 P3c 再迁入动作。
    consumed = false;
    return false;
}

bool EditorUiShell::PrepareImmersiveView(std::string& levelPath)
{
    return impl->materialInstanceEditorPanel != nullptr &&
        impl->materialInstanceEditorPanel->PrepareImmersiveView(levelPath);
}

void EditorUiShell::NotifyWorldChanged(
    const std::string& worldPath,
    uint64_t generation)
{
    impl->selectedModelMaterials.reset();
    impl->selectedMaterialSlotIndex.reset();
    impl->sceneModels.reset();
    impl->sceneActors.reset();
    impl->selectedActorIdentity.clear();
    ++impl->outlinerRevision;
    impl->pendingSceneSelectionClear = false;
    impl->pendingMaterialInstanceModelSelection.reset();
    impl->pendingMaterialInstanceSelection.reset();
    if (impl->materialInstanceEditorRuntime != nullptr)
    {
        impl->materialInstanceEditorRuntime->NotifyWorldChanged(
            worldPath, generation);
    }
}

void EditorUiShell::SetMaterialInstanceSceneModels(
    UiEditorSnapshotPayload payload)
{
    auto models = std::static_pointer_cast<
        const std::vector<EditorUi::MaterialInstanceModelSnapshot>>(
        std::move(payload));
    if (impl->sceneModels == models)
        return;
    impl->sceneModels = std::move(models);
    ++impl->outlinerRevision;
    if (!impl->selectedModelMaterials.has_value() ||
        impl->sceneModels == nullptr)
    {
        return;
    }

    const auto selected = std::find_if(
        impl->sceneModels->begin(),
        impl->sceneModels->end(),
        [this](const EditorUi::MaterialInstanceModelSnapshot& model)
        {
            return impl->selectedModelMaterials->objectId != 0 &&
                model.objectId == impl->selectedModelMaterials->objectId &&
                model.worldGeneration ==
                    impl->selectedModelMaterials->worldGeneration;
        });
    if (selected == impl->sceneModels->end())
    {
        impl->selectedModelMaterials.reset();
        impl->selectedMaterialSlotIndex.reset();
        return;
    }
    impl->selectedModelMaterials = *selected;
}

void EditorUiShell::SetSceneActors(UiEditorSnapshotPayload payload)
{
    auto actors = std::static_pointer_cast<
        const std::vector<EditorUi::EditorSceneActorSnapshot>>(
        std::move(payload));
    if (impl->sceneActors == actors)
        return;
    impl->sceneActors = std::move(actors);
    ++impl->outlinerRevision;
    if (impl->selectedActorIdentity.empty() || impl->sceneActors == nullptr)
        return;

    const bool stillPresent = std::any_of(
        impl->sceneActors->begin(),
        impl->sceneActors->end(),
        [this](const EditorUi::EditorSceneActorSnapshot& actor)
        {
            return actor.identity == impl->selectedActorIdentity;
        });
    if (stillPresent)
        return;
    impl->selectedActorIdentity.clear();
    impl->selectedModelMaterials.reset();
    impl->selectedMaterialSlotIndex.reset();
}

std::optional<MaterialInstanceModelContext>
EditorUiShell::ConsumeMaterialInstanceModelSelectionRequest()
{
    std::optional<MaterialInstanceModelContext> result =
        std::move(impl->pendingMaterialInstanceModelSelection);
    impl->pendingMaterialInstanceModelSelection.reset();
    return result;
}

std::optional<MaterialInstanceSelection>
EditorUiShell::ConsumeMaterialInstanceSelectionRequest()
{
    std::optional<MaterialInstanceSelection> result =
        std::move(impl->pendingMaterialInstanceSelection);
    impl->pendingMaterialInstanceSelection.reset();
    return result;
}

bool EditorUiShell::ConsumeSceneSelectionClearRequest()
{
    const bool pending = impl->pendingSceneSelectionClear;
    impl->pendingSceneSelectionClear = false;
    return pending;
}

bool EditorUiShell::RequestModelSelection(
    const MaterialInstanceModelContext& model)
{
    if (!impl->init.enabled || model.worldGeneration == 0 ||
        model.objectId == 0 || model.objectIdentity.empty())
    {
        return false;
    }

    impl->bridge->RequestViewModelRefresh(
        UiSectionBit(UiSectionId::SceneOutliner));
    EditorUi::MaterialInstanceModelSnapshot modelSnapshot;
    modelSnapshot.worldGeneration = model.worldGeneration;
    modelSnapshot.scenePath = model.scenePath;
    modelSnapshot.objectId = model.objectId;
    modelSnapshot.displayName = model.displayName;
    modelSnapshot.objectIdentity = model.objectIdentity;
    modelSnapshot.actorIdentity = model.actorIdentity;
    modelSnapshot.materials.reserve(model.materials.size());
    for (const MaterialInstanceModelMaterial& material : model.materials)
    {
        EditorUi::MaterialInstanceModelMaterialSnapshot materialSnapshot;
        materialSnapshot.objectId = material.objectId;
        materialSnapshot.materialSlotIndex = material.materialSlotIndex;
        materialSnapshot.materialSlotName = material.materialSlotName;
        materialSnapshot.materialInstancePath = material.materialInstancePath;
        modelSnapshot.materials.push_back(std::move(materialSnapshot));
    }
    impl->selectedModelMaterials = std::move(modelSnapshot);
    impl->selectedMaterialSlotIndex.reset();
    impl->pendingMaterialInstanceSelection.reset();
    impl->pendingMaterialInstanceModelSelection = model;
    impl->selectedActorIdentity = EditorUi::SceneActorIdentity(
        EditorUi::EditorSceneActorType::Mesh, model.objectIdentity);
    impl->pendingSceneSelectionClear = false;
    ++impl->outlinerRevision;
    return true;
}

bool EditorUiShell::RequestMaterialSelection(
    const MaterialInstanceSelection& selection)
{
    if (!impl->init.enabled || selection.worldGeneration == 0 ||
        selection.objectId == 0 || selection.objectIdentity.empty() ||
        selection.materialInstancePath.empty())
    {
        return false;
    }

    impl->bridge->RequestViewModelRefresh(
        UiSectionBit(UiSectionId::SceneOutliner));
    impl->selectedMaterialSlotIndex = selection.materialSlotIndex;
    impl->pendingMaterialInstanceModelSelection.reset();
    impl->pendingMaterialInstanceSelection = selection;
    impl->selectedActorIdentity = EditorUi::SceneActorIdentity(
        EditorUi::EditorSceneActorType::Mesh, selection.objectIdentity);
    impl->pendingSceneSelectionClear = false;
    ++impl->outlinerRevision;
    return true;
}

bool EditorUiShell::RequestActorSelection(const std::string& actorIdentity)
{
    if (!impl->init.enabled || actorIdentity.empty())
        return false;

    impl->bridge->RequestViewModelRefresh(
        UiSectionBit(UiSectionId::SceneOutliner));
    // 非网格 actor 没有 MI 文档：清掉材质槽选择，并要求渲染端清掉 mesh 描边。
    impl->selectedModelMaterials.reset();
    impl->selectedMaterialSlotIndex.reset();
    impl->pendingMaterialInstanceModelSelection.reset();
    impl->pendingMaterialInstanceSelection.reset();
    impl->selectedActorIdentity = actorIdentity;
    impl->pendingSceneSelectionClear = true;
    ++impl->outlinerRevision;
    return true;
}

bool EditorUiShell::SelectSceneObjectFromPick(
    const MaterialInstanceSelection& selection)
{
    if (!impl->init.enabled || impl->materialInstanceEditorRuntime == nullptr)
        return false;

    impl->bridge->RequestViewModelRefresh(
        UiSectionBit(UiSectionId::SceneOutliner));
    if (impl->materialInstanceEditorPanel != nullptr)
        impl->materialInstanceEditorPanel->ShowSceneDetails();
    impl->pendingMaterialInstanceSelection.reset();
    impl->pendingMaterialInstanceModelSelection.reset();

    EditorUi::MaterialInstanceModelSnapshot modelSnapshot;
    modelSnapshot.worldGeneration = selection.modelContext.worldGeneration;
    modelSnapshot.scenePath = selection.modelContext.scenePath;
    modelSnapshot.objectId = selection.modelContext.objectId;
    modelSnapshot.displayName = selection.modelContext.displayName;
    modelSnapshot.objectIdentity = selection.modelContext.objectIdentity;
    modelSnapshot.actorIdentity = selection.modelContext.actorIdentity;
    modelSnapshot.materials.reserve(selection.modelContext.materials.size());
    for (const MaterialInstanceModelMaterial& material :
         selection.modelContext.materials)
    {
        EditorUi::MaterialInstanceModelMaterialSnapshot materialSnapshot;
        materialSnapshot.objectId = material.objectId;
        materialSnapshot.materialSlotIndex = material.materialSlotIndex;
        materialSnapshot.materialSlotName = material.materialSlotName;
        materialSnapshot.materialInstancePath = material.materialInstancePath;
        modelSnapshot.materials.push_back(std::move(materialSnapshot));
    }
    impl->selectedModelMaterials = std::move(modelSnapshot);
    impl->selectedMaterialSlotIndex = selection.materialSlotIndex;
    impl->selectedActorIdentity = EditorUi::SceneActorIdentity(
        EditorUi::EditorSceneActorType::Mesh, selection.objectIdentity);
    if (selection.objectIdentity.rfind("__blueprint/", 0) == 0)
    {
        const auto id = selection.objectIdentity.substr(
            std::string_view("__blueprint/").size());
        impl->selectedActorIdentity =
            "blueprintInstance|" + id.substr(0, id.find('/'));
    }
    impl->pendingSceneSelectionClear = false;
    ++impl->outlinerRevision;
    return true;
}

void EditorUiShell::ClearMaterialInstanceSceneSelection()
{
    impl->bridge->RequestViewModelRefresh(
        UiSectionBit(UiSectionId::SceneOutliner));
    impl->selectedModelMaterials.reset();
    impl->selectedMaterialSlotIndex.reset();
    impl->selectedActorIdentity.clear();
    ++impl->outlinerRevision;
    impl->pendingSceneSelectionClear = false;
    impl->pendingMaterialInstanceModelSelection.reset();
    impl->pendingMaterialInstanceSelection.reset();
}

bool EditorUiShell::CancelBlueprintAssetDrag()
{
    return impl->materialInstanceEditorPanel != nullptr &&
        impl->materialInstanceEditorPanel->CancelBlueprintAssetDrag();
}

void EditorUiShell::RequestOpenJsonAsset(const std::string& assetPath)
{
    impl->bridge->RequestViewModelRefresh(UiSectionBit(UiSectionId::Assets));
    if (impl->jsonAssetLibrary != nullptr)
        impl->jsonAssetLibrary->RequestOpen(assetPath);
}

void EditorUiShell::RequestRefreshAssetCatalog()
{
    impl->bridge->RequestViewModelRefresh(UiSectionBit(UiSectionId::Assets));
    if (impl->thumbnailLibrary != nullptr)
        impl->thumbnailLibrary->Refresh();
    if (impl->jsonAssetLibrary != nullptr)
        impl->jsonAssetLibrary->RequestRefresh();
}

bool EditorUiShell::IsScenePickTarget(float mouseX, float mouseY) const
{
    // Play/Pause 下视口点击属于蓝图运行时，不能再被编辑器拾取截走。
    if (!impl->scriptsEditing || impl->offlineMode ||
        !impl->visible || impl->editorDocument == nullptr ||
        impl->context == nullptr ||
        !std::isfinite(mouseX) || !std::isfinite(mouseY) || mouseX < 0 ||
        mouseY < 0 || mouseX >= impl->viewportWidth ||
        mouseY >= impl->viewportHeight ||
        impl->materialInstanceEditorPanel == nullptr ||
        impl->materialInstanceEditorPanel->HasModal() ||
        impl->materialInstanceEditorPanel->IsWorkspaceDragging() ||
        !ContainsViewportPoint(
            GetSceneViewportRect(), mouseX, mouseY))
    {
        return false;
    }
    const Rml::Element* hit = impl->context->GetElementAtPoint(
        Rml::Vector2f(mouseX, mouseY));
    return impl->materialInstanceEditorPanel->IsSceneViewportElement(hit);
}

UiViewportRect EditorUiShell::GetSceneViewportRect() const
{
    if (impl->visible && impl->materialInstanceEditorPanel != nullptr)
        return impl->materialInstanceEditorPanel->GetSceneViewportRect();
    return UiViewportRect{0, 0, impl->viewportWidth, impl->viewportHeight};
}

bool EditorUiShell::IsMotionDebugViewport() const
{
    return impl->materialInstanceEditorPanel != nullptr &&
        impl->materialInstanceEditorPanel->IsMotionDebugViewport();
}

bool EditorUiShell::IsAssetEditorVisible() const
{
    return impl->materialInstanceEditorPanel != nullptr &&
        impl->materialInstanceEditorPanel->IsAssetEditorVisible();
}

bool EditorUiShell::HasModal() const
{
    return impl->materialInstanceEditorPanel != nullptr &&
        impl->materialInstanceEditorPanel->HasModal();
}

bool EditorUiShell::IsWorkspaceDragging() const
{
    return impl->materialInstanceEditorPanel != nullptr &&
        impl->materialInstanceEditorPanel->IsWorkspaceDragging();
}

bool EditorUiShell::IsEditorDocumentElement(const Rml::Element* element) const
{
    return element != nullptr && impl->editorDocument != nullptr &&
        element->GetOwnerDocument() == impl->editorDocument;
}

bool EditorUiShell::IsSceneViewportElement(const Rml::Element* element) const
{
    return impl->materialInstanceEditorPanel != nullptr &&
        impl->materialInstanceEditorPanel->IsSceneViewportElement(element);
}

bool EditorUiShell::IsSceneViewportInputActive() const
{
    return impl->sceneViewportInputActive;
}

void EditorUiShell::SetSceneViewportInputActive(bool active)
{
    impl->sceneViewportInputActive = active;
}

bool EditorUiShell::IsPointerCaptured() const
{
    return impl->editorPointerCaptured;
}

void EditorUiShell::CancelTransientInteraction()
{
    if (impl->materialInstanceEditorPanel != nullptr)
        impl->materialInstanceEditorPanel->CancelWorkspaceDrag();
    impl->editorPointerCaptured = false;
    impl->sceneViewportInputActive = false;
}

Editor::SceneDocumentService* EditorUiShell::SceneDocuments() const
{
    return impl->materialInstanceEditorRuntime != nullptr
        ? impl->materialInstanceEditorRuntime->SceneDocuments()
        : nullptr;
}

Editor::MaterialInstanceEditorRuntime* EditorUiShell::EditorRuntime() const
{
    return impl->materialInstanceEditorRuntime.get();
}

Editor::EditorThumbnailLibrary* EditorUiShell::ThumbnailLibrary() const
{
    return impl->thumbnailLibrary.get();
}

bool EditorUiShell::CanRenderThumbnails() const
{
    return impl->visible && !impl->sceneViewportInputActive &&
        !impl->editorPointerCaptured;
}

} // namespace VL
