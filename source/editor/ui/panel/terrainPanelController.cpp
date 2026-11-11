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
using namespace Panel::Detail;

void MaterialInstanceAssetEditorPanel::RefreshTerrainSummary()
{
    if (document == nullptr) return;
    const EditorSceneActorSnapshot* actor = SelectedActor();
    const bool terrainSelected =
        actor != nullptr && actor->type == EditorSceneActorType::Terrain;

    // 空状态与编辑工作区只随选中地形身份切换；层内容由层库自己的签名缓存。
    const std::string signature = terrainSelected
        ? actor->identity + "|" + std::to_string(actor->worldGeneration)
        : "empty";
    if (signature != terrainSummarySignature)
    {
        terrainSummarySignature = signature;
        widgetDocument.SetProperty("terrain-empty", "display", terrainSelected ? "none" : "flex");
        // 编辑器工作区始终保留：没有地形时笔刷库仍可选择默认笔刷，
        // 层库只显示显式空状态，不伪造任何地形层。
        widgetDocument.SetProperty("terrain-summary", "display", "flex");
    }
    RefreshTerrainLayerLibrary();
    RefreshTerrainSurfacePanel(SelectedTerrainActor());
}

void MaterialInstanceAssetEditorPanel::RefreshTerrainSurfacePanel(
    const EditorSceneActorSnapshot* actor)
{
    if (document == nullptr) return;
    if (actor == nullptr || actor->terrainPlan == nullptr)
    {
        SetMarkup("terrain-surface-size", "");
        SetMarkup("terrain-surface-height-scale", "");
        SetMarkup("terrain-surface-layer-count", "0");
        SetMarkup("terrain-surface-chunk-count", "0");
        SetMarkup("terrain-surface-section-count", "0");
        return;
    }

    const TerrainAssetDesc& desc = actor->terrainPlan->desc;
    SetMarkup("terrain-surface-size",
        Text(TerrainNumber(desc.size.x()) + " x " + TerrainNumber(desc.size.y()) + " cm"));
    SetMarkup("terrain-surface-height-scale", Text(TerrainNumber(desc.heightScale) + " cm"));
    SetMarkup("terrain-surface-layer-count", Text(std::to_string(desc.layers.size())));

    uint32_t chunksPerSide = 0;
    if (desc.chunkQuads != 0)
        chunksPerSide = desc.gridResolution / desc.chunkQuads;
    SetMarkup("terrain-surface-chunk-count",
        Text(std::to_string(chunksPerSide) + " x " + std::to_string(chunksPerSide) +
            " (" + std::to_string(static_cast<uint64_t>(chunksPerSide) * chunksPerSide) + ")"));
    SetMarkup("terrain-surface-section-count", Text(std::to_string(actor->sections.size())));
}

