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

bool MaterialInstanceAssetEditorPanel::IsActiveHairPreview() const
{
    const auto* active = resourceEditor.ActiveTab();
    return active != nullptr && active->kind == EditorAssetKind::Tressfx &&
        active->path == hairPreviewPath && !hairPreviewBlocked;
}

void MaterialInstanceAssetEditorPanel::RequestHairPreviewRotation(float deltaX, float deltaY)
{
    if (runtimeActionSink == nullptr || hairPreviewBlocked || deltaX == 0.0f) return;
    const uint64_t generation = CurrentModelPreviewGeneration();
    if (!generation) return;
    // 累计角度由 UI 维护，GT 只写绝对 TRS；World 使用原始场景身份 preview_model。
    hairRotation.Drag(deltaX, deltaY);
    UiAction action;
    action.type = UiActionType::RotatePreviewModel;
    action.previewRotation = {generation, hairRotation.yaw, 0.0f};
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

void MaterialInstanceAssetEditorPanel::RequestHairReset()
{
    // 复位不是重载：渲染线程在下一个正 delta 帧走一次 ResetPositions，
    // 20 MB TFX 不会被重新解析。
    if (runtimeActionSink == nullptr || !IsActiveHairPreview()) return;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active ? TressfxDocument(active->path) : nullptr;
    if (!authored || !TressfxGpuSimulationDefaults(authored->document)) return;
    if (!CurrentModelPreviewGeneration()) return;
    UiAction action;
    action.type = UiActionType::ResetHairSimulation;
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}


const EditorAssetDocumentResultPayload* MaterialInstanceAssetEditorPanel::TressfxDocument(
    const std::string& path) const
{
    for (const auto& entry : snapshot.tressfxDocuments)
        if (entry.assetPath == path) return &entry;
    return nullptr;
}

const EditorAssetDocumentResultPayload* MaterialInstanceAssetEditorPanel::ResourceDocument(
    const std::string& path) const
{
    const auto* blueprint = BlueprintDocument(path);
    return blueprint != nullptr ? blueprint : TressfxDocument(path);
}

void MaterialInstanceAssetEditorPanel::SubmitResourceDocumentCommand(
    EditorCommandType type, const std::string& path)
{
    // 共用关闭/保存状态机，但 revision 始终来自该 kind 唯一的作者 owner。
    const auto* authored = ResourceDocument(path);
    if (!authored) return;
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = authored->documentRevision;
    if (GetEditorAssetOperation(type) == EditorAssetOperation::Reload)
        command.payload = ReloadMaterialInstanceDocumentPayload{path, EditorDirtyDocumentPolicy::RequireClean};
    else command.payload = MaterialInstanceAssetPathPayload{path};
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::RequestTressfxDocument(const std::string& path)
{
    if (path.empty() || TressfxDocument(path) || !requestedTressfxDocuments.insert(path).second)
        return;
    // 快照晚于 UI 事件：按路径去重首次读取，不依赖某个 Details 段的可见性。
    EditorCommandEnvelope command;
    command.type = EditorCommandType::GetTressfxDocument;
    command.payload = MaterialInstanceAssetPathPayload{path};
    Submit(std::move(command));
}

void MaterialInstanceAssetEditorPanel::RequestHairSimulationDocument(const std::string& path)
{
    if (path.empty() || (hairSimulationRequested && hairSimulationPath == path)) return;
    hairSimulationPath = path;
    hairSimulationRequested = true;
    hairSimulationBoundRevision = 0;
    hairSimulationSaveRevision = 0;
    hairSimulationPendingSave = false;
    hairSimulationSaveWaitTicks = 0;
    hairSimulationObservedDefaults = nullptr;
    hairSimulationValues = nlohmann::json::object();
    hairSimulationStatus = "Reading simulation values...";
    RequestTressfxDocument(path);
}

// 预览场景 SC_model_preview 自己的 wind 参数。它只改当前模型预览 World，
// 不写关卡或头发资产；因此不会影响其他关卡的风。
std::string HairPreviewWindRows(const EditorUi::EditorSceneActorSnapshot* wind)
{
    std::string rows = "<div class=\"hair-wind-heading\">Preview Wind</div>";
    if (wind == nullptr)
    {
        rows += "<p class=\"hair-sim-note\">Preview scene has no PreviewWind object.</p>";
        return rows;
    }

    std::vector<UiWidgets::NumericComponent> direction;
    for (uint32_t component = 0; component < 3; ++component)
    {
        UiWidgets::NumericComponent entry;
        entry.id = "hair-wind-direction-" + std::to_string(component);
        entry.label = std::string(1, "XYZ"[component]);
        entry.value = "0";
        entry.change = {"hair-preview-wind", 0, component};
        direction.push_back(std::move(entry));
    }
    rows += UiWidgets::PropertyControlRow("Direction",
        UiWidgets::NumericComponents(direction));

    UiWidgets::NumericComponent strength;
    strength.id = "hair-wind-strength-0";
    strength.value = "0";
    strength.change = {"hair-preview-wind", 1, 0};
    strength.hasRange = true;
    strength.minValue = 0.0f;
    strength.maxValue = 3270.0f;
    rows += UiWidgets::PropertyControlRow("Strength (cm/s)",
        UiWidgets::NumericComponents({strength}));

    UiWidgets::NumericComponent gust;
    gust.id = "hair-wind-gust-0";
    gust.value = "0";
    gust.change = {"hair-preview-wind", 2, 0};
    gust.hasRange = true;
    gust.minValue = 0.0f;
    gust.maxValue = 1.0f;
    rows += UiWidgets::PropertyControlRow("Gust",
        UiWidgets::NumericComponents({gust}));
    rows += "<p class=\"hair-sim-note\">Preview scene wind only. "
        "This does not change the current level or save into the hair asset.</p>";
    return rows;
}

void MaterialInstanceAssetEditorPanel::RefreshHairSimulationSection(bool isHair, bool hairBlocked)
{
    const auto* active = resourceEditor.ActiveTab();
    const bool visible = isHair && !hairBlocked && active != nullptr;
    widgetDocument.SetProperty("resource-hair-simulation", "display", visible ? "block" : "none");
    if (!visible)
    {
        hairPreviewWindBound = false;
        hairPreviewWindPath.clear();
        return;
    }
    if (!hairSimulationRequested || hairSimulationPath != active->path)
        RequestHairSimulationDocument(active->path);
    const auto* previewWind = PreviewWindActor();
    if (previewWind == nullptr)
    {
        hairPreviewWindBound = false;
    }
    else if (!hairPreviewWindBound || hairPreviewWindPath != active->path ||
        hairPreviewWindGeneration != previewWind->worldGeneration)
    {
        // 只在新预览 World/资产绑定时取作者值；拖动期间快照晚一帧，不能每帧回写。
        hairPreviewWind = previewWind->worldWind;
        hairPreviewWindPath = active->path;
        hairPreviewWindGeneration = previewWind->worldGeneration;
        hairPreviewWindBound = true;
    }
    const auto* document = TressfxDocument(active->path);
    if (document != nullptr)
    {
        // revision 前进可能来自 Simulation 自己的保存，也可能来自 Groups 保存。
        // 只有 simulationDefaults 本身变化时才覆盖本地草稿，否则“保存 Groups”
        // 会把尚未保存的 Simulation 编辑静默丢掉。
        if (hairSimulationBoundRevision != document->documentRevision)
        {
            const uint64_t previousRevision = hairSimulationBoundRevision;
            hairSimulationBoundRevision = document->documentRevision;
            const nlohmann::json simulation = document->document.contains("simulationDefaults")
                ? document->document.at("simulationDefaults") : nlohmann::json::object();
            const bool changedExternally = hairSimulationObservedDefaults.is_null() ||
                hairSimulationObservedDefaults != simulation;
            if (changedExternally)
            {
                hairSimulationObservedDefaults = simulation;
                std::size_t count = 0;
                const TressfxSimulationControl* controls = TressfxSimulationControls(count);
                nlohmann::json values = nlohmann::json::object();
                for (std::size_t index = 0; index < count; ++index)
                {
                    const auto current = TressfxSimulationValue(simulation, controls[index]);
                    values[controls[index].field] = nlohmann::json::array(
                        {current[0], current[1], current[2]});
                }
                hairSimulationValues = std::move(values);
                // 保存成功后草稿回到 clean：明确提示要重载才会应用作者值并复位模拟。
                hairSimulationStatus = previousRevision != 0 && !document->dirty
                    ? "Defaults saved to the hair asset. Reload to apply inherited values and reset the simulation."
                    : hairSimulationPendingSave ? hairSimulationStatus
                    : "Values loaded from the asset draft.";
            }
        }
        // modify 成功后草稿变 dirty 且 revision 前进；把保存交给既有 Save Content 弹窗，
        // 由它按快照 revision 提交 tressfx.save。候选与草稿已相同的情况不推进 revision，
        // 因此额外用值相等判定解锁，避免停在“正在暂存”。
        if (hairSimulationPendingSave)
        {
            const nlohmann::json simulation = document->document.contains("simulationDefaults")
                ? document->document.at("simulationDefaults") : nlohmann::json::object();
            const bool staged = document->documentRevision > hairSimulationSaveRevision ||
                (document->dirty && TressfxSimulationValuesMatch(simulation, hairSimulationValues));
            if (staged)
            {
                hairSimulationPendingSave = false;
                hairSimulationSaveWaitTicks = 0;
                hairSimulationStatus = "Confirm the save in the Save Content dialog (writes the hair asset).";
                if (pendingClosePath != active->path)
                    OpenSaveDialog(active->path);
            }
            else if (++hairSimulationSaveWaitTicks > 180)
            {
                hairSimulationPendingSave = false;
                hairSimulationSaveWaitTicks = 0;
                hairSimulationStatus = snapshot.statusMessage.empty()
                    ? "Save failed: the asset draft was not modified." : snapshot.statusMessage;
            }
        }
    }
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    std::string rows;
    for (std::size_t index = 0; index < count; ++index)
    {
        std::vector<UiWidgets::NumericComponent> components;
        for (uint32_t component = 0; component < controls[index].components; ++component)
        {
            UiWidgets::NumericComponent entry;
            entry.id = "hair-sim-" + std::to_string(index) + "-" + std::to_string(component);
            entry.label = controls[index].components == 1
                ? std::string() : std::string(1, "XYZ"[component]);
            // markup 只描述固定控件结构；值在挂载后单独同步，避免编辑时替换焦点/拖动节点。
            entry.value = TressfxSimulationText(controls[index].defaults[component]);
            entry.change = {"hair-sim", index, component};
            if (controls[index].hasRange)
            {
                entry.hasRange = true;
                entry.minValue = controls[index].minValue;
                entry.maxValue = controls[index].maxValue;
            }
            components.push_back(std::move(entry));
        }
        std::string label = controls[index].label;
        if (controls[index].unit[0] != '\0')
            label += std::string(" (") + controls[index].unit + ")";
        rows += UiWidgets::PropertyControlRow(label, UiWidgets::NumericComponents(components));
    }
    rows += HairPreviewWindRows(PreviewWindActor());
    rows += "<div class=\"hair-sim-footer\">" +
        UiWidgets::Button("hair-sim-save", {"hair-save"},
            "Save to Hair Asset",
            UiWidgets::ButtonStyle::Command, false, "icons/save.png") +
        "<span id=\"hair-sim-status\" class=\"hair-sim-status\"></span></div>";
    // 这是资产级默认值编辑：保存写 TF_*.json，只影响未覆写这些字段的挂载；实时值只属于
    // 当前预览会话。必须写清，避免作者误以为在改关卡里的这一根头发。
    rows += "<p class=\"hair-sim-note\">Drag previews in this viewport only (not saved). "
        "Save writes the hair asset defaults (TF_*.json): inherited placement values are affected, and reloading "
        "resets the simulation. Placement overrides are edited in the Blueprint asset.</p>";
    SetMarkup("resource-hair-simulation", UiWidgets::CollapsibleSection(
        DetailsSectionId("hair-simulation"), "hair-simulation", "Simulation Defaults", rows,
        {"section"}, IsSectionExpanded("hair-simulation")));
    SetMarkup("hair-sim-status", Text(hairSimulationStatus));
    SetDisabled("hair-sim-save", document == nullptr);
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto values = PanelSimulationValue(hairSimulationValues, controls[index]);
        for (uint32_t component = 0; component < controls[index].components; ++component)
        {
            const std::string id = "hair-sim-" + std::to_string(index) + "-" + std::to_string(component);
            SetControlValue(id, TressfxSimulationText(values[component]));
            if (controls[index].hasRange)
                SetControlValue(id + "-slider", TressfxSimulationText(values[component]));
        }
    }
    if (hairPreviewWindBound)
    {
        for (uint32_t component = 0; component < 3; ++component)
            SetControlValue("hair-wind-direction-" + std::to_string(component),
                SceneActorNumber(hairPreviewWind.direction[component]));
        SetControlValue("hair-wind-strength-0",
            SceneActorNumber(hairPreviewWind.strengthCmPerSec));
        SetControlValue("hair-wind-strength-0-slider",
            SceneActorNumber(hairPreviewWind.strengthCmPerSec));
        SetControlValue("hair-wind-gust-0",
            SceneActorNumber(hairPreviewWind.gustAmplitude));
        SetControlValue("hair-wind-gust-0-slider",
            SceneActorNumber(hairPreviewWind.gustAmplitude));
    }
}

void MaterialInstanceAssetEditorPanel::SubmitHairSimulationLive(std::size_t controlIndex,
    const std::array<double, 3>& values)
{
    if (runtimeActionSink == nullptr || runtimeView.offlineMode || !IsActiveHairPreview()) return;
    const auto* active = resourceEditor.ActiveTab();
    const auto* authored = active ? TressfxDocument(active->path) : nullptr;
    if (!authored || !TressfxGpuSimulationDefaults(authored->document)) return;
    // 预览 World 还没发布时不要投递：命令队列不绑定 World 世代，早到的值会落到
    // 之后出现的同名对象上。
    const uint64_t generation = CurrentModelPreviewGeneration();
    if (!generation) return;
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    if (controlIndex >= count) return;
    UiAction action;
    action.type = UiActionType::SetHairSimulationParameter;
    action.previewHairParameter.worldGeneration = generation;
    action.previewHairParameter.field = controls[controlIndex].field;
    action.previewHairParameter.valueCount = controls[controlIndex].components;
    for (uint32_t component = 0; component < 3; ++component)
        action.previewHairParameter.values[component] = static_cast<float>(values[component]);
    runtimeActionSink->RequestRuntimeAction(std::move(action));
}

void MaterialInstanceAssetEditorPanel::SubmitHairSimulationSave()
{
    const auto* active = resourceEditor.ActiveTab();
    if (active == nullptr) return;
    const auto* document = TressfxDocument(active->path);
    if (document == nullptr)
    {
        // 文档还没打开：先取一次，等快照到达后再提交修改。
        RequestHairSimulationDocument(active->path);
        hairSimulationRequested = true;
        hairSimulationPendingSave = true;
        hairSimulationSaveRevision = 0;
        hairSimulationSaveWaitTicks = 0;
        hairSimulationStatus = "Reading the asset document...";
        return;
    }
    std::size_t count = 0;
    const TressfxSimulationControl* controls = TressfxSimulationControls(count);
    ModifyAssetDocumentPayload payload;
    payload.assetPath = active->path;
    for (std::size_t index = 0; index < count; ++index)
    {
        const auto values = PanelSimulationValue(hairSimulationValues, controls[index]);
        EditorAssetFieldChange change;
        change.field = std::string("simulationDefaults.") + controls[index].field;
        if (controls[index].components == 1)
            change.value = values[0];
        else
            change.value = std::array<double, 3>{values[0], values[1], values[2]};
        payload.changes.push_back(std::move(change));
    }
    EditorCommandEnvelope command;
    command.type = EditorCommandType::ModifyTressfxDocument;
    command.expectedDocumentRevision = document->documentRevision;
    command.payload = std::move(payload);
    Submit(std::move(command));
    hairSimulationPendingSave = true;
    hairSimulationSaveRevision = document->documentRevision;
    hairSimulationSaveWaitTicks = 0;
    hairSimulationStatus = "Staging simulation values...";
}

void MaterialInstanceAssetEditorPanel::SelectHairGroup(int index)
{
    const int count = static_cast<int>(hairGroups.Groups().size());
    if (index < -1 || index >= count) index = -1;
    if (hairBrushStrokeActive) CancelHairBrushStroke(false);
    if (index < 0 && hairGroupPaintActive) CancelHairBrushStroke(true);
    hairGroupSelected = index;
    // 切换目标组时收起逐组表单；range 文本已经由当前控制器生成，不能跨组复用。
    hairGroupAddRangeActive = false;
    hairGroupRangeStart.clear();
    hairGroupRangeEnd.clear();
    hairGroupRangeError.clear();
    hairGroupCreateError.clear();
    hairGroupStatus = index < 0
        ? "Default is the implicit group 0. It cannot be renamed, deleted, edited or used as a paint target."
        : "Preview uses the current Groups draft; Save to Hair Asset persists it.";
    SubmitTressfxGroupPreview();
}

void MaterialInstanceAssetEditorPanel::SubmitTressfxGroupPreview()
{
    if (runtimeActionSink == nullptr || !hairGroupsLoaded)
        return;
    const uint64_t generation = CurrentModelPreviewGeneration();
    if (generation == 0)
        return;
    // binding 必须先于选中/笔刷状态入队，保证同一帧 shader 读到的 group id 表
    // 与 UI 使用的草稿下标一致。
    SubmitTressfxGroupBindingPreview();
    // Default 只是隐式组 0，不应因为“选中行”就触发强高亮；只有命名组
    // 才作为 selected group 提高 tint 强度。
    int32_t selectedGroup = hairGroupSelected < 0
        ? -1 : hairGroupSelected + 1;
    const bool selectedIsPaintable = selectedGroup >= 0 && HairDraftBindingReady();
    if (!selectedIsPaintable) selectedGroup = -1;
    const uint32_t flags =
        (hairGroupColors ? VL::TressfxGroupPreviewColors : 0u) |
        (hairGroupSolo && selectedIsPaintable ? VL::TressfxGroupPreviewSolo : 0u);
    // 绑定预览、切换组或开关时只发一次；World 快照刷新不会重复投递同一状态。
    if (hairGroupPreviewSent && hairGroupPreviewSentGeneration == generation &&
        hairGroupPreviewSentSelected == selectedGroup &&
        hairGroupPreviewSentFlags == flags)
        return;
    UiAction action;
    action.type = UiActionType::SetTressfxGroupPreview;
    action.previewTressfxGroup.worldGeneration = generation;
    action.previewTressfxGroup.selectedGroup = selectedGroup;
    action.previewTressfxGroup.flags = flags;
    runtimeActionSink->RequestRuntimeAction(std::move(action));
    hairGroupPreviewSent = true;
    hairGroupPreviewSentGeneration = generation;
    hairGroupPreviewSentSelected = selectedGroup;
    hairGroupPreviewSentFlags = flags;
}

std::shared_ptr<const VL::Hair::TressfxGroupBinding>
MaterialInstanceAssetEditorPanel::EnsureHairDraftBinding()
{
    if (!hairGroupsLoaded || hairGroups.GuideCount() == 0)
        return nullptr;
    if (hairPaintDraftBinding && hairPaintDraftRevision == hairGroupsDraftRevision)
        return hairPaintDraftBinding;
    try
    {
        std::vector<VL::Hair::TressfxGroupDeclaration> declarations;
        declarations.reserve(hairGroups.Groups().size());
        for (const VL::Editor::TressfxGuideGroupDraft& group : hairGroups.Groups())
            declarations.push_back({group.name, group.guideRanges});
        hairPaintDraftBinding = VL::Hair::BuildTressfxGroupBinding(
            declarations, hairGroups.GuideCount(),
            "editor-draft:" + std::to_string(hairGroupsDraftRevision),
            "TressFX editor draft");
        hairGroupBindingPreviewError.clear();
    }
    catch (const std::exception& exception)
    {
        hairPaintDraftBinding.reset();
        hairGroupBindingPreviewError = exception.what();
        return nullptr;
    }
    hairPaintDraftRevision = hairGroupsDraftRevision;
    return hairPaintDraftBinding;
}

bool MaterialInstanceAssetEditorPanel::HairDraftBindingReady() const
{
    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    return hairGroupBindingPreviewSent && !hairGroupBindingPreviewQueued && hairGroupsLoaded &&
        hairGroupBindingPreviewSentRevision == hairGroupsDraftRevision &&
        hairGroupBindingPreviewSentGeneration == CurrentModelPreviewGeneration() &&
        section != nullptr &&
        hairGroupBindingPreviewSentObject == section->objectName;
}

void MaterialInstanceAssetEditorPanel::SubmitTressfxGroupBindingPreview()
{
    if (runtimeActionSink == nullptr || !hairGroupsLoaded)
    {
        hairGroupBindingPreviewSent = false;
        hairGroupBindingPreviewQueued = false;
        return;
    }
    const uint64_t generation = CurrentModelPreviewGeneration();
    const EditorSceneMeshSectionSnapshot* section = HairBrushSection();
    if (generation == 0 || section == nullptr)
    {
        hairGroupBindingPreviewSent = false;
        hairGroupBindingPreviewQueued = false;
        return;
    }
    const std::shared_ptr<const VL::Hair::TressfxGroupBinding> binding =
        EnsureHairDraftBinding();
    if (!binding)
    {
        hairGroupBindingPreviewSent = false;
        hairGroupBindingPreviewQueued = false;
        if (hairGroupBindingPreviewError.empty())
            hairGroupBindingPreviewError = "The Groups draft is not a valid TressFX binding.";
        hairGroupStatus = "Group preview sync failed: " + hairGroupBindingPreviewError;
        return;
    }
    if (hairGroupBindingPreviewSent &&
        hairGroupBindingPreviewSentRevision == hairGroupsDraftRevision &&
        hairGroupBindingPreviewSentGeneration == generation &&
        hairGroupBindingPreviewSentObject == section->objectName)
    {
        // 至少有了一次后续刷新，且没有新草稿 revision：命令已经离开 UI 队列。
        hairGroupBindingPreviewQueued = false;
        return;
    }
    UiAction action;
    action.type = UiActionType::SetTressfxGroupBindingPreview;
    action.previewTressfxGroupBinding.worldGeneration = generation;
    action.previewTressfxGroupBinding.draftRevision = hairGroupsDraftRevision;
    action.previewTressfxGroupBinding.objectName = section->objectName;
    action.previewTressfxGroupBinding.guideGroupIds = binding->guideGroupIds;
    action.previewTressfxGroupBinding.groupNames = binding->groupNames;
    action.previewTressfxGroupBinding.bindingKey = binding->key;
    runtimeActionSink->RequestRuntimeAction(std::move(action));
    hairGroupBindingPreviewSent = true;
    hairGroupBindingPreviewQueued = true;
    hairGroupBindingPreviewSentRevision = hairGroupsDraftRevision;
    hairGroupBindingPreviewSentGeneration = generation;
    hairGroupBindingPreviewSentObject = section->objectName;
    hairGroupBindingPreviewError.clear();
}


} // namespace VL::EditorUi
