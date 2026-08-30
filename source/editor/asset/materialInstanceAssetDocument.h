#pragma once

// 资产文档层只拥有 JSON 和值语义状态；它不持有 World、MaterialInstance 或 Vulkan 对象。

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "../command/editorCommand.h"

namespace VL::Editor::Asset
{

struct MaterialAssetRoots
{
    std::filesystem::path projectRoot;
    std::filesystem::path resourceRoot;
};

struct MaterialInstancePathResult
{
    std::string normalizedPath;
    std::filesystem::path absolutePath;
    std::string errorMessage;

    bool Succeeded() const noexcept
    {
        return !normalizedPath.empty() && errorMessage.empty();
    }
};

// MI 身份始终是 resourceRoot 下的逻辑路径；绝对路径只允许在入口处折算。
MaterialInstancePathResult NormalizeMaterialInstancePath(
    std::string_view path,
    const MaterialAssetRoots& roots);

// 无根目录版本用于命令 codec 或单元测试，只接受 resource-root-relative 路径。
MaterialInstancePathResult NormalizeMaterialInstancePath(std::string_view path);

struct MaterialParameterChannelSnapshot
{
    std::string component;
    std::string name;
    std::string description;
    std::optional<float> minimum;
    std::optional<float> maximum;
    nlohmann::json metadata = nlohmann::json::object();
};

struct MaterialParameterSnapshot
{
    std::string name;
    std::string type;
    nlohmann::json schema = nlohmann::json::object();
    nlohmann::json defaultValue;
    nlohmann::json baselineEffectiveValue;
    nlohmann::json workingEffectiveValue;
    nlohmann::json baselineOverrideValue;
    nlohmann::json workingOverrideValue;
    bool hasBaselineOverride = false;
    bool hasWorkingOverride = false;
    std::vector<MaterialParameterChannelSnapshot> channels;
};

struct MaterialTextureSnapshot
{
    std::string slot;
    std::string type;
    nlohmann::json schema = nlohmann::json::object();
    std::optional<std::string> defaultAssetPath;
    std::optional<std::string> baselineEffectiveAssetPath;
    std::optional<std::string> workingEffectiveAssetPath;
    std::optional<std::string> baselineOverrideAssetPath;
    std::optional<std::string> workingOverrideAssetPath;
    bool hasBaselineOverride = false;
    bool hasWorkingOverride = false;
};

enum class MaterialInstanceDocumentState
{
    Clean,
    Dirty,
    SourceChanged,
    ReloadFailed
};

const char* ToString(MaterialInstanceDocumentState state) noexcept;

struct MaterialInstanceDocumentSnapshot
{
    std::string normalizedAssetPath;
    std::filesystem::path absoluteAssetPath;
    std::string name;
    std::string materialPath;
    std::filesystem::path absoluteMaterialPath;
    std::string shadingModel;

    // 原始对象和工作对象都保留，便于后续保存时只改 parameters/textures。
    nlohmann::json materialSchema = nlohmann::json::object();
    nlohmann::json baselineJson = nlohmann::json::object();
    nlohmann::json workingJson = nlohmann::json::object();
    nlohmann::json baselineOverrides = nlohmann::json::object();
    nlohmann::json workingOverrides = nlohmann::json::object();

    std::vector<MaterialParameterSnapshot> parameters;
    std::vector<MaterialTextureSnapshot> textures;
    std::vector<EditorNavigationOrigin> navigationOrigins;

    EditorDocumentRevision revision = 1;
    MaterialInstanceDocumentState state = MaterialInstanceDocumentState::Clean;
    bool dirty = false;
    std::string diagnostic;
};

struct MaterialAssetOperationResult
{
    EditorCommandStatus status = EditorCommandStatus::Failed;
    EditorErrorCode errorCode = EditorErrorCode::None;
    std::string message;
    bool alreadyOpen = false;
    std::optional<EditorDocumentRevision> documentRevision;
    std::optional<MaterialInstanceDocumentSnapshot> document;

    bool Succeeded() const noexcept
    {
        return status == EditorCommandStatus::Succeeded;
    }
};

struct MaterialInstanceAssetEntry
{
    std::string normalizedAssetPath;
    std::string displayName;
    std::string materialPath;
    bool readable = true;
    std::string diagnostic;
};

struct MaterialInstanceAssetListResult
{
    EditorCommandStatus status = EditorCommandStatus::Failed;
    EditorErrorCode errorCode = EditorErrorCode::None;
    std::string message;
    std::vector<MaterialInstanceAssetEntry> entries;
    uint32_t pageIndex = 0;
    uint32_t pageSize = 0;
    uint32_t totalCount = 0;

    bool Succeeded() const noexcept
    {
        return status == EditorCommandStatus::Succeeded;
    }
};

enum class SceneMaterialReferenceKind
{
    Mesh,
    Terrain
};

struct SceneMaterialReference
{
    std::string scenePath;
    std::string objectIdentity;
    std::string objectType;
    SceneMaterialReferenceKind kind = SceneMaterialReferenceKind::Mesh;
    std::string assetPath;
    std::string assetType;
    std::optional<uint32_t> section;
    std::string slot;
    std::string materialInstancePath;
    EditorNavigationOrigin navigationOrigin;
};

struct SceneMaterialReferenceResult
{
    EditorCommandStatus status = EditorCommandStatus::Failed;
    EditorErrorCode errorCode = EditorErrorCode::None;
    std::string message;
    std::vector<SceneMaterialReference> references;

    bool Succeeded() const noexcept
    {
        return status == EditorCommandStatus::Succeeded;
    }
};

// 该服务由命令执行线程拥有。生产者只传递现有 editorCommand.h 中的 typed payload。
class MaterialInstanceAssetDocumentService
{
public:
    explicit MaterialInstanceAssetDocumentService(MaterialAssetRoots roots);