void MaterialInstanceAssetEditorPanel::RefreshTerrainLayerLibrary()
{
    if (document == nullptr) return;
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    const TerrainBuildPlan* plan = actor == nullptr ? nullptr : actor->terrainPlan.get();
    const size_t layerCount = plan == nullptr ? 0 : plan->desc.layers.size();
    if (layerCount == 0)
    {
        terrainPaintLayer = 0;
        if (terrainPanelMode == TerrainPanelMode::Paint)
            terrainPanelMode = TerrainPanelMode::Layers;
    }
    else if (terrainPaintLayer >= layerCount)
    {
        terrainPaintLayer = static_cast<uint32_t>(layerCount - 1);
    }
    const bool selectedValid = layerCount != 0 && terrainPaintLayer < layerCount;

    auto* scroll = document->GetElementById("terrain-layer-scroll");
    std::ostringstream sourceKey;
    sourceKey << (actor == nullptr ? 0 : reinterpret_cast<uintptr_t>(plan)) << '|'
        << (actor == nullptr ? std::string() : actor->identity) << '|'
        << (actor == nullptr ? 0 : actor->worldGeneration) << '|' << layerCount;
    if (sourceKey.str() != terrainLayerSourceKey)
    {
        terrainLayerSourceKey = sourceKey.str();
        terrainLayerHoverActive = false;
        terrainLayerScrollResetPending = true;
        terrainLayerWindowDirty = true;
    }
    if (terrainLayerScrollResetPending)
    {
        if (scroll != nullptr) scroll->SetScrollTop(0.0f);
        terrainLayerScrollResetPending = false;
        terrainLayerWindowDirty = true;
    }

    const float scrollTop = scroll == nullptr ? 0.0f : scroll->GetScrollTop();
    float viewportWidth = 0.0f;
    float viewportHeight = 0.0f;
    if (scroll != nullptr)
    {
        const Rml::Vector2f content = scroll->GetBox().GetSize(Rml::BoxArea::Content);
        viewportWidth = content.x;
        viewportHeight = content.y;
    }
    const bool viewportMeasured = std::isfinite(viewportWidth) &&
        std::isfinite(viewportHeight) && viewportWidth > 0.0f && viewportHeight > 0.0f;
    if (!viewportMeasured)
    {
        // 面板隐藏时布局尺寸可能为 0；沿用上一次有效尺寸，避免窗口在隐藏/显示之间抖动。
        if (terrainLayerWindow.valid)
        {
            viewportWidth = terrainLayerWindow.viewportWidth;
            viewportHeight = terrainLayerWindow.viewportHeight;
        }
        else
        {
            viewportWidth = TerrainLayerFallbackViewportWidth;
            viewportHeight = TerrainLayerFallbackViewportHeight;
        }
    }
    if (terrainLayerWindow.valid)
    {
        if (std::abs(viewportWidth - terrainLayerWindow.viewportWidth) > 0.5f ||
            std::abs(viewportHeight - terrainLayerWindow.viewportHeight) > 0.5f ||
            std::abs(scrollTop - terrainLayerLastScrollTop) > 0.5f)
            terrainLayerWindowDirty = true;
    }

    size_t visibleCount = 0;
    if (plan != nullptr)
    {
        for (size_t index = 0; index < layerCount; ++index)
        {
            if (TerrainLayerMatchesSearch(plan->desc.layers[index], terrainLayerSearch))
                ++visibleCount;
        }
    }
    if (!terrainLayerWindow.valid || terrainLayerWindowDirty ||
        terrainLayerWindow.filteredCount != visibleCount ||
        terrainLayerWindow.listView != terrainLayerListView)
    {
        TerrainLayerWindowState window;
        window.valid = true;
        window.listView = terrainLayerListView;
        window.filteredCount = visibleCount;
        window.viewportWidth = viewportWidth;
        window.viewportHeight = viewportHeight;
        window.columns = terrainLayerListView ? 1 : TerrainLayerGridColumns(viewportWidth);
        const float rowHeight = terrainLayerListView ?
            TerrainLayerListRowHeight : TerrainLayerGridRowHeight;
        window.totalRows = terrainLayerListView ? static_cast<int>(visibleCount) :
            static_cast<int>((visibleCount + static_cast<size_t>(window.columns) - 1) /
                static_cast<size_t>(window.columns));
        if (visibleCount <= TerrainLayerVirtualizeThreshold)
        {
            // 小集合全部挂载，避免为十几张卡片引入 spacer 和额外的布局分支。
            window.firstRow = 0;
            window.endRow = window.totalRows;
            window.firstIndex = 0;
            window.endIndex = visibleCount;
            window.topSpacerHeight = 0.0f;
            window.bottomSpacerHeight = 0.0f;
        }
        else
        {
            constexpr int OverscanRows = 1;
            const int firstVisibleRow = std::max(0,
                static_cast<int>(std::floor(scrollTop / rowHeight)));
            const int lastVisibleRow = std::max(firstVisibleRow,
                static_cast<int>(std::ceil((scrollTop + viewportHeight) / rowHeight)) - 1);
            window.firstRow = std::max(0, firstVisibleRow - OverscanRows);
            window.endRow = std::min(window.totalRows, lastVisibleRow + 1 + OverscanRows);
            window.firstRow = std::min(window.firstRow,
                std::max(0, window.totalRows - 1));
            if (window.endRow <= window.firstRow)
                window.endRow = std::min(window.totalRows, window.firstRow + 1);
            window.firstIndex = std::min(visibleCount,
                static_cast<size_t>(window.firstRow) *
                    static_cast<size_t>(window.columns));
            window.endIndex = std::min(visibleCount,
                static_cast<size_t>(window.endRow) *
                    static_cast<size_t>(window.columns));
            window.topSpacerHeight = static_cast<float>(window.firstRow) * rowHeight;
            window.bottomSpacerHeight =
                static_cast<float>(window.totalRows - window.endRow) * rowHeight;
        }
        terrainLayerWindow = window;
    }
    terrainLayerWindowDirty = false;
    terrainLayerLastScrollTop = scrollTop;

    // 窗口范围进入签名：滚动到同一行内不重建，跨行才替换卡片 DOM。
    std::ostringstream signature;
    signature << sourceKey.str() << '|' << terrainPaintLayer << '|'
        << static_cast<int>(terrainPanelMode) << '|' << terrainLayerListView << '|'
        << terrainLayerSearch << '|' << terrainLayerWindow.columns << ','
        << terrainLayerWindow.firstRow << ',' << terrainLayerWindow.endRow << ','
        << static_cast<int>(std::lround(terrainLayerWindow.topSpacerHeight)) << ','
        << static_cast<int>(std::lround(terrainLayerWindow.bottomSpacerHeight));
    if (signature.str() == terrainLayerLibrarySignature) return;
    terrainLayerLibrarySignature = signature.str();

    widgetDocument.SetClass("terrain-layer-cards", "terrain-layer-grid", !terrainLayerListView);
    widgetDocument.SetClass("terrain-layer-cards", "terrain-layer-list", terrainLayerListView);
    widgetDocument.SetClass("terrain-layer-view-grid", "active", !terrainLayerListView);
    widgetDocument.SetClass("terrain-layer-view-list", "active", terrainLayerListView);
    HideTerrainLayerHoverPreview();

    std::string cards;
    size_t filteredIndex = 0;
    if (plan != nullptr)
    {
        for (size_t index = 0; index < layerCount; ++index)
        {
            const TerrainLayerDesc& layer = plan->desc.layers[index];
            if (!TerrainLayerMatchesSearch(layer, terrainLayerSearch)) continue;
            if (filteredIndex >= terrainLayerWindow.firstIndex &&
                filteredIndex < terrainLayerWindow.endIndex)
            {
                const VL::Editor::TerrainLayerPreviewPaths previews =
                    TerrainPreviewPaths(plan->desc, index);
                cards += TerrainLayerCardMarkup(layer, TerrainLayerColor(layer),
                    previews.spherePath, index, selectedValid && terrainPaintLayer == index,
                    terrainLayerListView);
            }
            ++filteredIndex;
        }
    }
    widgetDocument.SetProperty("terrain-layer-top-spacer", "height",
        Pixels(terrainLayerWindow.topSpacerHeight));
    widgetDocument.SetProperty("terrain-layer-top-spacer", "display",
        terrainLayerWindow.topSpacerHeight > 0.0f ? "block" : "none");
    widgetDocument.SetProperty("terrain-layer-bottom-spacer", "height",
        Pixels(terrainLayerWindow.bottomSpacerHeight));
    widgetDocument.SetProperty("terrain-layer-bottom-spacer", "display",
        terrainLayerWindow.bottomSpacerHeight > 0.0f ? "block" : "none");
    SetMarkup("terrain-layer-cards", std::move(cards));
    SetMarkup("terrain-layer-visible-count",
        Text(std::to_string(visibleCount) + " / " + std::to_string(layerCount) +
            (layerCount == 1 ? " layer" : " layers")));

    std::string emptyText;
    if (layerCount == 0)
        emptyText = actor == nullptr
            ? "Select a Landscape actor to browse layers."
            : "This terrain has no authored layers.";
    else if (visibleCount == 0)
        emptyText = "No layers match \"" + terrainLayerSearch + "\".";
    SetMarkup("terrain-layer-empty", emptyText.empty() ? std::string() : Text(emptyText));
    widgetDocument.SetProperty("terrain-layer-empty", "display",
        emptyText.empty() ? "none" : "block");

    std::string status;
    if (!selectedValid)
    {
        status = "No layer selected | 0 layers";
    }
    else
    {
        const TerrainLayerDesc& layer = plan->desc.layers[terrainPaintLayer];
        const std::string name = layer.name.empty() ? "Unnamed Layer" : layer.name;
        status = "Selected: " + name +
            " | source set " + std::to_string(layer.sourceSet) +
            " | uvScale " + TerrainNumber(layer.uvScale) +
            " | height " + TerrainLayerRange(layer.heightMin, layer.heightMax) +
            " | slope " + TerrainLayerRange(layer.slopeMin, layer.slopeMax) +
            " | " + std::to_string(layerCount) + " layers";
    }
    SetMarkup("terrain-layer-status", Text(status));
    // 选中只改卡片 class；重建后恢复原 scrollTop，不能把用户滚到的位置弹回顶部。
    if (scroll != nullptr)
        scroll->SetScrollTop(scrollTop);
    // 卡片重建会清掉旧 DOM 目标；若鼠标仍停在层库上，用稳定层号在同一帧恢复悬停平面。
    if (terrainPanelMode == TerrainPanelMode::Sculpt)
        terrainLayerHoverActive = false;
    if (terrainLayerHoverActive)
    {
        Rml::Element* hoveredCard = document->GetElementById(
            "terrain-layer-card-" + std::to_string(terrainLayerHoverLayer));
        if (hoveredCard != nullptr)
        {
            const Rml::Vector2f cardOffset =
                hoveredCard->GetAbsoluteOffset(Rml::BoxArea::Border);
            const Rml::Vector2f cardSize =
                hoveredCard->GetBox().GetSize(Rml::BoxArea::Border);
            const bool pointerStillOnCard =
                terrainLayerHoverScreenX >= cardOffset.x &&
                terrainLayerHoverScreenY >= cardOffset.y &&
                terrainLayerHoverScreenX < cardOffset.x + cardSize.x &&
                terrainLayerHoverScreenY < cardOffset.y + cardSize.y;
            if (pointerStillOnCard)
            {
                RefreshTerrainLayerHoverPreview(
                    {terrainLayerHoverScreenX, terrainLayerHoverScreenY}, hoveredCard);
            }
            else
            {
                terrainLayerHoverActive = false;
                HideTerrainLayerHoverPreview();
            }
        }
        else
        {
            terrainLayerHoverActive = false;
            HideTerrainLayerHoverPreview();
        }
    }
}

