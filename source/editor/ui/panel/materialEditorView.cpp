#include "editor/ui/materialInstanceAssetEditorPanel.h"
#include "editor/ui/panel/editorPanelInternalHelpers.h"
#include "editor/ui/panel/particlePanelHelpers.h"
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
using namespace Panel::Detail;

void MaterialInstanceAssetEditorPanel::ResetTextureView()
{
    textureZoom = 1.0f;
    texturePan = {0, 0};
    textureDragButton = -1;
}

bool MaterialInstanceAssetEditorPanel::ProcessTexturePreviewInput(Rml::Event& event)
{
    const auto* active = resourceEditor.ActiveTab();
    if (!active || active->kind != EditorAssetKind::Texture || active->loading ||
        !active->snapshot || !active->snapshot->texturePreview || !IsResourceEditorVisible() ||
        HasModal() || docking.IsDragging() ||
        textureViewSource != EditorTexturePreviewSource(active->snapshot->revision))
    {
        textureDragButton = -1;
        return false;
    }
    auto* surface = document->GetElementById("resource-preview-surface");
    bool hit = false;
    for (auto* element = event.GetTargetElement(); element; element = element->GetParentNode())
        if (element == surface) { hit = true; break; }
    const auto& type = event.GetType();
    const Rml::Vector2f pointer(event.GetParameter<float>("mouse_x", 0), event.GetParameter<float>("mouse_y", 0));
    if (type == "mousedown" && hit)
    {
        const int button = event.GetParameter<int>("button", -1);
        if (button < 0 || button > 2) return false;
        CloseMenus();
        CloseContextMenu();
        textureDragButton = button;
        texturePointer = pointer;
        return true;
    }
    if (type == "mousemove" && textureDragButton >= 0)
    {
        texturePan += pointer - texturePointer;
        texturePointer = pointer;
    }
    else if (type == "mouseup" && textureDragButton == event.GetParameter<int>("button", -2))
    {
        textureDragButton = -1;
        return true;
    }
    else if (type == "mousescroll" && hit)
    {
        const float delta = event.GetParameter<float>("wheel_delta_y", 0);
        const float nextZoom = std::clamp(textureZoom * std::exp(-delta * 0.15f), 0.1f, 32.0f);
        const auto size = surface->GetBox().GetSize(Rml::BoxArea::Padding);
        const auto anchor = pointer - surface->GetAbsoluteOffset(Rml::BoxArea::Padding) - size * 0.5f;
        // 保持鼠标下的图片坐标不动；只改 UI 几何，不重新解码/上传像素。
        texturePan = anchor - (anchor - texturePan) * (nextZoom / textureZoom);
        textureZoom = nextZoom;
    }
    else if (type == "dblclick" && hit) ResetTextureView();
    else return false;
    RefreshResourceEditor();
    return true;
}

void MaterialInstanceAssetEditorPanel::RefreshDocumentTabs()
{
    std::vector<EditorDockResourceTab> tabs;
    for (const auto& tab : resourceEditor.Tabs())
    {
        const auto* resource = ResourceDocument(tab.path);
        const auto* particle = tab.kind == EditorAssetKind::ParticleSystem ? ParticleDocument(tab.path) : nullptr;
        const auto* terrain = tab.kind == EditorAssetKind::Terrain ? TerrainDocument(tab.path) : nullptr;
        const bool dirty = (resource && resource->dirty) || (particle && particle->dirty) ||
            (terrain && terrain->dirty);
        tabs.push_back({ResourcePanelId(tab.path), tab.path,
            std::filesystem::path(tab.path).filename().string() + (dirty ? " *" : ""),
            EditorAssetKindIcon(tab.kind), -1});
    }
    for (size_t index = 0; index < snapshot.documentTabs.size(); ++index)
    {
        const auto& tab = snapshot.documentTabs[index];
        tabs.push_back({MaterialPanelId(tab.assetPath), tab.assetPath,
            std::filesystem::path(tab.assetPath).filename().string() + (tab.dirty ? " *" : ""),
            EditorAssetKindIcon(EditorAssetKind::MaterialInstance), static_cast<int>(index)});
    }
    docking.SetResourceTabs(tabs);
}

