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

void MaterialInstanceAssetEditorPanel::RefreshMenus()
{
    RefreshLevelMenu();
    SetMarkup("menu-window-panels", WindowMenuMarkup());
    // 菜单栏之外的状态写入同样走缓存：静止帧不重复设置 class 与 display。
    widgetDocument.SetProperty("about-dialog", "display", aboutVisible ? "block" : "none");
    // 模态弹窗打开时上下文菜单必须收起：模态期间工作区命令都不可用，
    // 留一个点不动的弹层会让人以为命令失效。
    if (HasModal() && IsContextMenuOpen()) CloseContextMenu();
}

// Open Level 子菜单来自冻结的只读资产目录；目录代次不变时不重建条目和索引表。
void MaterialInstanceAssetEditorPanel::RefreshLevelMenu()
{
    const auto* catalog = snapshot.assetCatalog.get();
    const uint64_t revision = catalog == nullptr ? 0 : catalog->revision;
    if (menuCatalogReady && menuCatalog.get() == catalog && menuCatalogRevision == revision) return;
    menuCatalog = snapshot.assetCatalog;
    menuCatalogRevision = revision;
    menuCatalogReady = true;
    menuLevelPaths.clear();
    std::string markup;
    if (catalog != nullptr)
    {
        for (const auto& asset : catalog->assets)
        {
            if (asset.kind != EditorAssetKind::Scene) continue;
            const size_t index = menuLevelPaths.size();
            menuLevelPaths.push_back(asset.assetPath);
            const std::string label = asset.name.empty()
                ? std::filesystem::path(asset.assetPath).filename().string() : asset.name;
            markup += UiWidgets::MenuItem("menu-level-" + std::to_string(index),
                {"menu-level", index}, label, false, false);
        }
    }
    if (markup.empty())
        markup = UiWidgets::MenuItem("menu-level-empty", {"menu-level-empty", 0},
            "No levels in the asset catalog", false, true);
    SetMarkup("menu-file-levels", std::move(markup));
}

std::string MaterialInstanceAssetEditorPanel::WindowMenuMarkup() const
{
    return Panel::BuildWindowMenuMarkup(docking);
}

void MaterialInstanceAssetEditorPanel::RefreshViewportToolbar()
{
    if (document == nullptr) return;
    SetViewportToolbarClass("viewport-tool-select", viewportTransformTool == "select");
    SetViewportToolbarClass("viewport-tool-move", viewportTransformTool == "move");
    SetViewportToolbarClass("viewport-tool-rotate", viewportTransformTool == "rotate");
    SetViewportToolbarClass("viewport-tool-scale", viewportTransformTool == "scale");
    SetViewportToolbarClass("viewport-space-world", viewportTransformSpace == "world");
    SetViewportToolbarClass("viewport-space-local", viewportTransformSpace == "local");
    // view mode 直接映射运行时 debug view：0 是完整画面，1 是基础色。
    // 工具栏不声明自己没有设置过的调试视图，因此其他调试模式在这三个按钮上都没有高亮。
    SetViewportToolbarClass("viewport-view-lit", runtimeView.debugViewMode == 0);
    SetViewportToolbarClass("viewport-view-unlit", runtimeView.debugViewMode == 1);
    if (auto* check = document->GetElementById("viewport-snap-enabled"))
    {
        if (sceneSnapEnabled && !check->HasAttribute("checked")) check->SetAttribute("checked", "checked");
        if (!sceneSnapEnabled && check->HasAttribute("checked")) check->RemoveAttribute("checked");
    }
    SetControlValue("viewport-snap-move", Number(sceneMoveSnap));
    SetControlValue("viewport-snap-rotate", Number(sceneRotateSnap));
    SetControlValue("viewport-snap-scale", Number(sceneScaleSnap));
    SetControlValue("viewport-camera-speed", Number(cameraMoveSpeed));
    // 菜单只映射已有 debug view，不复制渲染状态；勾选与标题以运行时快照为准。
    const char* modes[] = {"Lit", "Base Color", "Emissive", "World Normal",
        "Roughness", "Metallic", "Ambient Occlusion", "Shadow", "Direct Lighting",
        "Indirect Diffuse", "Indirect Specular", "Shadow Cascade Index", "Shading Model"};
    std::string markup;
    for (size_t mode = 1; mode < sizeof(modes) / sizeof(modes[0]); ++mode)
        markup += UiWidgets::MenuItem("viewport-buffer-" + std::to_string(mode),
            {"context-view-mode", mode}, modes[mode], runtimeView.debugViewMode == static_cast<int>(mode), false);
    SetMarkup("viewport-buffer-modes", markup);
    const int mode = runtimeView.debugViewMode;
    SetMarkup("viewport-view-title", std::string(mode >= 0 && mode < 13 ? modes[mode] : "Debug View") + " v");
    RefreshViewportAxisIndicator();
    RefreshActorVisualization();
    RefreshSceneGizmo();
    RefreshTerrainBrushUi();
    RefreshHistoryControls();
}

