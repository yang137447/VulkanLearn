#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "core/logStore.h"
#include "core/performanceSnapshot.h"

#include "editor/command/editorCommand.h"
#include "editor/selection/materialInstanceSelection.h"
#include "editor/assets/editorJsonAssetTypes.h"
#include "editor/ui/editorSceneActorTypes.h"

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
    // 大纲里的非网格 actor（灯光/相机/环境/风剖面）没有 MI 文档，也没有渲染端选中态。
    // 这里只登记稳定身份，Details 从冻结快照派生，面板不保存第二份选择状态。
    virtual bool RequestActorSelection(const std::string& actorIdentity) = 0;
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

// 保存弹窗消费冻结的路径/版本；不从当前标签推断要保存哪个文件。
struct EditorModifiedDocumentSnapshot
{
    std::string assetPath;
    EditorAssetKind kind = EditorAssetKind::MaterialInstance;
    uint64_t revision = 0;
    EditorDocumentStatus status = EditorDocumentStatus::Dirty;
    std::string message;
};

struct EditorSaveFeedback
{
    uint64_t sequence = 0;
    std::string assetPath;
    bool succeeded = false;
    std::string message;
};
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

// BP 修改/历史命令的 GT 终态确认；失败不推进文档版本，也必须释放 producer 的等待状态。
struct EditorBlueprintCommandFeedback
{
    uint64_t sequence = 0;
    std::string assetPath;
    bool succeeded = false;
};

// Runtime 发布的只读资产视图；不依赖 UI 库，不携带文档或 GPU 对象指针。
struct MaterialInstanceEditorSnapshot
{
    std::shared_ptr<const LogSnapshot> logs;
    std::shared_ptr<const EditorJsonAssetCatalogSnapshot> assetCatalog;
    std::shared_ptr<const EditorJsonDocumentSnapshot> jsonDocument;
    std::shared_ptr<const EditorThumbnailSnapshot> thumbnails;
    bool assetCatalogLoading = false;
    bool jsonDocumentLoading = false;
    std::string selectedDocumentPath;
    std::string statusMessage;
    bool browserLoading = false;
    std::vector<MaterialInstanceAssetEntry> assets;
    std::vector<MaterialInstanceAssetEntry> documentTabs;
    std::vector<EditorModifiedDocumentSnapshot> modifiedDocuments;
    std::vector<EditorAssetDocumentResultPayload> blueprintDocuments;
    std::vector<EditorBlueprintCommandFeedback> blueprintCommandFeedback;
    std::vector<EditorSaveFeedback> saveFeedback;
    std::vector<EditorTextureAssetSnapshot> textureAssets;
    std::vector<MaterialInstanceModelSnapshot> sceneModels;
    // 世界大纲的冻结 actor 列表（EngineLoop 在稳定帧边界从 active World 拍出）。
    // 与 sceneModels 并列而不是替换：sceneModels 仍是 MI 文档侧的值语义视图。
    // shared_ptr<const ...> 与资产目录同一约定：逐帧不深拷贝 actor 字段数组。
    std::shared_ptr<const std::vector<EditorSceneActorSnapshot>> sceneActors;
    // 大纲选中态。mesh actor 的选择同时写 selectedModel/selectedMaterialSlotIndex，
    // 其余类型（灯光/相机/环境/风）只记录这个稳定身份。
    std::string selectedActorIdentity;
    std::optional<MaterialInstanceDocumentSnapshot> activeDocument;
    std::optional<MaterialInstanceModelSnapshot> selectedModel;
    std::optional<uint32_t> selectedMaterialSlotIndex;
    // EngineLoop 在稳定帧边界从渲染侧复制的 CPU+GPU 性能快照；
    // Performance 页只消费它，不访问 RenderSystem、World 或 Vulkan。
    PerformanceSnapshot performance;
};

} // namespace VL::EditorUi
