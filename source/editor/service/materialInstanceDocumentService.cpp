#include "editor/service/materialInstanceDocumentService.h"

#include "editor/service/material/materialInstanceDocumentIO.h"
#include "editor/service/material/materialInstanceDocumentPaths.h"
#include "editor/service/material/materialInstanceDocumentValidation.h"
#include "material/materialDescriptorSchema.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace VL::Editor
{
namespace
{

using Json = nlohmann::json;
using PersistenceNumericType =
    Persistence::MaterialInstanceNumericType;
using PersistenceNumericValue =
    Persistence::MaterialInstanceNumericValue;

using MaterialDocumentDetail::HasAssetFileName;
using MaterialDocumentDetail::IsRegularFile;
using MaterialDocumentDetail::ReadStringField;
using MaterialDocumentDetail::ServiceError;
using MaterialDocumentDetail::ThrowServiceError;
using MaterialDocumentDetail::ValidateEditorParameterRange;

std::string RenderStateFieldName(EditorMaterialRenderStateField field)
{
    return std::string(GetEditorMaterialRenderStateFieldName(field));
}

std::string RenderStateValueName(
    EditorMaterialRenderStateField field,
    const EditorMaterialRenderStateValue& value)
{
    if (!IsEditorMaterialRenderStateValueCompatible(field, value))
    {
        ThrowServiceError(
            EditorErrorCode::InvalidPayload,
            "material render state value does not match its field");
    }

    return std::visit(
        [](const auto& typedValue)
        {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, EditorMaterialRenderMode>)
            {
                return std::string(GetEditorMaterialRenderModeName(typedValue));
            }
            else if constexpr (std::is_same_v<ValueType, EditorMaterialCullMode>)
            {
                return std::string(GetEditorMaterialCullModeName(typedValue));
            }
            else
            {
                return std::string(GetEditorMaterialShadingModelName(typedValue));
            }
        },
        value);
}

std::optional<float> ReadOptionalRangeValue(
    const Json& object,
    std::string_view field)
{
    if (!object.contains(std::string(field)))
    {
        return std::nullopt;
    }
    const Json& value = object.at(std::string(field));
    if (!value.is_number())
    {
        return std::nullopt;
    }
    const float result = value.get<float>();
    if (!std::isfinite(result))
    {
        return std::nullopt;
    }
    return result;
}

std::string AddNewline(std::string text)
{
    if (text.empty() || text.back() != '\n')
    {
        text.push_back('\n');
    }
    return text;
}

} // namespace

const char* ToString(MaterialEditorDocumentState state) noexcept
{
    switch (state)
    {
    case MaterialEditorDocumentState::Clean:
        return "Clean";
    case MaterialEditorDocumentState::Dirty:
        return "Dirty";
    case MaterialEditorDocumentState::SaveFailed:
        return "SaveFailed";
    case MaterialEditorDocumentState::SourceChanged:
        return "SourceChanged";
    }
    return "Unknown";
}

const char* ToString(MaterialEditorValidationState state) noexcept
{
    switch (state)
    {
    case MaterialEditorValidationState::Unknown:
        return "Unknown";
    case MaterialEditorValidationState::Valid:
        return "Valid";
    case MaterialEditorValidationState::Invalid:
        return "Invalid";
    }
    return "Unknown";
}

MaterialInstanceDocumentService::MaterialInstanceDocumentService()
    : MaterialInstanceDocumentService(DiscoverDefaultConfig())
{
}

MaterialInstanceDocumentService::MaterialInstanceDocumentService(
    std::filesystem::path resourceRoot,
    std::filesystem::path projectRoot)
    : MaterialInstanceDocumentService(
          Config{std::move(resourceRoot), std::move(projectRoot), true})
{
}

MaterialInstanceDocumentService::MaterialInstanceDocumentService(
    Config configValue)
    : config(std::move(configValue))
{
    if (config.resourceRoot.empty())
    {
        config.resourceRoot = std::filesystem::current_path();
    }
    if (config.projectRoot.empty())
    {
        config.projectRoot = config.resourceRoot;
    }

    std::error_code resourceError;
    config.resourceRoot = std::filesystem::absolute(
        config.resourceRoot, resourceError)
                              .lexically_normal();
    if (resourceError)
    {
        throw std::invalid_argument(
            "Failed to normalize Material Editor resource root: " +
            resourceError.message());
    }

    std::error_code projectError;
    config.projectRoot = std::filesystem::absolute(
        config.projectRoot, projectError)
                             .lexically_normal();
    if (projectError)
    {
        throw std::invalid_argument(
            "Failed to normalize Material Editor project root: " +
            projectError.message());
    }
}

EditorMaterialParameterType
MaterialInstanceDocumentService::ToEditorParameterType(
    PersistenceNumericType type) noexcept
{
    switch (type)
    {
    case PersistenceNumericType::Float:
        return EditorMaterialParameterType::Float;
    case PersistenceNumericType::Vec2:
        return EditorMaterialParameterType::Vec2;
    case PersistenceNumericType::Vec3:
        return EditorMaterialParameterType::Vec3;
    case PersistenceNumericType::Vec4:
        return EditorMaterialParameterType::Vec4;
    case PersistenceNumericType::Color:
        return EditorMaterialParameterType::Color;
    }
    return EditorMaterialParameterType::Float;
}

PersistenceNumericType
MaterialInstanceDocumentService::ToPersistenceParameterType(
    EditorMaterialParameterType type) noexcept
{
    switch (type)
    {
    case EditorMaterialParameterType::Float:
        return PersistenceNumericType::Float;
    case EditorMaterialParameterType::Vec2:
        return PersistenceNumericType::Vec2;
    case EditorMaterialParameterType::Vec3:
        return PersistenceNumericType::Vec3;
    case EditorMaterialParameterType::Vec4:
        return PersistenceNumericType::Vec4;
    case EditorMaterialParameterType::Color:
        return PersistenceNumericType::Color;
    }
    return PersistenceNumericType::Float;
}

EditorMaterialParameterValue
MaterialInstanceDocumentService::ToEditorValue(
    const PersistenceNumericValue& value)
{
    return std::visit(
        [](const auto& typedValue) -> EditorMaterialParameterValue
        {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, Persistence::MaterialInstanceColor>)
            {
                return EditorColor{typedValue.components};
            }
            else
            {
                return typedValue;
            }
        },
        value);
}

PersistenceNumericValue
MaterialInstanceDocumentService::ToPersistenceValue(
    const EditorMaterialParameterValue& value)
{
    return std::visit(
        [](const auto& typedValue) -> PersistenceNumericValue
        {
            using ValueType = std::decay_t<decltype(typedValue)>;
            if constexpr (std::is_same_v<ValueType, EditorColor>)
            {
                return Persistence::MaterialInstanceColor{typedValue.components};
            }
            else
            {
                return typedValue;
            }
        },
        value);
}