VL::Editor::TerrainLayerPreviewPaths MaterialInstanceAssetEditorPanel::TerrainPreviewPaths(
    const TerrainAssetDesc& desc, size_t layerIndex)
{
    if (terrainLayerPreviewProvider != nullptr)
        return terrainLayerPreviewProvider->Get(desc, layerIndex);
    return terrainLayerPreviewCache.Get(desc, layerIndex);
}

void MaterialInstanceAssetEditorPanel::HideTerrainLayerHoverPreview()
{
    if (document == nullptr) return;
    widgetDocument.SetProperty("terrain-layer-hover-preview", "display", "none");
}

void MaterialInstanceAssetEditorPanel::RefreshTerrainLayerHoverPreview(
    const Rml::Vector2f& screenPoint, const Rml::Element* target)
{
    if (document == nullptr)
    {
        terrainLayerHoverActive = false;
        HideTerrainLayerHoverPreview();
        return;
    }
    const Rml::Element* card = target;
    while (card != nullptr && card != document &&
        card->GetAttribute<std::string>("data-action", "") != "terrain-layer-select")
        card = card->GetParentNode();
    if (card == nullptr || card == document)
    {
        terrainLayerHoverActive = false;
        HideTerrainLayerHoverPreview();
        return;
    }
    const size_t index = card->GetAttribute<size_t>("data-index", 0);
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    if (actor == nullptr || actor->terrainPlan == nullptr ||
        index >= actor->terrainPlan->desc.layers.size())
    {
        terrainLayerHoverActive = false;
        HideTerrainLayerHoverPreview();
        return;
    }
    Rml::Element* page = document->GetElementById("terrain-page");
    Rml::Element* preview = document->GetElementById("terrain-layer-hover-preview");
    if (page == nullptr || preview == nullptr)
    {
        terrainLayerHoverActive = false;
        HideTerrainLayerHoverPreview();
        return;
    }
    const Rml::Vector2f offset = page->GetAbsoluteOffset(Rml::BoxArea::Content);
    const Rml::Vector2f size = page->GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x <= 0.0f || size.y <= 0.0f)
    {
        terrainLayerHoverActive = false;
        HideTerrainLayerHoverPreview();
        return;
    }
    terrainLayerHoverActive = true;
    terrainLayerHoverLayer = index;
    terrainLayerHoverScreenX = screenPoint.x;
    terrainLayerHoverScreenY = screenPoint.y;
    constexpr float PreviewWidth = 160.0f;
    constexpr float PreviewHeight = 112.0f;
    // 容器仍有 -8° 旋转；留出旋转后的外接盒边距，避免贴边时被面板裁掉。
    constexpr float PreviewMargin = 14.0f;
    const float maximumX = std::max(PreviewMargin,
        size.x - PreviewWidth - PreviewMargin);
    const float maximumY = std::max(PreviewMargin,
        size.y - PreviewHeight - PreviewMargin);
    const float x = std::clamp(
        screenPoint.x - offset.x + 16.0f, PreviewMargin, maximumX);
    const float y = std::clamp(
        screenPoint.y - offset.y - 18.0f, PreviewMargin, maximumY);
    const VL::Editor::TerrainLayerPreviewPaths previews =
        TerrainPreviewPaths(actor->terrainPlan->desc, index);
    Rml::Element* image = document->GetElementById("terrain-layer-hover-image");
    const bool hasPlaneImage = image != nullptr && !previews.planePath.empty();
    if (image != nullptr)
    {
        if (hasPlaneImage)
        {
            if (terrainLayerHoverImageSource != previews.planePath)
            {
                image->SetAttribute("src", previews.planePath);
                terrainLayerHoverImageSource = previews.planePath;
            }
            widgetDocument.SetProperty("terrain-layer-hover-image", "display", "block");
        }
        else
        {
            widgetDocument.SetProperty("terrain-layer-hover-image", "display", "none");
        }
    }
    widgetDocument.SetProperty("terrain-layer-hover-preview", "background-color",
        TerrainLayerColor(actor->terrainPlan->desc.layers[index]));
    widgetDocument.SetProperty("terrain-layer-hover-preview", "left", Pixels(x));
    widgetDocument.SetProperty("terrain-layer-hover-preview", "top", Pixels(y));
    widgetDocument.SetProperty("terrain-layer-hover-preview", "display", "block");
}