void MaterialInstanceAssetEditorPanel::RefreshViewportAxisIndicator()
{
    if (document == nullptr) return;

    // 相机朝向来自冻结的大纲快照（EditorSceneCameraActor 带 forward/right/up），
    // 因此这里不需要新增快照字段，也不访问 renderer。
    const EditorSceneActorSnapshot* camera = nullptr;
    for (const EditorSceneActorSnapshot& actor : OutlinerActors())
    {
        if (actor.type == EditorSceneActorType::Camera)
        {
            camera = &actor;
            break;
        }
    }
    if (camera == nullptr) return;

    const auto& view = camera->hasViewportCamera ? camera->viewportCamera : camera->camera;
    const Eigen::Vector3f forward = view.forward.normalized();
    const Eigen::Vector3f right = view.right.normalized();
    const Eigen::Vector3f up = view.up.normalized();
    const auto triad = EditorUi::ProjectAxisIndicatorTriad(right, up, forward);

    std::array<size_t, 3> order = {0, 1, 2};
    std::sort(order.begin(), order.end(), [&triad](size_t lhs, size_t rhs)
    {
        return triad[lhs].depth > triad[rhs].depth;
    });

    // 先算量化后的形态签名：相机静止时它不变，于是不写 DOM。
    // 量化到 1 度 / 1 像素，避免浮点末位噪声让"静止帧"也一直重写属性。
    std::ostringstream signature;
    for (const EditorUi::AxisIndicatorProjection& projection : triad)
    {
        float offsetX = 0.0f;
        float offsetY = 0.0f;
        EditorUi::AxisIndicatorLabelOffset(projection, offsetX, offsetY);
        signature << (projection.degenerate ? 'D' : 'A')
                  << std::lround(EditorUi::AxisIndicatorRotationDegrees(projection)) << ','
                  << std::lround(offsetX) << ',' << std::lround(offsetY) << ';';
    }
    for (size_t index : order) signature << index;
    const std::string wanted = signature.str();
    if (wanted == axisIndicatorSignature) return;
    axisIndicatorSignature = wanted;

    static const char* kBarIds[3] = {
        "viewport-axis-bar-x", "viewport-axis-bar-y", "viewport-axis-bar-z"};
    static const char* kLabelIds[3] = {
        "viewport-axis-label-x", "viewport-axis-label-y", "viewport-axis-label-z"};

    // 远的先画、近的后画，让朝向观察者的轴盖住背向的轴。depth 越小越靠近观察者，
    // 因此按 depth 降序排列。
    for (size_t rank = 0; rank < order.size(); ++rank)
    {
        const size_t index = order[rank];
        const EditorUi::AxisIndicatorProjection& projection = triad[index];
        const std::string barId = kBarIds[index];
        const std::string labelId = kLabelIds[index];

        // 先摆好层序，再写形态：index 是稳定的轴身份，rank 是绘制顺序。
        widgetDocument.SetProperty(barId, "z-index", std::to_string(rank + 1));
        widgetDocument.SetProperty(labelId, "z-index",
            std::to_string(order.size() + rank + 1));

        // 固定盒尺寸，只改绘制变换和可见性；margin/display/width 会让转动相机触发整页布局。
        const char* visibility = projection.degenerate ? "hidden" : "visible";
        widgetDocument.SetProperty(barId, "visibility", visibility);
        widgetDocument.SetProperty(labelId, "visibility", visibility);
        if (projection.degenerate)
        {
            // 平行于视线时杆和字母一起隐藏，避免三个字母在锚点重叠。
            widgetDocument.SetProperty(labelId, "transform", "translate(0px, 0px)");
            continue;
        }

        float offsetX = 0.0f;
        float offsetY = 0.0f;
        EditorUi::AxisIndicatorLabelOffset(projection, offsetX, offsetY);
        widgetDocument.SetProperty(labelId, "transform",
            "translate(" + Pixels(offsetX) + ", " + Pixels(offsetY) + ")");
        widgetDocument.SetProperty(barId, "transform",
            std::string("rotate(") +
                std::to_string(EditorUi::AxisIndicatorRotationDegrees(projection)) + "deg)");
    }
}

