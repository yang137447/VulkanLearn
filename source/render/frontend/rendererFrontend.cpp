#include "render/frontend/rendererFrontend.h"

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
                "Cannot build RenderScene because a mesh draw snapshot is missing a mesh, material, or material instance handle.",
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
