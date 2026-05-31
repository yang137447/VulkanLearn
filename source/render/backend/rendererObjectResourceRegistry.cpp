#include "render/backend/rendererObjectResourceRegistry.h"

#include <utility>

#include "materialInstance.h"
#include "render/backend/rendererBackendVulkan.h"
#include "render/backend/rendererDescriptorContext.h"
#include "render/backend/rendererObjectResourceManager.h"
#include "render/backend/resolvedRenderScene.h"
#include "render/frontend/renderScene.h"
#include "render/resource/rendererResourceCache.h"

#include <memory>
#include <stdexcept>

namespace VL
{

RendererObjectResourceEntry::RendererObjectResourceEntry(std::string objectName)
    : objectName(std::move(objectName))
{
}

RendererObjectResourceEntry::~RendererObjectResourceEntry()
{
    Shutdown();
}

void RendererObjectResourceEntry::Initialize(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    MaterialInstance& materialInstance)
{
    this->rendererBackend = &rendererBackend;

    RendererObjectResourceManager resourceManager;
    resourceManager.InitializeObjectResources(
        rendererBackend,
        descriptorContext,
        objectName,
        materialInstance,
        resources);
}

void RendererObjectResourceEntry::Shutdown()
{
    RendererObjectResourceManager resourceManager;
    resourceManager.ShutdownObjectResources(rendererBackend, resources);
    rendererBackend = nullptr;
}

void RendererObjectResourceRegistry::InitializeResolvedSceneResources(
    RendererBackendVulkan& rendererBackend,
    const RendererDescriptorContext& descriptorContext,
    const RenderScene& renderScene,
    ResolvedRenderScene& resolvedRenderScene,
    RendererResourceCache& resourceCache) const
{
    for (ResolvedMaterialGroup& materialGroup : resolvedRenderScene.materialGroups)
    {
        for (ResolvedMaterialInstanceGroup& materialInstanceGroup : materialGroup.materialInstances)
        {
            materialInstanceGroup.materialInstance->RenderInitialize(rendererBackend);
            for (ResolvedDrawPacket& draw : materialInstanceGroup.draws)
            {
                if (draw.drawPacketIndex >= renderScene.drawPackets.size())
                {
                    throw std::runtime_error(
                        "Resolved draw packet references an invalid RenderDrawPacket index");
                }

                const RenderDrawPacket& drawPacket =
                    renderScene.drawPackets[draw.drawPacketIndex];
                std::shared_ptr<RendererObjectResourceEntry> objectResourceEntry;
                const std::shared_ptr<RendererObjectResourceEntry>* cachedEntry =
                    resourceCache.GetObjectResource(drawPacket.debugName);
                if (cachedEntry != nullptr && *cachedEntry != nullptr)
                {
                    objectResourceEntry = *cachedEntry;
                }
                else
                {
                    objectResourceEntry =
                        std::make_shared<RendererObjectResourceEntry>(drawPacket.debugName);
                    resourceCache.BindObjectResource(drawPacket.debugName, objectResourceEntry);
                }

                objectResourceEntry->Initialize(
                    rendererBackend,
                    descriptorContext,
                    *materialInstanceGroup.materialInstance);
                draw.objectResourceEntry = objectResourceEntry;
            }
        }
    }
}

} // namespace VL
