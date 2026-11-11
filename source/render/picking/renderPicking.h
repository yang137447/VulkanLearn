#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "render/frontend/renderScene.h"
#include "render/frontend/scenePicking.h"

namespace VL
{

// Owns the stable CPU copy used for editor picking and the selected-model
// identity published through WorldSnapshotBuildDesc. It never holds a live
// World pointer and never touches Vulkan resources.
class RenderPickingRuntime
{
public:
    void SetSceneEnabled(bool enabled) noexcept { pickingSceneEnabled = enabled; }
    bool IsSceneEnabled() const noexcept { return pickingSceneEnabled; }

    void SetPickingScene(RenderScene&& renderScene);
    void ResetPickingScene() noexcept;
    void InvalidatePickingScene() noexcept;
    RenderScene CopyPickingScene() const { return pickingRenderScene; }
    void SwapPickingScene(RenderScene& renderScene) noexcept;

    std::optional<MaterialInstanceSelection> Pick(
        std::string_view scenePath,
        uint64_t worldGeneration,
        float mouseX,
        float mouseY,
        uint32_t viewportWidth,
        uint32_t viewportHeight) const;
    std::vector<MaterialInstanceModelContext> GetModelContexts(
        std::string_view scenePath,
        uint64_t worldGeneration) const;

    void SetSelectedMaterialInstanceModel(
        const MaterialInstanceModelContext& model,
        uint64_t worldGeneration,
        bool hasActiveWorld);
    void SetSelectedMaterialInstance(
        const MaterialInstanceSelection& selection,
        uint64_t worldGeneration,
        bool hasActiveWorld);
    void ClearSelectedMaterialInstance() noexcept;

    bool HasSelectedDraw() const noexcept { return hasSelectedDraw; }
    bool SelectedAllMaterialSlots() const noexcept
    {
        return selectedAllMaterialSlots;
    }
    VL::RuntimeId SelectedObjectId() const noexcept { return selectedObjectId; }
    uint32_t SelectedMaterialSlotIndex() const noexcept
    {
        return selectedMaterialSlotIndex;
    }
    uint64_t SelectedWorldGeneration() const noexcept
    {
        return selectedWorldGeneration;
    }

private:
    VL::RenderScene pickingRenderScene;
    bool hasPickingRenderScene = false;
    bool pickingSceneEnabled = true;

    bool hasSelectedDraw = false;
    bool selectedAllMaterialSlots = false;
    VL::RuntimeId selectedObjectId = 0;
    uint32_t selectedMaterialSlotIndex = 0;
    uint64_t selectedWorldGeneration = 0;
};

} // namespace VL
