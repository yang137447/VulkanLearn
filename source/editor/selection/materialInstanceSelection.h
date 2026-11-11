#pragma once

#include "render/frontend/scenePicking.h"

namespace VL::Editor
{

class MaterialInstanceEditorRuntime;

namespace Selection
{

// P1: pure CPU scene picking is runtime-owned. These using-declarations keep
// the existing editor call sites source-compatible until P4 moves them.
using ::VL::AggregateSceneModel;
using ::VL::BuildScenePickRequest;
using ::VL::BuildSelectionFromModelMaterial;
using ::VL::MaterialInstanceModelContext;
using ::VL::MaterialInstanceModelMaterial;
using ::VL::MaterialInstanceSelection;
using ::VL::SceneObjectPicker;
using ::VL::ScenePickRequest;

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
