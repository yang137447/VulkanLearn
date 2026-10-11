#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "editor/command/editorCommand.h"
#include "editor/selection/materialInstanceSelection.h"
#include "editor/assets/editorJsonAssetTypes.h"

namespace VL::EditorUi
{

enum class EditorParameterType : uint8_t { Float, Vec2, Vec3, Vec4, Color };

struct EditorParameterValue
{
    EditorParameterType type = EditorParameterType::Float;
    std::array<float, 4> values{};
    uint32_t ComponentCount() const
    {
        switch (type)
        {
        case EditorParameterType::Float: return 1;
        case EditorParameterType::Vec2: return 2;
        case EditorParameterType::Vec3: return 3;
        case EditorParameterType::Vec4:
        case EditorParameterType::Color: return 4;
        }
        return 1;
    }
};

// 面板只生产中央协议的值语义命令，不保存文档、服务或 Vulkan 对象。
using EditorCommandEnvelope = VL::EditorCommandEnvelope;

class IEditorCommandSink
{
public:
    virtual ~IEditorCommandSink() = default;
    virtual void Submit(VL::EditorCommandEnvelope command) = 0;
};

// 面板只提交场景选择请求，具体的描边和 World 校验由 EngineLoop 在稳定帧
// 边界统一处理，避免 RmlUi 回调直接触碰 renderer-owned 状态。
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
};

struct MaterialInstanceModelSnapshot
{
    uint64_t worldGeneration = 0;
    std::string scenePath;
    VL::RuntimeId objectId = 0;
    std::string displayName;
    std::string objectIdentity;
    std::vector<MaterialInstanceModelMaterialSnapshot> materials;
};

// Runtime 发布的只读资产视图；不依赖 UI 库，不携带文档或 GPU 对象指针。
struct MaterialInstanceEditorSnapshot
{
    std::shared_ptr<const EditorJsonAssetCatalogSnapshot> assetCatalog;
    std::shared_ptr<const EditorJsonDocumentSnapshot> jsonDocument;
    bool assetCatalogLoading = false;
    bool jsonDocumentLoading = false;
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

} // namespace VL::EditorUi