bool MaterialInstanceAssetEditorPanel::IsTerrainSculptActive() const
{
    return landscapeToolActive && document != nullptr &&
        !IsAssetEditorVisible() && !HasModal() &&
        runtimeView.scripts.state == ScriptPlayState::Editing &&
        !runtimeView.offlineMode;
}

const EditorSceneActorSnapshot* MaterialInstanceAssetEditorPanel::SelectedTerrainActor() const
{
    const EditorSceneActorSnapshot* actor = SelectedActor();
    return actor != nullptr && actor->type == EditorSceneActorType::Terrain &&
        actor->terrainPlan != nullptr ? actor : nullptr;
}

bool MaterialInstanceAssetEditorPanel::IsTerrainHistoryContext() const
{
    // 地形历史优先：只有 Landscape 工具激活、选中地形且没有进行中的 stroke 时才接管 Undo/Redo。
    return landscapeToolActive && SelectedTerrainActor() != nullptr &&
        IsTerrainSculptActive() && !terrainBrushStrokeActive;
}

float MaterialInstanceAssetEditorPanel::ActiveTerrainBrushRadiusCm() const
{
    return landscapePaintMode ? terrainPaintBrush.radiusCm : terrainSculptBrush.radiusCm;
}

float MaterialInstanceAssetEditorPanel::TerrainBrushRadiusUv() const
{
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    if (actor == nullptr) return 0.0f;
    const float worldSpan = actor->terrainPlan->desc.size.x() *
        std::abs(actor->scale.x());
    if (!std::isfinite(worldSpan) || worldSpan < 1.0e-3f) return 0.0f;
    return std::clamp(ActiveTerrainBrushRadiusCm() / worldSpan, 1.0e-5f, 1.0f);
}