bool MaterialInstanceAssetEditorPanel::BuildSelectedSceneGizmo()
{
    sceneViewportGizmo = {};
    if (document == nullptr || landscapeToolActive || IsAssetEditorVisible() || HasModal() ||
        viewportTransformTool == "select")
        return false;
    const EditorSceneActorSnapshot* actor = SelectedActor();
    if (actor == nullptr || !SceneGizmoSupportsActor(actor->type))
        return false;
    const BlueprintGizmoMode mode = SceneGizmoMode(viewportTransformTool);
    if (!SceneGizmoSupportsMode(actor->type, mode))
        return false;
    const EditorSceneCameraActor* view = nullptr;
    for (const EditorSceneActorSnapshot& candidate : OutlinerActors())
        if (candidate.type == EditorSceneActorType::Camera)
        {
            view = candidate.hasViewportCamera ? &candidate.viewportCamera : &candidate.camera;
            break;
        }
    auto* image = document->GetElementById("editor-viewport-image");
    if (view == nullptr || image == nullptr || !image->IsVisible(true))
        return false;
    const auto size = image->GetBox().GetSize(Rml::BoxArea::Content);
    if (size.x <= 0 || size.y <= 0)
        return false;

    const Eigen::Vector3f position = sceneGizmoDragging ? sceneGizmoPosition : actor->position;
    const Eigen::Vector3f rotation = sceneGizmoDragging ? sceneGizmoRotation : actor->rotation;
    const Eigen::Vector3f scale = sceneGizmoDragging ? sceneGizmoScale : actor->scale;
    // Gizmo 的原点始终是 actor 世界位置；Local 只改变三轴朝向。
    // 父矩阵只携带原点与局部旋转，position 传 0，避免把作者位置投射两次。
    Eigen::Quaternionf parentRotation = Eigen::Quaternionf::Identity();
    Eigen::Matrix4f parentWorld = Eigen::Matrix4f::Identity();
    parentWorld.block<3, 1>(0, 3) = position;
    if (viewportTransformSpace == "local")
    {
        parentRotation = SceneGizmoRotation(rotation);
        parentWorld.block<3, 3>(0, 0) = parentRotation.toRotationMatrix();
    }
    BlueprintViewportGizmo gizmo = BuildBlueprintViewportGizmo(*view, size.x, size.y,
        parentWorld, parentRotation, Eigen::Vector3f::Zero(), Eigen::Vector3f::Zero(),
        scale, mode);
    if (!gizmo.valid) return false;
    gizmo.initialValue = mode == BlueprintGizmoMode::Translate ? position :
        mode == BlueprintGizmoMode::Rotate ? rotation : scale;
    sceneViewportGizmo = gizmo;
    return true;
}

