#pragma once

// 文件职责：提供不依赖 renderer、World 或 Vulkan 对象的 MI Asset Document。
// Document 只拥有磁盘 JSON、M_ schema、baseline/working override 和诊断快照；
// 任何 live MaterialInstance 或预览资源都不应穿过这个边界。

#include "editor/command/editorCommand.h"
#include "editor/persistence/materialInstancePersistence.h"
#include "editor/persistence/materialInstanceSparseCandidate.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace VL::Editor
{

enum class MaterialEditorDocumentState
{
    Clean,
    Dirty,
    SaveFailed,
    SourceChanged
};

enum class MaterialEditorValidationState
{
    Unknown,
    Valid,
    Invalid
};

const char* ToString(MaterialEditorDocumentState state) noexcept;
const char* ToString(MaterialEditorValidationState state) noexcept;

struct MaterialEditorParameterChannelSnapshot
{
    std::string name;
    std::string description;
    std::optional<float> min;
    std::optional<float> max;
};

struct MaterialEditorParameterSnapshot
{
    std::string name;
    std::string description;
    EditorMaterialParameterType type = EditorMaterialParameterType::Float;
    EditorMaterialParameterValue defaultValue = 0.0f;
    EditorMaterialParameterValue effectiveValue = 0.0f;
    std::optional<EditorMaterialParameterValue> overrideValue;
    bool active = true;
    std::optional<float> min;
    std::optional<float> max;
    std::vector<MaterialEditorParameterChannelSnapshot> channels;
};

struct MaterialEditorTextureBindingSnapshot
{
    std::string slotName;
    std::string description;
    std::string defaultAssetPath;
    std::string effectiveAssetPath;
    std::optional<std::string> overrideAssetPath;
    bool active = true;
};

struct MaterialEditorRenderStateSnapshot
{
    std::string name;
    std::string defaultValue;
    std::string effectiveValue;
    std::optional<std::string> overrideValue;
};

struct MaterialEditorReferenceSnapshot
{
    std::string sceneIdentity;
    std::string objectIdentity;
    std::string objectType;
    std::string sourceAssetPath;
    std::string selector;
    std::string materialInstancePath;
};

struct MaterialEditorTextureAssetSnapshot
{
    std::string assetPath;
    bool valid = false;
    std::string diagnostic;
};

struct MaterialEditorDocumentSnapshot
{
    std::string assetPath;
    std::string baseMaterialPath;
    std::string schemaDigest;
    std::string sourceDigest;
    EditorDocumentRevision revision = 0;
    MaterialEditorDocumentState state = MaterialEditorDocumentState::Clean;
    MaterialEditorValidationState validation =
        MaterialEditorValidationState::Unknown;
    std::string validationMessage;
    std::vector<MaterialEditorParameterSnapshot> parameters;
    std::vector<MaterialEditorTextureBindingSnapshot> textures;
    std::vector<MaterialEditorRenderStateSnapshot> renderStates;
    std::vector<MaterialEditorReferenceSnapshot> references;
    // 预览和自动化入口只消费完整序列化草稿，不保存任何运行时指针。
    std::string serializedWorkingDraft;
    std::string serializedBaselineDraft;
};

struct MaterialEditorAssetEntry
{
    std::string assetPath;
    bool dirty = false;
};

struct MaterialEditorSnapshot
{
    std::string selectedDocumentPath;
    std::string statusMessage;
    bool browserLoading = false;
    std::vector<MaterialEditorAssetEntry> assets;
    std::vector<MaterialEditorAssetEntry> documentTabs;
    std::vector<MaterialEditorTextureAssetSnapshot> textureAssets;
    std::optional<MaterialEditorDocumentSnapshot> activeDocument;
};

struct MaterialEditorServiceResult
{
    bool succeeded = false;
    EditorErrorCode errorCode = EditorErrorCode::None;
    std::string message;
    std::optional<EditorDocumentRevision> documentRevision;
    std::optional<MaterialEditorDocumentSnapshot> document;
    std::optional<MaterialEditorSnapshot> editor;
    std::vector<MaterialEditorReferenceSnapshot> references;
    std::string expectedDigest;
    std::string observedDigest;
    std::string newDigest;
};

class MaterialInstanceDocumentService
{
public:
    struct Config
    {
        std::filesystem::path resourceRoot;
        std::filesystem::path projectRoot;
        bool validateTextureSources = true;
    };

    MaterialInstanceDocumentService();
    explicit MaterialInstanceDocumentService(Config config);
    explicit MaterialInstanceDocumentService(
        std::filesystem::path resourceRoot,
        std::filesystem::path projectRoot = {});

    static std::string NormalizeMaterialInstancePath(std::string_view path);
    static std::string NormalizeTextureAssetPath(std::string_view path);

    const Config& GetConfig() const noexcept { return config; }
    const std::filesystem::path& GetResourceRoot() const noexcept
    {
        return config.resourceRoot;
    }

    MaterialEditorServiceResult ListMaterialInstanceAssets(
        std::string_view searchText,
        uint32_t pageIndex,
        uint32_t pageSize) const;

    MaterialEditorServiceResult OpenMaterialInstanceAsset(
        std::string_view assetPath,
        const std::optional<EditorNavigationOrigin>& origin = std::nullopt);

    MaterialEditorServiceResult SelectMaterialInstanceDocument(
        std::string_view assetPath);

    MaterialEditorServiceResult CloseMaterialInstanceAsset(
        std::string_view assetPath,
        EditorDirtyDocumentPolicy dirtyPolicy);

    MaterialEditorServiceResult GetMaterialInstanceDocument(
        std::string_view assetPath);

    MaterialEditorServiceResult GetSelectedMaterialInstanceDocument();

    MaterialEditorServiceResult GetMaterialInstanceReferenceContext(
        std::string_view assetPath);

    MaterialEditorServiceResult SetMaterialParameterOverride(
        std::string_view assetPath,
        std::string_view parameter,
        EditorMaterialParameterType parameterType,
        const EditorMaterialParameterValue& value);

    MaterialEditorServiceResult ClearMaterialParameterOverride(
        std::string_view assetPath,
        std::string_view parameter);

    MaterialEditorServiceResult SetMaterialTextureOverride(
        std::string_view assetPath,
        std::string_view slot,
        std::string_view textureAssetPath);

    MaterialEditorServiceResult ClearMaterialTextureOverride(
        std::string_view assetPath,
        std::string_view slot);

    MaterialEditorServiceResult SetMaterialRenderStateOverride(
        std::string_view assetPath,
        EditorMaterialRenderStateField field,
        const EditorMaterialRenderStateValue& value);

    MaterialEditorServiceResult ClearMaterialRenderStateOverride(
        std::string_view assetPath,
        EditorMaterialRenderStateField field);

    MaterialEditorServiceResult ResetMaterialInstanceOverrides(
        std::string_view assetPath,
        EditorResetScope scope);

    MaterialEditorServiceResult RevertMaterialInstanceDocument(
        std::string_view assetPath);

    MaterialEditorServiceResult ReloadMaterialInstanceDocument(
        std::string_view assetPath,
        EditorDirtyDocumentPolicy dirtyPolicy);

    MaterialEditorServiceResult ValidateMaterialInstanceDocument(
        std::string_view assetPath);

    MaterialEditorServiceResult SaveMaterialInstanceDocument(
        std::string_view assetPath);

    MaterialEditorServiceResult ResolveSceneMaterialAsset(
        const ResolveSceneMaterialAssetPayload& payload);

    // Batch 在同一份临时 working 状态上验证；失败时不改变当前文档。
    MaterialEditorServiceResult ExecuteBatch(
        const ExecuteEditorCommandBatchPayload& batch,
        std::optional<EditorDocumentRevision> expectedRevision);

    MaterialEditorSnapshot BuildSnapshot();
    std::optional<MaterialEditorDocumentSnapshot> BuildDocumentSnapshot(
        std::string_view assetPath);
    bool IsDocumentOpen(std::string_view assetPath) const;
    std::optional<EditorDocumentRevision> GetDocumentRevision(
        std::string_view assetPath) const;

private:
    using Json = nlohmann::json;
    using PersistenceOverrides = Persistence::MaterialInstanceSparseOverrides;
    using PersistenceDefaults = Persistence::MaterialInstanceDefaults;

    struct Document
    {
        std::string assetPath;
        std::filesystem::path absolutePath;
        std::string baseMaterialPath;
        std::filesystem::path baseMaterialAbsolutePath;
        Json sourceJson = Json::object();
        Json materialJson = Json::object();
        PersistenceDefaults defaults;
        PersistenceOverrides baselineOverrides;
        PersistenceOverrides workingOverrides;
        ContentDigest sourceDigest{};
        ContentDigest schemaDigest{};
        EditorDocumentRevision revision = 1;
        MaterialEditorDocumentState state = MaterialEditorDocumentState::Clean;
        MaterialEditorValidationState validation =
            MaterialEditorValidationState::Unknown;
        std::string validationMessage;
        std::vector<MaterialEditorReferenceSnapshot> references;
    };

    struct LoadedDocument
    {
        std::string assetPath;
        std::filesystem::path absolutePath;
        std::string baseMaterialPath;
        std::filesystem::path baseMaterialAbsolutePath;
        Json sourceJson;
        Json materialJson;
        PersistenceDefaults defaults;
        PersistenceOverrides overrides;
        ContentDigest sourceDigest{};
        ContentDigest schemaDigest{};
    };

    Config config;
    std::map<std::string, Document> documents;
    std::string selectedDocumentPath;

    static Config DiscoverDefaultConfig();
    static bool IsParentPath(const std::filesystem::path& path);
    static std::string NormalizeRelativePath(std::string_view path);
    static EditorMaterialParameterType ToEditorParameterType(
        Persistence::MaterialInstanceNumericType type) noexcept;
    static Persistence::MaterialInstanceNumericType ToPersistenceParameterType(
        EditorMaterialParameterType type) noexcept;
    static EditorMaterialParameterValue ToEditorValue(
        const Persistence::MaterialInstanceNumericValue& value);
    static Persistence::MaterialInstanceNumericValue ToPersistenceValue(
        const EditorMaterialParameterValue& value);

    std::filesystem::path ResolveMaterialDefinitionPath(
        std::string_view materialPath) const;
    std::filesystem::path ResolveResourceFile(
        std::string_view resourceRelativePath) const;
    LoadedDocument LoadDocumentFromDisk(std::string_view assetPath) const;
    Json ReadJsonFile(const std::filesystem::path& path) const;
    void ValidateManagedMaterialInstance(
        const Json& materialJson,
        const Json& materialInstanceJson,
        std::string_view materialInstancePath) const;
    void ValidateTextureReference(
        std::string_view textureAssetPath,
        std::string_view materialInstancePath) const;
    void ValidateWorkingState(
        const Document& document,
        const PersistenceOverrides& workingOverrides) const;
    Json BuildCandidateJson(
        const Document& document,
        const PersistenceOverrides& workingOverrides) const;
    Json BuildEffectiveParameters(
        const Document& document,
        const PersistenceOverrides& workingOverrides) const;
    Json BuildEffectiveTextures(
        const Document& document,
        const PersistenceOverrides& workingOverrides) const;
    void RefreshExternalChange(Document& document);
    Document* FindDocument(std::string_view assetPath);
    const Document* FindDocument(std::string_view assetPath) const;
    Document& RequireDocument(std::string_view assetPath);
    const Document& RequireDocument(std::string_view assetPath) const;
    std::string RequireNormalizedPath(std::string_view assetPath) const;
    MaterialEditorServiceResult MakeFailure(
        EditorErrorCode errorCode,
        std::string message,
        const Document* document = nullptr) const;
    MaterialEditorServiceResult MakeSuccess(
        std::string message,
        Document* document = nullptr);
    MaterialEditorServiceResult MakeSnapshotResult(
        MaterialEditorServiceResult result,
        Document* document);
    MaterialEditorServiceResult MakeDocumentResult(
        MaterialEditorServiceResult result,
        Document* document);
    void IncrementRevision(Document& document);
    void UpdateDirtyState(Document& document);
    MaterialEditorDocumentSnapshot MakeDocumentSnapshot(
        Document& document) const;
    MaterialEditorTextureAssetSnapshot InspectTextureAsset(
        const std::filesystem::path& absolutePath,
        std::string logicalPath) const;
    std::vector<MaterialEditorTextureAssetSnapshot> ListTextureAssets() const;
    std::vector<MaterialEditorAssetEntry> ListOpenDocumentEntries() const;
    static std::string JsonString(const Json& value);

    mutable bool materialInstanceAssetCatalogCached = false;
    mutable std::vector<MaterialEditorAssetEntry> materialInstanceAssetCatalog;
    mutable bool textureAssetCatalogCached = false;
    mutable std::vector<MaterialEditorTextureAssetSnapshot> textureAssetCatalog;
};

// 允许上层按职责名接入，同时保留完整类名作为稳定 API。
using MaterialEditorDocumentService = MaterialInstanceDocumentService;

} // namespace VL::Editor
