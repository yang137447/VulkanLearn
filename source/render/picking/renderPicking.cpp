#include "render/picking/renderPicking.h"

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace VL
{

void RenderPickingRuntime::SetPickingScene(RenderScene&& renderScene)
{
    pickingRenderScene = std::move(renderScene);
    hasPickingRenderScene = true;
}

void RenderPickingRuntime::InvalidatePickingScene() noexcept
{
    // 保留容器容量以便重新打开编辑器时复用；只撤销可拾取状态。
    hasPickingRenderScene = false;
}

void RenderPickingRuntime::ResetPickingScene() noexcept
{
    pickingRenderScene = RenderScene();
    hasPickingRenderScene = false;
}

void RenderPickingRuntime::SwapPickingScene(RenderScene& renderScene) noexcept
{
    std::swap(pickingRenderScene, renderScene);
}

std::optional<VL::MaterialInstanceSelection>
RenderPickingRuntime::Pick(
    std::string_view scenePath,
    uint64_t worldGeneration,
    float mouseX,
    float mouseY,
    uint32_t viewportWidth,
    uint32_t viewportHeight) const
{
    if (!hasPickingRenderScene ||
        pickingRenderScene.worldGeneration != worldGeneration)
    {
        return std::nullopt;
    }

    VL::ScenePickRequest request;
    request.mouseX = mouseX;
    request.mouseY = mouseY;
    request.viewportWidth = viewportWidth;
    request.viewportHeight = viewportHeight;
    return VL::SceneObjectPicker().Pick(
        pickingRenderScene,
        scenePath,
        request);
}

std::vector<VL::MaterialInstanceModelContext>
RenderPickingRuntime::GetModelContexts(
    std::string_view scenePath,
    uint64_t worldGeneration) const
{
    if (!hasPickingRenderScene ||
        pickingRenderScene.worldGeneration != worldGeneration)
    {
        return {};
    }

    std::vector<VL::MaterialInstanceModelContext> result;
    std::unordered_map<std::string, std::size_t> modelIndices;
    modelIndices.reserve(pickingRenderScene.drawPackets.size());
    for (const VL::RenderDrawPacket& drawPacket : pickingRenderScene.drawPackets)
    {
        if (drawPacket.objectId == 0 || !drawPacket.materialInstance.IsValid())
        {
            continue;
        }
        const std::string objectIdentity = drawPacket.sceneObjectIdentity.empty()
            ? drawPacket.debugName
            : drawPacket.sceneObjectIdentity;
        if (objectIdentity.empty())
        {
            continue;
        }

        const std::string key = std::to_string(drawPacket.objectId) + "|" +
            objectIdentity;
        auto [iterator, inserted] = modelIndices.emplace(key, result.size());
        if (inserted)
        {
            result.push_back(
                VL::AggregateSceneModel(
                    pickingRenderScene,
                    scenePath,
                    drawPacket.objectId,
                    objectIdentity));
        }
        static_cast<void>(iterator);
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const auto& left, const auto& right)
        {
            const std::string& leftName = left.displayName.empty()
                ? left.objectIdentity
                : left.displayName;
            const std::string& rightName = right.displayName.empty()
                ? right.objectIdentity
                : right.displayName;
            if (leftName != rightName)
            {
                return leftName < rightName;
            }
            return left.objectIdentity < right.objectIdentity;
        });
    return result;
}

void RenderPickingRuntime::SetSelectedMaterialInstanceModel(
    const VL::MaterialInstanceModelContext& model,
    uint64_t worldGeneration,
    bool hasActiveWorld)
{
    if (!hasActiveWorld ||
        model.worldGeneration != worldGeneration ||
        model.objectId == 0)
    {
        ClearSelectedMaterialInstance();
        return;
    }

    hasSelectedDraw = true;
    selectedAllMaterialSlots = true;
    selectedObjectId = model.objectId;
    selectedMaterialSlotIndex = 0;
    selectedWorldGeneration = model.worldGeneration;
}

void RenderPickingRuntime::SetSelectedMaterialInstance(
    const VL::MaterialInstanceSelection& selection,
    uint64_t worldGeneration,
    bool hasActiveWorld)
{
    if (!hasActiveWorld ||
        selection.worldGeneration != worldGeneration ||
        selection.objectId == 0)
    {
        ClearSelectedMaterialInstance();
        return;
    }

    hasSelectedDraw = true;
    selectedAllMaterialSlots = false;
    selectedObjectId = selection.objectId;
    selectedMaterialSlotIndex = selection.materialSlotIndex;
    selectedWorldGeneration = selection.worldGeneration;
}

void RenderPickingRuntime::ClearSelectedMaterialInstance() noexcept
{
    hasSelectedDraw = false;
    selectedAllMaterialSlots = false;
    selectedObjectId = 0;
    selectedMaterialSlotIndex = 0;
    selectedWorldGeneration = 0;
}

} // namespace VL
