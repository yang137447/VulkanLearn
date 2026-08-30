#include "editor/selection/materialInstanceSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "editor/command/editorCommand.h"
#include "editor/runtime/materialInstanceEditorRuntime.h"

namespace VL::Editor::Selection
{
namespace
{

struct SelectionRay
{
    Eigen::Vector3f origin = Eigen::Vector3f::Zero();
    Eigen::Vector3f direction = Eigen::Vector3f::Zero();
};

bool IsFinite(const Eigen::Vector3f& value)
{
    return value.allFinite();
}

bool IsFinite(const Eigen::Matrix4f& value)
{
    return value.allFinite();
}

std::optional<SelectionRay> BuildSelectionRay(
    const CameraSnapshot& camera,
    const ScenePickRequest& request)
{
    if (request.viewportWidth == 0 || request.viewportHeight == 0 ||
        !std::isfinite(request.mouseX) || !std::isfinite(request.mouseY) ||
        request.mouseX < 0.0f || request.mouseY < 0.0f ||
        request.mouseX > static_cast<float>(request.viewportWidth) ||
        request.mouseY > static_cast<float>(request.viewportHeight) ||
        !IsFinite(camera.viewProjection))
    {
        return std::nullopt;
    }

    const float normalizedX =
        request.mouseX / static_cast<float>(request.viewportWidth);
    const float normalizedY =
        request.mouseY / static_cast<float>(request.viewportHeight);
    // Camera::GetProjectionMatrix 已通过 ndcMatrix 把投影 Y 翻到 Vulkan 正向
    // viewport 的屏幕坐标约定，因此屏幕 y=0 对应最终 NDC y=-1；z 为 [0, 1]。
    const Eigen::Vector4f nearNdc(
        normalizedX * 2.0f - 1.0f,
        normalizedY * 2.0f - 1.0f,
        0.0f,
        1.0f);
    const Eigen::Vector4f farNdc(
        nearNdc.x(),
        nearNdc.y(),
        1.0f,
        1.0f);

    const Eigen::Matrix4f inverseViewProjection =
        camera.viewProjection.inverse();
    if (!IsFinite(inverseViewProjection))
    {
        return std::nullopt;
    }

    const Eigen::Vector4f nearWorldHomogeneous =
        inverseViewProjection * nearNdc;
    const Eigen::Vector4f farWorldHomogeneous =
        inverseViewProjection * farNdc;
    if (!nearWorldHomogeneous.allFinite() ||
        !farWorldHomogeneous.allFinite() ||
        std::abs(nearWorldHomogeneous.w()) <= std::numeric_limits<float>::epsilon() ||
        std::abs(farWorldHomogeneous.w()) <= std::numeric_limits<float>::epsilon())
    {
        return std::nullopt;
    }

    const Eigen::Vector3f origin =
        nearWorldHomogeneous.head<3>() / nearWorldHomogeneous.w();
    const Eigen::Vector3f farPoint =
        farWorldHomogeneous.head<3>() / farWorldHomogeneous.w();
    const Eigen::Vector3f unnormalizedDirection = farPoint - origin;
    if (!IsFinite(origin) || !IsFinite(unnormalizedDirection) ||
        unnormalizedDirection.squaredNorm() <=
            std::numeric_limits<float>::epsilon())
    {
        return std::nullopt;
    }

    SelectionRay ray;
    ray.origin = origin;
    ray.direction = unnormalizedDirection.normalized();
    return ray;
}

std::optional<float> IntersectRayAabb(
    const SelectionRay& ray,
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax)
{
    if (!IsFinite(boundsMin) || !IsFinite(boundsMax) ||
        (boundsMin.array() > boundsMax.array()).any())
    {
        return std::nullopt;
    }

    float entry = 0.0f;
    float exit = std::numeric_limits<float>::max();
    constexpr float parallelEpsilon = 1.0e-6f;
    for (int axis = 0; axis < 3; ++axis)
    {
        const float direction = ray.direction[axis];
        const float origin = ray.origin[axis];
        if (std::abs(direction) <= parallelEpsilon)
        {
            if (origin < boundsMin[axis] || origin > boundsMax[axis])
            {
                return std::nullopt;
            }
            continue;
        }

        float axisEntry = (boundsMin[axis] - origin) / direction;
        float axisExit = (boundsMax[axis] - origin) / direction;
        if (axisEntry > axisExit)
        {
            std::swap(axisEntry, axisExit);
        }
        entry = std::max(entry, axisEntry);
        exit = std::min(exit, axisExit);
        if (entry > exit)
        {
            return std::nullopt;
        }
    }

    return exit < 0.0f ? std::nullopt : std::optional<float>(entry);
}

bool IsBetterSelection(
    const MaterialInstanceSelection& candidate,
    const std::optional<MaterialInstanceSelection>& current)
{
    if (!current.has_value())
    {
        return true;
    }
    constexpr float distanceEpsilon = 1.0e-5f;
    if (candidate.distance < current->distance - distanceEpsilon)
    {
        return true;
    }
    if (std::abs(candidate.distance - current->distance) <= distanceEpsilon)
    {
        return candidate.objectId < current->objectId;
    }
    return false;
}

std::string PacketObjectIdentity(const RenderDrawPacket& drawPacket)
{
    return drawPacket.sceneObjectIdentity.empty()
        ? drawPacket.debugName
        : drawPacket.sceneObjectIdentity;
}

std::string PacketDisplayName(const RenderDrawPacket& drawPacket)
{
    const std::string objectIdentity = PacketObjectIdentity(drawPacket);
    return drawPacket.debugName.empty() ? objectIdentity : drawPacket.debugName;
}

bool IsValidAabb(
    const Eigen::Vector3f& boundsMin,
    const Eigen::Vector3f& boundsMax)
{
    return IsFinite(boundsMin) && IsFinite(boundsMax) &&
        !(boundsMin.array() > boundsMax.array()).any();
}

void IncludeAabb(
    Eigen::Vector3f& boundsMin,
    Eigen::Vector3f& boundsMax,
    bool& hasBounds,
    const Eigen::Vector3f& candidateMin,
    const Eigen::Vector3f& candidateMax)
{
    if (!IsValidAabb(candidateMin, candidateMax))
    {
        return;
    }

    if (!hasBounds)
    {
        boundsMin = candidateMin;
        boundsMax = candidateMax;
        hasBounds = true;
        return;
    }

    boundsMin = boundsMin.cwiseMin(candidateMin);
    boundsMax = boundsMax.cwiseMax(candidateMax);
}

MaterialInstanceModelMaterial BuildModelMaterial(
    const RenderDrawPacket& drawPacket)
{
    MaterialInstanceModelMaterial material;
    material.objectId = drawPacket.objectId;
    material.materialSlotIndex = drawPacket.materialSlotIndex;
    material.materialSlotName = drawPacket.materialSlotName;
    material.materialInstancePath = drawPacket.materialInstance.key;
    material.displayName = drawPacket.materialSlotName.empty()
        ? drawPacket.materialInstance.key
        : drawPacket.materialSlotName;
    material.worldBoundsMin = drawPacket.worldBoundsMin;
    material.worldBoundsMax = drawPacket.worldBoundsMax;
    material.hasWorldBounds = IsValidAabb(
        material.worldBoundsMin,
        material.worldBoundsMax);
    return material;
}

void MergeModelMaterial(
    MaterialInstanceModelMaterial& target,
    const MaterialInstanceModelMaterial& source)
{
    if (target.displayName.empty())
    {
        target.displayName = source.displayName;
    }
    IncludeAabb(
        target.worldBoundsMin,
        target.worldBoundsMax,
        target.hasWorldBounds,
        source.worldBoundsMin,
        source.worldBoundsMax);
}

} // namespace

MaterialInstanceModelContext AggregateSceneModel(
    const RenderScene& renderScene,
    std::string_view scenePath,
    RuntimeId objectId,
    std::string_view objectIdentity)
{
    MaterialInstanceModelContext model;
    model.worldGeneration = renderScene.worldGeneration;
    model.scenePath = std::string(scenePath);
    model.objectIdentity = std::string(objectIdentity);
    model.objectId = objectId;

    if (renderScene.worldGeneration == 0 || scenePath.empty() ||
        objectId == 0 || objectIdentity.empty())
    {
        return model;
    }

    for (const RenderDrawPacket& drawPacket : renderScene.drawPackets)
    {
        if (drawPacket.objectId == 0 || !drawPacket.materialInstance.IsValid() ||
            drawPacket.objectId != objectId ||
            PacketObjectIdentity(drawPacket) != objectIdentity ||
            drawPacket.materialInstance.key.empty())
        {
            continue;
        }

        if (model.displayName.empty())
        {
            model.displayName = PacketDisplayName(drawPacket);
        }
        IncludeAabb(
            model.worldBoundsMin,
            model.worldBoundsMax,
            model.hasWorldBounds,
            drawPacket.worldBoundsMin,
            drawPacket.worldBoundsMax);

        const MaterialInstanceModelMaterial candidate =
            BuildModelMaterial(drawPacket);
        const auto duplicate = std::find_if(
            model.materials.begin(),
            model.materials.end(),
            [&candidate](const MaterialInstanceModelMaterial& material)
            {
                return material.materialSlotIndex == candidate.materialSlotIndex &&
                    material.materialInstancePath == candidate.materialInstancePath;
            });
        if (duplicate != model.materials.end())
        {
            MergeModelMaterial(*duplicate, candidate);
            continue;
        }

        model.materials.push_back(candidate);
    }

    std::sort(
        model.materials.begin(),
        model.materials.end(),
        [](const MaterialInstanceModelMaterial& left,
           const MaterialInstanceModelMaterial& right)
        {
            if (left.materialSlotIndex != right.materialSlotIndex)
            {
                return left.materialSlotIndex < right.materialSlotIndex;
            }
            if (left.materialSlotName != right.materialSlotName)
            {
                return left.materialSlotName < right.materialSlotName;
            }
            return left.materialInstancePath < right.materialInstancePath;
        });
    return model;
}

std::optional<MaterialInstanceSelection> BuildSelectionFromModelMaterial(
    const MaterialInstanceModelContext& model,
    const MaterialInstanceModelMaterial& material,
    float distance)
{
    const RuntimeId objectId = material.objectId != 0
        ? material.objectId
        : model.objectId;
    if (model.worldGeneration == 0 || model.scenePath.empty() ||
        model.objectIdentity.empty() || objectId == 0 ||
        material.materialInstancePath.empty() || !std::isfinite(distance))
    {
        return std::nullopt;
    }

    if (model.objectId != 0 && material.objectId != 0 &&
        model.objectId != material.objectId)
    {
        return std::nullopt;
    }

    MaterialInstanceSelection selection;
    selection.worldGeneration = model.worldGeneration;
    selection.scenePath = model.scenePath;
    selection.objectId = objectId;
    selection.objectIdentity = model.objectIdentity;
    selection.materialSlotIndex = material.materialSlotIndex;
    selection.materialSlotName = material.materialSlotName;
    selection.materialInstancePath = material.materialInstancePath;
    selection.distance = distance;
    selection.modelContext = model;
    selection.displayName = material.displayName.empty()
        ? model.displayName
        : material.displayName;
    selection.worldBoundsMin = material.hasWorldBounds
        ? material.worldBoundsMin
        : model.worldBoundsMin;
    selection.worldBoundsMax = material.hasWorldBounds
        ? material.worldBoundsMax
        : model.worldBoundsMax;
    selection.hasWorldBounds = material.hasWorldBounds || model.hasWorldBounds;
    return selection;
}

namespace
{

void PopulateModelContext(
    const RenderScene& renderScene,
    MaterialInstanceSelection& selection)
{
    selection.modelContext = AggregateSceneModel(
        renderScene,
        selection.scenePath,
        selection.objectId,
        selection.objectIdentity);
}

} // namespace

std::optional<ScenePickRequest> BuildScenePickRequest(
    const PlatformEvent& event,
    uint32_t viewportWidth,
    uint32_t viewportHeight) noexcept
{
    if (event.type != PlatformEventType::MouseButtonDown ||
        event.mouseButton != PlatformMouseButton::Left ||
        viewportWidth == 0 || viewportHeight == 0)
    {
        return std::nullopt;
    }

    ScenePickRequest request;
    request.mouseX = event.mouseX;
    request.mouseY = event.mouseY;
    request.viewportWidth = viewportWidth;
    request.viewportHeight = viewportHeight;
    return request;
}

std::optional<MaterialInstanceSelection> SceneObjectPicker::Pick(
    const RenderScene& renderScene,
    std::string_view scenePath,
    const ScenePickRequest& request) const
{
    if (renderScene.worldGeneration == 0 || scenePath.empty())
    {
        return std::nullopt;
    }

    const std::optional<SelectionRay> ray =
        BuildSelectionRay(renderScene.camera, request);
    if (!ray.has_value())
    {
        return std::nullopt;
    }

    std::optional<MaterialInstanceSelection> selection;
    for (const RenderDrawPacket& drawPacket : renderScene.drawPackets)
    {
        if (drawPacket.objectId == 0 || !drawPacket.materialInstance.IsValid())
        {
            continue;
        }

        const std::optional<float> distance = IntersectRayAabb(
            *ray,
            drawPacket.worldBoundsMin,
            drawPacket.worldBoundsMax);
        if (!distance.has_value())
        {
            continue;
        }

        MaterialInstanceSelection candidate;
        candidate.worldGeneration = renderScene.worldGeneration;
        candidate.scenePath = std::string(scenePath);
        candidate.objectId = drawPacket.objectId;
        candidate.objectIdentity = PacketObjectIdentity(drawPacket);
        candidate.materialSlotIndex = drawPacket.materialSlotIndex;
        candidate.materialSlotName = drawPacket.materialSlotName;
        candidate.materialInstancePath = drawPacket.materialInstance.key;
        candidate.distance = *distance;
        candidate.displayName = PacketDisplayName(drawPacket);
        candidate.worldBoundsMin = drawPacket.worldBoundsMin;
        candidate.worldBoundsMax = drawPacket.worldBoundsMax;
        candidate.hasWorldBounds = IsValidAabb(
            candidate.worldBoundsMin,
            candidate.worldBoundsMax);
        if (candidate.objectIdentity.empty() || candidate.materialInstancePath.empty())
        {
            continue;
        }

        if (IsBetterSelection(candidate, selection))
        {
            selection = std::move(candidate);
        }
    }
    if (selection.has_value())
    {
        PopulateModelContext(renderScene, *selection);
    }
    return selection;
}

MaterialInstanceEditorRuntimeSelectionTarget::
    MaterialInstanceEditorRuntimeSelectionTarget(
        MaterialInstanceEditorRuntime& runtime)
    : runtime(&runtime)
{
}

bool MaterialInstanceEditorRuntimeSelectionTarget::OpenMaterialInstance(
    const MaterialInstanceSelection& selection)
{
    if (runtime == nullptr || !runtime->IsInitialized() ||
        selection.worldGeneration == 0 || selection.scenePath.empty() ||
        selection.objectIdentity.empty() ||
        selection.materialInstancePath.empty())
    {
        return false;
    }

    EditorCommandEnvelope command;
    command.source = EditorCommandSource::ImGui;
    command.type = EditorCommandType::OpenMaterialInstanceAsset;
    command.payload = OpenMaterialInstanceAssetPayload{
        selection.materialInstancePath,
        EditorNavigationOrigin{
            selection.scenePath,
            selection.objectIdentity,
            selection.materialSlotIndex,
            selection.materialSlotName}};
    runtime->Submit(std::move(command));
    return true;
}

bool MaterialInstanceSelectionRouter::Route(
    const MaterialInstanceSelection& selection) const
{
    return target != nullptr && target->OpenMaterialInstance(selection);
}

} // namespace VL::Editor::Selection
