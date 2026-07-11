#include "render/resource/rendererResourceCache.h"

#include <utility>

#include "materialInstance.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/resourceRetireQueue.h"

namespace VL
{
namespace
{

template <typename T>
void RetireResourceMap(
    std::unordered_map<std::string, std::shared_ptr<T>>& resources,
    const char* resourceKind,
    uint64_t ownerGeneration,
    uint64_t lastUsedEpoch)
{
    ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
    for (auto& [resourceKey, resource] : resources)
    {
        retireQueue.RetireShared(
            std::string(resourceKind) + ":" + resourceKey,
            ownerGeneration,
            lastUsedEpoch,
            std::move(resource));
    }
    resources.clear();
}

} // namespace

void RendererResourceCache::Clear()
{
    globalTextures.clear();
    ClearWorldLocalResources();
    currentWorldGeneration = 0;
}

void RendererResourceCache::BeginWorldLocalResourceLoad(uint64_t ownerGeneration)
{
    ClearWorldLocalResources();
    currentWorldGeneration = ownerGeneration;
}

void RendererResourceCache::ClearWorldLocalResources()
{
    ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
    const uint64_t lastUsedEpoch = retireQueue.GetLastSubmittedEpoch();

    RetireResourceMap(worldTextures, "WorldTexture", currentWorldGeneration, lastUsedEpoch);
    RetireResourceMap(renderableObjects, "RenderableObject", currentWorldGeneration, lastUsedEpoch);
    RetireResourceMap(materials, "Material", currentWorldGeneration, lastUsedEpoch);
    RetireResourceMap(materialInstances, "MaterialInstance", currentWorldGeneration, lastUsedEpoch);
    RetireResourceMap(objectResources, "ObjectGpuResources", currentWorldGeneration, lastUsedEpoch);
    RetireResourceMap(textures, "Texture", currentWorldGeneration, lastUsedEpoch);
    retireQueue.CollectCompletedEpoch(retireQueue.GetLastCompletedEpoch());
}

RendererResourceCache::WorldLocalResourceSnapshot RendererResourceCache::CaptureWorldLocalResources() const
{
    WorldLocalResourceSnapshot snapshot;
    snapshot.ownerGeneration = currentWorldGeneration;
    snapshot.worldTextures = worldTextures;
    snapshot.renderableObjects = renderableObjects;
    snapshot.materials = materials;
    snapshot.materialInstances = materialInstances;
    snapshot.objectResources = objectResources;
    snapshot.textures = textures;
    return snapshot;
}

void RendererResourceCache::RestoreWorldLocalResources(WorldLocalResourceSnapshot snapshot)
{
    ClearWorldLocalResources();
    currentWorldGeneration = snapshot.ownerGeneration;
    worldTextures = std::move(snapshot.worldTextures);
    renderableObjects = std::move(snapshot.renderableObjects);
    materials = std::move(snapshot.materials);
    materialInstances = std::move(snapshot.materialInstances);
    objectResources = std::move(snapshot.objectResources);
    textures = std::move(snapshot.textures);
}

void RendererResourceCache::ShutdownSwapchainDependentWorldResources()
{
    for (auto& [objectName, objectResource] : objectResources)
    {
        if (objectResource)
        {
            objectResource->Shutdown();
        }
    }

    for (auto& [materialKey, materialInstance] : materialInstances)
    {
        if (materialInstance)
        {
            materialInstance->ShutdownRenderResources();
        }
    }
}

void RendererResourceCache::BindGlobalTexture(std::string bindingName, std::shared_ptr<Texture> texture)
{
    if (!texture)
    {
        globalTextures.erase(bindingName);
        return;
    }

    globalTextures[std::move(bindingName)] = std::move(texture);
}

void RendererResourceCache::BindWorldTexture(std::string bindingName, std::shared_ptr<Texture> texture)
{
    if (!texture)
    {
        worldTextures.erase(bindingName);
        return;
    }

    worldTextures[std::move(bindingName)] = std::move(texture);
}

const std::shared_ptr<Texture>* RendererResourceCache::GetGlobalTexture(std::string_view bindingName) const
{
    auto textureIt = globalTextures.find(std::string(bindingName));
    if (textureIt != globalTextures.end())
    {
        return &textureIt->second;
    }

    textureIt = worldTextures.find(std::string(bindingName));
    if (textureIt != worldTextures.end())
    {
        return &textureIt->second;
    }

    return nullptr;
}

bool RendererResourceCache::HasGlobalTexture(std::string_view bindingName) const
{
    auto textureIt = globalTextures.find(std::string(bindingName));
    return textureIt != globalTextures.end() && textureIt->second != nullptr;
}

const std::shared_ptr<Texture>* RendererResourceCache::GetWorldTexture(std::string_view bindingName) const
{
    auto textureIt = worldTextures.find(std::string(bindingName));
    if (textureIt != worldTextures.end())
    {
        return &textureIt->second;
    }

    return nullptr;
}


void RendererResourceCache::BindRenderableObject(
    std::string objectKey,
    std::shared_ptr<RenderableObject> object)
{
    if (!object)
    {
        renderableObjects.erase(objectKey);
        return;
    }

    renderableObjects[std::move(objectKey)] = std::move(object);
}

const std::shared_ptr<RenderableObject>* RendererResourceCache::GetRenderableObject(
    std::string_view objectKey) const
{
    auto objectIt = renderableObjects.find(std::string(objectKey));
    if (objectIt == renderableObjects.end())
    {
        return nullptr;
    }

    return &objectIt->second;
}

void RendererResourceCache::BindMaterial(std::string materialKey, std::shared_ptr<Material> material)
{
    if (!material)
    {
        materials.erase(materialKey);
        return;
    }

    materials[std::move(materialKey)] = std::move(material);
}

const std::shared_ptr<Material>* RendererResourceCache::GetMaterial(std::string_view materialKey) const
{
    auto materialIt = materials.find(std::string(materialKey));
    if (materialIt == materials.end())
    {
        return nullptr;
    }

    return &materialIt->second;
}

void RendererResourceCache::BindMaterialInstance(
    std::string materialInstanceKey,
    std::shared_ptr<MaterialInstance> materialInstance)
{
    if (!materialInstance)
    {
        materialInstances.erase(materialInstanceKey);
        return;
    }

    materialInstances[std::move(materialInstanceKey)] = std::move(materialInstance);
}

const std::shared_ptr<MaterialInstance>* RendererResourceCache::GetMaterialInstance(
    std::string_view materialInstanceKey) const
{
    auto materialInstanceIt = materialInstances.find(std::string(materialInstanceKey));
    if (materialInstanceIt == materialInstances.end())
    {
        return nullptr;
    }

    return &materialInstanceIt->second;
}

void RendererResourceCache::BindObjectResource(
    std::string objectName,
    std::shared_ptr<RendererObjectResourceEntry> objectResource)
{
    if (!objectResource)
    {
        objectResources.erase(objectName);
        return;
    }

    objectResources[std::move(objectName)] = std::move(objectResource);
}

const std::shared_ptr<RendererObjectResourceEntry>* RendererResourceCache::GetObjectResource(
    std::string_view objectName) const
{
    auto objectResourceIt = objectResources.find(std::string(objectName));
    if (objectResourceIt == objectResources.end())
    {
        return nullptr;
    }

    return &objectResourceIt->second;
}

void RendererResourceCache::BindTexture(std::string textureKey, std::shared_ptr<Texture> texture)
{
    if (!texture)
    {
        textures.erase(textureKey);
        return;
    }

    textures[std::move(textureKey)] = std::move(texture);
}

const std::shared_ptr<Texture>* RendererResourceCache::GetTexture(std::string_view textureKey) const
{
    auto textureIt = textures.find(std::string(textureKey));
    if (textureIt == textures.end())
    {
        return nullptr;
    }

    return &textureIt->second;
}

} // namespace VL