void MaterialInstanceAssetEditorPanel::RefreshSceneGizmo()
{
    if (document == nullptr) return;
    if (!sceneGizmoMarkupMounted)
    {
        // 固定线段/命中槽位只挂载一次；相机移动和拖动只更新 transform/visibility，
        // 避免每帧重建整段 DOM 触发布局。
        std::string markup;
        for (size_t axis = 0; axis < 3; ++axis)
            for (size_t index = 0; index < 48; ++index)
            {
                const std::string suffix = std::to_string(axis) + "-" + std::to_string(index);
                markup += "<div id=\"scene-gizmo-line-" + suffix +
                    "\" class=\"actor-visual-line\"></div>";
                markup += "<div id=\"scene-gizmo-hit-" + suffix +
                    "\" data-action=\"scene-gizmo-axis\" data-axis=\"" +
                    std::to_string(axis) + "\"></div>";
            }
        SetMarkup("viewport-gizmo-overlay", std::move(markup));
        widgetDocument.SetProperty("viewport-gizmo-overlay", "position", "absolute");
        widgetDocument.SetProperty("viewport-gizmo-overlay", "left", "0px");
        widgetDocument.SetProperty("viewport-gizmo-overlay", "top", "0px");
        widgetDocument.SetProperty("viewport-gizmo-overlay", "width", "100%");
        widgetDocument.SetProperty("viewport-gizmo-overlay", "height", "100%");
        widgetDocument.SetProperty("viewport-gizmo-overlay", "pointer-events", "none");
        widgetDocument.SetProperty("viewport-gizmo-capture", "position", "absolute");
        widgetDocument.SetProperty("viewport-gizmo-capture", "left", "0px");
        widgetDocument.SetProperty("viewport-gizmo-capture", "top", "0px");
        widgetDocument.SetProperty("viewport-gizmo-capture", "width", "100%");
        widgetDocument.SetProperty("viewport-gizmo-capture", "height", "100%");
        widgetDocument.SetProperty("viewport-gizmo-capture", "z-index", "2");
        widgetDocument.SetProperty("viewport-gizmo-capture", "pointer-events", "auto");
        sceneGizmoMarkupMounted = true;
    }
    const bool visible = BuildSelectedSceneGizmo();
    static const char* colors[] = {"#e42a1f", "#2fd93f", "#3a5cfa"};
    for (size_t axis = 0; axis < 3; ++axis)
        for (size_t index = 0; index < 48; ++index)
        {
            const std::string suffix = std::to_string(axis) + "-" + std::to_string(index);
            const std::string lineId = "scene-gizmo-line-" + suffix;
            const std::string hitId = "scene-gizmo-hit-" + suffix;
            const auto& lines = sceneViewportGizmo.axes[axis].lines;
            const bool lineVisible = visible && index < lines.size();
            widgetDocument.SetProperty(lineId, "visibility", lineVisible ? "visible" : "hidden");
            widgetDocument.SetProperty(hitId, "visibility", lineVisible ? "visible" : "hidden");
            // 命中由视图内的像素距离完成，和蓝图页一致；DOM 只负责画线。
            // 若让 hit div 拦截指针，场景视口左键会绕过 Gizmo 命中测试。
            widgetDocument.SetProperty(hitId, "pointer-events", "none");
            if (!lineVisible) continue;
            const auto& line = lines[index];
            const Eigen::Vector2f delta = line.end - line.begin;
            const std::string transform = "translate(" + Number(line.begin.x()) + "px, " +
                Number(line.begin.y()) + "px) rotate(" +
                Number(std::atan2(delta.y(), delta.x()) * 180.0f / 3.14159265358979323846f) +
                "deg) scaleX(" + Number(delta.norm()) + ")";
            widgetDocument.SetProperty(lineId, "transform", transform);
            widgetDocument.SetProperty(lineId, "background-color", colors[axis]);
            widgetDocument.SetProperty(hitId, "position", "absolute");
            widgetDocument.SetProperty(hitId, "left", "0px");
            widgetDocument.SetProperty(hitId, "top", "-6px");
            widgetDocument.SetProperty(hitId, "width", "1px");
            widgetDocument.SetProperty(hitId, "height", "12px");
            widgetDocument.SetProperty(hitId, "transform-origin", "0px 0px");
            widgetDocument.SetProperty(hitId, "transform", transform);
            widgetDocument.SetProperty(hitId, "background-color", "#00000000");
            widgetDocument.SetProperty(hitId, "z-index", "1");
        }
    widgetDocument.SetProperty("viewport-gizmo-capture", "display",
        sceneGizmoDragging ? "block" : "none");
}

void MaterialInstanceAssetEditorPanel::SynchronizeSceneGizmoSelection()
{
    if (!sceneGizmoDragging) return;
    const EditorSceneActorSnapshot* actor = SelectedActor();
    const bool current = actor != nullptr &&
        actor->worldGeneration == sceneGizmoGeneration &&
        actor->identity == sceneGizmoIdentity && !IsAssetEditorVisible();
    if (current) return;
    // 同一 World 内切换/删除 actor 时恢复旧对象的 live 预览；World 换代时
    // 旧代数命令本来就应被拒绝，直接丢弃本地手势，不把旧值写到新关卡。
    CancelSceneGizmoDrag(actor != nullptr &&
        actor->worldGeneration == sceneGizmoGeneration);
}

UiAction MaterialInstanceAssetEditorPanel::SceneGizmoAction(bool preview) const
{
    UiAction action;
    action.type = UiActionType::SetActorProperty;
    auto& edit = action.actorProperty;
    edit.worldGeneration = sceneGizmoGeneration;
    edit.identity = sceneGizmoIdentity;
    edit.property = EditorActorProperty::Transform;
    edit.preview = preview;
    edit.transformPosition = {sceneGizmoPosition.x(), sceneGizmoPosition.y(), sceneGizmoPosition.z()};
    edit.transformRotation = {sceneGizmoRotation.x(), sceneGizmoRotation.y(), sceneGizmoRotation.z()};
    edit.transformScale = {sceneGizmoScale.x(), sceneGizmoScale.y(), sceneGizmoScale.z()};
    return action;
}