void MaterialInstanceDocumentService::UpdateDirtyState(
    Document& document)
{
    if (document.state == MaterialEditorDocumentState::SourceChanged ||
        document.state == MaterialEditorDocumentState::SaveFailed)
    {
        return;
    }
    document.state =
        document.workingOverrides.parameters ==
                document.baselineOverrides.parameters &&
            document.workingOverrides.textures ==
                document.baselineOverrides.textures &&
            document.workingOverrides.renderStates ==
                document.baselineOverrides.renderStates
        ? MaterialEditorDocumentState::Clean
        : MaterialEditorDocumentState::Dirty;
}

void MaterialInstanceDocumentService::IncrementRevision(
    Document& document)
{
    if (document.revision == std::numeric_limits<EditorDocumentRevision>::max())
    {
        document.revision = 1;
    }
    else
    {
        ++document.revision;
    }
}

MaterialInstanceDocumentService::Document*
MaterialInstanceDocumentService::FindDocument(std::string_view assetPath)
{
    const auto iterator = documents.find(std::string(assetPath));
    return iterator == documents.end() ? nullptr : &iterator->second;
}

const MaterialInstanceDocumentService::Document*
MaterialInstanceDocumentService::FindDocument(
    std::string_view assetPath) const
{
    const auto iterator = documents.find(std::string(assetPath));
    return iterator == documents.end() ? nullptr : &iterator->second;
}

std::string MaterialInstanceDocumentService::RequireNormalizedPath(
    std::string_view assetPath) const
{
    try
    {
        return NormalizeMaterialInstancePath(assetPath);
    }
    catch (const std::exception& exception)
    {
        ThrowServiceError(
            EditorErrorCode::InvalidAssetType,
            exception.what());
    }
}

MaterialInstanceDocumentService::Document&
MaterialInstanceDocumentService::RequireDocument(
    std::string_view assetPath)
{
    const std::string normalized = RequireNormalizedPath(assetPath);
    Document* document = FindDocument(normalized);
    if (document == nullptr)
    {
        ThrowServiceError(
            EditorErrorCode::DocumentNotOpen,
            "material instance document is not open: " + normalized);
    }
    return *document;
}