void MaterialInstanceAssetEditorPanel::RefreshTerrainBrushRing()
{
    if (document == nullptr) return;
    const int shape = landscapePaintMode ? terrainPaintBrush.shape : terrainSculptBrush.shape;
    widgetDocument.SetClass("viewport-brush-ring", "square",
        shape == TerrainBrushShapeIndex(Editor::BrushShape::Square));
    const bool visible = TerrainBrushRingVisible() && IsTerrainSculptActive();
    widgetDocument.SetProperty("viewport-brush-ring", "display", visible ? "block" : "none");
    // RmlUi 的 Element::visible 只在 Display/Visibility 属性变化时刷新；
    // 同时写 visibility 让隐藏状态下一帧一定生效，避免透明环继续吃掉视口事件。
    widgetDocument.SetProperty("viewport-brush-ring", "visibility", visible ? "visible" : "hidden");
    if (!visible) return;
    const float radius = std::max(8.0f, terrainBrushRingRadiusPixels);
    widgetDocument.SetProperty("viewport-brush-ring", "left",
        Pixels(terrainBrushPixel.x() - radius));
    widgetDocument.SetProperty("viewport-brush-ring", "top",
        Pixels(terrainBrushPixel.y() - radius));
    widgetDocument.SetProperty("viewport-brush-ring", "width", Pixels(radius * 2.0f));
    widgetDocument.SetProperty("viewport-brush-ring", "height", Pixels(radius * 2.0f));
}

bool MaterialInstanceAssetEditorPanel::ActivateTerrainPaintMode()
{
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    if (actor == nullptr || actor->terrainPlan == nullptr ||
        actor->terrainPlan->desc.layers.empty())
    {
        localStatus = "Paint requires a terrain with at least one layer.";
        return false;
    }
    CancelTerrainBrushStroke();
    ClearTerrainBrushHit();
    landscapePaintMode = true;
    landscapeToolActive = true;
    viewportTransformTool = "select";
    terrainPanelMode = TerrainPanelMode::Paint;
    RefreshViewportToolbar();
    return true;
}

void MaterialInstanceAssetEditorPanel::RefreshTerrainBrushLibrary()
{
    // 切换领域会重建分类下拉；RmlUi 在迁移 option 时会同步派发 change，
    // 必须把这次程序化更新挡在宿主事件之外，否则 Build 会重入并销毁正在遍历的 option。
    WidgetRefreshScope refresh(refreshingControls);
    // Layers 只切换视图；笔刷列隐藏时仍按最近一次 Sculpt/Paint 工具决定目录，
    // 这样从 Layers 返回时选中项和标题不会跳回另一套领域。
    const bool paintDomain = terrainPanelMode == TerrainPanelMode::Paint ||
        (terrainPanelMode == TerrainPanelMode::Layers && landscapePaintMode);
    const int catalogDomain = paintDomain ? 1 : 0;
    if (catalogDomain != brushLibraryCatalogDomain)
    {
        std::vector<UiWidgets::BrushLibraryEntry> entries;
        if (paintDomain)
        {
            for (const Editor::PaintBrushPreset& preset : Editor::PaintBrushProfiles())
            {
                const Editor::BrushProfile& profile = preset.profile;
                UiWidgets::BrushLibraryEntry entry;
                entry.id = Editor::BrushProfileId(profile);
                entry.name = profile.name;
                entry.category = profile.category;
                entry.shape = TerrainBrushShapeIndex(profile.shape);
                entry.falloff = TerrainBrushFalloffIndex(profile.falloff);
                entry.radiusCm = profile.radiusCm;
                entry.strength = preset.opacity;
                entries.push_back(std::move(entry));
            }
        }
        else
        {
            for (const Editor::BrushProfile& profile : Editor::SculptBrushProfiles())
            {
                UiWidgets::BrushLibraryEntry entry;
                entry.id = Editor::BrushProfileId(profile);
                entry.name = profile.name;
                entry.category = profile.category;
                entry.shape = TerrainBrushShapeIndex(profile.shape);
                entry.falloff = TerrainBrushFalloffIndex(profile.falloff);
                entry.radiusCm = profile.radiusCm;
                entry.strength = profile.strength;
                entries.push_back(std::move(entry));
            }
        }
        brushLibrary.SetEntries(std::move(entries));
        brushLibraryCatalogDomain = catalogDomain;
    }
    SetMarkup("terrain-brush-library-title",
        paintDomain ? "Paint Brushes" : "Sculpt Brushes");
    brushLibrary.SetSelectedId(paintDomain
        ? terrainPaintBrush.presetId : terrainSculptBrush.presetId);
    brushLibrary.Build();
}