bool MaterialInstanceAssetEditorPanel::BeginSceneGizmoDrag(size_t axis,
    const Eigen::Vector2f& pixel, const Eigen::Vector2f& offset)
{
    const EditorSceneActorSnapshot* actor = SelectedActor();
    if (actor == nullptr || !sceneViewportGizmo.valid ||
        axis >= sceneViewportGizmo.axes.size() ||
        !sceneViewportGizmo.axes[axis].visible)
        return false;
    sceneGizmoDragging = true;
    sceneGizmoAxis = axis;
    sceneGizmoStart = pixel;
    sceneGizmoOffset = offset;
    sceneGizmoGeneration = actor->worldGeneration;
    sceneGizmoIdentity = actor->identity;
    sceneDragGizmo = sceneViewportGizmo;
    sceneGizmoOriginalPosition = sceneGizmoPosition = actor->position;
    sceneGizmoOriginalRotation = sceneGizmoRotation = actor->rotation;
    sceneGizmoOriginalScale = sceneGizmoScale = actor->scale;
    RefreshSceneGizmo();
    return true;
}

bool MaterialInstanceAssetEditorPanel::UpdateSceneGizmoDrag(const Eigen::Vector2f& pixel)
{
    if (!sceneGizmoDragging || !sceneDragGizmo.valid) return false;
    Eigen::Vector3f value;
    const BlueprintGizmoMode mode = SceneGizmoMode(viewportTransformTool);
    const float previousRotation = sceneGizmoRotation[sceneGizmoAxis];
    if (!DragBlueprintViewportGizmo(sceneDragGizmo, sceneGizmoAxis, sceneGizmoStart,
        pixel, value, previousRotation))
        return false;
    if (sceneSnapEnabled)
    {
        const float step = mode == BlueprintGizmoMode::Translate ? sceneMoveSnap :
            mode == BlueprintGizmoMode::Rotate ? sceneRotateSnap : sceneScaleSnap;
        Eigen::Vector3f snapped;
        if (SnapBlueprintViewportValue(mode, sceneGizmoAxis,
            sceneDragGizmo.initialValue, value, step, snapped))
            value = snapped;
        else value = sceneDragGizmo.initialValue;
    }
    const EditorSceneActorSnapshot* actor = SelectedActor();
    // 刚体根只接受统一缩放；与蓝图编辑器保持同一规则。
    if (mode == BlueprintGizmoMode::Scale && actor != nullptr &&
        actor->type == EditorSceneActorType::Script && !actor->scriptPhysics.is_null())
        value = Eigen::Vector3f::Constant(value[sceneGizmoAxis]);

    sceneGizmoPosition = sceneGizmoOriginalPosition;
    sceneGizmoRotation = sceneGizmoOriginalRotation;
    sceneGizmoScale = sceneGizmoOriginalScale;
    if (mode == BlueprintGizmoMode::Translate) sceneGizmoPosition = value;
    else if (mode == BlueprintGizmoMode::Rotate) sceneGizmoRotation = value;
    else sceneGizmoScale = value;
    if (runtimeActionSink != nullptr)
        runtimeActionSink->RequestRuntimeAction(SceneGizmoAction(true));
    RefreshSceneGizmo();
    return true;
}

void MaterialInstanceAssetEditorPanel::CommitSceneGizmoDrag()
{
    if (!sceneGizmoDragging) return;
    const bool changed =
        !sceneGizmoPosition.isApprox(sceneGizmoOriginalPosition, 1.0e-6f) ||
        !sceneGizmoRotation.isApprox(sceneGizmoOriginalRotation, 1.0e-6f) ||
        !sceneGizmoScale.isApprox(sceneGizmoOriginalScale, 1.0e-6f);
    const UiAction commit = SceneGizmoAction(false);
    sceneGizmoDragging = false;
    sceneGizmoIdentity.clear();
    if (changed && runtimeActionSink != nullptr)
        runtimeActionSink->RequestRuntimeAction(commit);
    RefreshSceneGizmo();
}

void MaterialInstanceAssetEditorPanel::CancelSceneGizmoDrag(bool restore)
{
    if (!sceneGizmoDragging)
    {
        sceneGizmoAxis = 0;
        sceneGizmoIdentity.clear();
        return;
    }
    sceneGizmoPosition = sceneGizmoOriginalPosition;
    sceneGizmoRotation = sceneGizmoOriginalRotation;
    sceneGizmoScale = sceneGizmoOriginalScale;
    if (restore && runtimeActionSink != nullptr)
        runtimeActionSink->RequestRuntimeAction(SceneGizmoAction(true));
    sceneGizmoDragging = false;
    sceneGizmoIdentity.clear();
    RefreshSceneGizmo();
}