const MaterialInstanceDocumentService::Document&
MaterialInstanceDocumentService::RequireDocument(
    std::string_view assetPath) const
{
    const std::string normalized = RequireNormalizedPath(assetPath);
    const Document* document = FindDocument(normalized);
    if (document == nullptr)
    {
        ThrowServiceError(
            EditorErrorCode::DocumentNotOpen,
            "material instance document is not open: " + normalized);
    }
    return *document;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeFailure(
    EditorErrorCode errorCode,
    std::string message,
    const Document* document) const
{
    MaterialEditorServiceResult result;
    result.succeeded = false;
    result.errorCode = errorCode;
    result.message = std::move(message);
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(
            const_cast<Document&>(*document));
    }
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeSuccess(
    std::string message,
    Document* document)
{
    MaterialEditorServiceResult result;
    result.succeeded = true;
    result.errorCode = EditorErrorCode::None;
    result.message = std::move(message);
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(*document);
    }
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeSnapshotResult(
    MaterialEditorServiceResult result,
    Document* document)
{
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(*document);
    }
    result.editor = BuildSnapshot();
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::MakeDocumentResult(
    MaterialEditorServiceResult result,
    Document* document)
{
    if (document != nullptr)
    {
        result.documentRevision = document->revision;
        result.document = MakeDocumentSnapshot(*document);
    }
    return result;
}

std::string MaterialInstanceDocumentService::JsonString(
    const Json& value)
{
    return AddNewline(value.dump(4));
}

MaterialInstanceDocumentService::Json
MaterialInstanceDocumentService::BuildEffectiveParameters(
    const Document& document,
    const PersistenceOverrides& workingOverrides) const
{
    Json result = Json::object();
    for (const auto& [name, defaultValue] : document.defaults.parameters)
    {
        const auto overrideIt = workingOverrides.parameters.find(name);
        result[name] = Persistence::SerializeMaterialInstanceNumericValue(
            overrideIt == workingOverrides.parameters.end()
                ? defaultValue
                : overrideIt->second);
    }
    return result;
}

MaterialInstanceDocumentService::Json
MaterialInstanceDocumentService::BuildEffectiveTextures(
    const Document& document,
    const PersistenceOverrides& workingOverrides) const
{
    Json result = Json::object();
    for (const auto& [slot, defaultValue] : document.defaults.textures)
    {
        const auto overrideIt = workingOverrides.textures.find(slot);
        if (overrideIt != workingOverrides.textures.end())
        {
            result[slot] = overrideIt->second;
        }
        else if (defaultValue.has_value())
        {
            result[slot] = *defaultValue;
        }
    }
    return result;
}

MaterialInstanceDocumentService::Json
MaterialInstanceDocumentService::BuildCandidateJson(
    const Document& document,
    const PersistenceOverrides& workingOverrides) const
{
    const Persistence::MaterialInstanceSparseCandidate candidate =
        Persistence::BuildMaterialInstanceSparseCandidate(
            document.sourceJson,
            document.defaults,
            workingOverrides);
    const Json serialized =
        Persistence::SerializeMaterialInstanceSparseCandidate(candidate);
    // service 持有原始 root；候选只允许重写这两个 editor-managed map，
    // 以保留宏、渲染状态和 opaque 扩展字段。
    Json result = document.sourceJson;
    static constexpr std::array<std::string_view, 3> managedFields = {
        "parameters", "textures", "renderStateOverrides"};
    for (const std::string_view field : managedFields)
    {
        const std::string key(field);
        if (serialized.contains(key))
        {
            result[key] = serialized.at(key);
        }
        else
        {
            result.erase(key);
        }
    }
    return result;
}

MaterialEditorDocumentSnapshot
MaterialInstanceDocumentService::MakeDocumentSnapshot(
    Document& document) const
{
    MaterialEditorDocumentSnapshot result;
    result.assetPath = document.assetPath;
    result.baseMaterialPath = document.baseMaterialPath;
    result.schemaDigest = document.schemaDigest.ToHex();
    result.sourceDigest = document.sourceDigest.ToHex();
    result.revision = document.revision;
    result.state = document.state;
    result.validation = document.validation;
    result.validationMessage = document.validationMessage;
    result.references = document.references;

    // 用稳定顺序展示影响 shader variant 与 pipeline 的三个 MI 状态，避免
    // map 的字典序把检查器布局变成 cullMode/renderMode/shadingModel。
    static constexpr std::array<std::string_view, 3> renderStateNames = {
        "renderMode", "shadingModel", "cullMode"};
    for (const std::string_view stateName : renderStateNames)
    {
        const std::string name(stateName);
        const auto defaultIt = document.defaults.renderStates.find(name);
        if (defaultIt == document.defaults.renderStates.end())
        {
            continue;
        }

        MaterialEditorRenderStateSnapshot state;
        state.name = name;
        state.defaultValue = defaultIt->second;
        const auto workingIt = document.workingOverrides.renderStates.find(name);
        state.effectiveValue = workingIt ==
                document.workingOverrides.renderStates.end()
            ? defaultIt->second
            : workingIt->second;
        if (workingIt != document.workingOverrides.renderStates.end())
        {
            state.overrideValue = workingIt->second;
        }
        result.renderStates.push_back(std::move(state));
    }

    for (const auto& [name, defaultValue] : document.defaults.parameters)
    {
        MaterialEditorParameterSnapshot parameter;
        parameter.name = name;
        const Json& descriptor = document.materialJson.at("parameters").at(name);
        parameter.description = descriptor.value(
            "description", std::string());
        parameter.type = ToEditorParameterType(
            Persistence::GetMaterialInstanceNumericType(defaultValue));
        parameter.defaultValue = ToEditorValue(defaultValue);
        const auto workingIt = document.workingOverrides.parameters.find(name);
        parameter.effectiveValue = ToEditorValue(
            workingIt == document.workingOverrides.parameters.end()
                ? defaultValue
                : workingIt->second);
        if (workingIt != document.workingOverrides.parameters.end())
        {
            parameter.overrideValue = ToEditorValue(workingIt->second);
        }
        if (descriptor.contains("range") && descriptor.at("range").is_object())
        {
            parameter.min = ReadOptionalRangeValue(
                descriptor.at("range"), "min");
            parameter.max = ReadOptionalRangeValue(
                descriptor.at("range"), "max");
        }
        if (descriptor.contains("channels") &&
            descriptor.at("channels").is_object())
        {
            static constexpr std::array<std::string_view, 4> componentNames = {
                "x", "y", "z", "w"};
            const Json& channels = descriptor.at("channels");
            for (const std::string_view componentName : componentNames)
            {
                const std::string key(componentName);
                if (!channels.contains(key) || !channels.at(key).is_object())
                {
                    continue;
                }
                const Json& channel = channels.at(key);
                MaterialEditorParameterChannelSnapshot channelSnapshot;
                channelSnapshot.name = channel.value(
                    "name", key);
                channelSnapshot.description = channel.value(
                    "description", std::string());
                if (channel.contains("range") &&
                    channel.at("range").is_object())
                {
                    channelSnapshot.min = ReadOptionalRangeValue(
                        channel.at("range"), "min");
                    channelSnapshot.max = ReadOptionalRangeValue(
                        channel.at("range"), "max");
                }
                parameter.channels.push_back(std::move(channelSnapshot));
            }
        }
        result.parameters.push_back(std::move(parameter));
    }

    for (const auto& [slot, defaultValue] : document.defaults.textures)
    {
        MaterialEditorTextureBindingSnapshot texture;
        texture.slotName = slot;
        const Json& descriptor = document.materialJson.at("textures").at(slot);
        texture.description = descriptor.value(
            "description", std::string());
        if (defaultValue.has_value())
        {
            texture.defaultAssetPath = *defaultValue;
        }
        const auto workingIt = document.workingOverrides.textures.find(slot);
        if (workingIt != document.workingOverrides.textures.end())
        {
            texture.effectiveAssetPath = workingIt->second;
            texture.overrideAssetPath = workingIt->second;
        }
        else if (defaultValue.has_value())
        {
            texture.effectiveAssetPath = *defaultValue;
        }
        result.textures.push_back(std::move(texture));
    }

    try
    {
        result.serializedWorkingDraft = JsonString(
            BuildCandidateJson(document, document.workingOverrides));
    }
    catch (const std::exception&)
    {
        // 候选无效时退回原始 root；只读 fallback 不触碰 dirty/baseline。
        result.serializedWorkingDraft = JsonString(document.sourceJson);
    }

    try
    {
        result.serializedBaselineDraft = JsonString(
            BuildCandidateJson(document, document.baselineOverrides));
    }
    catch (const std::exception&)
    {
        // baseline fallback 也只读原始 root，不能把 working 草稿冒充基线。
        result.serializedBaselineDraft = JsonString(document.sourceJson);
    }
    return result;
}

std::vector<MaterialEditorAssetEntry>
MaterialInstanceDocumentService::ListOpenDocumentEntries() const
{
    std::vector<MaterialEditorAssetEntry> result;
    result.reserve(documents.size());
    for (const auto& [path, document] : documents)
    {
        MaterialEditorAssetEntry entry;
        entry.assetPath = path;
        entry.revision = document.revision;
        entry.state = document.state;
        entry.message = document.validationMessage;
        entry.dirty =
            document.state == MaterialEditorDocumentState::Dirty ||
            document.state == MaterialEditorDocumentState::SaveFailed ||
            document.state == MaterialEditorDocumentState::SourceChanged;
        result.push_back(std::move(entry));
    }
    return result;
}

MaterialEditorTextureAssetSnapshot
MaterialInstanceDocumentService::InspectTextureAsset(
    const std::filesystem::path& absolutePath,
    std::string logicalPath) const
{
    MaterialEditorTextureAssetSnapshot result;
    result.assetPath = std::move(logicalPath);
    try
    {
        const Json textureJson = ReadJsonFile(absolutePath);
        if (!textureJson.is_object() ||
            textureJson.value("type", std::string()) != "texture")
        {
            throw std::runtime_error("asset type is not texture");
        }
        const std::string source =
            ReadStringField(textureJson, "source", result.assetPath);
        const std::string normalizedSource = NormalizeRelativePath(source);
        if (config.validateTextureSources)
        {
            static_cast<void>(ResolveResourceFile(normalizedSource));
        }
        result.valid = true;
    }
    catch (const std::exception& exception)
    {
        result.valid = false;
        result.diagnostic = exception.what();
    }
    return result;
}

std::vector<MaterialEditorTextureAssetSnapshot>
MaterialInstanceDocumentService::ListTextureAssets() const
{
    if (textureAssetCatalogCached)
    {
        return textureAssetCatalog;
    }

    std::vector<MaterialEditorTextureAssetSnapshot> result;
    if (!std::filesystem::is_directory(config.resourceRoot))
    {
        textureAssetCatalogCached = true;
        textureAssetCatalog.clear();
        return textureAssetCatalog;
    }
    std::error_code iteratorError;
    std::filesystem::recursive_directory_iterator iterator(
        config.resourceRoot,
        std::filesystem::directory_options::skip_permission_denied,
        iteratorError);
    const std::filesystem::recursive_directory_iterator end;
    for (; iterator != end; iterator.increment(iteratorError))
    {
        if (iteratorError)
        {
            iteratorError.clear();
            continue;
        }
        const std::filesystem::path path = iterator->path();
        if (!IsRegularFile(path) || !HasAssetFileName(
                path.filename().generic_string(), "T_"))
        {
            continue;
        }
        std::error_code relativeError;
        const std::filesystem::path relative = std::filesystem::relative(
            path,
            config.resourceRoot,
            relativeError);
        if (relativeError)
        {
            continue;
        }
        result.push_back(InspectTextureAsset(
            path,
            relative.generic_string()));
    }
    std::sort(
        result.begin(),
        result.end(),
        [](const MaterialEditorTextureAssetSnapshot& left,
           const MaterialEditorTextureAssetSnapshot& right)
        {
            return left.assetPath < right.assetPath;
        });
    textureAssetCatalog = std::move(result);
    textureAssetCatalogCached = true;
    return textureAssetCatalog;
}

MaterialEditorSnapshot MaterialInstanceDocumentService::BuildSnapshot()
{
    MaterialEditorSnapshot result;
    result.selectedDocumentPath = selectedDocumentPath;
    result.documentTabs = ListOpenDocumentEntries();
    result.textureAssets = ListTextureAssets();

    const MaterialEditorServiceResult assets =
        ListMaterialInstanceAssets({}, 0, std::numeric_limits<uint32_t>::max());
    if (assets.editor.has_value())
    {
        result.assets = assets.editor->assets;
    }
    if (!selectedDocumentPath.empty())
    {
        const auto selected = documents.find(selectedDocumentPath);
        if (selected != documents.end())
        {
            result.activeDocument = MakeDocumentSnapshot(
                const_cast<Document&>(selected->second));
            if (result.activeDocument->state !=
                MaterialEditorDocumentState::Clean)
            {
                result.statusMessage = ToString(
                    result.activeDocument->state);
            }
        }
    }
    return result;
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ListMaterialInstanceAssets(
    std::string_view searchText,
    uint32_t pageIndex,
    uint32_t pageSize) const
{
    MaterialEditorServiceResult result;
    result.succeeded = true;
    result.errorCode = EditorErrorCode::None;
    result.message = "material instance assets listed";

    MaterialEditorSnapshot snapshot;
    snapshot.selectedDocumentPath = selectedDocumentPath;
    snapshot.documentTabs = ListOpenDocumentEntries();
    snapshot.textureAssets = ListTextureAssets();

    if (!materialInstanceAssetCatalogCached &&
        std::filesystem::is_directory(config.resourceRoot))
    {
        std::vector<MaterialEditorAssetEntry> discoveredAssets;
        std::error_code iteratorError;
        std::filesystem::recursive_directory_iterator iterator(
            config.resourceRoot,
            std::filesystem::directory_options::skip_permission_denied,
            iteratorError);
        const std::filesystem::recursive_directory_iterator end;
        for (; iterator != end; iterator.increment(iteratorError))
        {
            if (iteratorError)
            {
                iteratorError.clear();
                continue;
            }
            const std::filesystem::path path = iterator->path();
            if (!IsRegularFile(path) || !HasAssetFileName(
                    path.filename().generic_string(), "MI_"))
            {
                continue;
            }
            std::error_code relativeError;
            const std::filesystem::path relative = std::filesystem::relative(
                path,
                config.resourceRoot,
                relativeError);
            if (relativeError)
            {
                continue;
            }
            const std::string logicalPath = relative.generic_string();
            MaterialEditorAssetEntry entry;
            entry.assetPath = logicalPath;
            discoveredAssets.push_back(std::move(entry));
        }
        std::sort(
            discoveredAssets.begin(),
            discoveredAssets.end(),
            [](const MaterialEditorAssetEntry& left,
               const MaterialEditorAssetEntry& right)
            {
                return left.assetPath < right.assetPath;
            });
        materialInstanceAssetCatalog = std::move(discoveredAssets);
        materialInstanceAssetCatalogCached = true;
    }

    std::vector<MaterialEditorAssetEntry> allAssets;
    allAssets.reserve(materialInstanceAssetCatalog.size());
    for (const MaterialEditorAssetEntry& cachedAsset : materialInstanceAssetCatalog)
    {
        if (!searchText.empty() &&
            cachedAsset.assetPath.find(searchText) == std::string::npos)
        {
            continue;
        }
        MaterialEditorAssetEntry entry = cachedAsset;
        const auto openIt = documents.find(entry.assetPath);
        entry.dirty = openIt != documents.end() &&
            openIt->second.state != MaterialEditorDocumentState::Clean;
        allAssets.push_back(std::move(entry));
    }

    const std::size_t begin = std::min<std::size_t>(
        static_cast<std::size_t>(pageIndex) * pageSize,
        allAssets.size());
    const std::size_t end = std::min<std::size_t>(
        begin + pageSize,
        allAssets.size());
    snapshot.assets.assign(
        allAssets.begin() + static_cast<std::ptrdiff_t>(begin),
        allAssets.begin() + static_cast<std::ptrdiff_t>(end));
    if (!selectedDocumentPath.empty())
    {
        const auto selected = documents.find(selectedDocumentPath);
        if (selected != documents.end())
        {
            snapshot.activeDocument = MakeDocumentSnapshot(
                const_cast<Document&>(selected->second));
        }
    }
    result.editor = std::move(snapshot);
    return result;
}

namespace
{

MaterialEditorReferenceSnapshot MakeReferenceSnapshot(
    const EditorNavigationOrigin& origin,
    std::string materialInstancePath)
{
    MaterialEditorReferenceSnapshot result;
    result.sceneIdentity = origin.sceneIdentity;
    result.objectIdentity = origin.objectIdentity;
    result.selector = origin.slot;
    if (origin.section.has_value())
    {
        result.selector += "[" + std::to_string(*origin.section) + "]";
    }
    result.materialInstancePath = std::move(materialInstancePath);
    return result;
}

bool SameReference(
    const MaterialEditorReferenceSnapshot& left,
    const MaterialEditorReferenceSnapshot& right)
{
    return left.sceneIdentity == right.sceneIdentity &&
           left.objectIdentity == right.objectIdentity &&
           left.sourceAssetPath == right.sourceAssetPath &&
           left.selector == right.selector &&
           left.materialInstancePath == right.materialInstancePath;
}

} // namespace

MaterialEditorServiceResult
MaterialInstanceDocumentService::OpenMaterialInstanceAsset(
    std::string_view assetPath,
    const std::optional<EditorNavigationOrigin>& origin)
{
    try
    {
        const std::string normalized =
            NormalizeMaterialInstancePath(assetPath);
        auto existing = documents.find(normalized);
        if (existing != documents.end())
        {
            selectedDocumentPath = normalized;
            if (origin.has_value())
            {
                const MaterialEditorReferenceSnapshot reference =
                    MakeReferenceSnapshot(origin.value(), normalized);
                const auto duplicate = std::find_if(
                    existing->second.references.begin(),
                    existing->second.references.end(),
                    [&reference](const MaterialEditorReferenceSnapshot& item)
                    {
                        return SameReference(item, reference);
                    });
                if (duplicate == existing->second.references.end())
                {
                    existing->second.references.push_back(reference);
                }
            }
            return MakeSnapshotResult(
                MakeSuccess("material instance document focused", &existing->second),
                &existing->second);
        }

        const LoadedDocument loaded = LoadDocumentFromDisk(normalized);
        Document document;
        document.assetPath = loaded.assetPath;
        document.absolutePath = loaded.absolutePath;
        document.baseMaterialPath = loaded.baseMaterialPath;
        document.baseMaterialAbsolutePath = loaded.baseMaterialAbsolutePath;
        document.sourceJson = loaded.sourceJson;
        document.materialJson = loaded.materialJson;
        document.defaults = loaded.defaults;
        document.baselineOverrides = loaded.overrides;
        document.workingOverrides = loaded.overrides;
        document.sourceDigest = loaded.sourceDigest;
        document.schemaDigest = loaded.schemaDigest;
        if (origin.has_value())
        {
            document.references.push_back(
                MakeReferenceSnapshot(origin.value(), normalized));
        }
        const auto inserted = documents.emplace(normalized, std::move(document));
        selectedDocumentPath = normalized;
        return MakeSnapshotResult(
            MakeSuccess("material instance document opened", &inserted.first->second),
            &inserted.first->second);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::SelectMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        const std::string normalized = NormalizeMaterialInstancePath(assetPath);
        Document& document = RequireDocument(normalized);
        selectedDocumentPath = normalized;
        return MakeSnapshotResult(
            MakeSuccess("material instance document selected", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::GetMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        return MakeSnapshotResult(
            MakeSuccess("material instance document read", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::GetSelectedMaterialInstanceDocument()
{
    if (selectedDocumentPath.empty())
    {
        return MakeFailure(
            EditorErrorCode::DocumentNotOpen,
            "no material instance document is selected");
    }
    return GetMaterialInstanceDocument(selectedDocumentPath);
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::CloseMaterialInstanceAsset(
    std::string_view assetPath,
    EditorDirtyDocumentPolicy dirtyPolicy)
{
    try
    {
        const std::string normalized = NormalizeMaterialInstancePath(assetPath);
        Document& document = RequireDocument(normalized);
        const bool dirty = document.state != MaterialEditorDocumentState::Clean;
        if (dirty && dirtyPolicy == EditorDirtyDocumentPolicy::RequireClean)
        {
            return MakeFailure(
                EditorErrorCode::DocumentDirty,
                "material instance document has unsaved changes: " + normalized,
                &document);
        }
        documents.erase(normalized);
        if (selectedDocumentPath == normalized)
        {
            selectedDocumentPath = documents.empty()
                ? std::string()
                : documents.begin()->first;
        }
        MaterialEditorServiceResult result = MakeSuccess(
            "material instance document closed");
        result.editor = BuildSnapshot();
        return result;
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::GetMaterialInstanceReferenceContext(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        MaterialEditorServiceResult result = MakeSuccess(
            "material instance reference context read", &document);
        result.references = document.references;
        result.editor = BuildSnapshot();
        return result;
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::SetMaterialParameterOverride(
    std::string_view assetPath,
    std::string_view parameter,
    EditorMaterialParameterType parameterType,
    const EditorMaterialParameterValue& value)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }

        const auto defaultIt = document.defaults.parameters.find(
            std::string(parameter));
        if (defaultIt == document.defaults.parameters.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownParameter,
                "unknown material parameter: " + std::string(parameter),
                &document);
        }

        const PersistenceNumericType expectedType =
            Persistence::GetMaterialInstanceNumericType(defaultIt->second);
        if (ToPersistenceParameterType(parameterType) != expectedType ||
            Persistence::GetMaterialInstanceNumericType(
                ToPersistenceValue(value)) != expectedType ||
            !IsFiniteEditorMaterialParameterValue(value))
        {
            return MakeFailure(
                EditorErrorCode::ParameterTypeMismatch,
                "material parameter type/value does not match schema: " +
                    std::string(parameter),
                &document);
        }

        const auto descriptorIt = document.materialJson.at("parameters").find(
            std::string(parameter));
        if (descriptorIt != document.materialJson.at("parameters").end())
        {
            try
            {
                ValidateEditorParameterRange(
                    *descriptorIt,
                    value,
                    parameter);
            }
            catch (const ServiceError& exception)
            {
                return MakeFailure(
                    exception.errorCode,
                    exception.what(),
                    &document);
            }
        }

        PersistenceOverrides proposed = document.workingOverrides;
        const PersistenceNumericValue persistenceValue =
            ToPersistenceValue(value);
        if (Persistence::MaterialInstanceNumericValuesExactlyEqual(
                persistenceValue,
                defaultIt->second))
        {
            proposed.parameters.erase(std::string(parameter));
        }
        else
        {
            proposed.parameters[std::string(parameter)] = persistenceValue;
        }

        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft parameter is valid";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeDocumentResult(
            MakeSuccess("material parameter override updated", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ClearMaterialParameterOverride(
    std::string_view assetPath,
    std::string_view parameter)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        if (document.defaults.parameters.find(std::string(parameter)) ==
            document.defaults.parameters.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownParameter,
                "unknown material parameter: " + std::string(parameter),
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        proposed.parameters.erase(std::string(parameter));
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft parameter reset to material default";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeDocumentResult(
            MakeSuccess("material parameter override cleared", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::SetMaterialTextureOverride(
    std::string_view assetPath,
    std::string_view slot,
    std::string_view textureAssetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        const std::string slotName(slot);
        const auto defaultIt = document.defaults.textures.find(slotName);
        if (defaultIt == document.defaults.textures.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownTextureSlot,
                "unknown material texture slot: " + slotName,
                &document);
        }

        std::string normalizedTexturePath;
        try
        {
            normalizedTexturePath =
                NormalizeTextureAssetPath(textureAssetPath);
            ValidateTextureReference(
                normalizedTexturePath,
                document.assetPath);
        }
        catch (const ServiceError& exception)
        {
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        catch (const std::exception& exception)
        {
            return MakeFailure(
                EditorErrorCode::InvalidTextureAssetReference,
                exception.what(),
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        if (defaultIt->second.has_value() &&
            *defaultIt->second == normalizedTexturePath)
        {
            proposed.textures.erase(slotName);
        }
        else
        {
            proposed.textures[slotName] = normalizedTexturePath;
        }
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft texture binding is valid";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material texture override updated", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ClearMaterialTextureOverride(
    std::string_view assetPath,
    std::string_view slot)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        const std::string slotName(slot);
        if (document.defaults.textures.find(slotName) ==
            document.defaults.textures.end())
        {
            return MakeFailure(
                EditorErrorCode::UnknownTextureSlot,
                "unknown material texture slot: " + slotName,
                &document);
        }
        PersistenceOverrides proposed = document.workingOverrides;
        proposed.textures.erase(slotName);
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft texture binding reset to material default";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material texture override cleared", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::SetMaterialRenderStateOverride(
    std::string_view assetPath,
    EditorMaterialRenderStateField field,
    const EditorMaterialRenderStateValue& value)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }

        const std::string fieldName = RenderStateFieldName(field);
        const auto defaultIt = document.defaults.renderStates.find(fieldName);
        if (defaultIt == document.defaults.renderStates.end())
        {
            return MakeFailure(
                EditorErrorCode::InvalidPayload,
                "unknown material render state: " + fieldName,
                &document);
        }

        const std::string valueName = RenderStateValueName(field, value);
        PersistenceOverrides proposed = document.workingOverrides;
        if (valueName == defaultIt->second)
        {
            proposed.renderStates.erase(fieldName);
        }
        else
        {
            proposed.renderStates[fieldName] = valueName;
        }
        proposed.renderStatesManaged =
            !proposed.renderStates.empty() ||
            document.baselineOverrides.renderStatesManaged;

        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft render state is valid";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material render state override updated", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ClearMaterialRenderStateOverride(
    std::string_view assetPath,
    EditorMaterialRenderStateField field)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }

        const std::string fieldName = RenderStateFieldName(field);
        if (document.defaults.renderStates.find(fieldName) ==
            document.defaults.renderStates.end())
        {
            return MakeFailure(
                EditorErrorCode::InvalidPayload,
                "unknown material render state: " + fieldName,
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        proposed.renderStates.erase(fieldName);
        proposed.renderStatesManaged =
            !proposed.renderStates.empty() ||
            document.baselineOverrides.renderStatesManaged;
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft render state reset to material default";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material render state override cleared", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ResetMaterialInstanceOverrides(
    std::string_view assetPath,
    EditorResetScope scope)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        PersistenceOverrides proposed = document.workingOverrides;
        switch (scope)
        {
        case EditorResetScope::Parameters:
            proposed.parameters.clear();
            break;
        case EditorResetScope::Textures:
            proposed.textures.clear();
            break;
        case EditorResetScope::RenderStates:
            proposed.renderStates.clear();
            break;
        case EditorResetScope::All:
            proposed.parameters.clear();
            proposed.textures.clear();
            proposed.renderStates.clear();
            break;
        }
        if (scope == EditorResetScope::RenderStates ||
            scope == EditorResetScope::All)
        {
            proposed.renderStatesManaged =
                document.baselineOverrides.renderStatesManaged;
        }
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft overrides reset to material defaults";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material instance overrides reset", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::RevertMaterialInstanceDocument(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        PersistenceOverrides proposed = document.baselineOverrides;
        try
        {
            ValidateWorkingState(document, proposed);
        }
        catch (const ServiceError& exception)
        {
            document.validation = MaterialEditorValidationState::Invalid;
            document.validationMessage = exception.what();
            return MakeFailure(exception.errorCode, exception.what(), &document);
        }
        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = "draft reverted to the last saved baseline";
        UpdateDirtyState(document);
        IncrementRevision(document);
        return MakeSnapshotResult(
            MakeSuccess("material instance document reverted", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ReloadMaterialInstanceDocument(
    std::string_view assetPath,
    EditorDirtyDocumentPolicy dirtyPolicy)
{
    try
    {
        const std::string normalized = NormalizeMaterialInstancePath(assetPath);
        Document& current = RequireDocument(normalized);
        const bool dirty = current.state != MaterialEditorDocumentState::Clean;
        if (dirty && dirtyPolicy == EditorDirtyDocumentPolicy::RequireClean)
        {
            return MakeFailure(
                EditorErrorCode::DocumentDirty,
                "reload requires an explicit discard policy for dirty document: " +
                    normalized,
                &current);
        }

        const LoadedDocument loaded = LoadDocumentFromDisk(normalized);
        Document reloaded;
        reloaded.assetPath = loaded.assetPath;
        reloaded.absolutePath = loaded.absolutePath;
        reloaded.baseMaterialPath = loaded.baseMaterialPath;
        reloaded.baseMaterialAbsolutePath = loaded.baseMaterialAbsolutePath;
        reloaded.sourceJson = loaded.sourceJson;
        reloaded.materialJson = loaded.materialJson;
        reloaded.defaults = loaded.defaults;
        reloaded.baselineOverrides = loaded.overrides;
        reloaded.workingOverrides = loaded.overrides;
        reloaded.sourceDigest = loaded.sourceDigest;
        reloaded.schemaDigest = loaded.schemaDigest;
        reloaded.references = current.references;
        reloaded.revision = current.revision;
        IncrementRevision(reloaded);
        reloaded.validation = MaterialEditorValidationState::Unknown;
        reloaded.validationMessage.clear();

        const auto iterator = documents.find(normalized);
        iterator->second = std::move(reloaded);
        selectedDocumentPath = normalized;
        return MakeSnapshotResult(
            MakeSuccess("material instance document reloaded", &iterator->second),
            &iterator->second);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ResolveSceneMaterialAsset(
    const ResolveSceneMaterialAssetPayload& payload)
{
    try
    {
        const std::string scenePath = NormalizeRelativePath(
            payload.sceneIdentity);
        const Json sceneJson = ReadJsonFile(ResolveResourceFile(scenePath));
        if (!sceneJson.is_object() || !sceneJson.contains("objects") ||
            !sceneJson.at("objects").is_array())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene does not contain an objects array: " + scenePath);
        }

        const Json* selectedObject = nullptr;
        std::size_t selectedObjectIndex = 0;
        const Json& objects = sceneJson.at("objects");
        for (std::size_t index = 0; index < objects.size(); ++index)
        {
            const Json& object = objects.at(index);
            if (!object.is_object())
            {
                continue;
            }
            const bool identityMatches =
                object.value("name", std::string()) == payload.objectIdentity ||
                object.value("id", std::string()) == payload.objectIdentity ||
                object.value("objectId", std::string()) == payload.objectIdentity;
            if (identityMatches)
            {
                selectedObject = &object;
                selectedObjectIndex = index;
                break;
            }
        }

        if (selectedObject == nullptr)
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene object was not found: " + payload.objectIdentity);
        }

        const std::string objectType = selectedObject->value(
            "type", std::string());
        std::string sourceAssetPath;
        if (objectType == "mesh")
        {
            sourceAssetPath = ReadStringField(
                *selectedObject,
                "modelPath",
                payload.objectIdentity);
        }
        else if (objectType == "terrain")
        {
            sourceAssetPath = ReadStringField(
                *selectedObject,
                "terrainPath",
                payload.objectIdentity);
        }
        else
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene object is not a mesh or terrain: " +
                    payload.objectIdentity);
        }

        sourceAssetPath = NormalizeRelativePath(sourceAssetPath);
        const Json sourceAssetJson = ReadJsonFile(
            ResolveResourceFile(sourceAssetPath));
        if (!sourceAssetJson.is_object())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "referenced mesh/terrain asset is not an object: " +
                    sourceAssetPath);
        }

        Json slots = Json::array();
        if (sourceAssetJson.contains("materialSlots") &&
            sourceAssetJson.at("materialSlots").is_array())
        {
            slots = sourceAssetJson.at("materialSlots");
        }
        else if (sourceAssetJson.contains("materials") &&
                 sourceAssetJson.at("materials").is_array())
        {
            slots = sourceAssetJson.at("materials");
        }
        else if (sourceAssetJson.contains("materialInstancePath") &&
                 sourceAssetJson.at("materialInstancePath").is_string())
        {
            slots.push_back(Json{
                {"slot", "surface"},
                {"materialInstancePath",
                 sourceAssetJson.at("materialInstancePath")}});
        }

        if (slots.empty())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "referenced asset does not define material slots: " +
                    sourceAssetPath);
        }

        std::size_t slotIndex = 0;
        if (payload.section.has_value())
        {
            slotIndex = *payload.section;
        }
        else if (slots.size() != 1)
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "scene object has multiple material slots; section is required: " +
                    payload.objectIdentity);
        }
        if (slotIndex >= slots.size())
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "material section is outside the slot array: " +
                    std::to_string(slotIndex));
        }

        const Json& slot = slots.at(slotIndex);
        std::string materialInstancePath;
        std::string slotName = "section" + std::to_string(slotIndex);
        if (slot.is_string())
        {
            materialInstancePath = slot.get<std::string>();
        }
        else if (slot.is_object())
        {
            materialInstancePath = ReadStringField(
                slot,
                "materialInstancePath",
                sourceAssetPath);
            slotName = slot.value(
                "slot",
                slot.value("name", slotName));
        }
        else
        {
            ThrowServiceError(
                EditorErrorCode::ReferenceResolutionFailed,
                "material slot is neither a string nor an object: " +
                    sourceAssetPath);
        }

        materialInstancePath = NormalizeMaterialInstancePath(
            materialInstancePath);
        MaterialEditorReferenceSnapshot reference;
        reference.sceneIdentity = scenePath;
        reference.objectIdentity = payload.objectIdentity;
        reference.objectType = objectType;
        reference.sourceAssetPath = sourceAssetPath;
        reference.selector = slotName + "[" +
            std::to_string(slotIndex) + "]";
        reference.materialInstancePath = materialInstancePath;

        MaterialEditorServiceResult result = MakeSuccess(
            "scene material reference resolved: " + materialInstancePath);
        result.references.push_back(reference);

        // Resolve 本身不打开文档；若同一路径已经打开，则只补全当前导航来源。
        Document* document = FindDocument(materialInstancePath);
        if (document != nullptr)
        {
            const auto duplicate = std::find_if(
                document->references.begin(),
                document->references.end(),
                [&reference](const MaterialEditorReferenceSnapshot& item)
                {
                    return SameReference(item, reference);
                });
            if (duplicate == document->references.end())
            {
                document->references.push_back(reference);
            }
            result.documentRevision = document->revision;
            result.document = MakeDocumentSnapshot(*document);
        }
        result.editor = BuildSnapshot();
        static_cast<void>(selectedObjectIndex);
        return result;
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(
            EditorErrorCode::ReferenceResolutionFailed,
            exception.what());
    }
}

MaterialEditorServiceResult
MaterialInstanceDocumentService::ExecuteBatch(
    const ExecuteEditorCommandBatchPayload& batch,
    std::optional<EditorDocumentRevision> expectedRevision)
{
    try
    {
        if (const auto validationError = ValidateEditorCommandBatch(batch))
        {
            return MakeFailure(
                *validationError,
                "editor command batch failed protocol validation");
        }

        const auto readPath = [](const EditorCommandBatchItem& item)
            -> std::string
        {
            return std::visit(
                [](const auto& payload) -> std::string
                {
                    return payload.assetPath;
                },
                item.payload);
        };

        const std::string assetPath = NormalizeMaterialInstancePath(
            readPath(batch.commands.front()));
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        if (document.state == MaterialEditorDocumentState::SourceChanged)
        {
            return MakeFailure(
                EditorErrorCode::SourceChanged,
                document.validationMessage,
                &document);
        }
        if (expectedRevision.has_value() &&
            *expectedRevision != document.revision)
        {
            return MakeFailure(
                EditorErrorCode::StaleDocumentRevision,
                "expected document revision does not match the open document",
                &document);
        }

        PersistenceOverrides proposed = document.workingOverrides;
        bool saveRequested = false;
        bool validationRequested = false;
        for (const EditorCommandBatchItem& item : batch.commands)
        {
            if (NormalizeMaterialInstancePath(readPath(item)) != assetPath)
            {
                ThrowServiceError(
                    EditorErrorCode::InvalidPayload,
                    "all commands in a batch must target one MI asset");
            }

            switch (item.type)
            {
            case EditorCommandType::SetMaterialParameterOverride:
            {
                const auto& payload =
                    std::get<SetMaterialParameterOverridePayload>(item.payload);
                const auto defaultIt = document.defaults.parameters.find(
                    payload.parameter);
                if (defaultIt == document.defaults.parameters.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownParameter,
                        "unknown material parameter: " + payload.parameter);
                }
                const PersistenceNumericType expectedType =
                    Persistence::GetMaterialInstanceNumericType(
                        defaultIt->second);
                const PersistenceNumericValue value = ToPersistenceValue(
                    payload.value);
                if (ToPersistenceParameterType(payload.parameterType) !=
                        expectedType ||
                    Persistence::GetMaterialInstanceNumericType(value) !=
                        expectedType ||
                    !IsFiniteEditorMaterialParameterValue(payload.value))
                {
                    ThrowServiceError(
                        EditorErrorCode::ParameterTypeMismatch,
                        "material parameter type/value does not match schema: " +
                            payload.parameter);
                }
                const Json& descriptor = document.materialJson.at(
                    "parameters").at(payload.parameter);
                ValidateEditorParameterRange(
                    descriptor,
                    payload.value,
                    payload.parameter);
                if (Persistence::MaterialInstanceNumericValuesExactlyEqual(
                        value,
                        defaultIt->second))
                {
                    proposed.parameters.erase(payload.parameter);
                }
                else
                {
                    proposed.parameters[payload.parameter] = value;
                }
                break;
            }
            case EditorCommandType::ClearMaterialParameterOverride:
            {
                const auto& payload =
                    std::get<ClearMaterialParameterOverridePayload>(item.payload);
                if (document.defaults.parameters.find(payload.parameter) ==
                    document.defaults.parameters.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownParameter,
                        "unknown material parameter: " + payload.parameter);
                }
                proposed.parameters.erase(payload.parameter);
                break;
            }
            case EditorCommandType::SetMaterialTextureOverride:
            {
                const auto& payload =
                    std::get<SetMaterialTextureOverridePayload>(item.payload);
                const auto defaultIt = document.defaults.textures.find(
                    payload.slot);
                if (defaultIt == document.defaults.textures.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownTextureSlot,
                        "unknown material texture slot: " + payload.slot);
                }
                const std::string texturePath = NormalizeTextureAssetPath(
                    payload.textureAssetPath);
                ValidateTextureReference(texturePath, document.assetPath);
                if (defaultIt->second.has_value() &&
                    *defaultIt->second == texturePath)
                {
                    proposed.textures.erase(payload.slot);
                }
                else
                {
                    proposed.textures[payload.slot] = texturePath;
                }
                break;
            }
            case EditorCommandType::ClearMaterialTextureOverride:
            {
                const auto& payload =
                    std::get<ClearMaterialTextureOverridePayload>(item.payload);
                if (document.defaults.textures.find(payload.slot) ==
                    document.defaults.textures.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::UnknownTextureSlot,
                        "unknown material texture slot: " + payload.slot);
                }
                proposed.textures.erase(payload.slot);
                break;
            }
            case EditorCommandType::SetMaterialRenderStateOverride:
            {
                const auto& payload =
                    std::get<SetMaterialRenderStateOverridePayload>(item.payload);
                const std::string fieldName =
                    RenderStateFieldName(payload.field);
                const auto defaultIt = document.defaults.renderStates.find(fieldName);
                if (defaultIt == document.defaults.renderStates.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::InvalidPayload,
                        "unknown material render state: " + fieldName);
                }
                const std::string valueName =
                    RenderStateValueName(payload.field, payload.value);
                if (valueName == defaultIt->second)
                {
                    proposed.renderStates.erase(fieldName);
                }
                else
                {
                    proposed.renderStates[fieldName] = valueName;
                }
                proposed.renderStatesManaged =
                    !proposed.renderStates.empty() ||
                    document.baselineOverrides.renderStatesManaged;
                break;
            }
            case EditorCommandType::ClearMaterialRenderStateOverride:
            {
                const auto& payload =
                    std::get<ClearMaterialRenderStateOverridePayload>(item.payload);
                const std::string fieldName =
                    RenderStateFieldName(payload.field);
                if (document.defaults.renderStates.find(fieldName) ==
                    document.defaults.renderStates.end())
                {
                    ThrowServiceError(
                        EditorErrorCode::InvalidPayload,
                        "unknown material render state: " + fieldName);
                }
                proposed.renderStates.erase(fieldName);
                proposed.renderStatesManaged =
                    !proposed.renderStates.empty() ||
                    document.baselineOverrides.renderStatesManaged;
                break;
            }
            case EditorCommandType::ResetMaterialInstanceOverrides:
            {
                const auto& payload =
                    std::get<ResetMaterialInstanceOverridesPayload>(item.payload);
                if (payload.scope == EditorResetScope::Parameters ||
                    payload.scope == EditorResetScope::All)
                {
                    proposed.parameters.clear();
                }
                if (payload.scope == EditorResetScope::Textures ||
                    payload.scope == EditorResetScope::All)
                {
                    proposed.textures.clear();
                }
                if (payload.scope == EditorResetScope::RenderStates ||
                    payload.scope == EditorResetScope::All)
                {
                    proposed.renderStates.clear();
                }
                break;
            }
            case EditorCommandType::RevertMaterialInstanceDocument:
                proposed = document.baselineOverrides;
                break;
            case EditorCommandType::ValidateMaterialInstanceDocument:
                validationRequested = true;
                break;
            case EditorCommandType::SaveMaterialInstanceDocument:
                saveRequested = true;
                break;
            case EditorCommandType::ReloadMaterialInstanceDocument:
            case EditorCommandType::ConnectMaterialInstancePreview:
            case EditorCommandType::DisconnectMaterialInstancePreview:
            case EditorCommandType::ApplyMaterialInstancePreview:
            case EditorCommandType::RestoreMaterialInstancePreviewBaseline:
            default:
                ThrowServiceError(
                    EditorErrorCode::InvalidPayload,
                    "command type is not valid inside a document batch");
            }
        }

        ValidateWorkingState(document, proposed);
        const bool draftChanged =
            proposed.parameters != document.workingOverrides.parameters ||
            proposed.textures != document.workingOverrides.textures ||
            proposed.renderStates != document.workingOverrides.renderStates;

        MaterialEditorServiceResult result;
        if (saveRequested)
        {
            const Json candidate = BuildCandidateJson(document, proposed);
            const Persistence::MaterialInstanceSaveResult saveResult =
                Persistence::MaterialInstancePersistence::SaveTextIfUnchanged(
                    document.absolutePath,
                    document.sourceDigest,
                    JsonString(candidate));
            if (!saveResult.Succeeded())
            {
                const EditorErrorCode errorCode = saveResult.HasSourceConflict()
                    ? EditorErrorCode::SourceChanged
                    : (saveResult.status ==
                           Persistence::MaterialInstancePersistenceStatus::WriteFailed
                           ? EditorErrorCode::AtomicWriteFailed
                           : EditorErrorCode::AssetNotFound);
                result = MakeFailure(errorCode, saveResult.errorMessage, &document);
                result.expectedDigest = saveResult.expectedDigest.ToHex();
                result.observedDigest = saveResult.observedDigest.ToHex();
                result.newDigest = saveResult.newDigest.ToHex();
                return result;
            }

            document.sourceJson = candidate;
            document.baselineOverrides = proposed;
            document.workingOverrides = proposed;
            document.sourceDigest = saveResult.newDigest;
            document.state = MaterialEditorDocumentState::Clean;
            document.validation = MaterialEditorValidationState::Valid;
            document.validationMessage = "batch validated and saved";
            IncrementRevision(document);
            result = MakeSnapshotResult(
                MakeSuccess("editor command batch saved", &document),
                &document);
            result.expectedDigest = saveResult.expectedDigest.ToHex();
            result.observedDigest = saveResult.observedDigest.ToHex();
            result.newDigest = saveResult.newDigest.ToHex();
            return result;
        }

        document.workingOverrides = std::move(proposed);
        document.validation = MaterialEditorValidationState::Valid;
        document.validationMessage = validationRequested
            ? "batch draft validation succeeded"
            : "editor command batch applied";
        UpdateDirtyState(document);
        if (draftChanged)
        {
            IncrementRevision(document);
        }
        return MakeSnapshotResult(
            MakeSuccess("editor command batch applied", &document),
            &document);
    }
    catch (const ServiceError& exception)
    {
        return MakeFailure(exception.errorCode, exception.what());
    }
    catch (const std::exception& exception)
    {
        return MakeFailure(EditorErrorCode::ValidationFailed, exception.what());
    }
}

std::optional<MaterialEditorDocumentSnapshot>
MaterialInstanceDocumentService::BuildDocumentSnapshot(
    std::string_view assetPath)
{
    try
    {
        Document& document = RequireDocument(assetPath);
        RefreshExternalChange(document);
        return MakeDocumentSnapshot(document);
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

bool MaterialInstanceDocumentService::IsDocumentOpen(
    std::string_view assetPath) const
{
    try
    {
        return FindDocument(NormalizeMaterialInstancePath(assetPath)) != nullptr;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

std::optional<EditorDocumentRevision>
MaterialInstanceDocumentService::GetDocumentRevision(
    std::string_view assetPath) const
{
    try
    {
        const Document* document = FindDocument(
            NormalizeMaterialInstancePath(assetPath));
        if (document == nullptr)
        {
            return std::nullopt;
        }
        return document->revision;
    }
    catch (const std::exception&)
    {
        return std::nullopt;
    }
}

} // namespace VL::Editor
