#include "render/backend/resolvedRenderScene.h"

#include <string>
#include <utility>

#include "material.h"
#include "materialInstance.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/rendererResourceCache.h"
#include "renderableObject.h"

namespace VL
{

RuntimeResult<ResolvedRenderScene> ResolvedRenderSceneBuilder::Build(
    const RenderScene& renderScene,
    const RendererResourceCache& resourceCache) const
{
    ResolvedRenderScene resolvedScene;
    resolvedScene.materialGroups.reserve(renderScene.materialGroups.size());

    for (const MaterialDrawGroup& materialGroup : renderScene.materialGroups)
    {
        const std::shared_ptr<Material>* material = resourceCache.GetMaterial(materialGroup.material.key);
        if (material == nullptr || *material == nullptr)
        {
            return RuntimeResult<ResolvedRenderScene>::Failure(MakeRuntimeError(
                "ResolvedRenderScene.MissingMaterial",
                "RenderScene references a material that is not present in RendererResourceCache.",
                {},
                materialGroup.material.key));
        }

        ResolvedMaterialGroup resolvedMaterialGroup;
        resolvedMaterialGroup.material = *material;
        resolvedMaterialGroup.materialInstances.reserve(materialGroup.materialInstances.size());

        for (const MaterialInstanceDrawGroup& materialInstanceGroup : materialGroup.materialInstances)
        {
            const std::shared_ptr<MaterialInstance>* materialInstance =
                resourceCache.GetMaterialInstance(materialInstanceGroup.materialInstance.key);
            if (materialInstance == nullptr || *materialInstance == nullptr)
            {
                return RuntimeResult<ResolvedRenderScene>::Failure(MakeRuntimeError(
                    "ResolvedRenderScene.MissingMaterialInstance",
                    "RenderScene references a material instance that is not present in RendererResourceCache.",
                    {},
                    materialInstanceGroup.materialInstance.key));
            }

            ResolvedMaterialInstanceGroup resolvedInstanceGroup;
            resolvedInstanceGroup.materialInstance = *materialInstance;
            resolvedInstanceGroup.draws.reserve(materialInstanceGroup.drawPacketIndices.size());

            for (size_t drawPacketIndex : materialInstanceGroup.drawPacketIndices)
            {
                if (drawPacketIndex >= renderScene.drawPackets.size())
                {
                    return RuntimeResult<ResolvedRenderScene>::Failure(MakeRuntimeError(
                        "ResolvedRenderScene.DrawPacketIndexOutOfRange",
                        "RenderScene material grouping references draw packet index " +
                            std::to_string(drawPacketIndex) +
                            ", but draw packet count is " +
                            std::to_string(renderScene.drawPackets.size()) +
                            "."));
                }

                const RenderDrawPacket& packet = renderScene.drawPackets.at(drawPacketIndex);
                const std::shared_ptr<RenderableObject>* renderableObject =
                    resourceCache.GetRenderableObject(packet.mesh.key);
                if (renderableObject == nullptr || *renderableObject == nullptr)
                {
                    return RuntimeResult<ResolvedRenderScene>::Failure(MakeRuntimeError(
                        "ResolvedRenderScene.MissingRenderableObject",
                        "RenderScene references a mesh that is not present in RendererResourceCache.",
                        {},
                        packet.mesh.key));
                }

                ResolvedDrawPacket resolvedDraw;
                resolvedDraw.drawPacketIndex = drawPacketIndex;
                resolvedDraw.renderableObject = *renderableObject;

                const std::shared_ptr<RendererObjectResourceEntry>* objectResourceEntry =
                    resourceCache.GetObjectResource(packet.debugName);
                if (objectResourceEntry != nullptr)
                {
                    resolvedDraw.objectResourceEntry = *objectResourceEntry;
                }

                resolvedInstanceGroup.draws.push_back(resolvedDraw);
            }

            resolvedMaterialGroup.materialInstances.push_back(resolvedInstanceGroup);
        }

        resolvedScene.materialGroups.push_back(resolvedMaterialGroup);
    }

    return RuntimeResult<ResolvedRenderScene>::Success(std::move(resolvedScene));
}

} // namespace VL