bool MaterialInstanceAssetEditorPanel::ProcessSceneGizmoInput(Rml::Event& event)
{
    const auto& type = event.GetType();
    if (type == "keydown" && event.GetParameter<int>("key_identifier", 0) == Rml::Input::KI_ESCAPE)
    {
        if (!sceneGizmoDragging) return false;
        CancelSceneGizmoDrag();
        return true;
    }
    if (document == nullptr) return false;
    auto* image = document->GetElementById("editor-viewport-image");
    if (image == nullptr) return false;
    const auto offset = image->GetAbsoluteOffset(Rml::BoxArea::Content);
    const bool left = event.GetParameter<int>("button", -1) == 0;
    const Eigen::Vector2f pixel(event.GetParameter<float>("mouse_x", 0) - offset.x,
        event.GetParameter<float>("mouse_y", 0) - offset.y);
    if (sceneGizmoDragging)
    {
        // 拖动期间透明 capture 覆盖视口，所有鼠标事件都归本手势；
        // 释放/取消前绝不进入场景拾取或相机导航。
        if (type == "mousemove") UpdateSceneGizmoDrag(pixel);
        else if (type == "mouseup" && left)
        {
            UpdateSceneGizmoDrag(pixel);
            CommitSceneGizmoDrag();
        }
        if (type == "mousedown" || type == "mouseup" || type == "mousemove")
            return true;
    }
    if (type != "mousedown" || !left) return false;
    // 关卡视口的左键通常直接交给游戏/场景拾取；Gizmo 必须先在图像像素上
    // 做一次命中测试，命中时才消费这次按下。这里与蓝图页共用同一数学。
    if (event.GetTargetElement() == image && viewportTransformTool != "select")
    {
        if (const auto axis = HitBlueprintViewportGizmo(sceneViewportGizmo, pixel))
            return BeginSceneGizmoDrag(*axis, pixel,
                Eigen::Vector2f(offset.x, offset.y));
        return false;
    }
    Rml::Element* target = event.GetTargetElement();
    while (target != nullptr && target != document &&
        !target->HasAttribute("data-action"))
        target = target->GetParentNode();
    if (target == nullptr || target == document ||
        target->GetAttribute<std::string>("data-action", "") != "scene-gizmo-axis")
        return false;
    const size_t axis = target->GetAttribute<size_t>("data-axis", sceneViewportGizmo.axes.size());
    return BeginSceneGizmoDrag(axis, pixel, Eigen::Vector2f(offset.x, offset.y));
}

bool MaterialInstanceAssetEditorPanel::ProcessViewportShortcut(Rml::Event& event)
{
    if (event.GetType() != "keydown" || document == nullptr ||
        IsAssetEditorVisible() || HasModal()) return false;
    auto* focus = document->GetContext()->GetFocusElement();
    if (dynamic_cast<Rml::ElementFormControl*>(focus) != nullptr) return false;
    const int key = event.GetParameter<int>("key_identifier", 0);
    const char* tool = nullptr;
    if (key == Rml::Input::KI_Q) tool = "select";
    else if (key == Rml::Input::KI_W) tool = "move";
    else if (key == Rml::Input::KI_E) tool = "rotate";
    else if (key == Rml::Input::KI_R) tool = "scale";
    if (tool == nullptr) return false;
    if (sceneGizmoDragging) CancelSceneGizmoDrag();
    if (landscapeToolActive)
    {
        landscapeToolActive = false;
        landscapePaintMode = false;
        terrainPanelMode = TerrainPanelMode::Layers;
        CancelTerrainBrushStroke();
        ClearTerrainBrushHit();
    }
    viewportTransformTool = tool;
    RefreshViewportToolbar();
    RefreshSceneGizmo();
    return true;
}

bool MaterialInstanceAssetEditorPanel::ProcessTerrainSculptPointer(
    const std::string& type, int button, float mouseX, float mouseY)
{
    if (!IsTerrainSculptActive()) return false;
    if (landscapePaintMode)
    {
        const EditorSceneActorSnapshot* actor = SelectedTerrainActor();
        if (actor == nullptr || actor->terrainPlan == nullptr ||
            actor->terrainPlan->desc.layers.empty())
            return false;
    }
    if (type == "mouseleave")
    {
        ClearTerrainBrushHit();
        return false;
    }
    const bool left = button == 0;
    if (type == "mousemove")
    {
        UpdateTerrainBrushHit(mouseX, mouseY);
        if (terrainBrushStrokeActive)
        {
            if (terrainBrushHit)
            {
                if (landscapePaintMode)
                    SendTerrainPaintAction(TerrainBrushOperation::ApplyBrush, true);
                else SendTerrainBrushAction(TerrainBrushOperation::ApplyBrush, true);
            }
            return true;
        }
        return false;
    }
    if (type == "mousedown" && left)
    {
        UpdateTerrainBrushHit(mouseX, mouseY);
        if (!terrainBrushStrokeActive && terrainBrushHit)
        {
            terrainBrushStrokeActive = true;
            if (landscapePaintMode)
                SendTerrainPaintAction(TerrainBrushOperation::BeginStroke, true);
            else SendTerrainBrushAction(TerrainBrushOperation::BeginStroke, true);
            RefreshTerrainBrushUi();
        }
        // Landscape 左键永远不落到场景拾取/相机导航；无命中时只是空点击。
        return true;
    }
    if (type == "mouseup" && left)
    {
        if (terrainBrushStrokeActive) EndTerrainBrushStroke();
        return true;
    }
    return false;
}

