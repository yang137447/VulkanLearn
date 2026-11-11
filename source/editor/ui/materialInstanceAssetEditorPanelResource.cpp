#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "animation/motionDebug.h"
#include "debug/drawDebug.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <exception>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <tuple>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Elements/ElementFormControlInput.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/SystemInterface.h>

#include "core/renderSession.h"
#include "editor/brush/brushProfile.h"
#include "editor/ui/editorSceneActorTree.h"
#include "editor/ui/editorTextMatch.h"
#include "editor/ui/editorViewportAxisIndicator.h"
#include "editor/ui/editorActorVisualization.h"
#include "editor/ui/scriptComponentControls.h"
#include "editor/ui/tressfxGroupEditor.h"
#include "hair/tressfxGroupBinding.h"
#include "editor/ui/tressfxHairSimulationControls.h"
#include "hair/tressfxGroupBinding.h"
#include "hair/tressfxGroupPreviewState.h"
#include "editor/ui/scriptPhysicsControls.h"
#include "editor/ui/scriptPropertyControls.h"
#include "script/scriptTypeIdentity.h"
#include "ui/widgets/input/inputWidgets.h"
#include "ui/widgets/layout/layoutWidgets.h"
#include "ui/widgets/navigation/tabWidgets.h"
#include "ui/widgets/property/propertyWidgets.h"
#include "ui/widgets/text/textWidgets.h"
#include "ui/widgets/views/viewWidgets.h"
#include "ui/uiColorTransform.h"

