#include "editor/ui/materialInstanceAssetEditorPanel.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
#include <imgui.h>
#endif

namespace VL::EditorUi
{

uint32_t EditorParameterValue::ComponentCount() const
{
    return static_cast<uint32_t>(type) + 1u;
}

namespace
{
const char* TypeName(EditorParameterType type)
{
    switch (type)
    {
    case EditorParameterType::Float: return "float";
    case EditorParameterType::Vec2: return "vec2";
    case EditorParameterType::Vec3: return "vec3";
    case EditorParameterType::Vec4: return "vec4";
    }
    return "unknown";
}

uint32_t ParameterComponentCount(EditorParameterType type)
{
    switch (type)
    {
    case EditorParameterType::Float: return 1;
    case EditorParameterType::Vec2: return 2;
    case EditorParameterType::Vec3: return 3;
    case EditorParameterType::Vec4: return 4;
    }
    return 1;
}

const char* DefaultComponentName(uint32_t componentIndex)
{
    static constexpr std::array<const char*, 4> names = {"x", "y", "z", "w"};
    return componentIndex < names.size() ? names[componentIndex] : "value";
}

struct SliderRange
{
    float minValue = -1.0f;
    float maxValue = 1.0f;
};

SliderRange ResolveSliderRange(
    const EditorParameterSnapshot& parameter,
    const EditorParameterChannel& channel,
    float currentValue)
{
    if (channel.hasRange && std::isfinite(channel.minValue) &&
        std::isfinite(channel.maxValue) && channel.minValue <= channel.maxValue)
    {
        return {channel.minValue, channel.maxValue};
    }
    if (parameter.hasRange && std::isfinite(parameter.minValue) &&
        std::isfinite(parameter.maxValue) && parameter.minValue <= parameter.maxValue)
    {
        return {parameter.minValue, parameter.maxValue};
    }

    // 旧 M_ 文件没有范围时仍使用滑块，并根据当前值生成可编辑的稳定兜底范围。
    const float magnitude = std::abs(currentValue);
    float extent = std::max(1.0f, magnitude * 2.0f);
    if (!std::isfinite(extent)) extent = 1.0f;
    return {-extent, extent};
}

const char* DocStatus(EditorDocumentStatus status)
{
    switch (status)
    {
    case EditorDocumentStatus::Clean: return "Clean";
    case EditorDocumentStatus::Dirty: return "Modified";
    case EditorDocumentStatus::Saving: return "Saving";
    case EditorDocumentStatus::SaveFailed: return "Save failed";
    case EditorDocumentStatus::SourceChanged: return "Source changed";
    }
    return "Closed";
}

const char* ValidationStatus(EditorValidationStatus status)
{
    switch (status)
    {
    case EditorValidationStatus::Valid: return "Valid";
    case EditorValidationStatus::Invalid: return "Invalid";
    case EditorValidationStatus::Running: return "Checking";
    }
    return "Unknown";
}

const char* PreviewStatus(EditorPreviewStatus status)
{
    switch (status)
    {
    case EditorPreviewStatus::Connected: return "Connected";
    case EditorPreviewStatus::Applying: return "Applying";
    case EditorPreviewStatus::Unavailable: return "Unavailable";
    case EditorPreviewStatus::Failed: return "Failed";
    }
    return "Disconnected";
}

bool Matches(const std::string& text, const char* filter)
{
    return filter == nullptr || *filter == '\0' || text.find(filter) != std::string::npos;
}

Eigen::Vector3f ToEigenVector(const std::array<float, 3>& value)
{
    return Eigen::Vector3f(value[0], value[1], value[2]);
}

VL::Editor::Selection::MaterialInstanceModelContext ToSelectionModel(
    const MaterialInstanceModelSnapshot& model)
{
    VL::Editor::Selection::MaterialInstanceModelContext result;
    result.worldGeneration = model.worldGeneration;
    result.scenePath = model.scenePath;
    result.objectId = model.objectId;
    result.displayName = model.displayName;
    result.objectIdentity = model.objectIdentity;
    result.worldBoundsMin = ToEigenVector(model.worldBoundsMin);
    result.worldBoundsMax = ToEigenVector(model.worldBoundsMax);
    result.hasWorldBounds = result.worldBoundsMin.allFinite() &&
        result.worldBoundsMax.allFinite() &&
        !(result.worldBoundsMin.array() > result.worldBoundsMax.array()).any();
    result.materials.reserve(model.materials.size());
    for (const MaterialInstanceModelMaterialSnapshot& material : model.materials)
    {
        VL::Editor::Selection::MaterialInstanceModelMaterial selectionMaterial;
        selectionMaterial.objectId = material.objectId == 0
            ? model.objectId
            : material.objectId;
        selectionMaterial.materialSlotIndex = material.materialSlotIndex;
        selectionMaterial.materialSlotName = material.materialSlotName;
        selectionMaterial.materialInstancePath = material.materialInstancePath;
        selectionMaterial.displayName = material.materialSlotName;
        selectionMaterial.worldBoundsMin = ToEigenVector(material.worldBoundsMin);
        selectionMaterial.worldBoundsMax = ToEigenVector(material.worldBoundsMax);
        selectionMaterial.hasWorldBounds = selectionMaterial.worldBoundsMin.allFinite() &&
            selectionMaterial.worldBoundsMax.allFinite() &&
            !(selectionMaterial.worldBoundsMin.array() > selectionMaterial.worldBoundsMax.array()).any();
        result.materials.push_back(std::move(selectionMaterial));
    }
    return result;
}

VL::Editor::Selection::MaterialInstanceSelection ToSelection(
    const MaterialInstanceModelSnapshot& model,
    const MaterialInstanceModelMaterialSnapshot& material)
{
    VL::Editor::Selection::MaterialInstanceSelection result;
    result.worldGeneration = model.worldGeneration;
    result.scenePath = model.scenePath;
    result.objectId = material.objectId == 0 ? model.objectId : material.objectId;
    result.objectIdentity = model.objectIdentity;
    result.materialSlotIndex = material.materialSlotIndex;
    result.materialSlotName = material.materialSlotName;
    result.materialInstancePath = material.materialInstancePath;
    result.worldBoundsMin = ToEigenVector(material.worldBoundsMin);
    result.worldBoundsMax = ToEigenVector(material.worldBoundsMax);
    result.hasWorldBounds = result.worldBoundsMin.allFinite() &&
        result.worldBoundsMax.allFinite() &&
        !(result.worldBoundsMin.array() > result.worldBoundsMax.array()).any();
    result.modelContext = ToSelectionModel(model);
    return result;
}

VL::EditorMaterialParameterType ToCommandType(EditorParameterType type)
{
    switch (type)
    {
    case EditorParameterType::Float: return VL::EditorMaterialParameterType::Float;
    case EditorParameterType::Vec2: return VL::EditorMaterialParameterType::Vec2;
    case EditorParameterType::Vec3: return VL::EditorMaterialParameterType::Vec3;
    case EditorParameterType::Vec4: return VL::EditorMaterialParameterType::Vec4;
    }
    return VL::EditorMaterialParameterType::Float;
}

VL::EditorMaterialParameterValue ToCommandValue(const EditorParameterValue& value)
{
    switch (value.type)
    {
    case EditorParameterType::Float:
        return value.values[0];
    case EditorParameterType::Vec2:
        return VL::EditorVec2{value.values[0], value.values[1]};
    case EditorParameterType::Vec3:
        return VL::EditorVec3{value.values[0], value.values[1], value.values[2]};
    case EditorParameterType::Vec4:
        return VL::EditorVec4{value.values[0], value.values[1], value.values[2], value.values[3]};
    }
    return 0.0f;
}

EditorCommandEnvelope MakePathCommand(
    VL::EditorCommandType type,
    const std::string& assetPath,
    std::optional<uint64_t> revision = std::nullopt)
{
    EditorCommandEnvelope command;
    command.type = type;
    command.expectedDocumentRevision = revision;
    command.payload = VL::MaterialInstanceAssetPathPayload{assetPath};
    if (type == VL::EditorCommandType::ResetMaterialInstanceOverrides)
    {
        command.payload = VL::ResetMaterialInstanceOverridesPayload{
            assetPath,
            VL::EditorResetScope::All};
    }
    return command;
}

EditorCommandEnvelope MakeSetParameterCommand(
    const MaterialInstanceDocumentSnapshot& document,
    const EditorParameterSnapshot& parameter,
    const EditorParameterValue& value)
{
    EditorCommandEnvelope command;
    command.type = VL::EditorCommandType::SetMaterialParameterOverride;
    command.expectedDocumentRevision = document.revision;
    command.payload = VL::SetMaterialParameterOverridePayload{
        document.assetPath,
        parameter.name,
        ToCommandType(value.type),
        ToCommandValue(value)};
    return command;
}

EditorCommandEnvelope MakeClearParameterCommand(
    const MaterialInstanceDocumentSnapshot& document,
    const std::string& parameter)
{
    EditorCommandEnvelope command;
    command.type = VL::EditorCommandType::ClearMaterialParameterOverride;
    command.expectedDocumentRevision = document.revision;
    command.payload = VL::ClearMaterialParameterOverridePayload{
        document.assetPath,
        parameter};
    return command;
}

EditorCommandEnvelope MakeSetTextureCommand(
    const MaterialInstanceDocumentSnapshot& document,
    const std::string& slot,
    const std::string& textureAssetPath)
{
    EditorCommandEnvelope command;
    command.type = VL::EditorCommandType::SetMaterialTextureOverride;
    command.expectedDocumentRevision = document.revision;
    command.payload = VL::SetMaterialTextureOverridePayload{
        document.assetPath,
        slot,
        textureAssetPath};
    return command;
}

EditorCommandEnvelope MakeClearTextureCommand(
    const MaterialInstanceDocumentSnapshot& document,
    const std::string& slot)
{
    EditorCommandEnvelope command;
    command.type = VL::EditorCommandType::ClearMaterialTextureOverride;
    command.expectedDocumentRevision = document.revision;
    command.payload = VL::ClearMaterialTextureOverridePayload{
        document.assetPath,
        slot};
    return command;
}

EditorCommandEnvelope MakeSetRenderStateCommand(
    const MaterialInstanceDocumentSnapshot& document,
    VL::EditorMaterialRenderStateField field,
    VL::EditorMaterialRenderStateValue value)
{
    EditorCommandEnvelope command;
    command.type = VL::EditorCommandType::SetMaterialRenderStateOverride;
    command.expectedDocumentRevision = document.revision;
    command.payload = VL::SetMaterialRenderStateOverridePayload{
        document.assetPath,
        field,
        std::move(value)};
    return command;
}

EditorCommandEnvelope MakeClearRenderStateCommand(
    const MaterialInstanceDocumentSnapshot& document,
    VL::EditorMaterialRenderStateField field)
{
    EditorCommandEnvelope command;
    command.type = VL::EditorCommandType::ClearMaterialRenderStateOverride;
    command.expectedDocumentRevision = document.revision;
    command.payload = VL::ClearMaterialRenderStateOverridePayload{
        document.assetPath,
        field};
    return command;
}

} // namespace

MaterialInstanceAssetEditorPanel::MaterialInstanceAssetEditorPanel(IEditorCommandSink* sink)
    : commandSink(sink)
{
}

void MaterialInstanceAssetEditorPanel::SetCommandSink(IEditorCommandSink* sink)
{
    commandSink = sink;
}

void MaterialInstanceAssetEditorPanel::SetSceneSelectionSink(
    IMaterialInstanceSceneSelectionSink* sink)
{
    sceneSelectionSink = sink;
}

void MaterialInstanceAssetEditorPanel::SetSnapshot(const MaterialInstanceEditorSnapshot& value)
{
    snapshot = value;
    if (!snapshot.activeDocument ||
        snapshot.activeDocument->assetPath != pendingParameterEditAssetPath ||
        snapshot.activeDocument->revision != pendingParameterEditRevision)
    {
        pendingParameterEditAssetPath.clear();
        pendingParameterEditRevision = 0;
        pendingParameterEdits.clear();
    }
    if (!snapshot.activeDocument)
    {
        openTexturePicker = false;
        pickerSlot.clear();
    }
}

void MaterialInstanceAssetEditorPanel::SetVisible(bool value)
{
    visible = value;
}

void MaterialInstanceAssetEditorPanel::Build(const MaterialInstanceEditorSnapshot& value)
{
    SetSnapshot(value);
}

void MaterialInstanceAssetEditorPanel::Submit(EditorCommandEnvelope command)
{
    if (command.commandId == 0) command.commandId = nextCommandId++;
    if (commandSink == nullptr)
    {
        localStatus = "Command sink is not connected; no business state was changed.";
        return;
    }
    localStatus = "Submitted command #" + std::to_string(command.commandId);
    commandSink->Submit(std::move(command));
}

#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI

void MaterialInstanceAssetEditorPanel::Render()
{
    if (!visible) return;
    if (!ImGui::Begin("Material Instance Asset Editor", &visible))
    {
        ImGui::End();
        return;
    }
    RenderContents();
    ImGui::End();
}

void MaterialInstanceAssetEditorPanel::RenderContents()
{
    if (!visible) return;
    if (ImGui::BeginTable("MIEditorLayout", 2,
        ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Asset Navigation", ImGuiTableColumnFlags_WidthFixed, 300.0f);
        ImGui::TableSetupColumn("Asset Details", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); RenderNavigationContents();
        ImGui::TableSetColumnIndex(1); RenderInspectorContents();
        ImGui::EndTable();
    }
}

void MaterialInstanceAssetEditorPanel::RenderNavigationContents()
{
    RenderSceneModels();
    ImGui::Separator();
    RenderSelectedModelMaterials();
}

void MaterialInstanceAssetEditorPanel::RenderSceneModels()
{
    ImGui::SeparatorText("Scene Models");
    if (snapshot.sceneModels.empty())
    {
        ImGui::TextDisabled("No mesh models in the active scene.");
        return;
    }

    ImGui::BeginChild("MISceneModelList", ImVec2(0.0f, 180.0f), ImGuiChildFlags_Borders);
    for (const MaterialInstanceModelSnapshot& model : snapshot.sceneModels)
    {
        const bool selected = snapshot.selectedModel.has_value() &&
            snapshot.selectedModel->objectId == model.objectId &&
            model.objectId != 0;
        const std::string label = (model.displayName.empty()
            ? model.objectIdentity
            : model.displayName) + "###scene_model_" +
            std::to_string(model.objectId);
        if (ImGui::Selectable(label.c_str(), selected) &&
            sceneSelectionSink != nullptr)
        {
            sceneSelectionSink->RequestModelSelection(ToSelectionModel(model));
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        {
            ImGui::SetTooltip("%s\n%s", model.displayName.c_str(), model.objectIdentity.c_str());
        }
    }
    ImGui::EndChild();
}

void MaterialInstanceAssetEditorPanel::RenderInspectorContents()
{
    // 检查器 Dock 复用 Build 阶段冻结的快照，避免多窗口重复拉取运行时状态。
    RenderToolbar();
    ImGui::Separator();
    RenderTabs();
    RenderDetails();
    RenderStatusBar();
}

void MaterialInstanceAssetEditorPanel::RenderStatusBar()
{
    ImGui::Separator();
    if (!localStatus.empty()) ImGui::TextWrapped("%s", localStatus.c_str());
    if (!snapshot.statusMessage.empty()) ImGui::TextWrapped("%s", snapshot.statusMessage.c_str());
    if (snapshot.activeDocument)
    {
        const auto& document = *snapshot.activeDocument;
        ImGui::Text("Document: %s | Validation: %s | Preview: %s",
            DocStatus(document.status), ValidationStatus(document.validation), PreviewStatus(document.preview));
    }
}

void MaterialInstanceAssetEditorPanel::RenderToolbar()
{
    if (!snapshot.activeDocument.has_value())
    {
        ImGui::TextDisabled("No document");
        return;
    }

    const MaterialInstanceDocumentSnapshot& document =
        snapshot.activeDocument.value();
    // 窄 Inspector 中按操作阶段分行，避免按钮依赖窗口宽度硬挤在一行。
    if (ImGui::Button("Save") || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, ImGuiInputFlags_RouteFocused))
        Submit(MakePathCommand(
            VL::EditorCommandType::SaveMaterialInstanceDocument,
            document.assetPath,
            document.revision));
    ImGui::SameLine();
    if (ImGui::Button("Revert"))
        Submit(MakePathCommand(
            VL::EditorCommandType::RevertMaterialInstanceDocument,
            document.assetPath,
            document.revision));
    ImGui::SameLine();
    if (ImGui::Button("Reset All"))
        Submit(MakePathCommand(
            VL::EditorCommandType::ResetMaterialInstanceOverrides,
            document.assetPath,
            document.revision));

    if (ImGui::Button("Validate"))
        Submit(MakePathCommand(
            VL::EditorCommandType::ValidateMaterialInstanceDocument,
            document.assetPath));
    ImGui::SameLine();
    if (ImGui::Button("Connect Preview"))
        Submit(MakePathCommand(
            VL::EditorCommandType::ConnectMaterialInstancePreview,
            document.assetPath,
            document.revision));

    const bool previewConnected =
        document.preview == EditorPreviewStatus::Connected ||
        document.preview == EditorPreviewStatus::Applying;
    ImGui::BeginDisabled(!previewConnected);
    if (ImGui::Button("Apply Preview"))
    {
        EditorCommandEnvelope command;
        command.type = VL::EditorCommandType::ApplyMaterialInstancePreview;
        command.expectedDocumentRevision = document.revision;
        command.payload = VL::ApplyMaterialInstancePreviewPayload{
            document.assetPath,
            document.revision};
        Submit(std::move(command));
    }
    ImGui::SameLine();
    if (ImGui::Button("Restore Baseline"))
        Submit(MakePathCommand(
            VL::EditorCommandType::RestoreMaterialInstancePreviewBaseline,
            document.assetPath,
            document.revision));
    ImGui::EndDisabled();
    ImGui::TextDisabled("Status: %s", DocStatus(document.status));
}

void MaterialInstanceAssetEditorPanel::RenderSelectedModelMaterials()
{
    ImGui::SeparatorText("Selected Model Materials");
    if (!snapshot.selectedModel.has_value())
    {
        ImGui::TextWrapped("Click a model in the 3D viewport to inspect its material instances.");
        return;
    }

    const MaterialInstanceModelSnapshot& model = snapshot.selectedModel.value();
    ImGui::TextWrapped("Object: %s", model.displayName.empty()
        ? model.objectIdentity.c_str()
        : model.displayName.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Scene: %s", model.scenePath.c_str());
    ImGui::PopStyleColor();
    ImGui::BeginChild("MIAssetList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    for (std::size_t index = 0; index < model.materials.size(); ++index)
    {
        const MaterialInstanceModelMaterialSnapshot& material = model.materials[index];
        bool dirty = false;
        for (const MaterialInstanceAssetEntry& tab : snapshot.documentTabs)
        {
            if (tab.assetPath == material.materialInstancePath)
            {
                dirty = tab.dirty;
                break;
            }
        }

        const std::string slotLabel = material.materialSlotName.empty()
            ? "Slot " + std::to_string(material.materialSlotIndex)
            : material.materialSlotName;
        const std::string label = slotLabel + (dirty ? " *" : "") +
            "###selected_model_material_" + std::to_string(index);
        ImGui::PushID(static_cast<int>(index));
        const bool selected = snapshot.selectedMaterialSlotIndex.has_value() &&
            snapshot.selectedModel.has_value() &&
            snapshot.selectedModel->objectId == model.objectId &&
            snapshot.selectedMaterialSlotIndex.value() == material.materialSlotIndex;
        const bool documentSelected = !snapshot.selectedMaterialSlotIndex.has_value() &&
            material.materialInstancePath == snapshot.selectedDocumentPath;
        if (ImGui::Selectable(
                label.c_str(),
                selected || documentSelected))
        {
            if (sceneSelectionSink != nullptr)
            {
                sceneSelectionSink->RequestMaterialSelection(ToSelection(model, material));
            }
            EditorCommandEnvelope command;
            command.type = VL::EditorCommandType::OpenMaterialInstanceAsset;
            command.payload = VL::OpenMaterialInstanceAssetPayload{
                material.materialInstancePath,
                EditorNavigationOrigin{
                    model.scenePath,
                    model.objectIdentity,
                    material.materialSlotIndex,
                    material.materialSlotName}};
            Submit(std::move(command));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", material.materialInstancePath.c_str());
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
    if (model.materials.empty()) ImGui::TextDisabled("Selected model has no material instance bindings.");
    ImGui::EndChild();
}

void MaterialInstanceAssetEditorPanel::RenderTabs()
{
    ImGui::SeparatorText("Documents");
    if (!ImGui::BeginTabBar("MIAssetDocuments")) return;
    for (const auto& tab : snapshot.documentTabs)
    {
        std::string label = tab.assetPath + (tab.dirty ? " *" : "") + "###" + tab.assetPath;
        if (ImGui::BeginTabItem(label.c_str()))
        {
            if (tab.assetPath != snapshot.selectedDocumentPath)
                Submit(MakePathCommand(
                    VL::EditorCommandType::SelectMaterialInstanceDocument,
                    tab.assetPath));
            ImGui::EndTabItem();
        }
    }
    ImGui::EndTabBar();
}

void MaterialInstanceAssetEditorPanel::RenderDetails()
{
    if (!snapshot.activeDocument)
    {
        ImGui::TextDisabled("Open an MI asset to view Details.");
        return;
    }
    const auto& document = *snapshot.activeDocument;
    ImGui::Text("MI Asset: %s", document.assetPath.c_str());
    if (ImGui::CollapsingHeader("General", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Text("M_ schema: %s", document.baseMaterialPath.c_str());
        ImGui::Text("Schema digest: %s", document.schemaDigest.c_str());
        ImGui::Text("Revision: %llu", static_cast<unsigned long long>(document.revision));
    }
    if (ImGui::CollapsingHeader("Render States", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::TextDisabled(
            "These overrides can change shader variants, passes, and pipeline state.");
        if (ImGui::BeginTable("MIRenderStates", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("State", ImGuiTableColumnFlags_WidthFixed, 140.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();
            for (const auto& renderState : document.renderStates)
            {
                RenderRenderState(document, renderState);
            }
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Parameters", ImGuiTreeNodeFlags_DefaultOpen))
    {
        ImGui::Checkbox("Show only active parameters", &showOnlyActive);
        ImGui::SameLine();
        ImGui::TextDisabled(
            "Edit directly; highlighted Reset restores the M_ default.");
        if (ImGui::BeginTable("MIParameters", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Parameter", ImGuiTableColumnFlags_WidthFixed, 180.0f);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableHeadersRow();
            for (const auto& parameter : document.parameters)
                if (!showOnlyActive || parameter.active) RenderParameter(document, parameter);
            ImGui::EndTable();
        }
    }
    if (ImGui::CollapsingHeader("Texture Bindings", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (ImGui::BeginTable("MITextures", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 120.0f);
            ImGui::TableSetupColumn("T_ asset", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthFixed, 92.0f);
            ImGui::TableHeadersRow();
            for (const auto& texture : document.textures) RenderTexture(document, texture);
            ImGui::EndTable();
        }
    }
}

void MaterialInstanceAssetEditorPanel::RenderRenderState(
    const MaterialInstanceDocumentSnapshot& document,
    const EditorRenderStateSnapshot& state)
{
    ImGui::PushID(state.name.c_str());
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::TextUnformatted(state.name.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
    {
        ImGui::SetTooltip(
            "Default: %s\n%s",
            state.defaultValue.c_str(),
            state.overrideValue.has_value() ? "MI override" : "M_ default");
    }

    ImGui::TableSetColumnIndex(1);
    ImGui::SetNextItemWidth(-FLT_MIN);
    bool changed = false;
    if (state.name == "renderMode")
    {
        static constexpr std::array<VL::EditorMaterialRenderMode, 9> options = {{
            VL::EditorMaterialRenderMode::Opaque,
            VL::EditorMaterialRenderMode::OpaqueClip,
            VL::EditorMaterialRenderMode::ForwardOpaque,
            VL::EditorMaterialRenderMode::ForwardEyeInner,
            VL::EditorMaterialRenderMode::ForwardEyeCornea,
            VL::EditorMaterialRenderMode::TransparentAlphaBlend,
            VL::EditorMaterialRenderMode::TransparentAlphaBlendWriteDepth,
            VL::EditorMaterialRenderMode::TransparentAdditive,
            VL::EditorMaterialRenderMode::ThinTranslucent}};
        if (ImGui::BeginCombo("##render_state", state.effectiveValue.c_str()))
        {
            for (const auto option : options)
            {
                const std::string optionName =
                    std::string(VL::GetEditorMaterialRenderModeName(option));
                const bool selected = optionName == state.effectiveValue;
                if (ImGui::Selectable(optionName.c_str(), selected))
                {
                    Submit(MakeSetRenderStateCommand(
                        document,
                        VL::EditorMaterialRenderStateField::RenderMode,
                        option));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    else if (state.name == "cullMode")
    {
        static constexpr std::array<VL::EditorMaterialCullMode, 3> options = {{
            VL::EditorMaterialCullMode::Back,
            VL::EditorMaterialCullMode::Front,
            VL::EditorMaterialCullMode::None}};
        if (ImGui::BeginCombo("##render_state", state.effectiveValue.c_str()))
        {
            for (const auto option : options)
            {
                const std::string optionName =
                    std::string(VL::GetEditorMaterialCullModeName(option));
                const bool selected = optionName == state.effectiveValue;
                if (ImGui::Selectable(optionName.c_str(), selected))
                {
                    Submit(MakeSetRenderStateCommand(
                        document,
                        VL::EditorMaterialRenderStateField::CullMode,
                        option));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    else if (state.name == "shadingModel")
    {
        static constexpr std::array<VL::EditorMaterialShadingModel, 11> options = {{
            VL::EditorMaterialShadingModel::DefaultLit,
            VL::EditorMaterialShadingModel::Unlit,
            VL::EditorMaterialShadingModel::Subsurface,
            VL::EditorMaterialShadingModel::PreintegratedSkin,
            VL::EditorMaterialShadingModel::ClearCoat,
            VL::EditorMaterialShadingModel::SubsurfaceProfile,
            VL::EditorMaterialShadingModel::TwoSidedFoliage,
            VL::EditorMaterialShadingModel::Hair,
            VL::EditorMaterialShadingModel::Cloth,
            VL::EditorMaterialShadingModel::Eye,
            VL::EditorMaterialShadingModel::ThinTranslucent}};
        if (ImGui::BeginCombo("##render_state", state.effectiveValue.c_str()))
        {
            for (const auto option : options)
            {
                const std::string optionName =
                    std::string(VL::GetEditorMaterialShadingModelName(option));
                const bool selected = optionName == state.effectiveValue;
                if (ImGui::Selectable(optionName.c_str(), selected))
                {
                    Submit(MakeSetRenderStateCommand(
                        document,
                        VL::EditorMaterialRenderStateField::ShadingModel,
                        option));
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    (void)changed;

    ImGui::TableSetColumnIndex(2);
    const bool overridden = state.overrideValue.has_value();
    ImGui::BeginDisabled(!overridden);
    if (overridden)
    {
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    }
    if (ImGui::SmallButton("Reset"))
    {
        const auto field = state.name == "renderMode"
            ? VL::EditorMaterialRenderStateField::RenderMode
            : state.name == "cullMode"
                ? VL::EditorMaterialRenderStateField::CullMode
                : VL::EditorMaterialRenderStateField::ShadingModel;
        Submit(MakeClearRenderStateCommand(document, field));
    }
    if (overridden) ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::PopID();
}

void MaterialInstanceAssetEditorPanel::RenderParameter(
    const MaterialInstanceDocumentSnapshot& document, const EditorParameterSnapshot& parameter)
{
    ImGui::PushID(parameter.name.c_str());
    const bool overridden = parameter.overrideValue.has_value();
    EditorParameterValue edited = parameter.effectiveValue;
    if (pendingParameterEditAssetPath == document.assetPath &&
        pendingParameterEditRevision == document.revision)
    {
        const auto pending = pendingParameterEdits.find(parameter.name);
        if (pending != pendingParameterEdits.end())
        {
            edited = pending->second;
        }
    }
    bool changed = false;
    bool editFinished = false;

    const uint32_t componentCount = ParameterComponentCount(parameter.type);
    for (uint32_t componentIndex = 0; componentIndex < componentCount; ++componentIndex)
    {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (componentIndex == 0)
        {
            ImGui::TextUnformatted(parameter.name.c_str());
            if (!parameter.active) { ImGui::SameLine(); ImGui::TextDisabled("(inactive)"); }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                ImGui::SetTooltip(
                    "Type: %s\nSource: %s\n%s",
                    TypeName(parameter.type),
                    overridden ? "MI override" : "M_ default",
                    parameter.description.c_str());
        }

        ImGui::TableSetColumnIndex(1);
        const EditorParameterChannel& channel = parameter.channels[componentIndex];
        const char* componentName = channel.name.empty()
            ? DefaultComponentName(componentIndex)
            : channel.name.c_str();
        ImGui::TextDisabled("%s", componentName);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::PushID(static_cast<int>(componentIndex));
        const SliderRange range = ResolveSliderRange(
            parameter, channel, edited.values[componentIndex]);
        if (ImGui::SliderFloat(
                "##value",
                &edited.values[componentIndex],
                range.minValue,
                range.maxValue,
                "%.3f",
                ImGuiSliderFlags_AlwaysClamp))
        {
            changed = true;
        }
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            editFinished = true;
        }
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
        {
            const char* description = channel.description.empty()
                ? parameter.description.c_str()
                : channel.description.c_str();
            if (*description != '\0') ImGui::SetTooltip("%s", description);
        }
        ImGui::PopID();

        ImGui::TableSetColumnIndex(2);
        if (componentIndex == 0)
        {
            ImGui::BeginDisabled(!overridden);
            if (overridden)
            {
                ImGui::PushStyleColor(
                    ImGuiCol_Button,
                    ImGui::GetStyleColorVec4(ImGuiCol_Header));
                ImGui::PushStyleColor(
                    ImGuiCol_ButtonHovered,
                    ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
                ImGui::PushStyleColor(
                    ImGuiCol_ButtonActive,
                    ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
            }
            if (ImGui::SmallButton("Reset"))
                Submit(MakeClearParameterCommand(document, parameter.name));
            if (overridden)
            {
                ImGui::PopStyleColor(3);
            }
            ImGui::EndDisabled();
        }
    }

    if (changed)
    {
        pendingParameterEditAssetPath = document.assetPath;
        pendingParameterEditRevision = document.revision;
        pendingParameterEdits[parameter.name] = edited;
    }

    const auto pending = pendingParameterEdits.find(parameter.name);
    if (editFinished && (changed || pending != pendingParameterEdits.end()) &&
        std::all_of(edited.values.begin(), edited.values.begin() + edited.ComponentCount(),
            [](float value) { return std::isfinite(value); }))
    {
        Submit(MakeSetParameterCommand(document, parameter, edited));
    }
    ImGui::PopID();
}

void MaterialInstanceAssetEditorPanel::RenderTexture(
    const MaterialInstanceDocumentSnapshot& document, const EditorTextureBindingSnapshot& texture)
{
    ImGui::PushID(texture.slotName.c_str()); ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
    const bool overridden = texture.overrideAssetPath.has_value();
    ImGui::TextWrapped("%s", texture.slotName.c_str());
    if (!texture.active) { ImGui::SameLine(); ImGui::TextDisabled("(inactive)"); }
    ImGui::TableSetColumnIndex(1);
    ImGui::TextWrapped("%s",
        texture.effectiveAssetPath.empty() ? "<unbound>" : texture.effectiveAssetPath.c_str());
    ImGui::TableSetColumnIndex(2);
    if (ImGui::SmallButton("Browse"))
    {
        // 选择 T_ 资产直接提交 Set 命令，由文档服务建立 sparse override。
        pickerSlot = texture.slotName;
        openTexturePicker = true;
        ImGui::OpenPopup("MITexturePicker");
    }
    ImGui::BeginDisabled(texture.effectiveAssetPath.empty());
    if (ImGui::SmallButton("Open"))
    {
        EditorCommandEnvelope command;
        command.type = VL::EditorCommandType::OpenTextureAsset;
        command.payload = VL::OpenTextureAssetPayload{
            document.assetPath,
            texture.effectiveAssetPath};
        Submit(std::move(command));
    }
    ImGui::EndDisabled();
    ImGui::BeginDisabled(!overridden);
    if (overridden)
    {
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImGui::GetStyleColorVec4(ImGuiCol_Header));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImGui::GetStyleColorVec4(ImGuiCol_HeaderHovered));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImGui::GetStyleColorVec4(ImGuiCol_HeaderActive));
    }
    if (ImGui::SmallButton("Reset"))
        Submit(MakeClearTextureCommand(document, texture.slotName));
    if (overridden) ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (openTexturePicker && ImGui::BeginPopup("MITexturePicker"))
    {
        ImGui::Text("Choose validated T_ asset for %s", pickerSlot.c_str());
        ImGui::InputTextWithHint("##texture_search", "Search T_*.json", textureSearch.data(), textureSearch.size());
        for (const auto& asset : snapshot.textureAssets)
        {
            if (!asset.valid || !Matches(asset.assetPath, textureSearch.data())) continue;
            if (ImGui::Selectable(asset.assetPath.c_str()))
            {
                Submit(MakeSetTextureCommand(
                    document,
                    pickerSlot,
                    asset.assetPath));
                openTexturePicker = false; ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

#else

void MaterialInstanceAssetEditorPanel::Render()
{
}

void MaterialInstanceAssetEditorPanel::RenderContents()
{
}

void MaterialInstanceAssetEditorPanel::RenderNavigationContents()
{
}

void MaterialInstanceAssetEditorPanel::RenderInspectorContents()
{
}

#endif

} // namespace VL::EditorUi