// 工具条激活态只在真正变化时写一次 class；静止帧不重复触碰 RmlUi 样式，也不重建 markup。
void MaterialInstanceAssetEditorPanel::SetViewportToolbarClass(const std::string& id, bool active)
{
    widgetDocument.SetClass(id, "active", active);
}

void MaterialInstanceAssetEditorPanel::ToggleMenu(const std::string& menu)
{
    openMenu = openMenu == menu ? std::string() : menu;
    ApplyMenuState();
}

void MaterialInstanceAssetEditorPanel::CloseMenus()
{
    openMenu.clear();
    ApplyMenuState();
}

void MaterialInstanceAssetEditorPanel::ApplyMenuState()
{
    for (const char* menu : {"file", "edit", "window", "help", "view", "debug-draw"})
        widgetDocument.SetClass("menu-" + std::string(menu), "open", openMenu == menu);
}

bool MaterialInstanceAssetEditorPanel::IsInsideMenuBar(const Rml::Element* element) const
{
    if (document == nullptr) return false;
    const auto* menuBar = document->GetElementById("editor-menubar");
    for (const auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current == menuBar || current->GetId() == "menu-view" || current->GetId() == "menu-debug-draw") return true;
    return false;
}

bool MaterialInstanceAssetEditorPanel::IsInsideContextMenu(const Rml::Element* element) const
{
    if (document == nullptr) return false;
    const auto* menu = document->GetElementById("editor-context-menu");
    for (const auto* current = element; current != nullptr && current != document;
        current = current->GetParentNode())
        if (current == menu) return true;
    return false;
}

// RmlUi 6.2 没有 contextmenu 事件：右键只表现为带 button=1 的 mousedown/mouseup。
// 这里在右键抬起时打开菜单，一是抬起不会打断将来可能出现的右键拖动，
// 二是抬起时的指针位置就是用户指向的位置。目标在这一刻解析成稳定地址。
void MaterialInstanceAssetEditorPanel::OpenContextMenu(Rml::Event& event)
{
    if (document == nullptr) return;
    ContextMenuKind kind = ContextMenuKind::None;
    std::string target;
    std::string title;
    Rml::Element* element = event.GetTargetElement();
    if (element != nullptr && IsSceneViewportElement(element))
    {
        // 视口没有拾取后端，因此菜单里只有视口级动作，不含任何逐对象条目。
        kind = ContextMenuKind::Viewport;
        title = "Viewport";
    }
    else
    {
        for (Rml::Element* current = element; current != nullptr && current != document;
            current = current->GetParentNode())
        {
            // 树行是 flex 容器：折叠符、标题与行尾眼睛是三个并列靶面，行内空白不属于任何一个。
            // 命中 .tree-row 时统一改读行标题按钮的地址，因此整行右键都指向同一行。
            Rml::Element* addressed = current->IsClassSet("tree-row")
                ? current->QuerySelector(".list-item") : current;
            const std::string action = addressed == nullptr ? std::string()
                : addressed->GetAttribute<std::string>("data-action", "");
            // 大纲 actor 行的三个点击靶面（行标题、折叠符、行尾眼睛）携带同一个 actor 下标，
            // 因此在行的任何位置右键都指向同一个 actor；分组行没有真实动作，不提供菜单。
            if (action == "outliner-actor" || action == "outliner-expand" ||
                action == "outliner-visible" || action == "outliner-slot")
            {
                // Play/Pause 下场景对象归蓝图运行时所有，大纲不再提供编辑菜单。
                if (!SceneEditingActive()) break;
                const size_t index = addressed->GetAttribute<size_t>("data-index", 0);
                if (index >= OutlinerActors().size()) break;
                const EditorSceneActorSnapshot& actor = OutlinerActors()[index];
                kind = ContextMenuKind::Actor;
                target = actor.identity;
                title = actor.displayName;
                break;
            }
            if (action == "browser-item")
            {
                // DOM 上的 data-path 是当前挂载节点的完整资产地址（不是虚拟窗口下标）。
                const std::string path = addressed->GetAttribute<std::string>("data-path", "");
                const auto className = contentBrowser.BlueprintClass(path);
                if (!className.empty())
                {
                    kind = ContextMenuKind::BlueprintClass;
                    target = className;
                    title = className;
                    break;
                }
                EditorAssetKind assetKind = EditorAssetKind::MaterialInstance;
                std::string name;
                // 目录节点在只读目录里没有条目：目录没有真实可执行动作，因此不给菜单。
                if (path.empty() || !FindCatalogAssetKind(path, assetKind, name)) break;
                kind = ContextMenuKind::BrowserAsset;
                target = path;
                title = name;
                break;
            }
        }
    }
    contextMenuKind = kind;
    contextMenuTarget = target;
    if (kind == ContextMenuKind::None)
    {
        CloseContextMenu();
        return;
    }
    // 目标有稳定地址不等于它有可执行动作（例如相机只有逐对象动作之外的空集合）：
    // 空菜单一律不打开，避免出现一个"什么都没有"的弹层。
    std::string items = ContextMenuItemsMarkup();
    if (items.empty())
    {
        CloseContextMenu();
        return;
    }
    const Rml::Vector2f point = event.GetUnprojectedMouseScreenPos();
    contextMenuX = point.x;
    contextMenuY = point.y;
    SetMarkup("editor-context-menu-title", Text(title));
    SetMarkup("editor-context-menu-items", std::move(items));
    ApplyContextMenuState();
}

