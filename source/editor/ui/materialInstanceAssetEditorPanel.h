#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "editor/command/editorCommand.h"
#include "editor/selection/materialInstanceSelection.h"

namespace VL::EditorUi
{

enum class EditorParameterType : uint8_t { Float, Vec2, Vec3, Vec4 };

struct EditorParameterValue
{
    EditorParameterType type = EditorParameterType::Float;
    std::array<float, 4> values{};
    uint32_t ComponentCount() const;
};

// 面板只生产中央协议的值语义命令，不保存文档、服务或 Vulkan 对象。
using EditorCommandEnvelope = VL::EditorCommandEnvelope;

class IEditorCommandSink
{
public:
    virtual ~IEditorCommandSink() = default;
    virtual void Submit(VL::EditorCommandEnvelope command) = 0;
};

// 面板只提交场景选择请求，具体的描边、World 校验和相机聚焦由 EngineLoop
// 在稳定帧边界统一处理，避免 ImGui 回调直接触碰 renderer-owned 状态。
class IMaterialInstanceSceneSelectionSink
{
public:
    virtual ~IMaterialInstanceSceneSelectionSink() = default;
    virtual bool RequestModelSelection(
        const VL::Editor::Selection::MaterialInstanceModelContext& model) = 0;
    virtual bool RequestMaterialSelection(
        const VL::Editor::Selection::MaterialInstanceSelection& selection) = 0;
};

enum class EditorDocumentStatus : uint8_t { Clean, Dirty, Saving, SaveFailed, SourceChanged };
enum class EditorValidationStatus : uint8_t { Unknown, Valid, Invalid, Running };
enum class EditorPreviewStatus : uint8_t { Disconnected, Connected, Applying, Unavailable, Failed };

struct EditorParameterChannel
{
    std::string name;
    std::string description;
    bool hasRange = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
};

struct EditorParameterSnapshot
{
    std::string name;
    std::string description;
    EditorParameterType type = EditorParameterType::Float;
    EditorParameterValue defaultValue;
    EditorParameterValue effectiveValue;
    std::optional<EditorParameterValue> overrideValue;
    bool active = true;
    bool hasRange = false;
    float minValue = 0.0f;
    float maxValue = 0.0f;
    std::array<EditorParameterChannel, 4> channels;
};

struct EditorTextureBindingSnapshot
{
    std::string slotName;
    std::string description;
    std::string defaultAssetPath;
    std::string effectiveAssetPath;
    std::optional<std::string> overrideAssetPath;
    bool active = true;
};

struct EditorRenderStateSnapshot
{
    std::string name;
    std::string defaultValue;
    std::string effectiveValue;
    std::optional<std::string> overrideValue;
};

struct EditorTextureAssetSnapshot
{
    std::string assetPath;
    bool valid = false;
    std::string diagnostic;
};

struct MaterialInstanceDocumentSnapshot
{
    std::string assetPath;
    std::string baseMaterialPath;
    std::string schemaDigest;
    uint64_t revision = 0;
    EditorDocumentStatus status = EditorDocumentStatus::Clean;
    EditorValidationStatus validation = EditorValidationStatus::Unknown;
    EditorPreviewStatus preview = EditorPreviewStatus::Disconnected;
    std::string validationMessage;
    std::string previewMessage;
    std::vector<EditorParameterSnapshot> parameters;
    std::vector<EditorTextureBindingSnapshot> textures;
    std::vector<EditorRenderStateSnapshot> renderStates;
};

struct MaterialInstanceAssetEntry { std::string assetPath; bool dirty = false; };

struct MaterialInstanceModelMaterialSnapshot
{
    VL::RuntimeId objectId = 0;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    std::string materialInstancePath;
    std::array<float, 3> worldBoundsMin{};
    std::array<float, 3> worldBoundsMax{};
};

struct MaterialInstanceModelSnapshot
{
    uint64_t worldGeneration = 0;
    std::string scenePath;
    VL::RuntimeId objectId = 0;
    std::string displayName;
    std::string objectIdentity;
    std::array<float, 3> worldBoundsMin{};
    std::array<float, 3> worldBoundsMax{};
    std::vector<MaterialInstanceModelMaterialSnapshot> materials;
};

struct MaterialInstanceEditorSnapshot
{
    std::string selectedDocumentPath;
    std::string statusMessage;
    bool browserLoading = false;
    std::vector<MaterialInstanceAssetEntry> assets;
    std::vector<MaterialInstanceAssetEntry> documentTabs;
    std::vector<EditorTextureAssetSnapshot> textureAssets;
    std::vector<MaterialInstanceModelSnapshot> sceneModels;
    std::optional<MaterialInstanceDocumentSnapshot> activeDocument;
    std::optional<MaterialInstanceModelSnapshot> selectedModel;
    std::optional<uint32_t> selectedMaterialSlotIndex;
};

class MaterialInstanceAssetEditorPanel
{
public:
    explicit MaterialInstanceAssetEditorPanel(IEditorCommandSink* commandSink = nullptr);
    void SetCommandSink(IEditorCommandSink* commandSink);
    void SetSceneSelectionSink(IMaterialInstanceSceneSelectionSink* selectionSink);
    void SetSnapshot(const MaterialInstanceEditorSnapshot& snapshot);
    void SetVisible(bool visible);
    bool IsVisible() const { return visible; }
    void Build(const MaterialInstanceEditorSnapshot& snapshot);
    // 上层每帧只 Build 一次，多个 Dock 内容入口共享同一份只读快照。
    void RenderNavigationContents();
    void RenderInspectorContents();
    // 保留单窗口兼容入口；只组合内容，不重复 Build 或创建独立窗口。
    void RenderContents();
    void Render();

private:
    void Submit(EditorCommandEnvelope command);
    std::optional<uint64_t> ActiveRevision() const;
#ifdef VULKANLEARN_ENABLE_DEVELOPER_UI
    void RenderToolbar();
    void RenderSceneModels();
    void RenderSelectedModelMaterials();
    void RenderTabs();
    void RenderDetails();
    void RenderGeneral(const MaterialInstanceDocumentSnapshot& document);
    void RenderParameters(const MaterialInstanceDocumentSnapshot& document);
    void RenderRenderStates(const MaterialInstanceDocumentSnapshot& document);
    void RenderTextureBindings(const MaterialInstanceDocumentSnapshot& document);
    void RenderParameter(const MaterialInstanceDocumentSnapshot& document, const EditorParameterSnapshot& parameter);
    void RenderTexture(const MaterialInstanceDocumentSnapshot& document, const EditorTextureBindingSnapshot& texture);
    void RenderRenderState(const MaterialInstanceDocumentSnapshot& document, const EditorRenderStateSnapshot& state);
    void RenderStatusBar();
    bool DrawValue(const EditorParameterSnapshot& parameter, EditorParameterValue& value) const;
#endif
    IEditorCommandSink* commandSink = nullptr;
    IMaterialInstanceSceneSelectionSink* sceneSelectionSink = nullptr;
    MaterialInstanceEditorSnapshot snapshot;
    std::string pickerSlot;
    std::string localStatus;
    uint64_t nextCommandId = 1;
    bool visible = true;
    bool showOnlyActive = false;
    bool openTexturePicker = false;
    std::array<char, 256> assetSearch{};
    std::array<char, 256> textureSearch{};
    std::string pendingParameterEditAssetPath;
    uint64_t pendingParameterEditRevision = 0;
    std::unordered_map<std::string, EditorParameterValue> pendingParameterEdits;
};

} // namespace VL::EditorUi