void MaterialInstanceAssetEditorPanel::RefreshAnimationControls(const std::string& prefix, bool enabled)
{
    SetDisabled(prefix + "-play", !enabled || runtimeView.animationPlaying || runtimeView.animationClips.empty());
    SetDisabled(prefix + "-pause", !enabled || !runtimeView.animationPlaying);
    SetDisabled(prefix + "-clip", !enabled || runtimeView.animationClips.empty());
    SetDisabled(prefix + "-time", !enabled || runtimeView.animationDuration <= 0);
    if (!enabled) { SetMarkup(prefix + "-status", ""); return; }
    SetMarkup(prefix + "-status", Text(std::to_string(runtimeView.animationJointCount) +
        " joints | " + SceneActorNumber(runtimeView.animationTime) + " / " +
        SceneActorNumber(runtimeView.animationDuration) + " s"));
    std::string options;
    for (size_t i = 0; i < runtimeView.animationClips.size(); ++i)
        options += "<option value=\"" + std::to_string(i) + "\">" + Text(runtimeView.animationClips[i]) + "</option>";
    widgetDocument.SetMarkup(prefix + "-clip", options);
    widgetDocument.SetControlValue(prefix + "-clip", std::to_string(runtimeView.animationClip));
    auto* timeline = document->GetElementById(prefix + "-time");
    if (timeline != document->GetContext()->GetFocusElement())
    {
        timeline->SetAttribute("max", SceneActorNumber(runtimeView.animationDuration));
        widgetDocument.SetControlValue(prefix + "-time", SceneActorNumber(runtimeView.animationTime));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshRetargetAnimation(const EditorResourceEditorTab& tab)
{
    const auto values = !tab.loading && tab.snapshot ? tab.snapshot->retargetPreview : nullptr;
    const bool visible = retargetEditor.AnimationVisible() && IsResourceEditorVisible();
    if (visible && values && runtimeActionSink &&
        (requestedRetargetPath != tab.path || requestedRetargetRevision != tab.snapshot->revision ||
            requestedPreviewPath != values->targetModelPath || requestedPreviewBlueprint))
    {
        RequestAssetPreview(values->targetModelPath, false, tab.path, tab.snapshot->revision);
    }
    const uint64_t generation = visible ? CurrentModelPreviewGeneration() : 0;
    const bool animated = generation && runtimeView.animationWorldGeneration == generation &&
        runtimeView.animationJointCount != 0;
    widgetDocument.SetProperty("retarget-preview-image", "display", generation ? "block" : "none");
    widgetDocument.SetProperty("retarget-preview-status", "display", generation ? "none" : "block");
    SetMarkup("retarget-preview-status", Text(values ? "Loading paired model preview..." :
        tab.snapshot && !tab.snapshot->previewDiagnostic.empty() ? tab.snapshot->previewDiagnostic :
        "Animation preview requires preview.targetModelPath"));
    RefreshAnimationControls("retarget-animation", animated);
    std::string assets;
    if (animated)
        for (size_t index = 0; index < runtimeView.animationClips.size(); ++index)
            assets += UiWidgets::ListRow({"animation-browser-clip", index}, runtimeView.animationClips[index],
                index == runtimeView.animationClip, "icons/play.png");
    SetMarkup("retarget-animation-assets", std::move(assets));
    SetDisabled("retarget-animation-frame", !generation);
    if (animated && initializedRetargetGeneration != generation && !runtimeView.animationClips.empty())
    {
        initializedRetargetGeneration = generation;
        UiAction action;
        action.type = UiActionType::ControlAnimationPreview;
        action.previewCamera.worldGeneration = generation;
        action.stringValue = "previewClip";
        action.floatValue = static_cast<float>(runtimeView.animationClip);
        runtimeActionSink->RequestRuntimeAction(std::move(action));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshResourceEditor()
{
    if (document == nullptr) return;
    WidgetRefreshScope refresh(refreshingControls);
    RefreshDocumentTabs();
    {
        // 选择缓存只服务仍打开的粒子标签；关闭或替换文档时同步清理。
        std::unordered_set<std::string> openParticlePaths;
        for (const auto& tab : resourceEditor.Tabs())
        {
            if (tab.kind == EditorAssetKind::ParticleSystem)
                openParticlePaths.insert(tab.path);
        }
        for (auto it = particleEmitterSelectionByPath.begin();
             it != particleEmitterSelectionByPath.end();)
        {
            if (openParticlePaths.count(it->first) == 0)
                it = particleEmitterSelectionByPath.erase(it);
            else
                ++it;
        }
        for (auto it = particleModuleByPath.begin();
             it != particleModuleByPath.end();)
        {
            if (openParticlePaths.count(it->first) == 0)
                it = particleModuleByPath.erase(it);
            else
                ++it;
        }
    }
    const auto* active = resourceEditor.ActiveTab();
    const bool blueprintAsset = active && active->kind == EditorAssetKind::Blueprint;
    RefreshParticleEditorMarkup();
    const bool terrainAsset = active && active->kind == EditorAssetKind::Terrain;
    for (const char* id : {"blueprint-asset-editor", "blueprint-asset-save", "blueprint-asset-revert"})
        widgetDocument.SetProperty(id, "display", blueprintAsset ? "block" : "none");
    widgetDocument.SetProperty("terrain-asset-editor", "display", terrainAsset ? "block" : "none");
    for (const char* id : {"terrain-asset-save", "terrain-asset-revert"})
        widgetDocument.SetProperty(id, "display", terrainAsset ? "block" : "none");
    widgetDocument.SetProperty("resource-preview-panel", "display",
        blueprintAsset || terrainAsset ? "none" : "flex");
    widgetDocument.SetProperty("resource-details-panel", "display",
        blueprintAsset || terrainAsset ? "none" : "flex");
    const bool retargetAsset = active && active->kind == EditorAssetKind::Retarget;
    widgetDocument.SetClass("resource-editor", "retarget-active", retargetAsset);
    for (const char* id : {"blueprint-asset-editor", "blueprint-asset-save", "blueprint-asset-revert"})
        widgetDocument.SetProperty(id, "display", blueprintAsset ? "block" : "none");
    widgetDocument.SetProperty("retarget-asset-editor", "display", retargetAsset ? "flex" : "none");
    widgetDocument.SetProperty("resource-preview-panel", "display",
        blueprintAsset || terrainAsset || retargetAsset ? "none" : "flex");
    widgetDocument.SetProperty("resource-details-panel", "display",
        blueprintAsset || terrainAsset || retargetAsset ? "none" : "flex");
    if (retargetAsset)
    {
        for (const char* id : {"resource-model-tools", "resource-texture-tools"})
            widgetDocument.SetClass(id, "visible", false);
        SetDisabled("resource-reload", active->loading);
        auto visibleTab = *active;
        if (reloadingRetargetPath == active->path)
        {
            if (active->snapshot && active->snapshot->revision > reloadingRetargetRevision)
                reloadingRetargetPath.clear();
            else visibleTab.loading = true;
        }
        retargetEditor.Build(visibleTab);
        RefreshRetargetAnimation(visibleTab);
        return;
    }
    if (blueprintAsset)
    {
        terrainAssetEditor.Build(nullptr, false);
        for (const char* id : {"resource-model-tools", "resource-texture-tools"})
            widgetDocument.SetClass(id, "visible", false);
        SetDisabled("resource-reload", !BlueprintDocument(active->path));
        RefreshBlueprintAssetEditor();
        return;
    }
    if (terrainAsset)
    {
        for (const char* id : {"resource-model-tools", "resource-texture-tools"})
            widgetDocument.SetClass(id, "visible", false);
        const auto* terrain = TerrainDocument(active->path);
        SetDisabled("resource-reload", active->loading || terrain == nullptr);
        SetMarkup("resource-preview-title", Text(terrain != nullptr &&
            terrain->document.is_object() && terrain->document.contains("name")
            ? terrain->document.value("name", std::string())
            : std::filesystem::path(active->path).filename().string()));
        SetMarkup("resource-preview-kind", "Terrain");
        SetMarkup("resource-preview-path", Text(active->path));
        terrainAssetEditor.Build(terrain, active->loading);
        return;
    }
    terrainAssetEditor.Build(nullptr, false);
    const bool isTressfx = active != nullptr && active->kind == EditorAssetKind::Tressfx;
    const bool isMesh = active != nullptr && (active->kind == EditorAssetKind::Mesh || isTressfx);
    // 快照到达后以作者文档为准复核毛发判定（目录可能缺失或过期）；目录只在
    // 打开时提供同步判定，避免一帧内先请求了本不该加载的预览。
    if (isMesh && !active->loading && active->snapshot)
    {
        if (active->snapshot->type == "tressfx")
        {
            hairPreviewPath = active->path;
            hairPreviewBlocked = active->snapshot->tressfxSkinning;
        }
        else if (hairPreviewPath == active->path)
        {
            hairPreviewPath.clear();
            hairPreviewBlocked = false;
        }
    }
    else if (!isMesh || active->path != hairPreviewPath)
    {
        hairPreviewPath.clear();
        hairPreviewBlocked = false;
    }
    const bool isHair = isTressfx && hairPreviewPath == active->path;
    const bool hairBlocked = isHair && hairPreviewBlocked;
    RefreshHairSimulationSection(isHair, hairBlocked);
    // Groups 是 TF 资产级作者数据；即使 skinned 预览被阻断，也仍然允许编辑/保存。
    RefreshHairGroupsSection(isHair, hairBlocked);
    const bool previewReady =
        (HasCurrentModelPreview() && !hairBlocked) || HasParticlePreview();
    const bool animated = isMesh && previewReady && runtimeView.animationWorldGeneration ==
        CurrentModelPreviewGeneration() && runtimeView.animationJointCount != 0;
    widgetDocument.SetProperty("resource-animation-tools", "display", animated ? "flex" : "none");
    widgetDocument.SetProperty("resource-motion-features", "display", animated ? "block" : "none");
    if (animated)
    {
        RefreshAnimationControls("resource-animation", true);
        SetMarkup("resource-motion-status", Text(runtimeView.animationFeatureStatus));
        // 固定通道结构，只更新数值 span；播放时不重建整表或改变 Details 滚动位置。
        std::string rows;
        for (size_t i = 0; i < runtimeView.animationFeatureChannels.size(); ++i)
            rows += "<div class=\"motion-feature-row\"><span>" + Text(runtimeView.animationFeatureChannels[i]) +
                "</span><span id=\"motion-feature-" + std::to_string(i) + "\"></span></div>";
        SetMarkup("resource-motion-values", rows);
        for (size_t i = 0; i < runtimeView.animationFeatureChannels.size(); ++i)
            SetMarkup("motion-feature-" + std::to_string(i), i < runtimeView.animationFeatures.size()
                ? Text(SceneActorNumber(runtimeView.animationFeatures[i])) : "--");
    }
    const auto* texture = active && !active->loading && active->snapshot
        ? active->snapshot->texturePreview.get() : nullptr;
    const bool textureReady = active && active->kind == EditorAssetKind::Texture && texture;
    widgetDocument.SetProperty("resource-texture-image", "display", textureReady ? "block" : "none");
    auto* textureImage = document->GetElementById("resource-texture-image");
    const std::string textureSource = textureReady ? EditorTexturePreviewSource(active->snapshot->revision) : "";
    if (textureViewSource != textureSource)
    {
        textureViewSource = textureSource;
        ResetTextureView();
    }
    widgetDocument.SetClass("resource-texture-tools", "visible", active && active->kind == EditorAssetKind::Texture);
    SetDisabled("resource-texture-fit", !textureReady);
    if (textureImage->GetAttribute<std::string>("src", "") != textureSource)
        textureImage->SetAttribute("src", textureSource);
    if (textureReady)
    {
        // 按面板可用区域等比例取景，不能沿用场景图像的强制拉伸布局。
        const auto size = document->GetElementById("resource-preview-surface")->GetBox().GetSize(Rml::BoxArea::Padding);
        const float scale = std::min(std::max(1.0f, size.x - 16) / texture->width,
            std::max(1.0f, size.y - 16) / texture->height);
        const float width = texture->width * scale * textureZoom, height = texture->height * scale * textureZoom;
        widgetDocument.SetProperty("resource-texture-image", "width", std::to_string(width) + "px");
        widgetDocument.SetProperty("resource-texture-image", "height", std::to_string(height) + "px");
        widgetDocument.SetProperty("resource-texture-image", "left", std::to_string((size.x - width) * 0.5f + texturePan.x) + "px");
        widgetDocument.SetProperty("resource-texture-image", "top", std::to_string((size.y - height) * 0.5f + texturePan.y) + "px");
    }
    SetDisabled("resource-frame", !previewReady || !isMesh);
    widgetDocument.SetProperty("resource-preview-image", "display", previewReady ? "block" : "none");
    for (const char* id : {"resource-preview-icon", "resource-preview-status", "resource-preview-path"})
        widgetDocument.SetProperty(id, "display", previewReady || textureReady ? "none" : "block");
    // 毛发工具只在可预览的 tressfx 资产上出现；挡预览时隐藏复位按钮，
    // 让“没有视口”和“有视口但没东西可复位”在 UI 上不混淆。
    widgetDocument.SetProperty("resource-hair-tools", "display", isHair && !hairBlocked ? "flex" : "none");
    const auto* hairDocument = isHair ? TressfxDocument(active->path) : nullptr;
    SetDisabled("resource-hair-reset", !previewReady || !hairDocument ||
        !TressfxGpuSimulationDefaults(hairDocument->document));
    widgetDocument.SetClass("resource-model-tools", "visible", isMesh);
    widgetDocument.SetClass("resource-model-info", "visible", isMesh);
    for (const char* id : {"resource-tool-select", "resource-tool-move", "resource-tool-rotate", "resource-tool-scale",
                           "resource-space-world", "resource-space-local"})
        SetDisabled(id, !isMesh);
    if (active == nullptr)
    {
        SetDisabled("resource-reload", true);
        SetMarkup("resource-preview-title", "Asset Editor");
        SetMarkup("resource-preview-kind", "");
        SetMarkup("resource-preview-status", "Open a Mesh or Texture from Content Browser.");
        SetMarkup("resource-preview-path", "");
        auto* previewIcon = document->GetElementById("resource-preview-icon");
        if (previewIcon->GetAttribute<std::string>("src", "") != "icons/mesh.png")
            previewIcon->SetAttribute("src", "icons/mesh.png");
        resourceJsonInspector.Build(nullptr, {}, false);
        return;
    }

    SetDisabled("resource-reload", active->loading);
    SetMarkup("resource-preview-title", Text(active->snapshot != nullptr &&
        !active->snapshot->name.empty() ? active->snapshot->name :
        std::filesystem::path(active->path).filename().string()));
    SetMarkup("resource-preview-kind", Text(EditorAssetKindName(active->kind)));
    if (animated) SetMarkup("resource-preview-kind", "Skeletal Mesh");
    if (textureReady)
        SetMarkup("resource-preview-kind", Text(std::to_string(texture->sourceWidth) + " x " +
            std::to_string(texture->sourceHeight) + " | RGB | " + active->snapshot->textureColorSpace));
    SetMarkup("resource-preview-path", Text(active->path));
    SetMarkup("resource-preview-status", active->loading
        ? (active->kind == EditorAssetKind::Texture ? "Loading texture preview..." : "Reading JSON metadata...")
        : hairBlocked ? Text(std::string(TressfxHairSkinnedAssetMessage))
        : isMesh ? (isHair ? "Waiting for hair preview" : "Waiting for model preview")
        : active->snapshot && !active->snapshot->previewDiagnostic.empty()
            ? Text(active->snapshot->previewDiagnostic)
            : active->kind == EditorAssetKind::ParticleSystem
                ? "Loading particle preview..."
                : "Preview unavailable: referenced data is not loaded.");
    auto* previewIcon = document->GetElementById("resource-preview-icon");
    const std::string icon = EditorAssetKindIcon(active->kind);
    if (previewIcon->GetAttribute<std::string>("src", "") != icon)
        previewIcon->SetAttribute("src", icon);
    resourceJsonInspector.Build(active->snapshot, active->path, active->loading);
    if (isMesh)
    {
        for (const auto& pair : {std::pair<const char*, const char*>("resource-tool-select", "select"),
                                 {"resource-tool-move", "move"}, {"resource-tool-rotate", "rotate"},
                                 {"resource-tool-scale", "scale"}})
            widgetDocument.SetClass(pair.first, "active", resourceTransformTool == pair.second);
        widgetDocument.SetClass("resource-space-world", "active", resourceTransformSpace == "world");
        widgetDocument.SetClass("resource-space-local", "active", resourceTransformSpace == "local");
    }
    if (isMesh)
    {
        const std::string name = active->snapshot != nullptr && !active->snapshot->name.empty()
            ? active->snapshot->name : std::filesystem::path(active->path).filename().string();
        SetMarkup("resource-model-name", Text("Name: " + name));
        SetMarkup("resource-model-path", Text("Path: " + active->path));
        SetMarkup("resource-model-status", Text("Status: " +
            std::string(active->loading ? "Reading metadata" :
                active->snapshot != nullptr && active->snapshot->diagnostic.empty() ? "Metadata ready" : "Diagnostic")));
        SetMarkup("resource-model-fields", Text("Metadata fields: " + std::to_string(
            active->snapshot != nullptr ? active->snapshot->fields.size() : 0)));
        SetMarkup("resource-model-description", Text(active->snapshot != nullptr
            ? active->snapshot->description : std::string{}));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshControls()
{
    // File/Edit 菜单项与工具栏按钮共用同一份文档命令可用性。
    const auto* activeTab = resourceEditor.ActiveTab();
    const bool particleCommands =
        activeTab != nullptr &&
        activeTab->kind == EditorAssetKind::ParticleSystem &&
        IsResourceEditorVisible();
    for (const char* id : DocumentCommandIds)
        SetDisabled(id, particleCommands
            ? !ParticlePanelDetail::ParticleMenuCommandAllowed(id)
            : !snapshot.activeDocument);
    SetMarkup("editor-notice", Text(!docking.Notice().empty() ? docking.Notice() :
        localStatus.empty() ? snapshot.statusMessage : localStatus));
    if (!snapshot.activeDocument)
    {
        SetMarkup("document-title", "Material Instance");
        SetMarkup("document-status", "");
        SetDisabled("apply-preview", true);
        SetDisabled("restore-preview", true);
        return;
    }
    const auto& asset = *snapshot.activeDocument;
    SetMarkup("document-title", Text(std::filesystem::path(asset.assetPath).filename().string()));
    SetMarkup("document-status", Text(std::string(DocumentStatus(asset.status)) +
        " | Preview: " + PreviewStatus(asset.preview) + " | " + asset.validationMessage +
        (asset.previewMessage.empty() ? "" : " | " + asset.previewMessage)));
    const bool connected = asset.preview == EditorPreviewStatus::Connected;
    SetDisabled("apply-preview", !connected);
    SetDisabled("restore-preview", !connected);
    for (size_t index = 0; index < asset.renderStates.size(); ++index)
    {
        SetControlValue("state-" + std::to_string(index), asset.renderStates[index].effectiveValue);
        SetDisabled("reset-state-" + std::to_string(index), !asset.renderStates[index].overrideValue);
    }
    for (size_t index = 0; index < asset.parameters.size(); ++index)
    {
        const auto& parameter = asset.parameters[index];
        for (uint32_t component = 0; component < parameter.effectiveValue.ComponentCount(); ++component)
            SetControlValue("parameter-" + std::to_string(index) + "-" + std::to_string(component),
                Number(parameter.effectiveValue.values[component]));
        // 色块与十六进制读数逐帧跟随有效线性值；两者都走既有缓存，静止帧不重复写。
        if (parameter.type == EditorParameterType::Color)
        {
            const auto readout = UiWidgets::MakeColorReadout(parameter.effectiveValue.values);
            widgetDocument.SetProperty("color-swatch-" + std::to_string(index), "background-color", readout.rgb);
            SetMarkup("color-hex-" + std::to_string(index), Text(readout.hex));
        }
        SetDisabled("reset-parameter-" + std::to_string(index), !parameter.overrideValue);
    }
    for (size_t index = 0; index < asset.textures.size(); ++index)
    {
        const auto& texture = asset.textures[index];
        // 目录与有效绑定未变时，连选项字符串也不生成；不能只在写 DOM 时才判相等。
        if (textureOptionsDirty || index >= textureOptionSelections.size() ||
            textureOptionSelections[index] != texture.effectiveAssetPath)
        {
            std::string options = Option(texture.effectiveAssetPath, texture.effectiveAssetPath);
            for (const auto& candidate : snapshot.textureAssets)
            {
                if (candidate.valid && candidate.assetPath != texture.effectiveAssetPath)
                    options += Option(candidate.assetPath, candidate.assetPath);
            }
            SetMarkup("texture-" + std::to_string(index), std::move(options));
        }
        SetControlValue("texture-" + std::to_string(index), asset.textures[index].effectiveAssetPath);
        SetDisabled("reset-texture-" + std::to_string(index), !asset.textures[index].overrideAssetPath);
    }
    if (textureOptionsDirty || textureOptionSelections.size() != asset.textures.size())
        textureOptionSelections.resize(asset.textures.size());
    for (size_t index = 0; index < asset.textures.size(); ++index)
        textureOptionSelections[index] = asset.textures[index].effectiveAssetPath;
    textureOptionsDirty = false;
}

void MaterialInstanceAssetEditorPanel::RequestUndoRedo(bool redo)
{
    // 全局 Undo/Redo 按上下文路由，不新增第二套历史栈。
    if (IsTerrainHistoryContext())
    {
        const TerrainBrushOperation operation = redo ?
            TerrainBrushOperation::Redo : TerrainBrushOperation::Undo;
        if (landscapePaintMode) SendTerrainPaintAction(operation, false);
        else SendTerrainBrushAction(operation, false);
        return;
    }
    if (RequestParticleHistory(redo)) return;
    if (RequestBlueprintHistory(redo)) return;
    if (!runtimeActionSink || HasModal() || runtimeView.scripts.state != ScriptPlayState::Editing ||
        runtimeView.offlineMode || IsAssetEditorVisible() ||
        !(redo ? runtimeView.scripts.canRedo : runtimeView.scripts.canUndo))
        return;
    UiAction request;
    request.type = redo ? UiActionType::RedoSceneEdit : UiActionType::UndoSceneEdit;
    request.actorProperty.worldGeneration = runtimeView.scripts.worldGeneration;
    runtimeActionSink->RequestRuntimeAction(std::move(request));
}

void MaterialInstanceAssetEditorPanel::RefreshHistoryControls()
{
    if (document == nullptr) return;
    bool canUndo = false;
    bool canRedo = false;
    if (IsTerrainHistoryContext())
    {
        // 地形历史尚无 canUndo/canRedo 快照；保持乐观，空历史由引擎反馈报告。
        canUndo = canRedo = true;
    }
    else if (const auto* tab = resourceEditor.ActiveTab();
        tab && tab->kind == EditorAssetKind::Blueprint && IsResourceEditorVisible())
    {
        const auto* authored = BlueprintDocument(tab->path);
        const bool enabled = authored && runtimeView.scripts.available && !runtimeView.offlineMode &&
            runtimeView.scripts.state == ScriptPlayState::Editing && pendingBlueprintPath.empty() &&
            !blueprintCommandAwaitingSnapshot && !blueprintGizmoDragging && !HasModal();
        canUndo = enabled && authored->canUndo;
        canRedo = enabled && authored->canRedo;
    }
    else
    {
        const auto& scripts = runtimeView.scripts;
        const bool enabled = !runtimeView.offlineMode && !IsAssetEditorVisible() &&
            !HasModal() && scripts.state == ScriptPlayState::Editing;
        canUndo = enabled && scripts.canUndo;
        canRedo = enabled && scripts.canRedo;
    }
    SetDisabled("menu-edit-undo", !canUndo);
    SetDisabled("menu-edit-redo", !canRedo);
    SetDisabled("workspace-undo", !canUndo);
    SetDisabled("workspace-redo", !canRedo);
}

void MaterialInstanceAssetEditorPanel::RefreshScriptControls()
{
    if (document == nullptr) return;
    const auto& scripts = runtimeView.scripts;
    const bool level = !runtimeView.offlineMode && !IsAssetEditorVisible();
    const bool enabled = scripts.available && level;
    bool hasActors = false;
    for (const auto& actor : OutlinerActors())
        if (actor.type == EditorSceneActorType::Script) hasActors = true;
    SetDisabled("script-play", !enabled || !hasActors || scripts.state == ScriptPlayState::Playing);
    SetDisabled("script-pause", !enabled || scripts.state != ScriptPlayState::Playing);
    SetDisabled("script-stop", !enabled || scripts.state == ScriptPlayState::Editing);
    SetDisabled("script-reload", !enabled);
    SetDisabled("script-compile", !enabled || !scripts.buildAvailable || scripts.buildBusy);
    SetDisabled("script-time-scale", !enabled);
    SetControlValue("script-time-scale", Number(scripts.timeScale));
    SetDisabled("menu-script-compile", !enabled || !scripts.buildAvailable || scripts.buildBusy);
    SetDisabled("menu-script-auto", !enabled || !scripts.buildAvailable);
    widgetDocument.SetClass("menu-script-auto", "checked", scripts.autoCompile);
    const bool canAuthor = scripts.available && scripts.state == ScriptPlayState::Editing;
    SetDisabled("script-add", !canAuthor || (!scriptPlacementCreateAsset && !level) || scripts.classes.empty());
    SetDisabled("script-placement-mode", !canAuthor);
    widgetDocument.SetProperty("script-placement-mesh-row", "display", scriptPlacementCreateAsset ? "none" : "flex");
    widgetDocument.SetProperty("script-new-path-row", "display", scriptPlacementCreateAsset ? "flex" : "none");
    if (auto* button = document->GetElementById("script-add"))
    {
        const std::string title = scriptPlacementCreateAsset ?
            "Create Blueprint Asset" : "Place C# instance";
        if (button->GetAttribute<std::string>("title", "") != title) button->SetAttribute("title", title);
    }
    SetDisabled("script-placement-toggle", !canAuthor);
    RefreshHistoryControls();
    widgetDocument.SetProperty("script-placement", "display",
        scripts.available && scriptPlacementVisible ? "block" : "none");
    std::string options;
    for (const auto& type : scripts.classes)
        options += Option(type.at("name").get<std::string>(), type.at("name").get<std::string>());
    SetMarkup("script-class", std::move(options));
    SetMarkup("script-feedback", Text(scripts.feedback));
    widgetDocument.SetProperty("script-feedback", "display", scripts.feedback.empty() ? "none" : "block");
}

void MaterialInstanceAssetEditorPanel::UpdatePerformance(const PerformanceSnapshot& performance)
{
    // 面板自己按 5 Hz 取样；这里只传冻结快照、可见性与同一份 UI 单调时钟。
    performancePanel.Build(performance, docking.IsPanelActive("performance"),
        Rml::GetSystemInterface()->GetElapsedTime());
}

void MaterialInstanceAssetEditorPanel::UpdateScriptPerformance(
    const nlohmann::json& scriptPerformance)
{
    // Log 的 Scripts 页展示脚本时间/计数器；它是时间序列，随性能分节采样，
    // 不通过事件驱动的脚本分节刷新。
    logPanel.BuildPerformance(scriptPerformance, docking.IsPanelActive("log"));
}

void MaterialInstanceAssetEditorPanel::UpdateRuntimeView(const UiViewModelSnapshot& view)
{
    if (view.scripts.worldGeneration != runtimeView.scripts.worldGeneration ||
        view.scripts.state != ScriptPlayState::Editing || view.offlineMode)
        actorDragPath.clear();
    const bool refreshOffline = !runtimeControlsCurrent || !SameOfflineControls(runtimeView, view);
    runtimeView = view;
    WidgetRefreshScope refresh(refreshingControls);
    // Grid 是编辑器显示偏好；Play/Pause 下控件禁用但保留用户选择，
    // 回到 Editing 后恢复同一开关状态。
    const bool sceneEditing = view.scripts.state == ScriptPlayState::Editing &&
        !view.offlineMode;
    auto* gridToggle = document ? document->GetElementById("viewport-grid-toggle") : nullptr;
    if (gridToggle && gridToggle->HasAttribute("checked") != view.editorGridVisible)
    {
        if (view.editorGridVisible) gridToggle->SetAttribute("checked", "");
        else gridToggle->RemoveAttribute("checked");
    }
    SetDisabled("viewport-grid-toggle", !sceneEditing);
    SetDisabled("viewport-actor-visuals", !sceneEditing);
    // 变换工具和角落坐标轴只属于编辑器视口；Play/Pause 下镜头与操作都归蓝图，
    // 这些控件既不可用也不应继续显示。
    widgetDocument.SetProperty("viewport-axis-indicator", "display",
        sceneEditing ? "block" : "none");
    widgetDocument.SetProperty("viewport-transform-tools", "display",
        sceneEditing ? "flex" : "none");
    widgetDocument.SetProperty("viewport-transform-space", "display",
        sceneEditing ? "flex" : "none");
    const bool motionEnabled = view.motionDebug && view.motionDebug->enabled;
    auto* motionToggle = document ? document->GetElementById("viewport-motion-debug") : nullptr;
    if (motionToggle && motionToggle->HasAttribute("checked") != motionEnabled)
    {
        if (motionEnabled) motionToggle->SetAttribute("checked", "");
        else motionToggle->RemoveAttribute("checked");
    }
    SetDisabled("viewport-motion-debug", !view.motionWorldGeneration || view.offlineMode);
    const bool matching = motionEnabled && !view.motionDebug->databaseIdentity.empty();
    const std::string motionStatus = matching ? "MM #" + std::to_string(view.motionDebug->searchCount) +
        " | " + view.motionDebug->decisionReason +
        " | Cyan: actual | Amber: desired/no-collision | Red: query" :
        motionEnabled ? view.motionDebug->summary : "";
    SetMarkup("viewport-motion-status", Text(motionStatus));
    widgetDocument.SetClass("viewport-motion-status", "motion-matching-summary", matching);
    widgetDocument.SetProperty("viewport-motion-status", "display",
        motionEnabled && !view.offlineMode ? "block" : "none");
    RefreshMotionMatching();
    const bool debugAvailable = view.debugDraw && view.motionWorldGeneration && !view.offlineMode;
    SetDisabled("viewport-debug-draw", !debugAvailable);
    if (!debugAvailable && openMenu == "debug-draw") CloseMenus();
    if (debugAvailable)
    {
        const auto& debug = *view.debugDraw;
        auto* toggle = document->GetElementById("debug-draw-enabled");
        if (toggle->HasAttribute("checked") != debug.enabled)
        {
            if (debug.enabled) toggle->SetAttribute("checked", ""); else toggle->RemoveAttribute("checked");
        }
        std::string categories;
        for (size_t i = 0; i < debug.categories.size(); ++i)
        {
            const auto& category = debug.categories[i];
            categories += "<label class=\"debug-draw-check\"><input type=\"checkbox\" data-action=\"debug-draw-category\" data-index=\"" +
                std::to_string(i) + "\"" + (category.enabled ? " checked=\"checked\"" : "") + "/><span>" +
                Text(category.name) + "</span></label>";
        }
        SetMarkup("debug-draw-categories", std::move(categories));
        std::string owners = Option("", "All Owners");
        for (const auto& owner : debug.owners)
            if (!owner.empty()) owners += Option(owner, owner);
        if (!debug.ownerFilter.empty() &&
            std::find(debug.owners.begin(), debug.owners.end(), debug.ownerFilter) == debug.owners.end())
            owners += Option(debug.ownerFilter, debug.ownerFilter);
        SetMarkup("debug-draw-owner", std::move(owners));
        SetControlValue("debug-draw-owner", debug.ownerFilter);
        SetMarkup("debug-draw-count", std::to_string(debug.visibleCount) + " / " + std::to_string(debug.storedCount));
    }
    if (const auto* tab = resourceEditor.ActiveTab(); tab && tab->kind == EditorAssetKind::Blueprint)
        RefreshBlueprintAssetEditor();
    // 相机和统计变化不触碰离线表单；视口工具条继续独立更新。
    RefreshViewportToolbar();
    RefreshScriptControls();
    contentBrowser.Build(snapshot, runtimeView.scripts.classes);
    if (!refreshOffline || document == nullptr) return;
    SetControlValue("offline-samples", std::to_string(view.offlineSamples));
    SetControlValue("offline-bounces", std::to_string(view.offlineBounces));
    SetControlValue("offline-lightSamples", std::to_string(view.offlineLightSamples));
    SetControlValue("offline-seed", std::to_string(view.offlineSeed));
    SetControlValue("offline-aov", std::to_string(view.offlineAov));
    SetControlValue("offline-exposure", Number(view.offlineExposure));
    SetMarkup("offline-status", std::to_string(view.offlineCompletedSamples) + " / " +
        std::to_string(view.offlineSamples) + " spp");
    SetMarkup("offline-notice", Text(view.offlineUiNotice.empty()
        ? view.offlineError : view.offlineUiNotice));
    const auto state = static_cast<Offline::SessionState>(view.offlineState);
    const bool active = view.offlineMode && (state == Offline::SessionState::Preparing ||
        state == Offline::SessionState::Rendering);
    SetDisabled("offline-pause", !active);
    SetDisabled("offline-resume", !view.offlineMode || state != Offline::SessionState::Paused);
    SetDisabled("offline-stop", !active && (!view.offlineMode || state != Offline::SessionState::Paused));
    SetDisabled("offline-restart", !view.offlineMode);
    SetDisabled("offline-export", !view.offlineMode || view.offlineCompletedSamples == 0);
    widgetDocument.SetClass("mode-realtime", "selected", !view.offlineMode);
    widgetDocument.SetClass("mode-offline", "selected", view.offlineMode);
    const auto* focus = document->GetContext()->GetFocusElement();
    // 聚焦输入保留用户的半成品；失焦后的第一帧仍需用权威快照校正被拒绝的值。
    runtimeControlsCurrent = focus == nullptr || focus->GetId().find("offline-") != 0;
}

void MaterialInstanceAssetEditorPanel::RefreshMotionMatchingCandidate(
    size_t slot, const Animation::MotionCandidate* candidate)
{
    const auto prefix = "motion-candidate-" + std::to_string(slot);
    SetMarkup(prefix + "-clip", candidate ? Text(candidate->clipId) : "-");
    SetMarkup(prefix + "-time", candidate ? MotionNumber(candidate->time) + "s" : "-");
    SetMarkup(prefix + "-trajectory", candidate ? MotionNumber(candidate->trajectoryCost) : "-");
    SetMarkup(prefix + "-pose", candidate ? MotionNumber(candidate->poseCost) : "-");
    SetMarkup(prefix + "-policy", candidate ? MotionNumber(candidate->policyCost) : "-");
    SetMarkup(prefix + "-total", candidate ? MotionNumber(candidate->totalCost) : "-");
    std::string status;
    if (candidate)
    {
        status = candidate->eligible ? (candidate->continuing ? "continuing" : "eligible") :
            "rejected: " + candidate->rejection;
        if (candidate->quantizationSeconds > 0)
            status += " | quantized " + MotionNumber(candidate->quantizationSeconds) + "s";
    }
    SetMarkup(prefix + "-reason", Text(status));
    widgetDocument.SetClass(prefix, "motion-rejected", candidate && !candidate->eligible);
    auto* row = document->GetElementById(prefix);
    const std::string title = candidate ? candidate->clipId + " @ " + MotionNumber(candidate->time) +
        "s | " + status : "";
    if (row && row->GetAttribute<std::string>("title", "") != title)
        row->SetAttribute("title", title);
    SetMarkup(prefix + "-detail", Text(title));
}

void MaterialInstanceAssetEditorPanel::RefreshMotionMatchingQuery()
{
    const auto& debug = *runtimeView.motionDebug;
    auto channels = debug.channels;
    if (channels.empty())
        for (size_t channel = 0; channel < debug.features.size(); ++channel)
            channels.push_back("[" + std::to_string(channel) + "]");
    if (channels != motionMatchingQueryChannels)
    {
        std::string rows;
        for (size_t channel = 0; channel < channels.size(); ++channel)
        {
            const auto prefix = "motion-query-channel-" + std::to_string(channel);
            rows += "<div id=\"" + prefix + "\" class=\"motion-query-channel\">"
                "<div class=\"motion-query-channel-name\">" + Text(channels[channel]) + "</div>"
                "<div class=\"motion-query-channel-values\"><span id=\"" + prefix +
                "-raw\"></span><span id=\"" + prefix + "-normalized\"></span></div></div>";
        }
        SetMarkup("motion-query-channels", std::move(rows));
        motionMatchingQueryChannels = std::move(channels);
    }
    // RmlUi 不会显示 HTML title；完整身份与查询有实际可展开的固定节点，暂停只读缓存值。
    SetMarkup("motion-query-database", Text("DB " + debug.databaseIdentity));
    SetMarkup("motion-query-payload", Text("Statistics " + debug.payloadIdentity));
    SetMarkup("motion-query-component", Text("Component " + debug.componentIdentity));
    for (size_t channel = 0; channel < motionMatchingQueryChannels.size(); ++channel)
    {
        const auto prefix = "motion-query-channel-" + std::to_string(channel);
        SetMarkup(prefix + "-raw", "Raw " + (channel < debug.features.size() ?
            MotionNumber(debug.features[channel]) : "-"));
        SetMarkup(prefix + "-normalized", "N " + (channel < debug.normalizedQuery.size() ?
            MotionNumber(debug.normalizedQuery[channel]) : "-"));
    }
}

void MaterialInstanceAssetEditorPanel::RefreshMotionMatching()
{
    if (!document) return;
    const auto& debug = runtimeView.motionDebug;
    const bool visible = debug && debug->enabled && !debug->databaseIdentity.empty() &&
        runtimeView.motionWorldGeneration && !runtimeView.offlineMode;
    widgetDocument.SetProperty("viewport-motion-matching", "display", visible ? "block" : "none");
    if (!visible)
    {
        motionMatchingVisible = false;
        return;
    }
    if (!motionMatchingRowsMounted)
    {
        std::string rows;
        for (size_t slot = 0; slot < 7; ++slot)
        {
            const auto prefix = "motion-candidate-" + std::to_string(slot);
            const auto label = slot == 0 ? "Selected" : slot == 1 ? "Continue" :
                "#" + std::to_string(slot - 1);
            rows += "<div id=\"" + prefix + "\" class=\"motion-candidate\">"
                "<div class=\"motion-candidate-heading\"><span class=\"motion-candidate-kind\">" +
                label + "</span><span id=\"" + prefix + "-clip\" class=\"motion-candidate-clip\"></span>"
                "<span id=\"" + prefix + "-time\" class=\"motion-candidate-time\"></span>"
                "<span id=\"" + prefix + "-reason\" class=\"motion-candidate-reason\"></span></div>"
                "<div class=\"motion-candidate-costs\"><span id=\"" + prefix + "-trajectory\"></span>"
                "<span id=\"" + prefix + "-pose\"></span><span id=\"" + prefix + "-policy\"></span>"
                "<span id=\"" + prefix + "-total\"></span></div>"
                "<span id=\"" + prefix + "-detail\" class=\"motion-candidate-detail\"></span></div>";
        }
        SetMarkup("motion-matching-rows", std::move(rows));
        motionMatchingRowsMounted = true;
    }
    // 候选槽位只挂载一次。运行中的数值至多 10 Hz 刷新，决策/代际变化立即显示；
    // 暂停的相同快照经 WidgetDocument 值缓存后不再写 DOM。
    const std::string refreshKey = debug->componentIdentity + "|" + debug->databaseIdentity + "|" +
        debug->payloadIdentity + "|" + debug->decisionReason + "|" + std::to_string(debug->searchCount) +
        (debug->queryReady ? "|ready" : "|warming") + "|" +
        (debug->queryUsesAchievedVelocity ? "blocked|" : "desired|") +
        debug->playbackClipId + "|" + std::to_string(debug->playbackTime) + "|" +
        std::to_string(debug->playbackDuration) + "|" + (debug->playbackPlaying ? "play|" : "pause|") +
        (debug->replayActive ? "replay|" : "live|") + std::to_string(debug->replayFrame) + "|" +
        std::to_string(debug->replayFrames) + "|" +
        std::to_string(static_cast<int>(runtimeView.scripts.state));
    const double now = Rml::GetSystemInterface()->GetElapsedTime();
    if (motionMatchingVisible && refreshKey == motionMatchingRefreshKey &&
        now - motionMatchingRefreshTime < 0.1) return;
    motionMatchingVisible = true;
    motionMatchingRefreshTime = now;
    motionMatchingRefreshKey = refreshKey;
    SetMarkup("motion-matching-title", Text("Motion Matching | #" + std::to_string(debug->searchCount) +
        " | DB " + debug->databaseIdentity.substr(0, 12)));
    SetMarkup("motion-matching-database", Text("DB " + debug->databaseIdentity.substr(0, 12) +
        " | stats " + debug->payloadIdentity.substr(0, 12)));
    SetMarkup("motion-matching-query", Text("Query " + std::to_string(debug->features.size()) +
        "D " + (debug->queryReady ? "ready" : "warming") +
        (debug->queryUsesAchievedVelocity ? " blocked" : "") + " | raw " +
        MotionQueryReadout(debug->features) + " | N " + MotionQueryReadout(debug->normalizedQuery)));
    const std::string databaseTitle = "DB " + debug->databaseIdentity + "\nStatistics " +
        debug->payloadIdentity + "\nComponent " + debug->componentIdentity;
    auto* databaseLabel = document->GetElementById("motion-matching-database");
    if (databaseLabel->GetAttribute<std::string>("title", "") != databaseTitle)
        databaseLabel->SetAttribute("title", databaseTitle);
    auto* matchingTitle = document->GetElementById("motion-matching-title");
    if (matchingTitle->GetAttribute<std::string>("title", "") != databaseTitle)
        matchingTitle->SetAttribute("title", databaseTitle);
    std::string queryTitle = "Search " + std::to_string(debug->searchCount) + " | raw / normalized";
    for (size_t channel = 0; channel < debug->features.size(); ++channel)
    {
        queryTitle += "\n" + (channel < debug->channels.size() ? debug->channels[channel] :
            "[" + std::to_string(channel) + "]") + ": " + MotionNumber(debug->features[channel]) + " / " +
            (channel < debug->normalizedQuery.size() ? MotionNumber(debug->normalizedQuery[channel]) : "-");
    }
    auto* queryLabel = document->GetElementById("motion-matching-query");
    if (queryLabel->GetAttribute<std::string>("title", "") != queryTitle)
        queryLabel->SetAttribute("title", queryTitle);
    SetMarkup("motion-matching-decision", Text(debug->decisionReason + " | allowed " +
        std::to_string(debug->allowedFrameCount) + " | rejected " +
        std::to_string(debug->policyRejectedCount) + " | blend " + MotionNumber(debug->blendAlpha)));
    // 时间轴只读原生播放器；拖动期间不覆盖焦点控件，暂停后 seek 保持暂停。
    const bool hasPlayback = debug->playbackDuration > 0;
    SetDisabled("motion-timeline-play", !hasPlayback || debug->playbackPlaying);
    SetDisabled("motion-timeline-pause", !hasPlayback || !debug->playbackPlaying);
    SetDisabled("motion-timeline-seek", !hasPlayback);
    SetMarkup("motion-timeline-status", Text(MotionNumber(debug->playbackTime) + " / " +
        MotionNumber(debug->playbackDuration) + " s" + (debug->playbackPlaying ? " >" : " ||")));
    auto* motionTimeline = document->GetElementById("motion-timeline-seek");
    if (motionTimeline && motionTimeline != document->GetContext()->GetFocusElement())
    {
        motionTimeline->SetAttribute("max", MotionNumber(debug->playbackDuration));
        widgetDocument.SetControlValue("motion-timeline-seek", MotionNumber(debug->playbackTime));
    }
    auto* timelineStatus = document->GetElementById("motion-timeline-status");
    const std::string timelineTitle = debug->playbackClipId + "\n" +
        (debug->playbackPlaying ? "playing" : "paused");
    if (timelineStatus && timelineStatus->GetAttribute<std::string>("title", "") != timelineTitle)
        timelineStatus->SetAttribute("title", timelineTitle);
    // 查询回放：时间轴只选择历史快照；回放时 World 会暂停被聚焦播放器。
    const bool hasReplay = debug->replayFrames > 0;
    SetDisabled("motion-replay", !hasReplay);
    SetDisabled("motion-replay-seek", !hasReplay);
    auto* replayToggle = document->GetElementById("motion-replay");
    if (replayToggle && replayToggle->HasAttribute("checked") != debug->replayActive)
    {
        if (debug->replayActive) replayToggle->SetAttribute("checked", "");
        else replayToggle->RemoveAttribute("checked");
    }
    auto* replaySeek = document->GetElementById("motion-replay-seek");
    if (replaySeek && replaySeek != document->GetContext()->GetFocusElement())
    {
        replaySeek->SetAttribute("max", std::to_string(std::max(1, debug->replayFrames - 1)));
        widgetDocument.SetControlValue("motion-replay-seek", std::to_string(debug->replayFrame));
    }
    SetMarkup("motion-replay-status", Text(debug->replayActive ?
        "replay " + std::to_string(debug->replayFrame) + "/" + std::to_string(debug->replayFrames) :
        hasReplay ? "live " + std::to_string(debug->replayFrame) + "/" + std::to_string(debug->replayFrames) :
        "no history"));
    RefreshMotionMatchingQuery();
    RefreshMotionMatchingCandidate(0, debug->selected ? &*debug->selected : nullptr);
    RefreshMotionMatchingCandidate(1, debug->continuation ? &*debug->continuation : nullptr);
    for (size_t rank = 0; rank < 5; ++rank)
        RefreshMotionMatchingCandidate(rank + 2, rank < debug->topCandidates.size() ?
            &debug->topCandidates[rank] : nullptr);
}


} // namespace VL::EditorUi