void MaterialInstanceAssetEditorPanel::CloseContextMenu()
{
    contextMenuKind = ContextMenuKind::None;
    contextMenuTarget.clear();
    ApplyContextMenuState();
}

void MaterialInstanceAssetEditorPanel::ApplyContextMenuState()
{
    widgetDocument.SetClass("editor-context-menu", "open", IsContextMenuOpen());
    widgetDocument.SetProperty("editor-context-menu", "left", Pixels(contextMenuX));
    widgetDocument.SetProperty("editor-context-menu", "top", Pixels(contextMenuY));
}

// 条目在打开时生成一次：菜单存活期间快照可能换代，因此动作执行时一律按稳定地址
// 重新查目标，查不到就给出诊断，而不是退回面板的当前选择。
std::string MaterialInstanceAssetEditorPanel::ContextMenuItemsMarkup() const
{
    Panel::ContextMenuMarkupInput input;
    input.kind = contextMenuKind;
    input.actor = contextMenuKind == ContextMenuKind::Actor ? ContextMenuActor() : nullptr;
    input.target = &contextMenuTarget;
    input.modifiedDocuments = &snapshot.modifiedDocuments;
    input.scriptsAvailable = runtimeView.scripts.available;
    input.scriptState = runtimeView.scripts.state;
    input.debugViewMode = runtimeView.debugViewMode;
    return Panel::ContextMenuItemsMarkup(input);
}

const EditorSceneActorSnapshot* MaterialInstanceAssetEditorPanel::ContextMenuActor() const
{
    return Panel::FindContextMenuActor(contextMenuTarget, snapshot);
}

// 只读目录（缺目录时退回旧 MI 资产列表）里查路径：既是"资产还是目录"的判据，
// 也是菜单标题的名字来源。打开动作仍复用 OpenBrowserAsset 的既有命令路由。
bool MaterialInstanceAssetEditorPanel::FindCatalogAssetKind(const std::string& path,
    EditorAssetKind& kind, std::string& name) const
{
    return Panel::FindCatalogAssetKind(snapshot, path, kind, name);
}

void MaterialInstanceAssetEditorPanel::ToggleContextActorVisibility()
{
    if (!SceneEditingActive()) return;
    Panel::ToggleContextActorVisibility(contextMenuTarget, snapshot,
        [this](UiAction action) {
            if (runtimeActionSink != nullptr) runtimeActionSink->RequestRuntimeAction(std::move(action));
        },
        localStatus);
}

void MaterialInstanceAssetEditorPanel::OpenContextActorMaterial(size_t slotIndex)
{
    if (!SceneEditingActive()) return;
    Panel::OpenContextActorMaterial(contextMenuTarget, slotIndex, snapshot,
        [this](const MaterialInstanceModelSnapshot& model, size_t index) {
            SelectModelMaterial(model, index);
        },
        localStatus);
}

void MaterialInstanceAssetEditorPanel::OpenContextBrowserAsset()
{
    Panel::OpenContextBrowserAsset(contextMenuTarget, snapshot,
        [this](const std::string& path) { OpenBrowserAsset(path); },
        localStatus);
}


} // namespace VL::EditorUi
