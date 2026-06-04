#include "render/frontend/rendererFrontend.h"

#include <string>
#include <utility>

namespace VL
{
namespace
{

RenderDrawPacket BuildDrawPacket(const MeshDrawSnapshot& drawSnapshot)
{
    RenderDrawPacket packet;
    packet.objectId = drawSnapshot.objectId;
    packet.debugName = drawSnapshot.debugName;
    packet.mesh = drawSnapshot.mesh;
    packet.material = drawSnapshot.material;
    packet.materialInstance = drawSnapshot.materialInstance;
    packet.model = drawSnapshot.model;
    packet.previousModel = drawSnapshot.previousModel;
    packet.worldBoundsMin = drawSnapshot.worldBoundsMin;
    packet.worldBoundsMax = drawSnapshot.worldBoundsMax;
    return packet;
}

std::string BuildInvalidDrawPacketMessage(const MeshDrawSnapshot& drawSnapshot)
{
    std::string message =
        "Cannot build RenderScene because mesh draw snapshot '" +
        drawSnapshot.debugName +
        "' is missing required handles:";
    if (!drawSnapshot.mesh.IsValid())
    {
        message += " mesh";
    }
    if (!drawSnapshot.material.IsValid())
    {
        message += " material";
    }
    if (!drawSnapshot.materialInstance.IsValid())
    {
        message += " materialInstance";
    }
    return message;
}

MaterialDrawGroup& FindOrAddMaterialGroup(
    std::vector<MaterialDrawGroup>& materialGroups,
    const ResourceHandle& material)
{
    for (MaterialDrawGroup& group : materialGroups)
    {
        if (group.material.key == material.key && group.material.generation == material.generation)
        {
            return group;
        }
    }

    MaterialDrawGroup group;
    group.material = material;
    materialGroups.push_back(std::move(group));
    return materialGroups.back();
}

MaterialInstanceDrawGroup& FindOrAddMaterialInstanceGroup(
    MaterialDrawGroup& materialGroup,
    const ResourceHandle& materialInstance)
{
    for (MaterialInstanceDrawGroup& group : materialGroup.materialInstances)
    {
        if (group.materialInstance.key == materialInstance.key &&
            group.materialInstance.generation == materialInstance.generation)
        {
            return group;
        }
    }

    MaterialInstanceDrawGroup group;
    group.materialInstance = materialInstance;
    materialGroup.materialInstances.push_back(std::move(group));
    return materialGroup.materialInstances.back();
}

void AddDrawPacketToGroups(RenderScene& renderScene, const RenderDrawPacket& packet, size_t packetIndex)
{
    MaterialDrawGroup& materialGroup = FindOrAddMaterialGroup(
        renderScene.materialGroups,
        packet.material);
    MaterialInstanceDrawGroup& materialInstanceGroup = FindOrAddMaterialInstanceGroup(
        materialGroup,
        packet.materialInstance);
    materialInstanceGroup.drawPacketIndices.push_back(packetIndex);
}

} // namespace

RuntimeResult<RenderScene> RendererFrontend::BuildRenderScene(const WorldSnapshot& snapshot) const
{
    RenderScene renderScene;
    renderScene.worldGeneration = snapshot.worldGeneration;
    renderScene.frameIndex = snapshot.frameIndex;
    renderScene.camera = snapshot.camera;
    renderScene.lights = snapshot.lights;
    renderScene.environment = snapshot.environment;
    renderScene.debugViewMode = snapshot.debugViewMode;

    renderScene.drawPackets.reserve(snapshot.meshDraws.size());
    for (const MeshDrawSnapshot& drawSnapshot : snapshot.meshDraws)
    {
        if (!drawSnapshot.mesh.IsValid() ||
            !drawSnapshot.material.IsValid() ||
            !drawSnapshot.materialInstance.IsValid())
        {
            return RuntimeResult<RenderScene>::Failure(MakeRuntimeError(
                "RendererFrontend.InvalidDrawPacket",
                BuildInvalidDrawPacketMessage(drawSnapshot),
                {},
                drawSnapshot.debugName));
        }

        renderScene.drawPackets.push_back(BuildDrawPacket(drawSnapshot));
        AddDrawPacketToGroups(
            renderScene,
            renderScene.drawPackets.back(),
            renderScene.drawPackets.size() - 1);
    }

    return RuntimeResult<RenderScene>::Success(std::move(renderScene));
}

} // namespace VL