void MaterialInstanceAssetEditorPanel::ApplyTerrainBrushPreset(const std::string& presetId)
{
    if (presetId.empty()) return;
    const bool paintDomain = terrainPanelMode == TerrainPanelMode::Paint ||
        (terrainPanelMode == TerrainPanelMode::Layers && landscapePaintMode);
    if (paintDomain)
    {
        for (const Editor::PaintBrushPreset& preset : Editor::PaintBrushProfiles())
        {
            if (Editor::BrushProfileId(preset.profile) != presetId) continue;
            terrainPaintBrush.radiusCm = preset.profile.radiusCm;
            terrainPaintBrush.hardness = preset.hardness;
            terrainPaintBrush.opacity = preset.opacity;
            terrainPaintBrush.flow = preset.flow;
            terrainPaintBrush.noise = preset.profile.noise;
            terrainPaintBrush.noiseSeed = preset.profile.noiseSeed;
            terrainPaintBrush.shape = TerrainBrushShapeIndex(preset.profile.shape);
            terrainPaintBrush.presetId = presetId;
            RefreshTerrainBrushUi();
            return;
        }
        return;
    }

    for (const Editor::BrushProfile& profile : Editor::SculptBrushProfiles())
    {
        if (Editor::BrushProfileId(profile) != presetId) continue;
        terrainSculptBrush.radiusCm = profile.radiusCm;
        terrainSculptBrush.strength = profile.strength;
        terrainSculptBrush.falloff = TerrainBrushFalloffValue(profile.falloff);
        terrainSculptBrush.shape = TerrainBrushShapeIndex(profile.shape);
        terrainSculptBrush.presetId = presetId;
        RefreshTerrainBrushUi();
        return;
    }
}

void MaterialInstanceAssetEditorPanel::SynchronizeSculptBrushPresetId()
{
    for (const Editor::BrushProfile& profile : Editor::SculptBrushProfiles())
    {
        if (!TerrainSculptBrushMatches(profile, terrainSculptBrush)) continue;
        terrainSculptBrush.presetId = Editor::BrushProfileId(profile);
        return;
    }
    // 手动编辑后没有精确匹配的预设：保留用户数值，只取消预设高亮。
    terrainSculptBrush.presetId.clear();
}

void MaterialInstanceAssetEditorPanel::SynchronizePaintBrushPresetId()
{
    for (const Editor::PaintBrushPreset& preset : Editor::PaintBrushProfiles())
    {
        if (!TerrainPaintBrushMatches(preset, terrainPaintBrush)) continue;
        terrainPaintBrush.presetId = Editor::BrushProfileId(preset.profile);
        return;
    }
    terrainPaintBrush.presetId.clear();
}

void MaterialInstanceAssetEditorPanel::RefreshTerrainBrushUi()
{
    if (document == nullptr) return;
    SetViewportToolbarClass("viewport-tool-landscape", landscapeToolActive);
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    const size_t layerCount = actor != nullptr && actor->terrainPlan
        ? actor->terrainPlan->desc.layers.size() : 0;
    const bool hasLayers = layerCount != 0;
    SetViewportToolbarClass("viewport-landscape-sculpt",
        landscapeToolActive && !landscapePaintMode);
    SetViewportToolbarClass("viewport-landscape-paint",
        landscapeToolActive && landscapePaintMode);
    SetDisabled("viewport-landscape-paint", !hasLayers);
    SetDisabled("terrain-mode-sculpt", actor == nullptr);
    SetDisabled("terrain-mode-paint", actor == nullptr || !hasLayers);
    if (!hasLayers)
    {
        landscapePaintMode = false;
        if (terrainPanelMode == TerrainPanelMode::Paint)
            terrainPanelMode = TerrainPanelMode::Layers;
    }
    if (actor == nullptr && terrainPanelMode != TerrainPanelMode::Layers)
        terrainPanelMode = TerrainPanelMode::Layers;
    if (terrainPaintLayer >= layerCount)
        terrainPaintLayer = layerCount == 0 ? 0 : static_cast<uint32_t>(layerCount - 1);

    const bool sculptMode = terrainPanelMode == TerrainPanelMode::Sculpt;
    const bool paintMode = terrainPanelMode == TerrainPanelMode::Paint;
    const bool layersMode = terrainPanelMode == TerrainPanelMode::Layers;
    // 两套领域共用同一个笔刷列：Sculpt/Paint 各自显示自己的目录与参数，
    // Layers 只浏览层，不保留会被误认为可落笔的笔刷控件。
    const bool brushVisible = actor != nullptr && !layersMode;
    widgetDocument.SetProperty("terrain-brush-column", "display",
        brushVisible ? "flex" : "none");
    widgetDocument.SetProperty("viewport-brush-settings", "display",
        brushVisible ? "flex" : "none");
    widgetDocument.SetProperty("terrain-sculpt-settings", "display",
        sculptMode && brushVisible ? "block" : "none");
    for (int mode = 0; mode < 3; ++mode)
        widgetDocument.SetClass("viewport-brush-mode-" + std::to_string(mode),
            "active", sculptMode && brushVisible && terrainSculptMode == mode);
    widgetDocument.SetProperty("viewport-landscape-controls", "display",
        landscapeToolActive ? "flex" : "none");
    widgetDocument.SetProperty("viewport-paint-settings", "display",
        paintMode && landscapePaintMode && hasLayers && brushVisible ? "flex" : "none");
    // Sculpt 右侧只看地形几何摘要；层卡属于 Paint/Layers，不能在 Sculpt 中抢空间。
    widgetDocument.SetProperty("terrain-layer-library", "display",
        sculptMode ? "none" : "flex");
    widgetDocument.SetProperty("terrain-surface-panel", "display",
        sculptMode && actor != nullptr ? "flex" : "none");
    widgetDocument.SetProperty("terrain-panel-filter-row", "display",
        sculptMode ? "none" : "flex");
    if (sculptMode) HideTerrainLayerHoverPreview();
    widgetDocument.SetClass("terrain-mode-sculpt", "active",
        sculptMode);
    widgetDocument.SetClass("terrain-mode-paint", "active",
        paintMode);
    widgetDocument.SetClass("terrain-mode-layers", "active",
        layersMode);
    SetViewportToolbarClass("viewport-paint-mode", !landscapePaintErase);
    SetViewportToolbarClass("viewport-paint-erase", landscapePaintErase);
    SetControlValue("viewport-brush-radius", Number(terrainSculptBrush.radiusCm));
    SetControlValue("viewport-brush-strength", Number(terrainSculptBrush.strength));
    SetControlValue("viewport-brush-falloff", Number(terrainSculptBrush.falloff));
    SetControlValue("terrain-paint-radius", Number(terrainPaintBrush.radiusCm));
    SetControlValue("terrain-paint-hardness", Number(terrainPaintBrush.hardness));
    SetControlValue("terrain-paint-opacity", Number(terrainPaintBrush.opacity));
    SetControlValue("terrain-paint-flow", Number(terrainPaintBrush.flow));
    SetMarkup("viewport-landscape-write",
        landscapePaintMode ? "Write Paint" : "Write Heightmap");
    RefreshTerrainBrushLibrary();
    RefreshTerrainBrushRing();
    RefreshTerrainLayerLibrary();
}

