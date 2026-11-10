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

#include "editor/ui/panel/hairPanelHelpers.h"

namespace VL::EditorUi
{
using namespace Panel::Detail;
using namespace HairPanelDetail;

const EditorSceneMeshSectionSnapshot* MaterialInstanceAssetEditorPanel::HairBrushSection() const
{
    if (!IsActiveHairPreview()) return nullptr;
    const uint64_t generation = CurrentModelPreviewGeneration();
    if (generation == 0) return nullptr;
    for (const EditorSceneActorSnapshot& actor : OutlinerActors())
    {
        if (actor.worldGeneration != generation) continue;
        for (const EditorSceneMeshSectionSnapshot& section : actor.sections)
            if (section.tressfxRootCloud && section.tressfxRootCloud->RootCount() != 0)
                return &section;
    }
    return nullptr;
}

int32_t MaterialInstanceAssetEditorPanel::SavedHairGroupIndex(
    const EditorSceneMeshSectionSnapshot* section, int draftIndex) const
{
    if (section == nullptr) return -1;
    if (draftIndex < 0) return 0;
    const auto& groups = hairGroups.Groups();
    if (draftIndex >= static_cast<int>(groups.size())) return -1;
    // 这里只用于读取已保存/placement 解析后的 asset default。预览 GPU 组号
    // 已经改为草稿下标；不要再用该方法门控 Paint 可用性。
    const std::string& name = groups[static_cast<size_t>(draftIndex)].originName;
    if (name.empty()) return -1;
    if (!section->tressfxGroupBinding)
        return -1;
    const auto& savedGroupNames = section->tressfxGroupBinding->groupNames;
    for (size_t index = 0; index < savedGroupNames.size(); ++index)
        if (savedGroupNames[index] == name)
            return static_cast<int32_t>(index) + 1;
    return -1;
}

bool MaterialInstanceAssetEditorPanel::HairBrushReady() const
{
    return IsActiveHairPreview() && hairGroupsLoaded &&
        hairGroups.GuideCount() != 0 &&
        hairGroupSelected >= 0 &&
        hairGroupSelected < static_cast<int>(hairGroups.Groups().size()) &&
        CurrentModelPreviewGeneration() != 0 && HairBrushSection() != nullptr &&
        HairDraftBindingReady();
}

bool MaterialInstanceAssetEditorPanel::EnsureHairPaintSession(
    const EditorSceneMeshSectionSnapshot* section)
{
    if (section == nullptr || !section->tressfxRootCloud)
        return false;
    const uint64_t generation = CurrentModelPreviewGeneration();
    if (!EnsureHairDraftBinding())
        return false;
    if (hairPaint.HasSession() && generation == hairPaintSessionGeneration &&
        hairPaintSessionBinding == hairPaintDraftBinding)
        return true;
    // 冲突分析使用当前草稿 binding：Replace Existing=OFF 时必须看到手填/撤销后的
    // range，否则会把已属于其他组的 guide 当成 Default。几何选择仍用 root cloud。
    hairPaint.BeginSession(hairPaintDraftBinding);
    hairPaintSessionGeneration = generation;
    hairPaintSessionBinding = hairPaintDraftBinding;
    return true;
}

void MaterialInstanceAssetEditorPanel::ResetHairPaintSession()
{
    hairPaint.ClearSession();
    hairPaintSessionGeneration = 0;
    hairPaintSessionBinding.reset();
}

std::string MaterialInstanceAssetEditorPanel::HairBrushConflictText(
    const std::vector<VL::Hair::GroupPaintConflict>& conflicts) const
{
    std::string text;
    for (size_t index = 0; index < conflicts.size(); ++index)
    {
        if (index != 0) text += "; ";
        text += std::to_string(conflicts[index].guideCount) +
            " guides belong to \"" + conflicts[index].groupName + "\"";
    }
    return text;
}

void MaterialInstanceAssetEditorPanel::ToggleHairGroupPaint(bool active)
{
    if (active && !HairBrushReady())
    {
        hairBrushStatus = "Select a named group with an available hair preview first; Default cannot be painted.";
        return;
    }
    if (!active)
    {
        CancelHairBrushStroke(true);
        return;
    }
    hairGroupPaintActive = true;
    // 新一次 Paint Groups 会话从零开始；Ctrl+Z 只覆盖本次会话的提交。
    hairPaintUndo.clear();
    ResetHairPaintSession();
    hairBrushStrokeActive = false;
    hairBrushHits.clear();
    hairBrushConflicts.clear();
    hairBrushStamp = {};
    hairLassoDragging = false;
    hairLassoScreenPoints.clear();
    hairBrushPreviewActive = false;
    hairBrushRingSent = false;
    hairBrushStatus = "Painting ready. Left drag adds, Shift adds, Alt removes; "
        "Alt+wheel adjusts radius, Enter or release commits, Esc exits.";
}

bool MaterialInstanceAssetEditorPanel::PrepareHairBrushInputs(
    VL::Hair::BrushViewport& viewport, VL::Hair::BrushCamera& camera,
    VL::Hair::BrushSettings& settings) const
{
    if (document == nullptr) return false;
    Rml::Element* image = document->GetElementById("resource-preview-image");
    if (image == nullptr) return false;
    const Rml::Vector2f imageOffset = image->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f imageSize = image->GetBox().GetSize(Rml::BoxArea::Border);
    if (imageSize.x <= 0.0f || imageSize.y <= 0.0f) return false;

    viewport.x = static_cast<int32_t>(std::lround(imageOffset.x));
    viewport.y = static_cast<int32_t>(std::lround(imageOffset.y));
    viewport.width = static_cast<uint32_t>(std::lround(imageSize.x));
    viewport.height = static_cast<uint32_t>(std::lround(imageSize.y));

    camera.position = Eigen::Vector3f(runtimeView.motionCamera.position[0],
        runtimeView.motionCamera.position[1], runtimeView.motionCamera.position[2]);
    camera.forward = Eigen::Vector3f(runtimeView.motionCamera.forward[0],
        runtimeView.motionCamera.forward[1], runtimeView.motionCamera.forward[2]);
    camera.right = Eigen::Vector3f(runtimeView.motionCamera.right[0],
        runtimeView.motionCamera.right[1], runtimeView.motionCamera.right[2]);
    camera.up = Eigen::Vector3f(runtimeView.motionCamera.up[0],
        runtimeView.motionCamera.up[1], runtimeView.motionCamera.up[2]);
    camera.horizontalFovDegrees = runtimeView.motionCamera.horizontalFovDegrees;
    camera.clipNear = runtimeView.motionCamera.clipNear;
    camera.clipFar = runtimeView.motionCamera.clipFar;

    settings.radiusPixels = hairBrushRadiusPx;
    settings.maxNormalAngleDegrees = 60.0f;
    settings.planeThicknessWorld = 0.0f;
    settings.planeSearchRadiusWorld = 0.0f;
    return true;
}

bool MaterialInstanceAssetEditorPanel::SampleHairBrushStamp(float screenX, float screenY)
{
    hairBrushCursorX = screenX;
    hairBrushCursorY = screenY;
    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    if (section == nullptr || !section->tressfxRootCloud)
    {
        hairBrushStamp = {};
        return false;
    }
    VL::Hair::BrushViewport viewport;
    VL::Hair::BrushCamera camera;
    VL::Hair::BrushSettings settings;
    if (!PrepareHairBrushInputs(viewport, camera, settings))
    {
        hairBrushStamp = {};
        return false;
    }
    hairBrushStamp = VL::Hair::SurfaceBrush().Sample(
        Eigen::Vector2f(screenX, screenY), viewport, camera,
        *section->tressfxRootCloud, section->tressfxRootLocalToWorld, settings);
    return hairBrushStamp.valid;
}

void MaterialInstanceAssetEditorPanel::UpdateHairBrushSample(
    float screenX, float screenY, bool applyToStroke)
{
    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    if (section == nullptr || !hairGroupsLoaded || hairGroupSelected < 0)
    {
        hairBrushStamp = {};
        hairBrushHits.clear();
        hairBrushConflicts.clear();
        hairBrushStatus = "No hair root cloud is available for this preview.";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    if (!EnsureHairPaintSession(section))
    {
        hairBrushStamp = {};
        hairBrushHits.clear();
        hairBrushConflicts.clear();
        hairBrushStatus = "Hair group binding is unavailable; reload the asset.";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    VL::Hair::BrushViewport viewport;
    VL::Hair::BrushCamera camera;
    VL::Hair::BrushSettings settings;
    if (!PrepareHairBrushInputs(viewport, camera, settings) ||
        !SampleHairBrushStamp(screenX, screenY))
    {
        hairBrushHits.clear();
        hairBrushConflicts.clear();
        hairBrushStatus = "Painting -> " +
            hairGroups.Groups()[static_cast<size_t>(hairGroupSelected)].name +
            " | no root surface under cursor";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    hairBrushHits.clear();
    hairBrushConflicts.clear();
    hairBrushHits = VL::Hair::RootCloudSelector().Select(
        *section->tressfxRootCloud, hairBrushStamp,
        section->tressfxRootLocalToWorld, settings);

    if (!HairDraftBindingReady())
    {
        hairBrushStatus = "The Groups draft is still syncing to the preview; wait a frame and retry.";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    // 草稿 binding 已同步，GPU 组号与控制器统一使用草稿下标。
    const uint32_t targetGroupId = static_cast<uint32_t>(hairGroupSelected) + 1;
    const VL::Hair::GroupPaintMode previewMode = applyToStroke
        ? hairBrushStrokeMode : hairBrushMode;
    const bool replace = hairBrushReplaceExisting;
    // 会话共享当前草稿 binding；hover 不复制 guide 映射，也不改会话。
    const VL::Hair::GroupPaintSummary summary = hairPaint.AnalyzeHits(
        hairBrushHits, targetGroupId, previewMode, replace);
    hairBrushConflicts = summary.conflicts;

    std::ostringstream status;
    status << "Painting -> "
           << hairGroups.Groups()[static_cast<size_t>(hairGroupSelected)].name
           << " | +" << summary.eligibleGuides << " guides in brush"
           << " | r=" << std::fixed << std::setprecision(1)
           << hairBrushStamp.radiusWorld << " cm";
    if (hairBrushStamp.fallback)
        status << " | plane fallback; screen ring fallback (PCA / -viewDir / nearest roots)";
    const std::string conflicts = HairBrushConflictText(summary.conflicts);
    if (!conflicts.empty()) status << " | " << conflicts;
    hairBrushStatus = status.str();

    if (applyToStroke)
        hairPaint.AddHits(hairBrushHits);
    // hover 也必须投递圆环；ghost 只在笔画期间有权重。
    SubmitTressfxBrushPreview();
    UpdateHairBrushOverlay();
}

void MaterialInstanceAssetEditorPanel::BeginHairBrushStroke(
    float screenX, float screenY, bool shift, bool alt)
{
    if (!HairBrushReady())
    {
        hairBrushStatus = "The selected group is not paintable.";
        return;
    }
    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    if (section == nullptr || !HairDraftBindingReady())
    {
        hairBrushStatus = "The Groups draft is still syncing to the preview; wait a frame and retry.";
        return;
    }
    const uint32_t targetGroupId = static_cast<uint32_t>(hairGroupSelected) + 1;
    hairBrushStrokeMode = alt ? VL::Hair::GroupPaintMode::Remove :
        (shift ? VL::Hair::GroupPaintMode::Add : hairBrushMode);
    if (!EnsureHairPaintSession(section))
    {
        hairBrushStatus = "Hair group binding is unavailable; reload the asset.";
        return;
    }
    hairPaint.BeginStroke(targetGroupId, hairBrushStrokeMode, hairBrushReplaceExisting);
    hairBrushStrokeActive = true;
    UpdateHairBrushSample(screenX, screenY, true);
}

void MaterialInstanceAssetEditorPanel::BeginHairLassoStroke(
    float screenX, float screenY, bool shift, bool alt)
{
    if (!HairBrushReady())
    {
        hairBrushStatus = "The selected group is not paintable.";
        return;
    }
    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    if (section == nullptr || !HairDraftBindingReady())
    {
        hairBrushStatus = "The Groups draft is still syncing to the preview; wait a frame and retry.";
        return;
    }
    const uint32_t targetGroupId = static_cast<uint32_t>(hairGroupSelected) + 1;
    hairBrushStrokeMode = alt ? VL::Hair::GroupPaintMode::Remove :
        (shift ? VL::Hair::GroupPaintMode::Add : hairBrushMode);
    if (!EnsureHairPaintSession(section))
    {
        hairBrushStatus = "Hair group binding is unavailable; reload the asset.";
        return;
    }
    hairPaint.BeginStroke(targetGroupId, hairBrushStrokeMode, hairBrushReplaceExisting);
    hairBrushStrokeActive = true;
    hairLassoDragging = true;
    hairLassoScreenPoints.clear();
    AppendHairLassoPoint(screenX, screenY);
    // stamp 只提供套索平面；套索不在手势期间累积圆形命中。
    SampleHairBrushStamp(screenX, screenY);
    hairBrushHits.clear();
    hairBrushConflicts.clear();
    hairBrushStatus = hairBrushStamp.valid
        ? "Lasso: drag to enclose roots; release commits, Esc cancels."
        : "Lasso: no root surface under the cursor.";
    SubmitTressfxBrushPreview();
    UpdateHairBrushOverlay();
}

void MaterialInstanceAssetEditorPanel::AppendHairLassoPoint(float screenX, float screenY)
{
    if (!hairLassoDragging) return;
    const Eigen::Vector2f point(screenX, screenY);
    // 去掉亚像素抖动，避免一次手势生成数千个近重合顶点。
    if (!hairLassoScreenPoints.empty() &&
        (hairLassoScreenPoints.back() - point).norm() < 1.5f)
        return;
    hairLassoScreenPoints.push_back(point);
}

void MaterialInstanceAssetEditorPanel::UpdateHairLassoPreview()
{
    if (!hairLassoDragging || hairLassoScreenPoints.size() < 3) return;
    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    if (section == nullptr || !HairDraftBindingReady()) return;
    const Eigen::Vector2f centroid = ScreenCentroid(hairLassoScreenPoints);
    VL::Hair::BrushViewport viewport;
    VL::Hair::BrushCamera camera;
    VL::Hair::BrushSettings settings;
    if (!SampleHairBrushStamp(centroid.x(), centroid.y()) ||
        !PrepareHairBrushInputs(viewport, camera, settings))
        return;
    const auto hits = VL::Hair::RootCloudSelector().SelectLasso(
        *section->tressfxRootCloud, hairBrushStamp, section->tressfxRootLocalToWorld,
        viewport, camera, hairLassoScreenPoints, settings);
    // 多边形可能变小，先重置同一笔的权重并集，再写入当前命中，避免 ghost 只增不减。
    hairPaint.BeginStroke(static_cast<uint32_t>(hairGroupSelected) + 1,
        hairBrushStrokeMode, hairBrushReplaceExisting);
    hairPaint.AddHits(hits);
    hairBrushHits = hits;
    hairBrushStatus = "Lasso: " + std::to_string(hits.size()) +
        " guides selected; release commits, Esc cancels.";
    SubmitTressfxBrushPreview();
    UpdateHairBrushOverlay();
}

void MaterialInstanceAssetEditorPanel::CommitHairLassoStroke()
{
    if (!hairLassoDragging) return;
    previewDragButton = -1;
    previewDragGeneration = 0;
    hairLassoDragging = false;
    if (!hairBrushStrokeActive || hairLassoScreenPoints.size() < 3)
    {
        hairPaint.CancelStroke();
        hairBrushStrokeActive = false;
        hairLassoScreenPoints.clear();
        hairBrushStatus = "Lasso needs at least three points; the groups draft was not changed.";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    const Eigen::Vector2f centroid = ScreenCentroid(hairLassoScreenPoints);

    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    VL::Hair::BrushViewport viewport;
    VL::Hair::BrushCamera camera;
    VL::Hair::BrushSettings settings;
    if (section == nullptr || !HairDraftBindingReady() ||
        !SampleHairBrushStamp(centroid.x(), centroid.y()) ||
        !PrepareHairBrushInputs(viewport, camera, settings))
    {
        hairPaint.CancelStroke();
        hairBrushStrokeActive = false;
        hairLassoScreenPoints.clear();
        hairBrushStatus = "Lasso could not establish a brush plane; the groups draft was not changed.";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    hairBrushHits = VL::Hair::RootCloudSelector().SelectLasso(
        *section->tressfxRootCloud, hairBrushStamp, section->tressfxRootLocalToWorld,
        viewport, camera, hairLassoScreenPoints, settings);
    hairLassoScreenPoints.clear();
    // 拖动期间 ghost 已按中间多边形更新；提交前用最终多边形重置同一笔的权重。
    hairPaint.BeginStroke(static_cast<uint32_t>(hairGroupSelected) + 1,
        hairBrushStrokeMode, hairBrushReplaceExisting);
    hairPaint.AddHits(hairBrushHits);
    if (hairBrushHits.empty())
    {
        hairPaint.CancelStroke();
        hairBrushStrokeActive = false;
        hairBrushStatus = "No guides inside the lasso; nothing was written to the groups draft.";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    // 套索已经没有“松开前补一次圆形采样”的语义，命中已在上面一次性加入。
    CommitHairBrushStroke(false);
}

void MaterialInstanceAssetEditorPanel::CommitHairBrushStroke(bool sampleCurrent)
{
    if (!hairBrushStrokeActive) return;
    // 光标手势属于笔刷，提交后不能把 previewDragButton 留在左键状态，
    // 否则下一次 hover 会被旧代码解释成“左键拖动旋转预览模型”。
    previewDragButton = -1;
    previewDragGeneration = 0;
    // 圆形笔刷在松开前补一次采样，避免最后一段移动没有进入选择；套索已自行累积。
    if (sampleCurrent)
        UpdateHairBrushSample(hairBrushCursorX, hairBrushCursorY, true);
    const auto commit = hairPaint.CommitStroke();
    hairBrushStrokeActive = false;
    hairBrushHits.clear();
    if (!commit.has_value())
    {
        hairBrushStatus = "No guides selected; nothing was written to the groups draft.";
        SubmitTressfxBrushPreview();
        UpdateHairBrushOverlay();
        return;
    }
    if (hairGroupSelected < 0)
    {
        SubmitTressfxBrushPreview();
        return;
    }
    const std::string targetName =
        hairGroups.Groups()[static_cast<size_t>(hairGroupSelected)].name;
    // Undo 快照必须在写草稿前捕获，包含 overrides/名称/声明顺序。
    if (hairPaintUndo.size() >= 16) hairPaintUndo.erase(hairPaintUndo.begin());
    hairPaintUndo.push_back(hairGroups);
    std::string error;
    if (!hairGroups.ApplyPaintedGuides(static_cast<size_t>(hairGroupSelected),
            commit->addGuides, commit->removeGuides, error))
    {
        hairPaintUndo.pop_back();
        hairBrushStatus = "Paint commit rejected: " + error;
        SubmitTressfxBrushPreview();
        return;
    }
    int newIndex = -1;
    for (size_t index = 0; index < hairGroups.Groups().size(); ++index)
        if (hairGroups.Groups()[index].name == targetName)
        {
            newIndex = static_cast<int>(index);
            break;
        }
    hairGroupSelected = newIndex;
    if (hairGroupSelected < 0)
    {
        hairGroupPaintActive = false;
        hairBrushStatus = "The target group became empty and was deleted.";
    }
    else
    {
        std::ostringstream status;
        status << (commit->mode == VL::Hair::GroupPaintMode::Add ? "Added " : "Removed ")
               << commit->addGuides.size() + commit->removeGuides.size()
               << " guides " << (commit->mode == VL::Hair::GroupPaintMode::Add
                   ? "to \"" : "from \"") << targetName << "\"";
        const std::string conflicts = HairBrushConflictText(commit->conflicts);
        if (!conflicts.empty()) status << " | " << conflicts;
        status << " | saved only after Save to Hair Asset";
        hairBrushStatus = status.str();
    }
    SubmitTressfxBrushPreview();
    SubmitTressfxGroupPreview();
    UpdateHairBrushOverlay();
}

void MaterialInstanceAssetEditorPanel::CancelHairBrushStroke(bool exitPaint)
{
    previewDragButton = -1;
    previewDragGeneration = 0;
    hairBrushStrokeActive = false;
    hairPaint.CancelStroke();
    hairBrushHits.clear();
    hairBrushConflicts.clear();
    hairBrushStamp = {};
    hairLassoDragging = false;
    hairLassoScreenPoints.clear();
    if (exitPaint)
    {
        hairGroupPaintActive = false;
        hairBrushStatus = "Paint mode cancelled; the groups draft was not changed.";
    }
    SubmitTressfxBrushPreview();
    UpdateHairBrushOverlay();
}

void MaterialInstanceAssetEditorPanel::SubmitTressfxBrushPreview()
{
    if (runtimeActionSink == nullptr) return;
    const uint64_t generation = CurrentModelPreviewGeneration();
    // 切到非头发预览时仍要清掉上一帧圆环；此时没有 section/目标组，只发清除语义。
    const bool hairPreview = IsActiveHairPreview();
    const EditorSceneMeshSectionSnapshot* section = hairPreview ? HairBrushSection() : nullptr;
    if (section == nullptr)
    {
        if (!hairBrushLastObjectName.empty())
        {
            UiAction clear;
            clear.type = UiActionType::SetTressfxBrushPreview;
            clear.previewTressfxBrush.worldGeneration = hairBrushLastGeneration;
            clear.previewTressfxBrush.objectName = hairBrushLastObjectName;
            clear.previewTressfxBrush.ringVisible = false;
            runtimeActionSink->RequestRuntimeAction(std::move(clear));
        }
        hairBrushLastObjectName.clear();
        hairBrushLastGeneration = 0;
        hairBrushPreviewActive = false;
        hairBrushRingSent = false;
        return;
    }
    if (generation == 0) return;
    const bool draftReady = HairDraftBindingReady();
    const uint32_t targetGroupId = draftReady && hairGroupSelected >= 0
        ? static_cast<uint32_t>(hairGroupSelected) + 1u : 0u;
    // 直接填充 UiAction，避免“request -> action”两次完整权重拷贝。
    UiAction action;
    action.type = UiActionType::SetTressfxBrushPreview;
    action.previewTressfxBrush.worldGeneration = generation;
    action.previewTressfxBrush.objectName = section->objectName;
    action.previewTressfxBrush.targetGroupId = targetGroupId;
    action.previewTressfxBrush.mode =
        hairBrushStrokeActive ? hairBrushStrokeMode : hairBrushMode;
    if (hairBrushStrokeActive && draftReady && hairGroupSelected >= 0)
    {
        const auto& weights = hairPaint.StrokeWeights();
        action.previewTressfxBrush.weights.reserve(weights.size());
        for (const auto& [guide, weight] : weights)
            action.previewTressfxBrush.weights.push_back({guide, weight});
    }
    const bool weightsActive = !action.previewTressfxBrush.weights.empty();

    // 圆环与 ghost 分开去重：hover 只移动圆环时权重仍是空的，但也必须投递命令。
    const bool ringVisible = hairPreview && hairGroupPaintActive && hairBrushStamp.valid;
    bool ringChanged = !hairBrushRingSent || ringVisible != hairBrushRingSentVisible;
    if (ringVisible)
    {
        ringChanged = ringChanged ||
            !hairBrushStamp.position.isApprox(hairBrushRingSentCenter, 1.0e-4f) ||
            !hairBrushStamp.normal.isApprox(hairBrushRingSentNormal, 1.0e-4f) ||
            std::abs(hairBrushStamp.radiusWorld - hairBrushRingSentRadius) > 1.0e-4f;
    }
    const bool weightsChanged = weightsActive || hairBrushPreviewActive;
    if (!ringChanged && !weightsChanged) return;

    action.previewTressfxBrush.ringVisible = ringVisible;
    if (ringVisible)
    {
        action.previewTressfxBrush.ringCenter = hairBrushStamp.position;
        action.previewTressfxBrush.ringNormal = hairBrushStamp.normal;
        action.previewTressfxBrush.ringRadiusWorld = hairBrushStamp.radiusWorld;
    }
    runtimeActionSink->RequestRuntimeAction(std::move(action));
    hairBrushLastObjectName = section->objectName;
    hairBrushLastGeneration = generation;
    hairBrushPreviewActive = weightsActive;
    hairBrushRingSent = true;
    hairBrushRingSentVisible = ringVisible;
    hairBrushRingSentCenter = hairBrushStamp.position;
    hairBrushRingSentNormal = hairBrushStamp.normal;
    hairBrushRingSentRadius = hairBrushStamp.radiusWorld;
}

void MaterialInstanceAssetEditorPanel::UpdateHairBrushOverlay()
{
    const bool visible = hairGroupPaintActive &&
        (hairBrushStamp.valid || hairLassoDragging);
    widgetDocument.SetProperty("hair-brush-overlay", "display",
        visible ? "block" : "none");
    if (!visible)
    {
        widgetDocument.SetProperty("hair-brush-ring", "display", "none");
        return;
    }
    Rml::Element* surface = document->GetElementById("resource-preview-surface");
    if (surface == nullptr) return;
    SetMarkup("hair-brush-hud", Text(hairBrushStatus));
    // 正常路径由 World 的 DebugDraw 绘制贴合头皮工作面的圆环；只有平面 PCA/
    // 退化回退时才显示屏幕圆环作为降级提示。
    if (!hairBrushStamp.valid || !hairBrushStamp.fallback)
    {
        widgetDocument.SetProperty("hair-brush-ring", "display", "none");
        return;
    }
    const Rml::Vector2f offset = surface->GetAbsoluteOffset(Rml::BoxArea::Border);
    // 降级圆环不是命中依据：它按屏幕像素半径画在 cursor 处；实际选择仍由
    // SurfaceBrush 的世界平面 H/normal/r_world 完成，状态栏给出真实 cm 值。
    const float radiusPx = std::max(2.0f, hairBrushRadiusPx);
    widgetDocument.SetProperty("hair-brush-ring", "display", "block");
    widgetDocument.SetProperty("hair-brush-ring", "left",
        std::to_string(std::lround(hairBrushCursorX - offset.x - radiusPx)) + "px");
    widgetDocument.SetProperty("hair-brush-ring", "top",
        std::to_string(std::lround(hairBrushCursorY - offset.y - radiusPx)) + "px");
    widgetDocument.SetProperty("hair-brush-ring", "width",
        std::to_string(std::lround(radiusPx * 2.0f)) + "px");
    widgetDocument.SetProperty("hair-brush-ring", "height",
        std::to_string(std::lround(radiusPx * 2.0f)) + "px");
    widgetDocument.SetProperty("hair-brush-ring", "border-radius",
        std::to_string(std::lround(radiusPx)) + "px");
}

void MaterialInstanceAssetEditorPanel::UndoLastHairBrushCommit()
{
    if (hairPaintUndo.empty())
    {
        hairBrushStatus = "No paint commit to undo in this session.";
        return;
    }
    CancelHairBrushStroke(false);
    hairGroups = std::move(hairPaintUndo.back());
    hairPaintUndo.pop_back();
    if (hairGroupSelected >= static_cast<int>(hairGroups.Groups().size()))
        hairGroupSelected = static_cast<int>(hairGroups.Groups().size()) - 1;
    hairBrushStatus = "Undid the previous paint commit in this session; Save was not performed.";
    SubmitTressfxGroupPreview();
    UpdateHairBrushOverlay();
}

void MaterialInstanceAssetEditorPanel::RefreshHairGroupsSection(bool isHair, bool hairBlocked)
{
    const auto* active = resourceEditor.ActiveTab();
    const bool visible = isHair && active != nullptr;
    widgetDocument.SetProperty("resource-hair-groups", "display", visible ? "block" : "none");
    if (!visible)
    {
        // 隐藏时丢弃绑定身份，避免切回同一路径时误用上一个草稿的选中/表单状态。
        hairGroupsPath.clear();
        hairGroupsLoaded = false;
        hairGroupsObservedDocument = nullptr;
        hairGroupsBoundRevision = 0;
        hairGroupsBoundGuideCount = 0;
        hairGroupSelected = -1;
        hairGroupPreviewSent = false;
        hairGroupPreviewSentGeneration = 0;
        hairGroupPreviewSentSelected = -2;
        hairGroupPreviewSentFlags = 0;
        hairGroupBindingPreviewSent = false;
        hairGroupBindingPreviewQueued = false;
        hairGroupBindingPreviewSentRevision = 0;
        hairGroupBindingPreviewSentGeneration = 0;
        hairGroupBindingPreviewSentObject.clear();
        hairGroupBindingPreviewError.clear();
        hairPaintDraftBinding.reset();
        hairPaintDraftRevision = 0;
        hairGroupPaintActive = false;
        hairBrushStrokeActive = false;
        hairBrushHits.clear();
        hairBrushConflicts.clear();
        hairBrushStamp = {};
        hairLassoActive = false;
        hairLassoDragging = false;
        hairLassoScreenPoints.clear();
        // section 即将失效；用最后一次对象名显式清除 RT ghost 权重。
        if (runtimeActionSink != nullptr && !hairBrushLastObjectName.empty())
        {
            UiAction clear;
            clear.type = UiActionType::SetTressfxBrushPreview;
            clear.previewTressfxBrush.worldGeneration = hairBrushLastGeneration;
            clear.previewTressfxBrush.objectName = hairBrushLastObjectName;
            clear.previewTressfxBrush.ringVisible = false;
            runtimeActionSink->RequestRuntimeAction(std::move(clear));
        }
        hairBrushLastObjectName.clear();
        hairBrushLastGeneration = 0;
        ResetHairPaintSession();
        hairPaintUndo.clear();
        hairBrushStatus.clear();
        hairBrushPreviewActive = false;
        hairGroupCreateActive = false;
        hairGroupAddRangeActive = false;
        hairGroupsPendingSave = false;
        hairGroupsSaveWaitTicks = 0;
        widgetDocument.SetProperty("hair-group-legend-overlay", "display", "none");
        SubmitTressfxBrushPreview();
        return;
    }

    // guideCount 只用于编辑期校验 end <= guideCount；文档快照优先，目录作为回退。
    uint32_t guideCount = active->snapshot ? active->snapshot->tressfxGuideCount : 0;
    if (guideCount == 0)
    {
        const EditorJsonAssetEntry* entry = FindCatalogEntry(snapshot.assetCatalog, active->path);
        if (entry != nullptr) guideCount = entry->tressfxGuideCount;
    }

    const auto* document = TressfxDocument(active->path);
    if (document == nullptr) RequestTressfxDocument(active->path);
    if (hairGroupsPath != active->path)
    {
        hairGroupsPath = active->path;
        hairGroupsLoaded = false;
        hairGroupsObservedDocument = nullptr;
        hairGroupsBoundRevision = 0;
        hairGroupsBoundGuideCount = 0;
        hairGroupSelected = -1;
        hairGroupPreviewSent = false;
        hairGroupPreviewSentGeneration = 0;
        hairGroupPreviewSentSelected = -2;
        hairGroupPreviewSentFlags = 0;
        hairGroupBindingPreviewSent = false;
        hairGroupBindingPreviewQueued = false;
        hairGroupBindingPreviewSentRevision = 0;
        hairGroupBindingPreviewSentGeneration = 0;
        hairGroupBindingPreviewSentObject.clear();
        hairGroupBindingPreviewError.clear();
        hairPaintDraftBinding.reset();
        hairPaintDraftRevision = 0;
        hairGroupPaintActive = false;
        hairBrushStrokeActive = false;
        hairBrushHits.clear();
        hairBrushConflicts.clear();
        hairBrushStamp = {};
        hairLassoActive = false;
        hairLassoDragging = false;
        hairLassoScreenPoints.clear();
        ResetHairPaintSession();
        hairPaintUndo.clear();
        hairBrushStatus.clear();
        hairBrushPreviewActive = false;
        hairGroupCreateActive = false;
        hairGroupCreateName.clear();
        hairGroupCreateStart.clear();
        hairGroupCreateEnd.clear();
        hairGroupCreateError.clear();
        hairGroupAddRangeActive = false;
        hairGroupRangeStart.clear();
        hairGroupRangeEnd.clear();
        hairGroupRangeError.clear();
        hairGroupStatus.clear();
        hairGroupsPendingSave = false;
        hairGroupsSaveRevision = 0;
        hairGroupsSaveWaitTicks = 0;
        hairGroupsPendingReset = false;
        hairGroupsPendingValue = nlohmann::json();
    }

    if (document == nullptr)
    {
        widgetDocument.SetProperty("hair-group-legend-overlay", "display", "none");
        SetMarkup("resource-hair-groups", UiWidgets::CollapsibleSection(
            DetailsSectionId("hair-groups"), "hair-groups", "Groups",
            "<p class=\"hair-group-note\">Reading the hair asset document...</p>",
            {"section"}, IsSectionExpanded("hair-groups")));
        widgetDocument.SetClass(DetailsSectionId("hair-groups"), "collapsed",
            !IsSectionExpanded("hair-groups"));
        SetMarkup("hair-group-status", Text(hairGroupStatus));
        return;
    }

    // revision 前进可能来自 Simulation 保存；只有文档里的 groups 实际变化时才覆盖
    // 本地草稿，否则两个独立草稿会在对方保存时互相丢弃未保存编辑。
    const bool hasDocumentGroups = document->document.contains("groups") &&
        document->document.at("groups").is_array() &&
        !document->document.at("groups").empty();
    const nlohmann::json currentGroups = hasDocumentGroups
        ? document->document.at("groups") : nlohmann::json();
    const bool groupsSectionChanged =
        hairGroupsObservedDocument.is_null() != currentGroups.is_null() ||
        (!hairGroupsObservedDocument.is_null() &&
            hairGroupsObservedDocument != currentGroups);
    if (hairGroupsBoundRevision != document->documentRevision ||
        hairGroupsBoundGuideCount != guideCount)
    {
        const bool firstBind = hairGroupsBoundRevision == 0;
        const bool guideCountChanged = hairGroupsBoundGuideCount != guideCount;
        hairGroupsBoundRevision = document->documentRevision;
        hairGroupsBoundGuideCount = guideCount;
        if (firstBind || guideCountChanged || groupsSectionChanged)
        {
            // guideCount 改变会改变展开长度；旧 binding 不能继续复用。
            hairPaintDraftBinding.reset();
            hairPaintDraftRevision = 0;
            std::string error;
            hairGroupsObservedDocument = currentGroups;
            if (!hairGroups.Load(document->document, guideCount, error))
            {
                hairGroupsLoaded = false;
                hairGroupStatus = "Groups unavailable: " + error;
                hairGroupSelected = -1;
                hairGroupCreateActive = false;
                hairGroupAddRangeActive = false;
            }
            else
            {
                hairGroupsLoaded = true;
                if (hairGroupStatus.rfind("Groups unavailable:", 0) == 0) hairGroupStatus.clear();
                const int count = static_cast<int>(hairGroups.Groups().size());
                if (hairGroupSelected >= count) hairGroupSelected = count - 1;
            }
        }
    }

    // modify 成功后草稿变 dirty 且 revision 前进；保存交给既有 Save Content 弹窗。
    if (hairGroupsPendingSave)
    {
        const bool staged = document->documentRevision > hairGroupsSaveRevision ||
            (document->dirty && HairGroupsMatchDocument(document->document,
                hairGroupsPendingReset, hairGroupsPendingValue));
        if (staged)
        {
            hairGroupsPendingSave = false;
            hairGroupsSaveWaitTicks = 0;
            hairGroupStatus = "Confirm the save in the Save Content dialog (writes the hair asset).";
            if (pendingClosePath != active->path) OpenSaveDialog(active->path);
        }
        else if (++hairGroupsSaveWaitTicks > 180)
        {
            hairGroupsPendingSave = false;
            hairGroupsSaveWaitTicks = 0;
            hairGroupStatus = snapshot.statusMessage.empty()
                ? "Save failed: the asset draft was not modified." : snapshot.statusMessage;
        }
    }

    const auto& groups = hairGroups.Groups();
    // 草稿结构签名：只遍历组/range（不是 guide），用于让笔刷会话在 Add/Remove/
    // Delete/撤销后重建冲突分析，而不是继续使用旧的共享 binding。
    std::string draftSignature;
    for (const VL::Editor::TressfxGuideGroupDraft& group : groups)
    {
        draftSignature += group.name;
        draftSignature += ':';
        for (const auto& range : group.guideRanges)
        {
            draftSignature += std::to_string(range.first);
            draftSignature += ',';
            draftSignature += std::to_string(range.second);
            draftSignature += ';';
        }
        draftSignature += group.simulationOverrides.dump();
        draftSignature += '|';
    }
    if (draftSignature != hairGroupsDraftSignature)
    {
        hairGroupsDraftSignature = std::move(draftSignature);
        ++hairGroupsDraftRevision;
    }
    // 草稿 revision 变化后立刻同步一次 GPU binding；后续静帧 Refresh 不重复发送。
    SubmitTressfxGroupBindingPreview();
    const nlohmann::json simulation = document->document.contains("simulationDefaults")
        ? document->document.at("simulationDefaults") : nlohmann::json::object();
    const bool canPaint = hairGroupsLoaded && hairGroupSelected >= 0 &&
        HairBrushSection() != nullptr && CurrentModelPreviewGeneration() != 0 &&
        static_cast<size_t>(hairGroupSelected) < groups.size() &&
        HairDraftBindingReady();
    if (hairGroupPaintActive && !canPaint)
        CancelHairBrushStroke(true);

    std::string rows;
    rows += "<p class=\"hair-group-note\">Groups are authored data. The preview uses the current "
        "draft binding immediately; Save to Hair Asset persists names/ranges. Per-group "
        "simulation overrides still take effect after save and reload; placement overrides "
        "apply last.</p>";
    if (hairBlocked)
        rows += "<p class=\"hair-group-note\">Preview is unavailable for skinned hair; group "
            "editing still saves to the asset.</p>";

    // 预览开关只作用于当前 TF 预览 World；默认颜色开、Solo 关，避免关卡头发被染色。
    rows += "<div class=\"hair-group-actions hair-group-preview-actions\">"
        "<button id=\"hair-group-colors\" class=\"hair-group-toggle" +
        std::string(hairGroupColors ? " active" : "") +
        "\" data-action=\"hair-group-colors\">Group Colors: " +
        (hairGroupColors ? "On" : "Off") + "</button>"
        "<button id=\"hair-group-solo\" class=\"hair-group-toggle" +
        std::string(hairGroupSolo ? " active" : "") +
        "\" data-action=\"hair-group-solo\">Solo</button></div>";

    // Paint 目标只能是有声明的命名组；Default(0) 永远不可写入。控件仍显示，
    // 但无 preview generation / 根点云时禁用，状态行说明原因。
    rows += "<div class=\"hair-group-paint-controls\">"
        "<button id=\"hair-group-paint\" data-action=\"hair-group-paint\" class=\"" +
        std::string(hairGroupPaintActive ? "active" : "") + "\"" +
        (canPaint ? "" : " disabled=\"disabled\"") + ">" +
        (hairGroupPaintActive ? "Stop Painting" : "Paint Groups") + "</button>"
        "<label class=\"hair-group-paint-label\">Brush</label>" +
        UiWidgets::Input("hair-brush-radius", {"hair-brush-radius"}, {}) +
        "<span class=\"hair-group-paint-unit\">px</span>"
        "<label class=\"hair-group-paint-label\">Mode</label>" +
        UiWidgets::Select("hair-brush-mode", {"hair-brush-mode"},
            UiWidgets::Option("add", "Add") + UiWidgets::Option("remove", "Remove")) +
        "<label class=\"hair-group-paint-check\"><input id=\"hair-brush-replace\" "
        "type=\"checkbox\" data-action=\"hair-brush-replace\"" +
        std::string(hairBrushReplaceExisting ? " checked=\"checked\"" : "") +
        "/><span>Replace existing</span></label>"
        "<button id=\"hair-lasso\" data-action=\"hair-lasso-toggle\" class=\"hair-group-toggle" +
        std::string(hairLassoActive ? " active" : "") + "\"" +
        (canPaint ? "" : " disabled=\"disabled\"") + ">Lasso</button></div>";
    if (hairGroupPaintActive)
    {
        rows += "<div class=\"hair-group-paint-state\">" +
            Text(hairBrushStatus.empty() ? "Hover the preview to select roots." : hairBrushStatus) +
            "</div>";
    }
    else if (!canPaint)
    {
        rows += "<div class=\"hair-group-paint-state\">Select a named group in a live hair preview; "
            "the Groups draft must finish syncing to the renderable before painting.</div>";
    }

    const std::string defaultCount = hairGroups.GuideCount() == 0
        ? std::string("--") : std::to_string(hairGroups.DefaultGuideCount()) + " guides";
    rows += "<button id=\"hair-group-default\" class=\"hair-group-row" +
        std::string(hairGroupSelected < 0 ? " selected" : "") +
        "\" data-action=\"hair-group-default\">"
        "<span class=\"hair-group-swatch hair-group-swatch-default\"></span>"
        "<span class=\"hair-group-name\">Default</span>"
        "<span class=\"hair-group-count\">" + Text(defaultCount) + "</span></button>";

    for (std::size_t index = 0; index < groups.size(); ++index)
    {
        const VL::Editor::TressfxGuideGroupDraft& group = groups[index];
        rows += "<button id=\"hair-group-row-" + std::to_string(index) +
            "\" class=\"hair-group-row" +
            (hairGroupSelected == static_cast<int>(index) ? " selected" : "") +
            "\" data-action=\"hair-group-select\" data-index=\"" + std::to_string(index) + "\">" +
            HairGroupSwatch(VL::Editor::TressfxGroupEditor::GroupColor(group.name)) +
            "<span class=\"hair-group-name\">" + Text(group.name) + "</span>"
            "<span class=\"hair-group-count\">" + std::to_string(hairGroups.GuideCountFor(index)) +
            " guides</span>"
            "<span class=\"hair-group-overrides\">" +
            std::to_string(group.simulationOverrides.size()) + " overrides</span></button>";
    }

    // 视口图例的紧凑投影：只列色块 + 名称；完整 ranges/override 仍在组列表行里。
    rows += "<div class=\"hair-group-legend\"><span class=\"hair-group-legend-title\">Legend</span>"
        "<span class=\"hair-group-legend-item\">" +
        HairGroupSwatch(VL::Hair::TressfxDefaultGroupColor()) +
        "<span>Default</span></span>";
    for (const VL::Editor::TressfxGuideGroupDraft& group : groups)
    {
        rows += "<span class=\"hair-group-legend-item\">" +
            HairGroupSwatch(VL::Editor::TressfxGroupEditor::GroupColor(group.name)) +
            "<span>" + Text(group.name) + "</span></span>";
    }
    rows += "</div>";

    if (hairGroupCreateActive)
    {
        rows += "<div class=\"hair-group-form\">"
            "<div class=\"hair-group-form-row\"><label class=\"hair-group-form-label\">Name</label>" +
            UiWidgets::Input("hair-group-create-name", {"hair-group-create-field", 0, 0}, {}) +
            "<button id=\"hair-group-create-submit\" data-action=\"hair-group-create-submit\">Create</button>"
            "<button id=\"hair-group-create-cancel\" data-action=\"hair-group-create-cancel\">Cancel</button>"
            "</div><div id=\"hair-group-create-error\" class=\"hair-group-status\"></div></div>";
    }
    else
    {
        rows += "<div class=\"hair-group-actions\">" +
            UiWidgets::Button("hair-group-create-open", {"hair-group-create-open"}, "+ New Group") +
            "</div>";
    }

    if (hairGroupSelected >= 0 && hairGroupSelected < static_cast<int>(groups.size()))
    {
        const std::size_t selected = static_cast<std::size_t>(hairGroupSelected);
        const VL::Editor::TressfxGuideGroupDraft& group = groups[selected];
        // 已保存组显示 placement 解析后的值；新建/改名的草稿组没有求解绑定，
        // 只能回退文档 simulationDefaults，并在行内标注原因。
        const EditorSceneMeshSectionSnapshot* assetSection = HairBrushSection();
        const int32_t selectedSavedIndex =
            SavedHairGroupIndex(assetSection, hairGroupSelected);
        const bool selectedSaved = selectedSavedIndex >= 0;
        rows += "<div class=\"hair-group-inspector\"><div class=\"hair-group-inspector-head\">" +
            HairGroupSwatch(VL::Editor::TressfxGroupEditor::GroupColor(group.name)) +
            "<span>Group: " + Text(group.name) + "</span></div>";
        rows += UiWidgets::PropertyControlRow("Name",
            UiWidgets::Input("hair-group-name", {"hair-group-rename"}, group.name));

        // 区间不是作者要看的层级：Add/Remove 全部通过笔刷完成。
        rows += "<p class=\"hair-group-note\">Guides are edited with the brush. "
            "Use Paint Groups to add; switch to Remove or hold Alt to subtract.</p>";

        rows += "<div class=\"hair-group-subhead\">Overrides</div>";
        for (const HairGroupOverrideSpec& spec : HairGroupOverrideSpecs)
        {
            const bool inherited = !group.simulationOverrides.contains(spec.field);
            rows += "<div class=\"hair-group-override-row\">"
                "<label class=\"hair-group-override-label\">" + Text(spec.label) + "</label>"
                "<input id=\"hair-group-override-" + std::string(spec.field) +
                "\" type=\"checkbox\" data-action=\"hair-group-override-toggle\" data-key=\"" +
                spec.field + "\"" + (inherited ? "" : " checked=\"checked\"") + "/>"
                "<input id=\"hair-group-override-" + std::string(spec.field) +
                "-value\" type=\"text\" data-action=\"hair-group-override-value\" data-key=\"" +
                spec.field + "\" value=\"\"/>"
                "<span class=\"hair-group-default\">(asset default " +
                Text(HairGroupAssetDefaultText(assetSection, selectedSaved,
                    selectedSavedIndex, simulation, spec.field)) +
                ")</span></div>";
        }
        rows += "<div class=\"hair-group-actions\">" +
            UiWidgets::Button("hair-group-clear-overrides", {"hair-group-clear-overrides"},
                "Clear Overrides") +
            UiWidgets::Button("hair-group-delete", {"hair-group-delete"}, "Delete Group") +
            "</div></div>";
    }

    rows += "<div class=\"hair-group-footer\">" +
        UiWidgets::Button("hair-group-save", {"hair-group-save"}, "Save to Hair Asset",
            UiWidgets::ButtonStyle::Command, false, "icons/save.png") +
        "</div><span id=\"hair-group-status\" class=\"hair-group-status\"></span>"
        "<span id=\"hair-brush-status\" class=\"hair-group-status\">" +
        Text(hairBrushStatus) + "</span>";

    SetMarkup("resource-hair-groups", UiWidgets::CollapsibleSection(
        DetailsSectionId("hair-groups"), "hair-groups", "Groups", rows,
        {"section"}, IsSectionExpanded("hair-groups")));
    widgetDocument.SetClass(DetailsSectionId("hair-groups"), "collapsed",
        !IsSectionExpanded("hair-groups"));

    // 预览图左上角的紧凑图例：与同步到 GPU 的草稿 binding 同源；
    // binding 尚未入队时隐藏，避免显示实际不存在的颜色。Group Colors 关闭时隐藏。
    const EditorSceneMeshSectionSnapshot* legendSection = HairBrushSection();
    std::string legend = "<span class=\"hair-group-legend-title\">Groups</span>"
        "<span class=\"hair-group-legend-item\">" +
        HairGroupSwatch(VL::Hair::TressfxDefaultGroupColor()) +
        "Default</span>";
    if (legendSection != nullptr && HairDraftBindingReady())
        for (const VL::Editor::TressfxGuideGroupDraft& group : groups)
            legend += "<span class=\"hair-group-legend-item\">" +
                HairGroupSwatch(VL::Editor::TressfxGroupEditor::GroupColor(group.name)) +
                Text(group.name) + "</span>";
    SetMarkup("hair-group-legend-overlay", legend);
    widgetDocument.SetProperty("hair-group-legend-overlay", "display",
        hairGroupColors && legendSection != nullptr && HairDraftBindingReady()
            ? "block" : "none");

    // 结构 markup 不携带用户正在输入的值；只在这些控件挂载后同步一次。
    SetControlValue("hair-group-create-name", hairGroupCreateName);
    SetControlValue("hair-brush-radius", SceneActorNumber(hairBrushRadiusPx));
    SetControlValue("hair-brush-mode",
        hairBrushMode == VL::Hair::GroupPaintMode::Remove ? "remove" : "add");
    if (hairGroupSelected >= 0 && hairGroupSelected < static_cast<int>(groups.size()))
    {
        const VL::Editor::TressfxGuideGroupDraft& group = groups[static_cast<std::size_t>(hairGroupSelected)];
        const EditorSceneMeshSectionSnapshot* assetSection = HairBrushSection();
        const int32_t selectedSavedIndex =
            SavedHairGroupIndex(assetSection, hairGroupSelected);
        const bool selectedSaved = selectedSavedIndex >= 0;
        for (const HairGroupOverrideSpec& spec : HairGroupOverrideSpecs)
        {
            const bool inherited = !group.simulationOverrides.contains(spec.field);
            const std::string valueId = "hair-group-override-" + std::string(spec.field) + "-value";
            SetControlValue(valueId, TressfxSimulationText(inherited
                ? HairGroupAssetDefault(assetSection, selectedSaved,
                    selectedSavedIndex, simulation, spec.field)
                : group.simulationOverrides.at(spec.field).get<double>()));
            SetDisabled(valueId, inherited);
        }
    }

    // Create 只在名称通过控制器校验时可用；新组从空组开始，guide 由笔刷填充。
    // 因此必须在这里把校验失败原因写进行内错误，而不是等一个永远到不了的 submit。
    bool createValid = false;
    if (hairGroupCreateActive)
    {
        std::string error;
        const bool anyInput = !hairGroupCreateName.empty();
        VL::Editor::TressfxGroupEditor probe = hairGroups;
        createValid = probe.AddEmptyGroup(hairGroupCreateName, error);
        if (createValid || !anyInput) hairGroupCreateError.clear();
        else hairGroupCreateError = error;
    }
    SetDisabled("hair-group-create-submit", !createValid);
    SetMarkup("hair-group-create-error", Text(hairGroupCreateError));
    SetMarkup("hair-group-status", Text(hairGroupStatus));
    // 加载失败时控制器是空的；若仍允许保存，WriteGroupArray 会把原始 groups 删掉。
    SetDisabled("hair-group-save", !hairGroupsLoaded);
    SetDisabled("hair-group-paint", !canPaint);
    SetDisabled("hair-group-select-viewport", !canPaint);
    // 首次绑定或状态变化时投递一次预览；没有预览 generation 时保持静默。
    SubmitTressfxGroupPreview();
    // 组切换/加载失败导致 Paint 退出时，同步移除世界圆环。
    SubmitTressfxBrushPreview();
}

void MaterialInstanceAssetEditorPanel::SubmitHairGroupsSave()
{
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr) return;
    if (!hairGroupsLoaded)
    {
        hairGroupStatus = "Groups could not be loaded; saving is disabled to protect the asset.";
        return;
    }
    const auto* document = TressfxDocument(active->path);
    if (document == nullptr)
    {
        RequestTressfxDocument(active->path);
        hairGroupStatus = "Reading the asset document...";
        return;
    }

    std::string error;
    if (!hairGroups.Validate(error))
    {
        hairGroupStatus = error;
        return;
    }

    nlohmann::json candidate = document->document;
    hairGroups.WriteGroupArray(candidate);
    const bool reset = !candidate.contains("groups");
    const nlohmann::json value = reset ? nlohmann::json() : candidate.at("groups");
    if (HairGroupsMatchDocument(document->document, reset, value))
    {
        hairGroupStatus = "No group changes to stage.";
        return;
    }

    ModifyAssetDocumentPayload payload;
    payload.assetPath = active->path;
    EditorAssetFieldChange change;
    change.field = "groups";
    if (reset) change.reset = true;
    else change.value = value;
    payload.changes.push_back(std::move(change));

    EditorCommandEnvelope command;
    command.type = EditorCommandType::ModifyTressfxDocument;
    command.expectedDocumentRevision = document->documentRevision;
    command.payload = std::move(payload);
    Submit(std::move(command));

    hairGroupsPendingSave = true;
    hairGroupsSaveRevision = document->documentRevision;
    hairGroupsSaveWaitTicks = 0;
    hairGroupsPendingReset = reset;
    hairGroupsPendingValue = value;
    hairGroupStatus = "Staging groups...";
}


} // namespace VL::EditorUi
