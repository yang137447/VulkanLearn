#include "editor/runtime/materialInstanceEditorRuntime.h"
#include "editor/command/editorCommandJsonCodec.h"

#include "editor/service/materialInstanceDocumentService.h"

#include <algorithm>
#include <array>
#include <exception>
#include <fstream>
#include <limits>
#include <type_traits>
#include <utility>

#include <nlohmann/json.hpp>
#include "editor/runtime/materialInstanceEditorRuntimeInternal.h"

namespace VL::Editor
{
void MaterialInstanceEditorRuntime::ApplyServiceResult(
    const MaterialEditorServiceResult& result)
{
    if (result.editor.has_value())
    {
        snapshot = ConvertSnapshot(result.editor.value());
    }
    else if (result.document.has_value() && snapshot.activeDocument.has_value() &&
        snapshot.activeDocument->assetPath == result.document->assetPath)
    {
        // 高频参数编辑只返回当前文档，保留资产目录和场景导航快照。
        snapshot.activeDocument = ConvertDocument(result.document.value());
    }
    else
    {
        RefreshSnapshotFromService();
    }

    if (!result.message.empty())
    {
        lastCommandMessage = FormatServiceMessage(result);
        SetStatus(lastCommandMessage);
    }
    UpdateBusRevision(result);
    ApplyRuntimeOverlay();
}

void MaterialInstanceEditorRuntime::RefreshSnapshotFromService()
{
    if (service == nullptr)
    {
        return;
    }

    try
    {
        snapshot = ConvertSnapshot(service->BuildSnapshot());
    }
    catch (const std::exception& exception)
    {
        diagnostic = exception.what();
        SetStatus(diagnostic);
    }
}

void MaterialInstanceEditorRuntime::ApplyRuntimeOverlay()
{
    if (!snapshot.activeDocument.has_value())
    {
        return;
    }

    EditorUi::MaterialInstanceDocumentSnapshot& document =
        snapshot.activeDocument.value();
    const Preview::MaterialInstancePreviewStatus previewStatus =
        previewController->GetStatus();
    const Preview::MaterialInstancePreviewPathNormalizationResult pathResult =
        Preview::NormalizeMaterialInstancePath(document.assetPath);
    const bool targetsActiveDocument = pathResult.Succeeded() &&
        previewStatus.materialInstancePath.has_value() &&
        previewStatus.materialInstancePath.value() == pathResult.path.value();

    if (!targetsActiveDocument)
    {
        document.preview = EditorUi::EditorPreviewStatus::Unavailable;
        document.previewMessage = PreviewUnavailableMessage;
        if (!worldPath.empty())
        {
            document.previewMessage += " Active World: " + worldPath;
        }
        return;
    }

    document.preview = ToEditorPreviewStatus(previewStatus.state);
    document.previewMessage = previewStatus.diagnosticMessage;
    if (document.previewMessage.empty())
    {
        document.previewMessage = previewStatus.IsConnected()
            ? "Runtime material instance preview is connected."
            : PreviewUnavailableMessage;
    }
}

void MaterialInstanceEditorRuntime::RefreshModifiedDocuments()
{
    snapshot.saveFeedback = saveFeedback;
    snapshot.blueprintCommandFeedback = blueprintCommandFeedback;
    snapshot.particleCommandFeedback = particleCommandFeedback;
    snapshot.modifiedDocuments.clear();
    // 文档条目直接来自 GT service，不依赖当前活动页；保存关卡不切换 MI 或预览 World。
    if (service)
    {
        const auto tabs = service->OpenDocumentEntries();
        for (const auto& tab : tabs)
            if (tab.dirty)
                snapshot.modifiedDocuments.push_back({tab.assetPath, EditorUi::EditorAssetKind::MaterialInstance,
                    tab.revision, ConvertDocumentState(tab.state), tab.message});
        snapshot.documentTabs.clear();
        for (const auto& tab : tabs)
            snapshot.documentTabs.push_back({tab.assetPath, tab.dirty});
        for (auto& asset : snapshot.assets)
        {
            asset.dirty = false;
            for (const auto& tab : tabs)
                if (tab.assetPath == asset.assetPath) asset.dirty = tab.dirty;
        }
    }
    if (assetDocuments)
    {
        snapshot.blueprintDocuments = assetDocuments->BlueprintDocuments().DocumentSnapshots();
        snapshot.particleDocuments =
            assetDocuments->ParticleDocuments().DocumentSnapshots();
        snapshot.terrainDocuments = assetDocuments->TerrainDocuments().DocumentSnapshots();
        snapshot.tressfxDocuments = assetDocuments->TressfxDocuments().DocumentSnapshots();
        const auto scenes = assetDocuments->ModifiedDocuments();
        // GT 作者适配器绕过命令执行修改 Scene 草稿；发布保存列表时必须同步总线版本，
        // 否则上一次 Save 留下的 revision 会拒绝之后的合法保存。
        for (const auto& document : scenes)
            commandBus->SetDocumentRevision(document.assetPath, document.revision);
        snapshot.modifiedDocuments.insert(snapshot.modifiedDocuments.end(), scenes.begin(), scenes.end());
    }
}

void MaterialInstanceEditorRuntime::RecordSaveFeedback(
    const EditorCommandEnvelope& command, const EditorCommandResult& result)
{
    if (command.type != EditorCommandType::SaveSceneDocument &&
        command.type != EditorCommandType::SaveMeshDocument &&
        command.type != EditorCommandType::SaveTressfxDocument &&
        command.type != EditorCommandType::SaveTextureDocument &&
        command.type != EditorCommandType::SaveBlueprintDocument &&
        command.type != EditorCommandType::SaveParticleDocument &&
        command.type != EditorCommandType::SaveTerrainDocument &&
        command.type != EditorCommandType::SaveMaterialInstanceDocument) return;
    const auto path = GetEditorCommandAssetPath(command);
    if (!path) return;
    EditorUi::EditorSaveFeedback feedback{++saveSequence, *path,
        result.status == EditorCommandStatus::Succeeded, result.message};
    for (auto& previous : saveFeedback)
        if (previous.assetPath == *path) { previous = std::move(feedback); return; }
    saveFeedback.push_back(std::move(feedback));
}

void MaterialInstanceEditorRuntime::RecordBlueprintCommandFeedback(
    const EditorCommandEnvelope& command, const EditorCommandResult& result)
{
    if (command.type != EditorCommandType::ModifyBlueprintDocument &&
        command.type != EditorCommandType::UndoBlueprintDocument &&
        command.type != EditorCommandType::RedoBlueprintDocument) return;
    const auto path = GetEditorCommandAssetPath(command);
    if (!path) return;
    // Tick 完整 drain 后才发布 UI；每路径保留本轮最后终态，无需为每次鼠标手势累积结果列表。
    EditorUi::EditorBlueprintCommandFeedback feedback{++blueprintCommandSequence, *path,
        result.status == EditorCommandStatus::Succeeded};
    for (auto& previous : blueprintCommandFeedback)
        if (previous.assetPath == *path) { previous = std::move(feedback); return; }
    blueprintCommandFeedback.push_back(std::move(feedback));
}

void MaterialInstanceEditorRuntime::RecordParticleCommandFeedback(
    const EditorCommandEnvelope& command,
    const EditorCommandResult& result)
{
    switch (command.type)
    {
    case EditorCommandType::ModifyParticleDocument:
    case EditorCommandType::SaveParticleDocument:
    case EditorCommandType::RevertParticleDocument:
    case EditorCommandType::ReloadParticleDocument:
    case EditorCommandType::ValidateParticleDocument:
    case EditorCommandType::CloseParticleDocument:
    case EditorCommandType::UndoParticleDocument:
    case EditorCommandType::RedoParticleDocument:
        break;
    default:
        return;
    }
    const auto path = GetEditorCommandAssetPath(command);
    if (!path) return;
    EditorUi::EditorParticleCommandFeedback feedback{
        ++particleCommandSequence, *path, command.type,
        result.status == EditorCommandStatus::Succeeded,
        result.message};
    for (auto& previous : particleCommandFeedback)
        if (previous.assetPath == *path &&
            previous.type == command.type)
        {
            previous = std::move(feedback);
            return;
        }
    particleCommandFeedback.push_back(std::move(feedback));
}

void MaterialInstanceEditorRuntime::UpdateBusRevision(
    const MaterialEditorServiceResult& result)
{
    if (result.document.has_value())
    {
        commandBus->SetDocumentRevision(
            result.document->assetPath,
            result.document->revision);
    }
    if (result.editor.has_value() &&
        result.editor->activeDocument.has_value())
    {
        const auto& document = result.editor->activeDocument.value();
        commandBus->SetDocumentRevision(document.assetPath, document.revision);
    }
}

void MaterialInstanceEditorRuntime::SetStatus(std::string message)
{
    if (message.empty())
    {
        return;
    }
    snapshot.statusMessage = std::move(message);
}

EditorUi::MaterialInstanceEditorSnapshot
MaterialInstanceEditorRuntime::ConvertSnapshot(
    const MaterialEditorSnapshot& source)
{
    EditorUi::MaterialInstanceEditorSnapshot result;
    result.selectedDocumentPath = source.selectedDocumentPath;
    result.statusMessage = source.statusMessage;
    result.browserLoading = source.browserLoading;

    result.assets.reserve(source.assets.size());
    for (const MaterialEditorAssetEntry& asset : source.assets)
    {
        result.assets.push_back(
            EditorUi::MaterialInstanceAssetEntry{asset.assetPath, asset.dirty});
    }

    result.documentTabs.reserve(source.documentTabs.size());
    for (const MaterialEditorAssetEntry& tab : source.documentTabs)
    {
        result.documentTabs.push_back(
            EditorUi::MaterialInstanceAssetEntry{tab.assetPath, tab.dirty});
    }

    result.textureAssets.reserve(source.textureAssets.size());
    for (const MaterialEditorTextureAssetSnapshot& asset : source.textureAssets)
    {
        result.textureAssets.push_back(EditorUi::EditorTextureAssetSnapshot{
            asset.assetPath,
            asset.valid,
            asset.diagnostic});
    }

    if (source.activeDocument.has_value())
    {
        result.activeDocument = ConvertDocument(source.activeDocument.value());
    }
    return result;
}

EditorUi::MaterialInstanceDocumentSnapshot
MaterialInstanceEditorRuntime::ConvertDocument(
    const MaterialEditorDocumentSnapshot& source)
{
    EditorUi::MaterialInstanceDocumentSnapshot result;
    result.assetPath = source.assetPath;
    result.baseMaterialPath = source.baseMaterialPath;
    result.schemaDigest = source.schemaDigest;
    result.revision = source.revision;
    result.status = ConvertDocumentState(source.state);
    result.validation = ConvertValidationState(source.validation);
    result.preview = EditorUi::EditorPreviewStatus::Unavailable;
    result.validationMessage = source.validationMessage;
    result.previewMessage = PreviewUnavailableMessage;

    result.parameters.reserve(source.parameters.size());
    for (const MaterialEditorParameterSnapshot& parameter : source.parameters)
    {
        result.parameters.push_back(ConvertParameter(parameter));
    }

    result.textures.reserve(source.textures.size());
    for (const MaterialEditorTextureBindingSnapshot& texture : source.textures)
    {
        result.textures.push_back(ConvertTexture(texture));
    }
    result.renderStates.reserve(source.renderStates.size());
    for (const MaterialEditorRenderStateSnapshot& renderState : source.renderStates)
    {
        result.renderStates.push_back(ConvertRenderState(renderState));
    }
    return result;
}

EditorUi::EditorParameterSnapshot
MaterialInstanceEditorRuntime::ConvertParameter(
    const MaterialEditorParameterSnapshot& source)
{
    EditorUi::EditorParameterSnapshot result;
    result.name = source.name;
    result.description = source.description;
    result.type = ConvertParameterType(source.type);
    result.defaultValue = ConvertParameterValue(source.defaultValue);
    result.effectiveValue = ConvertParameterValue(source.effectiveValue);
    if (source.overrideValue.has_value())
    {
        result.overrideValue = ConvertParameterValue(source.overrideValue.value());
    }
    result.active = source.active;
    if (source.min.has_value() && source.max.has_value())
    {
        result.hasRange = true;
        result.minValue = source.min.value();
        result.maxValue = source.max.value();
    }

    const std::size_t channelCount = std::min(
        source.channels.size(),
        result.channels.size());
    for (std::size_t index = 0; index < channelCount; ++index)
    {
        const MaterialEditorParameterChannelSnapshot& sourceChannel =
            source.channels[index];
        EditorUi::EditorParameterChannel& resultChannel = result.channels[index];
        resultChannel.name = sourceChannel.name;
        resultChannel.description = sourceChannel.description;
        if (sourceChannel.min.has_value() && sourceChannel.max.has_value())
        {
            resultChannel.hasRange = true;
            resultChannel.minValue = sourceChannel.min.value();
            resultChannel.maxValue = sourceChannel.max.value();
        }
    }
    return result;
}

EditorUi::EditorTextureBindingSnapshot
MaterialInstanceEditorRuntime::ConvertTexture(
    const MaterialEditorTextureBindingSnapshot& source)
{
    EditorUi::EditorTextureBindingSnapshot result;
    result.slotName = source.slotName;
    result.description = source.description;
    result.defaultAssetPath = source.defaultAssetPath;
    result.effectiveAssetPath = source.effectiveAssetPath;
    result.overrideAssetPath = source.overrideAssetPath;
    result.active = source.active;
    return result;
}

EditorUi::EditorRenderStateSnapshot
MaterialInstanceEditorRuntime::ConvertRenderState(
    const MaterialEditorRenderStateSnapshot& source)
{
    EditorUi::EditorRenderStateSnapshot result;
    result.name = source.name;
    result.defaultValue = source.defaultValue;
    result.effectiveValue = source.effectiveValue;
    result.overrideValue = source.overrideValue;
    return result;
}

EditorUi::EditorParameterType MaterialInstanceEditorRuntime::ConvertParameterType(
    EditorMaterialParameterType type)
{
    switch (type)
    {
    case EditorMaterialParameterType::Float:
        return EditorUi::EditorParameterType::Float;
    case EditorMaterialParameterType::Vec2:
        return EditorUi::EditorParameterType::Vec2;
    case EditorMaterialParameterType::Vec3:
        return EditorUi::EditorParameterType::Vec3;
    case EditorMaterialParameterType::Vec4:
        return EditorUi::EditorParameterType::Vec4;
    case EditorMaterialParameterType::Color:
        return EditorUi::EditorParameterType::Color;
    }
    return EditorUi::EditorParameterType::Float;
}

EditorUi::EditorParameterValue MaterialInstanceEditorRuntime::ConvertParameterValue(
    const EditorMaterialParameterValue& value)
{
    EditorUi::EditorParameterValue result;
    std::visit(
        [&result](const auto& sourceValue)
        {
            using ValueType = std::decay_t<decltype(sourceValue)>;
            if constexpr (std::is_same_v<ValueType, float>)
            {
                result.type = EditorUi::EditorParameterType::Float;
                result.values[0] = sourceValue;
            }
            else if constexpr (std::is_same_v<ValueType, EditorVec2>)
            {
                result.type = EditorUi::EditorParameterType::Vec2;
                std::copy(sourceValue.begin(), sourceValue.end(), result.values.begin());
            }
            else if constexpr (std::is_same_v<ValueType, EditorVec3>)
            {
                result.type = EditorUi::EditorParameterType::Vec3;
                std::copy(sourceValue.begin(), sourceValue.end(), result.values.begin());
            }
            else if constexpr (std::is_same_v<ValueType, EditorColor>)
            {
                result.type = EditorUi::EditorParameterType::Color;
                std::copy(
                    sourceValue.components.begin(),
                    sourceValue.components.end(),
                    result.values.begin());
            }
            else
            {
                result.type = EditorUi::EditorParameterType::Vec4;
                std::copy(sourceValue.begin(), sourceValue.end(), result.values.begin());
            }
        },
        value);
    return result;
}

EditorUi::EditorDocumentStatus MaterialInstanceEditorRuntime::ConvertDocumentState(
    MaterialEditorDocumentState state)
{
    switch (state)
    {
    case MaterialEditorDocumentState::Clean:
        return EditorUi::EditorDocumentStatus::Clean;
    case MaterialEditorDocumentState::Dirty:
        return EditorUi::EditorDocumentStatus::Dirty;
    case MaterialEditorDocumentState::SaveFailed:
        return EditorUi::EditorDocumentStatus::SaveFailed;
    case MaterialEditorDocumentState::SourceChanged:
        return EditorUi::EditorDocumentStatus::SourceChanged;
    }
    return EditorUi::EditorDocumentStatus::SaveFailed;
}

EditorUi::EditorValidationStatus
MaterialInstanceEditorRuntime::ConvertValidationState(
    MaterialEditorValidationState state)
{
    switch (state)
    {
    case MaterialEditorValidationState::Unknown:
        return EditorUi::EditorValidationStatus::Unknown;
    case MaterialEditorValidationState::Valid:
        return EditorUi::EditorValidationStatus::Valid;
    case MaterialEditorValidationState::Invalid:
        return EditorUi::EditorValidationStatus::Invalid;
    }
    return EditorUi::EditorValidationStatus::Unknown;
}

} // namespace VL::Editor