void MaterialInstanceAssetEditorPanel::ClearTerrainBrushHit()
{
    if (!terrainBrushHit && terrainBrushPlacementId.empty()) return;
    terrainBrushHit = false;
    terrainBrushUv = Eigen::Vector2f::Zero();
    terrainBrushPixel = Eigen::Vector2f::Zero();
    terrainBrushWorldPoint = Eigen::Vector3f::Zero();
    terrainBrushRingRadiusPixels = 0.0f;
    terrainBrushPlacementId.clear();
    RefreshTerrainBrushRing();
}

void MaterialInstanceAssetEditorPanel::UpdateTerrainBrushHit(float mouseX, float mouseY)
{
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    if (!IsTerrainSculptActive() || actor == nullptr)
    {
        ClearTerrainBrushHit();
        return;
    }
    const EditorSceneCameraActor* camera = nullptr;
    for (const EditorSceneActorSnapshot& candidate : OutlinerActors())
        if (candidate.type == EditorSceneActorType::Camera)
        {
            camera = candidate.hasViewportCamera ? &candidate.viewportCamera : &candidate.camera;
            break;
        }
    auto* image = document->GetElementById("editor-viewport-image");
    if (camera == nullptr || image == nullptr || !image->IsVisible(true))
    {
        ClearTerrainBrushHit();
        return;
    }
    const auto offset = image->GetAbsoluteOffset(Rml::BoxArea::Content);
    const auto size = image->GetBox().GetSize(Rml::BoxArea::Content);
    const Eigen::Vector2f pixel(mouseX - offset.x, mouseY - offset.y);
    if (size.x <= 0 || size.y <= 0 || pixel.x() < 0 || pixel.y() < 0 ||
        pixel.x() >= size.x || pixel.y() >= size.y)
    {
        ClearTerrainBrushHit();
        return;
    }
    TerrainBrushSurfaceHit hit;
    try
    {
        hit = PickTerrainBrushSurface(*camera,
            size.x, size.y, pixel, *actor->terrainPlan, actor->position,
            actor->rotation, actor->scale);
    }
    catch (const std::exception&)
    {
        ClearTerrainBrushHit();
        return;
    }
    if (!hit.valid)
    {
        ClearTerrainBrushHit();
        return;
    }
    terrainBrushHit = true;
    terrainBrushUv = hit.uv;
    terrainBrushPixel = pixel;
    terrainBrushWorldPoint = hit.worldPosition;
    terrainBrushGeneration = actor->worldGeneration;
    terrainBrushPlacementId = actor->identity;
    terrainBrushRingRadiusPixels = TerrainBrushRadiusPixels(*camera,
        size.x, size.y, hit.worldPosition, ActiveTerrainBrushRadiusCm(), actor->scale);
    RefreshTerrainBrushRing();
}