namespace VL::EditorUi
{
namespace
{
using namespace Panel::Detail;
} // namespace

void MaterialInstanceAssetEditorPanel::OpenResourceAsset(
    const std::string& path, EditorAssetKind kind)
{
    FlushBlueprintAssetEdits();
    FlushParticleAssetEdits();
    if (!resourceEditor.Open(path, kind))
    {
        localStatus = "Too many resource editors are open.";
        return;
    }
    viewingJson = false;
    resourceEditorVisible = true;
    localStatus.clear();
    RequestActiveResourceMetadata();
    // 作者 owner 的生命周期属于资产标签，不能依赖 Simulation 段是否可见。
    if (kind == EditorAssetKind::Tressfx)
        RequestTressfxDocument(path);
    if (kind == EditorAssetKind::Blueprint)
    {
        EditorCommandEnvelope command;
        command.type = EditorCommandType::GetBlueprintDocument;
        command.payload = MaterialInstanceAssetPathPayload{path};
        Submit(std::move(command));
    }
    if (kind == EditorAssetKind::ParticleSystem)
    {
        EditorCommandEnvelope command;
        command.type = EditorCommandType::GetParticleDocument;
        command.payload = MaterialInstanceAssetPathPayload{path};
        Submit(std::move(command));
    }
    if (kind == EditorAssetKind::Mesh || kind == EditorAssetKind::Tressfx)
    {
        // 毛发资产：目录扫描已带顶层 type 与 simulation.skinning，打开时即可决定
        // 是否请求预览。带 skinning 的资产不进入视口（由蓝图 TressFX 组件承载），
        // 这里直接跳过请求；快照到达后还会以作者文档复核一次。
        const EditorJsonAssetEntry* entry = FindCatalogEntry(snapshot.assetCatalog, path);
        hairPreviewPath = kind == EditorAssetKind::Tressfx ? path : std::string();
        hairPreviewBlocked = entry != nullptr && entry->tressfxSkinning;
        hairRotation.Reset();
        if (hairPreviewBlocked)
            localStatus = std::string(TressfxHairSkinnedAssetMessage);
        else if (runtimeActionSink != nullptr)
        {
            // 回调只投递路径；GT 复用 previewmodel 的 LoadWorld 事务，不在 UI 内加载资产。
            // 即使标签已打开也重新请求，保证切过关卡后再次双击仍能回到该模型。
            RequestModelPreview(path);
        }
    }
    RefreshResourceEditor();
    docking.SetPanelVisible(ResourcePanelId(path), true);
    docking.Activate(ResourcePanelId(path));
}

void MaterialInstanceAssetEditorPanel::RequestCloseResource(const std::string& path)
{
    for (const auto& tab : resourceEditor.Tabs())
    {
        if (tab.path != path) continue;
        const auto* authored = ResourceDocument(path);
        const auto* particle = ParticleDocument(path);
        if ((authored && authored->dirty) || (particle && particle->dirty) ||
            pendingBlueprintPath == path || pendingParticlePath == path ||
            (particleCommandPath == path && !inFlightParticleChanges.empty()) ||
            (hairSimulationPath == path && hairSimulationPendingSave))
        {
            FlushBlueprintAssetEdits();
            FlushParticleAssetEdits();
            pendingClosePath = path;
            pendingCloseKind = tab.kind;
            saveThenClose = saveCloseSubmitted = false;
            discardBlueprintThenClose = discardBlueprintCloseSubmitted = false;
            discardParticleThenClose =
                discardParticleCloseSubmitted = false;
            return;
        }
        CloseResource(path);
        return;
    }
}

void MaterialInstanceAssetEditorPanel::CloseResource(const std::string& path)
{
    const std::string next = docking.PanelAfterClose(ResourcePanelId(path));
    resourceEditor.Close(path);
    requestedTressfxDocuments.erase(path);
    // 后台关闭只移除标签，不切预览 World，也不覆盖当前页面的选择。
    RefreshResourceEditor();
    if (!next.empty()) ActivateWorkspacePanel(next);
}

void MaterialInstanceAssetEditorPanel::ActivateWorkspacePanel(const std::string& panel)
{
    if (panel == "viewport")
    {
        docking.SetPanelVisible("viewport", true);
        ReturnToLevel();
        return;
    }
    const std::string path = docking.ResourcePathForPanel(panel);
    if (path.empty()) return;
    FlushPendingEdits();
    FlushBlueprintAssetEdits();
    viewingJson = false;
    docking.SetPanelVisible(panel, true);
    docking.Activate(panel);
    if (panel.rfind("resource:material:", 0) == 0)
    {
        resourceEditorVisible = false;
        EditorCommandEnvelope command;
        command.type = EditorCommandType::SelectMaterialInstanceDocument;
        command.payload = MaterialInstanceAssetPathPayload{path};
        Submit(std::move(command));
        return;
    }
    if (!resourceEditor.Activate(path)) return;
    resourceEditorVisible = true;
    RequestActiveResourceMetadata();
    const auto* active = resourceEditor.ActiveTab();
    if (active && (active->kind == EditorAssetKind::Mesh || active->kind == EditorAssetKind::Tressfx) &&
        !HasCurrentModelPreview())
        RequestModelPreview(path);
    RefreshResourceEditor();
}

void MaterialInstanceAssetEditorPanel::RequestActiveResourceMetadata()
{
    const auto* active = resourceEditor.ActiveTab();
    if (active != nullptr && active->kind == EditorAssetKind::Terrain)
    {
        EditorCommandEnvelope command;
        command.type = EditorCommandType::GetTerrainDocument;
        command.payload = MaterialInstanceAssetPathPayload{active->path};
        Submit(std::move(command));
    }
    // 读取器只保留最新请求；切回未完成的标签须补读，已缓存的标签不重复 IO。
    if (active != nullptr && (active->snapshot == nullptr || active->loading) &&
        jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(active->path);
}

void MaterialInstanceAssetEditorPanel::RequestResourceReload()
{
    const auto* tab = resourceEditor.ActiveTab();
    if (tab && tab->kind == EditorAssetKind::Retarget && tab->snapshot)
    {
        // GT 的 loading 快照可能晚一帧；请求发出就封住旧图像，直到新元数据 revision 到达。
        reloadingRetargetPath = tab->path;
        reloadingRetargetRevision = tab->snapshot->revision;
        previewDragButton = -1;
    }
    if (tab && tab->kind == EditorAssetKind::Blueprint)
    {
        if (!pendingBlueprintPath.empty()) { localStatus = "Save or revert pending edits before reloading."; return; }
        SubmitBlueprintAssetCommand(EditorCommandType::ReloadBlueprintDocument, tab->path);
        return;
    }
    if (tab && tab->kind == EditorAssetKind::Terrain)
    {
        const auto* terrain = TerrainDocument(tab->path);
        if (terrain == nullptr) return;
        EditorCommandEnvelope command;
        command.type = EditorCommandType::ReloadTerrainDocument;
        command.expectedDocumentRevision = terrain->documentRevision;
        command.payload = ReloadMaterialInstanceDocumentPayload{
            tab->path, EditorDirtyDocumentPolicy::RequireClean};
        Submit(std::move(command));
        return;
    }
    if (tab && tab->kind == EditorAssetKind::Tressfx)
    {
        const auto* authored = TressfxDocument(tab->path);
        if (!authored || authored->dirty || hairSimulationPendingSave)
        {
            localStatus = "Save or revert pending edits before reloading.";
            return;
        }
        SubmitResourceDocumentCommand(EditorCommandType::ReloadTressfxDocument, tab->path);
        if (jsonAssetRequestSink != nullptr)
            jsonAssetRequestSink->RequestOpenJsonAsset(tab->path);
        return;
    }
    resourceEditor.RequestReload();
    const std::string path = resourceEditor.ConsumeReloadRequest();
    if (path.empty()) return;
    if (jsonAssetRequestSink != nullptr)
        jsonAssetRequestSink->RequestOpenJsonAsset(path);
    const auto* active = resourceEditor.ActiveTab();
    if (active != nullptr && (active->kind == EditorAssetKind::Mesh || active->kind == EditorAssetKind::Tressfx))
        RequestModelPreview(path);
}

void MaterialInstanceAssetEditorPanel::RequestModelPreview(const std::string& path)
{
    // 打开、切标签与 Reload 共用入口；蒙皮资产只能由角色组件提供预览姿态。
    const auto* active = resourceEditor.ActiveTab();
    if (active && active->path == path && active->kind == EditorAssetKind::Tressfx)
    {
        const auto* entry = FindCatalogEntry(snapshot.assetCatalog, path);
        const bool blocked = active->snapshot ? active->snapshot->tressfxSkinning :
            entry != nullptr && entry->tressfxSkinning;
        if (blocked) return;
    }
    RequestAssetPreview(path, false);
}

void MaterialInstanceAssetEditorPanel::RequestAssetPreview(const std::string& path, bool material,
    const std::string& retargetPath, uint64_t retargetRevision)
{
    previewDragButton = -1;
    if (runtimeActionSink == nullptr) return;
    if (!runtimeView.activeWorldPath.empty() &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() != "SC_model_preview.json" &&
        std::filesystem::path(runtimeView.activeWorldPath).filename() != "SC_particle_preview.json")
        levelWorldPath = runtimeView.activeWorldPath;
    // 新的预览目标从零姿态开始；旋转只属于当前预览世界，不跨资产累积。
    if (requestedPreviewPath != path) hairRotation.Reset();
    requestedPreviewPath = path;
    requestedRetargetPath = retargetPath;
    requestedRetargetRevision = retargetRevision;
    initializedRetargetGeneration = 0;
    requestedPreviewMaterial = material;
    requestedPreviewBlueprint = false;
    requestedPreviewGeneration = 0;
    for (const auto& actor : OutlinerActors())
        requestedPreviewGeneration = std::max(requestedPreviewGeneration, actor.worldGeneration);
    UiAction request;
    request.type = material ? UiActionType::PreviewMaterial : retargetPath.empty() ?
        UiActionType::PreviewModel : UiActionType::PreviewRetarget;
    request.stringValue = retargetPath.empty() ? path : retargetPath;
    if (!retargetPath.empty()) request.retargetPreviewModel = path;
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

bool MaterialInstanceAssetEditorPanel::IsResourceEditorVisible() const
{
    for (const auto& tab : resourceEditor.Tabs())
        if (docking.IsPanelActive(ResourcePanelId(tab.path))) return true;
    return false;
}

bool MaterialInstanceAssetEditorPanel::PrepareImmersiveView(std::string& levelToLoad)
{
    levelToLoad.clear();
    if (HasModal()) return false;
    const bool previewWorld = !requestedPreviewPath.empty() ||
        std::filesystem::path(runtimeView.activeWorldPath).filename() == "SC_model_preview.json" ||
        std::filesystem::path(runtimeView.activeWorldPath).filename() == "SC_particle_preview.json";
    if (previewWorld)
    {
        if (levelWorldPath.empty())
        {
            localStatus = "Open a level before entering immersive mode.";
            return false;
        }
        levelToLoad = levelWorldPath;
        requestedPreviewPath.clear();
    }
    // 这里只选择目标页面；宿主等关卡事务完成后隐藏 UI，失败时仍保留可操作的工作区。
    resourceEditorVisible = false;
    docking.SetPanelVisible("viewport", true);
    docking.Activate("viewport");
    return true;
}

void MaterialInstanceAssetEditorPanel::ReturnToLevel()
{
    previewDragButton = -1;
    resourceEditorVisible = false;
    docking.Activate("viewport");
    // 当前只有一个活动 World；返回关卡须重新走加载事务，不能把模型 World 冒充关卡。
    if (!requestedPreviewPath.empty() && !levelWorldPath.empty() && runtimeActionSink != nullptr)
    {
        UiAction request;
        request.type = UiActionType::LoadWorld;
        request.stringValue = levelWorldPath;
        runtimeActionSink->RequestRuntimeAction(std::move(request));
        requestedPreviewPath.clear();
    }
}

bool MaterialInstanceAssetEditorPanel::HasCurrentModelPreview() const
{
    return CurrentModelPreviewGeneration() != 0;
}

bool MaterialInstanceAssetEditorPanel::IsMotionDebugViewport() const
{
    return docking.IsPanelActive("viewport") ||
        (IsResourceEditorVisible() && HasCurrentModelPreview());
}

bool MaterialInstanceAssetEditorPanel::IsSceneViewportElement(const Rml::Element* element) const
{
    // 候选表可滚动，命中属于编辑器，不能同时触发角色输入或关卡拾取。
    for (auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current->GetId() == "viewport-motion-matching") return false;
    return docking.IsSceneViewportElement(element);
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentModelPreviewGeneration() const
{
    if (requestedPreviewMaterial) return 0;
    const auto* active = resourceEditor.ActiveTab();
    if (!active) return 0;
    if (active->kind == EditorAssetKind::Retarget)
    {
        if (!retargetEditor.AnimationVisible() || active->loading || !active->snapshot ||
            !active->snapshot->retargetPreview || active->path != requestedRetargetPath ||
            active->snapshot->revision != requestedRetargetRevision) return 0;
    }
    else if ((active->kind != EditorAssetKind::Mesh && active->kind != EditorAssetKind::Tressfx) ||
        active->path != requestedPreviewPath) return 0;
    // JSON 读取完成不能证明 GPU 模型已加载；只在新预览 World 的冻结快照发布后展示。
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration &&
            actor.sceneObjectIdentity == "preview_model" &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
        {
            if (active->kind == EditorAssetKind::Retarget)
            {
                bool paired = false;
                for (const auto& source : OutlinerActors())
                    paired |= source.worldGeneration == actor.worldGeneration &&
                        source.sceneObjectIdentity == "preview_retarget_source|" + requestedRetargetPath;
                if (!paired) continue;
            }
            return actor.worldGeneration;
        }
    return 0;
}

bool MaterialInstanceAssetEditorPanel::IsMaterialEditorVisible() const
{
    return snapshot.activeDocument &&
        docking.IsPanelActive(MaterialPanelId(snapshot.activeDocument->assetPath));
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentMaterialPreviewGeneration() const
{
    if (!requestedPreviewMaterial || !snapshot.activeDocument ||
        snapshot.activeDocument->assetPath != requestedPreviewPath) return 0;
    // 只认已发布的新 World 和同路径材质槽，切标签时不能把上一颗球的画面当成新材质。
    for (const auto& actor : OutlinerActors())
        if (actor.worldGeneration > requestedPreviewGeneration &&
            actor.sceneObjectIdentity == "preview_material" &&
            std::filesystem::path(actor.scenePath).filename() == "SC_model_preview.json")
            for (const auto& section : actor.sections)
                if (section.materialInstancePath == requestedPreviewPath) return actor.worldGeneration;
    return 0;
}

uint64_t MaterialInstanceAssetEditorPanel::CurrentPreviewCameraGeneration() const
{
    return IsMaterialEditorVisible() ? CurrentMaterialPreviewGeneration() :
        IsResourceEditorVisible() ? (requestedPreviewBlueprint ? CurrentBlueprintPreviewGeneration() :
            CurrentModelPreviewGeneration()) : 0;
}

void MaterialInstanceAssetEditorPanel::RefreshMaterialPreview()
{
    if (IsMaterialEditorVisible() && (!requestedPreviewMaterial ||
        requestedPreviewPath != snapshot.activeDocument->assetPath))
        RequestAssetPreview(snapshot.activeDocument->assetPath, true);
    // 最后一份材质关闭后，恢复记住的关卡；不能留下预览 World 冒充关卡。
    if (requestedPreviewMaterial && snapshot.documentTabs.empty() && docking.IsPanelActive("viewport"))
        ReturnToLevel();
    const uint64_t generation = CurrentMaterialPreviewGeneration();
    widgetDocument.SetProperty("material-preview-image", "display", generation ? "block" : "none");
    widgetDocument.SetProperty("material-preview-status", "display", generation ? "none" : "block");
    SetDisabled("material-frame", !generation);
    SetMarkup("material-preview-status", Text(snapshot.activeDocument && !snapshot.activeDocument->previewMessage.empty()
        ? snapshot.activeDocument->previewMessage : "Loading material preview..."));
    // 新球从磁盘基线加载；重连成功后补一次未保存草稿，避免切回标签丢失预览效果。
    if (generation && IsMaterialEditorVisible() && appliedMaterialPreviewGeneration != generation &&
        snapshot.activeDocument->preview == EditorPreviewStatus::Connected)
    {
        appliedMaterialPreviewGeneration = generation;
        if (snapshot.activeDocument->status == EditorDocumentStatus::Dirty)
            SubmitPathCommand(EditorCommandType::ApplyMaterialInstancePreview);
    }
}

void MaterialInstanceAssetEditorPanel::RequestPreviewCamera(ModelPreviewNavigation mode, float x, float y)
{
    const uint64_t generation = CurrentPreviewCameraGeneration();
    if (!runtimeActionSink || !generation || HasModal()) return;
    UiAction action;
    action.type = UiActionType::NavigateModelPreview;
    action.previewCamera = {mode, generation, x, y};
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

bool MaterialInstanceAssetEditorPanel::ProcessModelPreviewInput(Rml::Event& event)
{
    const uint64_t generation = CurrentPreviewCameraGeneration();
    if (!generation || HasModal() || docking.IsDragging())
    {
        previewDragButton = -1;
        return false;
    }
    if (previewDragGeneration != generation) previewDragButton = -1;
    const bool imageHit = event.GetTargetElement() == document->GetElementById(
        IsMaterialEditorVisible() ? "material-preview-image" :
            requestedPreviewBlueprint ? "blueprint-asset-preview-image" :
                !requestedRetargetPath.empty() ? "retarget-preview-image" : "resource-preview-image");
    const auto& type = event.GetType();
    const float x = event.GetParameter<float>("mouse_x", 0);
    const float y = event.GetParameter<float>("mouse_y", 0);
    if (hairGroupPaintActive && IsActiveHairPreview() && HairBrushSection() != nullptr)
    {
        if (type == "mousedown" && imageHit &&
            event.GetParameter<int>("button", -1) == 0)
        {
            CloseMenus();
            CloseContextMenu();
            if (hairLassoActive)
                BeginHairLassoStroke(x, y,
                    event.GetParameter<int>("shift_key", 0) != 0,
                    event.GetParameter<int>("alt_key", 0) != 0);
            else
                BeginHairBrushStroke(x, y,
                    event.GetParameter<int>("shift_key", 0) != 0,
                    event.GetParameter<int>("alt_key", 0) != 0);
            if (hairBrushStrokeActive)
            {
                previewDragButton = 0;
                previewDragGeneration = generation;
                previewPointerX = x;
                previewPointerY = y;
            }
            return true;
        }
        if (type == "mousemove")
        {
            if (hairLassoDragging)
            {
                AppendHairLassoPoint(x, y);
                // 每 3 个采样更新一次 ghost：整屏套索不必按鼠标频率重跑 40k 选择。
                if (hairLassoScreenPoints.size() % 3 == 0)
                    UpdateHairLassoPreview();
                return true;
            }
            if (hairBrushStrokeActive)
            {
                UpdateHairBrushSample(x, y, true);
                return true;
            }
            // 右键/中键相机拖动仍走原路径；hover 采样只在没有其它鼠标手势时生效。
            if (previewDragButton < 0 && imageHit)
            {
                UpdateHairBrushSample(x, y, false);
                return true;
            }
        }
        if (type == "mouseup" && hairLassoDragging &&
            event.GetParameter<int>("button", -1) == 0)
        {
            CommitHairLassoStroke();
            previewDragButton = -1;
            return true;
        }
        if (type == "mouseup" && hairBrushStrokeActive &&
            event.GetParameter<int>("button", -1) == 0)
        {
            CommitHairBrushStroke();
            previewDragButton = -1;
            return true;
        }
        if (type == "mousescroll" && imageHit)
        {
            // 滚轮仍归相机缩放；只有 Alt+滚轮才调笔刷半径。未按 Alt 时不消费事件，
            // 继续走下面通用的 RequestPreviewCamera(Zoom)。
            if (event.GetParameter<int>("alt_key", 0) != 0)
            {
                const float delta = event.GetParameter<float>("wheel_delta_y", 0);
                hairBrushRadiusPx = std::clamp(
                    hairBrushRadiusPx + delta * 4.0f, 1.0f, 200.0f);
                SetControlValue("hair-brush-radius", SceneActorNumber(hairBrushRadiusPx));
                hairBrushStatus = "Brush radius: " +
                    SceneActorNumber(hairBrushRadiusPx) + " px.";
                UpdateHairBrushSample(x, y, hairBrushStrokeActive);
                return true;
            }
        }
    }
    if (type == "mousedown" && imageHit)
    {
        const int button = event.GetParameter<int>("button", -1);
        if (button < 0 || button > 2) return false;
        CloseMenus();
        CloseContextMenu();
        previewDragButton = button;
        previewDragGeneration = generation;
        previewPointerX = x;
        previewPointerY = y;
        return true;
    }
    if (type == "mousemove" && previewDragButton >= 0)
    {
        // 手势归开始命中的图像所有；拖到 Details 上仍继续，到抬起/失焦才取消。
        const float dx = x - previewPointerX;
        const float dy = y - previewPointerY;
        previewPointerX = x;
        previewPointerY = y;
        if (dx != 0 || dy != 0)
        {
            // 毛发视口：左键转模型（转根点才会触发毛发惯性），中/右键仍转相机。
            // 相机绕轨道对物理没有作用，因此两条手势必须分开。
            if (previewDragButton == 0 && IsActiveHairPreview())
                RequestHairPreviewRotation(dx, dy);
            else
                RequestPreviewCamera(previewDragButton == 2 ? ModelPreviewNavigation::Pan : ModelPreviewNavigation::Orbit, dx, dy);
        }
        return true;
    }
    if (type == "mouseup" && previewDragButton == event.GetParameter<int>("button", -2))
    {
        previewDragButton = -1;
        return true;
    }
    if (type == "mousescroll" && imageHit)
    {
        RequestPreviewCamera(ModelPreviewNavigation::Zoom, 0, event.GetParameter<float>("wheel_delta_y", 0));
        return true;
    }
    if (type == "dblclick" && imageHit)
    {
        RequestPreviewCamera(ModelPreviewNavigation::Frame);
        return true;
    }
    return false;
}

} // namespace VL::EditorUi