    const MaterialAssetRoots& Roots() const noexcept { return roots; }

    MaterialInstanceAssetListResult ListMaterialInstanceAssets(
        const ListMaterialInstanceAssetsPayload& payload) const;
    MaterialInstanceAssetListResult ListMaterialInstanceAssets(
        std::string_view searchText = {},
        uint32_t pageIndex = 0,
        uint32_t pageSize = 50) const;

    MaterialAssetOperationResult Open(
        const OpenMaterialInstanceAssetPayload& payload);
    MaterialAssetOperationResult Open(
        std::string_view assetPath,
        std::optional<EditorNavigationOrigin> origin = std::nullopt);

    MaterialAssetOperationResult Select(
        const MaterialInstanceAssetPathPayload& payload);
    MaterialAssetOperationResult Select(std::string_view assetPath);

    MaterialAssetOperationResult Get(
        const MaterialInstanceAssetPathPayload& payload) const;
    MaterialAssetOperationResult Get(std::string_view assetPath) const;

    MaterialAssetOperationResult SetParameter(
        const SetMaterialParameterOverridePayload& payload);
    MaterialAssetOperationResult SetParameter(
        std::string_view assetPath,
        std::string_view parameter,
        EditorMaterialParameterType parameterType,
        const EditorMaterialParameterValue& value);
    MaterialAssetOperationResult SetParameter(
        std::string_view assetPath,
        std::string_view parameter,
        EditorMaterialParameterType parameterType,
        const nlohmann::json& value);

    MaterialAssetOperationResult ClearParameter(
        const ClearMaterialParameterOverridePayload& payload);
    MaterialAssetOperationResult ClearParameter(
        std::string_view assetPath,
        std::string_view parameter);

    MaterialAssetOperationResult SetTexture(
        const SetMaterialTextureOverridePayload& payload);
    MaterialAssetOperationResult SetTexture(
        std::string_view assetPath,
        std::string_view slot,
        std::string_view textureAssetPath);

    MaterialAssetOperationResult ClearTexture(
        const ClearMaterialTextureOverridePayload& payload);
    MaterialAssetOperationResult ClearTexture(
        std::string_view assetPath,
        std::string_view slot);

    MaterialAssetOperationResult Reset(
        const ResetMaterialInstanceOverridesPayload& payload);
    MaterialAssetOperationResult Reset(
        std::string_view assetPath,
        EditorResetScope scope = EditorResetScope::All);

    MaterialAssetOperationResult Revert(
        const MaterialInstanceAssetPathPayload& payload);
    MaterialAssetOperationResult Revert(std::string_view assetPath);

    MaterialAssetOperationResult Reload(
        const ReloadMaterialInstanceDocumentPayload& payload);
    MaterialAssetOperationResult Reload(
        std::string_view assetPath,
        EditorDirtyDocumentPolicy dirtyPolicy =
            EditorDirtyDocumentPolicy::RequireClean);

    MaterialAssetOperationResult Close(
        const CloseMaterialInstanceAssetPayload& payload);
    MaterialAssetOperationResult Close(
        std::string_view assetPath,
        EditorDirtyDocumentPolicy dirtyPolicy =
            EditorDirtyDocumentPolicy::RequireClean);

    SceneMaterialReferenceResult ResolveSceneMaterialAsset(
        const ResolveSceneMaterialAssetPayload& payload) const;
    SceneMaterialReferenceResult ResolveSceneMaterialAsset(
        std::string_view scenePath,
        std::string_view objectIdentity,
        std::optional<uint32_t> section = std::nullopt) const;

    bool HasOpenDocument(std::string_view assetPath) const;
    std::optional<MaterialInstanceDocumentSnapshot> GetSelectedSnapshot() const;
    const std::optional<std::string>& SelectedPath() const noexcept
    {
        return selectedPath;
    }

private:
    struct Document
    {
        std::string normalizedPath;
        std::filesystem::path absolutePath;
        nlohmann::json baselineJson = nlohmann::json::object();
        nlohmann::json workingJson = nlohmann::json::object();
        nlohmann::json materialJson = nlohmann::json::object();
        nlohmann::json baselineParameters = nlohmann::json::object();
        nlohmann::json baselineTextures = nlohmann::json::object();
        nlohmann::json workingParameters = nlohmann::json::object();
        nlohmann::json workingTextures = nlohmann::json::object();
        std::string materialPath;
        std::filesystem::path absoluteMaterialPath;
        EditorDocumentRevision revision = 1;
        MaterialInstanceDocumentState state =
            MaterialInstanceDocumentState::Clean;
        std::string diagnostic;
        std::vector<EditorNavigationOrigin> navigationOrigins;
    };

    MaterialAssetOperationResult MakeDocumentResult(
        const Document& document,
        std::string message,
        bool alreadyOpen = false) const;
    MaterialInstanceDocumentSnapshot BuildSnapshot(
        const Document& document) const;
    MaterialAssetOperationResult MakeFailure(
        EditorCommandStatus status,
        EditorErrorCode errorCode,
        std::string message) const;

    Document* FindDocument(std::string_view assetPath);
    const Document* FindDocument(std::string_view assetPath) const;
    MaterialInstancePathResult NormalizePath(std::string_view assetPath) const;
    MaterialAssetOperationResult OpenNormalized(
        const MaterialInstancePathResult& pathResult,
        std::optional<EditorNavigationOrigin> origin);

    MaterialAssetRoots roots;
    std::map<std::string, Document> documents;
    std::optional<std::string> selectedPath;
};

} // namespace VL::Editor::Asset
