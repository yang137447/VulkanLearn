#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "platform/platformEvent.h"
#include "render/frontend/renderScene.h"

namespace VL::Editor
{

class MaterialInstanceEditorRuntime;

namespace Selection
{

struct ScenePickRequest
{
    float mouseX = 0.0f;
    float mouseY = 0.0f;
    uint32_t viewportWidth = 0;
    uint32_t viewportHeight = 0;
};

struct MaterialInstanceModelMaterial
{
    RuntimeId objectId = 0;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    std::string materialInstancePath;
    std::string displayName;
};

struct MaterialInstanceModelContext
{
    uint64_t worldGeneration = 0;
    std::string scenePath;
    std::string objectIdentity;
    std::vector<MaterialInstanceModelMaterial> materials;
    RuntimeId objectId = 0;
    std::string displayName;
};

struct MaterialInstanceSelection
{
    uint64_t worldGeneration = 0;
    std::string scenePath;
    RuntimeId objectId = 0;
    std::string objectIdentity;
    uint32_t materialSlotIndex = 0;
    std::string materialSlotName;
    std::string materialInstancePath;
    float distance = 0.0f;
    MaterialInstanceModelContext modelContext;
    std::string displayName;
};

// 从稳定的 RenderScene 快照聚合一个场景模型，供 UI 列表和后续 selection
// 回写共享同一份值语义数据。
MaterialInstanceModelContext AggregateSceneModel(
    const RenderScene& renderScene,
    std::string_view scenePath,
    RuntimeId objectId,
    std::string_view objectIdentity);

// 从模型聚合结果中的一个材质项构造完整选择。该函数不依赖 renderer/UI，适合
// 场景点击和 MI 列表点击复用。
std::optional<MaterialInstanceSelection> BuildSelectionFromModelMaterial(
    const MaterialInstanceModelContext& model,
    const MaterialInstanceModelMaterial& material,
    float distance = 0.0f);

// 将平台鼠标事件转换为视口拾取请求。调用者应先让 UI 处理事件，只有未被
// UI 消费的左键事件才应交给这个函数，避免点击 ImGui/RmlUi 控件误选场景。
std::optional<ScenePickRequest> BuildScenePickRequest(
    const PlatformEvent& event,
    uint32_t viewportWidth,
    uint32_t viewportHeight) noexcept;

// 在 renderer 的 CPU RenderScene 上做轻量级射线/AABB 拾取。这里故意不持有
// Vulkan buffer 或 live World 指针，点击结果只依赖稳定帧快照，适合 GT/RT
// 分离和场景换代后的 generation 校验。
class SceneObjectPicker
{
public:
    std::optional<MaterialInstanceSelection> Pick(
        const RenderScene& renderScene,
        std::string_view scenePath,
        const ScenePickRequest& request) const;
};

class IMaterialInstanceSelectionTarget
{
public:
    virtual ~IMaterialInstanceSelectionTarget() = default;
    virtual bool OpenMaterialInstance(
        const MaterialInstanceSelection& selection) = 0;
};

// 这是 selection 到 editor runtime 的非 UI 路由适配器。它只提交 Open 命令，
// 面板显示、焦点和 F1 可见性仍由 UiSubsystem/UI agent 决定。
class MaterialInstanceEditorRuntimeSelectionTarget final
    : public IMaterialInstanceSelectionTarget
{
public:
    explicit MaterialInstanceEditorRuntimeSelectionTarget(
        MaterialInstanceEditorRuntime& runtime);

    bool OpenMaterialInstance(
        const MaterialInstanceSelection& selection) override;

private:
    MaterialInstanceEditorRuntime* runtime = nullptr;
};

class MaterialInstanceSelectionRouter
{
public:
    explicit MaterialInstanceSelectionRouter(
        IMaterialInstanceSelectionTarget* target = nullptr)
        : target(target)
    {
    }

    void SetTarget(IMaterialInstanceSelectionTarget* value) noexcept
    {
        target = value;
    }

    bool Route(const MaterialInstanceSelection& selection) const;

private:
    IMaterialInstanceSelectionTarget* target = nullptr;
};

} // namespace Selection
} // namespace VL::Editor