void MaterialInstanceAssetEditorPanel::SendTerrainBrushAction(
    TerrainBrushOperation operation, bool includeBrush)
{
    if (runtimeActionSink == nullptr) return;
    const bool strokeOperation = operation == TerrainBrushOperation::EndStroke ||
        operation == TerrainBrushOperation::CancelStroke;
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    // stroke 的结束/取消只依赖 Begin 时保存的 generation/placement：
    // 中途切到普通 Mesh 或清空选择也必须能把 engine 的 active stroke 收尾。
    if (strokeOperation)
    {
        if (terrainBrushGeneration == 0 || terrainBrushPlacementId.empty()) return;
    }
    else if (actor == nullptr)
    {
        return;
    }
    if ((operation == TerrainBrushOperation::BeginStroke ||
         operation == TerrainBrushOperation::ApplyBrush) && !terrainBrushHit)
        return;
    UiAction action;
    action.type = UiActionType::TerrainEdit;
    auto& brush = action.terrainBrush;
    brush.operation = operation;
    brush.worldGeneration = strokeOperation ? terrainBrushGeneration : actor->worldGeneration;
    brush.placementId = strokeOperation ? terrainBrushPlacementId : actor->identity;
    if (includeBrush)
    {
        brush.u = terrainBrushUv.x();
        brush.v = terrainBrushUv.y();
        brush.strength = terrainSculptBrush.strength;
        brush.falloff = terrainSculptBrush.falloff;
        brush.mode = terrainSculptMode;
        brush.shape = terrainSculptBrush.shape;
    }
    brush.radiusUv = strokeOperation ? 0.0f : TerrainBrushRadiusUv();
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

void MaterialInstanceAssetEditorPanel::EndTerrainBrushStroke()
{
    if (!terrainBrushStrokeActive) return;
    if (landscapePaintMode) SendTerrainPaintAction(TerrainBrushOperation::EndStroke, true);
    else SendTerrainBrushAction(TerrainBrushOperation::EndStroke, true);
    terrainBrushStrokeActive = false;
    RefreshTerrainBrushUi();
}

void MaterialInstanceAssetEditorPanel::SendTerrainPaintAction(
    TerrainBrushOperation operation, bool includeBrush)
{
    if (runtimeActionSink == nullptr) return;
    const bool strokeOperation = operation == TerrainBrushOperation::EndStroke ||
        operation == TerrainBrushOperation::CancelStroke;
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    if (strokeOperation)
    {
        if (terrainBrushGeneration == 0 || terrainBrushPlacementId.empty()) return;
    }
    else if (actor == nullptr || actor->terrainPlan == nullptr ||
        actor->terrainPlan->desc.layers.empty())
    {
        return;
    }
    if ((operation == TerrainBrushOperation::BeginStroke ||
         operation == TerrainBrushOperation::ApplyBrush) && !terrainBrushHit)
        return;

    UiAction action;
    action.type = UiActionType::TerrainPaint;
    auto& paint = action.terrainPaint;
    paint.operation = operation;
    paint.worldGeneration = strokeOperation ? terrainBrushGeneration : actor->worldGeneration;
    paint.placementId = strokeOperation ? terrainBrushPlacementId : actor->identity;
    paint.layerIndex = terrainPaintLayer;
    paint.erase = landscapePaintErase;
    if (includeBrush)
    {
        paint.u = terrainBrushUv.x();
        paint.v = terrainBrushUv.y();
        paint.radiusUv = TerrainBrushRadiusUv();
        // Paint 的 opacity/flow/noise 各自表达独立语义；strength 固定为 1，
        // 避免旧的 opacity*flow 中间映射再次压暗落笔。hardness 仍映射为 falloff。
        paint.strength = 1.0f;
        paint.falloff = std::clamp(1.0f - terrainPaintBrush.hardness, 0.0f, 1.0f);
        paint.opacity = std::clamp(terrainPaintBrush.opacity, 0.0f, 1.0f);
        paint.flow = std::clamp(terrainPaintBrush.flow, 0.0f, 1.0f);
        paint.noise = std::clamp(terrainPaintBrush.noise, 0.0f, 1.0f);
        paint.noiseSeed = terrainPaintBrush.noiseSeed;
        paint.shape = terrainPaintBrush.shape;
    }
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

void MaterialInstanceAssetEditorPanel::CancelTerrainBrushStroke()
{
    if (!terrainBrushStrokeActive) return;
    if (landscapePaintMode) SendTerrainPaintAction(TerrainBrushOperation::CancelStroke, true);
    else SendTerrainBrushAction(TerrainBrushOperation::CancelStroke, true);
    terrainBrushStrokeActive = false;
    RefreshTerrainBrushUi();
}

void MaterialInstanceAssetEditorPanel::SynchronizeTerrainBrushSelection()
{
    if (!landscapeToolActive)
    {
        CancelTerrainBrushStroke();
        ClearTerrainBrushHit();
        return;
    }
    if (!IsTerrainSculptActive())
    {
        CancelTerrainBrushStroke();
        ClearTerrainBrushHit();
        return;
    }
    const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
    if (terrainBrushStrokeActive && actor != nullptr &&
        actor->worldGeneration != terrainBrushGeneration)
    {
        // World 换代后旧代数命令会被执行端拒绝；这里只丢弃本地手势，
        // 不再发一笔注定失败、只会刷诊断的 CancelStroke。
        terrainBrushStrokeActive = false;
        ClearTerrainBrushHit();
        return;
    }
    const bool differentActor = actor == nullptr ||
        (terrainBrushGeneration != 0 && actor->worldGeneration != terrainBrushGeneration) ||
        (!terrainBrushPlacementId.empty() && actor->identity != terrainBrushPlacementId);
    if (terrainBrushStrokeActive && differentActor)
    {
        CancelTerrainBrushStroke();
        ClearTerrainBrushHit();
        return;
    }
    if (!terrainBrushStrokeActive && terrainBrushHit && differentActor)
        ClearTerrainBrushHit();
}


} // namespace VL::EditorUi
