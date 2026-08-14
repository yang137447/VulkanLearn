#include "render/resource/rendererResourceCache.h"

#include <utility>

#include "materialInstance.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/resourceRetireQueue.h"

namespace VL
{

bool RendererResourceCache::WorldLocalResourcePackage::Empty() const noexcept
{
    return worldTextures.empty() &&
        renderableObjects.empty() &&
        materials.empty() &&
        materialInstances.empty() &&
        objectResources.empty() &&
        textures.empty();
}

RendererResourceCache::RendererResourceCache()
    : worldLocalResources(
          std::make_shared<WorldLocalResourcePackage>())
{
}

RendererResourceCache::RendererResourceCache(
    std::unordered_map<std::string, std::shared_ptr<Texture>>
        inheritedGlobalTextures,
    uint64_t ownerGeneration)
    : globalTextures(std::move(inheritedGlobalTextures)),
      worldLocalResources(
          std::make_shared<WorldLocalResourcePackage>()),
      retireWorldLocalResourcesOnClear(false)
{
    worldLocalResources->ownerGeneration = ownerGeneration;
}

void RendererResourceCache::Clear()
{
    globalTextures.clear();
    ClearWorldLocalResources();
}

RendererResourceCache RendererResourceCache::BeginCandidate(
    uint64_t ownerGeneration) const
{
    return RendererResourceCache(
        globalTextures,
        ownerGeneration);
}

RendererResourceCache::ImmutableWorldLocalResourceRefs
RendererResourceCache::CaptureActiveWorldLocalResources() const noexcept
{
    return worldLocalResources;
}

RendererResourceCache::WorldLocalResourcePackageHandle
RendererResourceCache::CommitCandidate(
    RendererResourceCache&& candidate) noexcept
{
    worldLocalResources.swap(candidate.worldLocalResources);
    return std::move(candidate.worldLocalResources);
}

RendererResourceCache::WorldLocalResourcePackage&
RendererResourceCache::GetMutableWorldLocalResources()
{
    if (!worldLocalResources.unique())
    {
        worldLocalResources =
            std::make_shared<WorldLocalResourcePackage>(
                *worldLocalResources);
    }
    return *worldLocalResources;
}

void RendererResourceCache::BeginWorldLocalResourceLoad(uint64_t ownerGeneration)
{
    ClearWorldLocalResources();
    GetMutableWorldLocalResources().ownerGeneration = ownerGeneration;
}

void RendererResourceCache::ClearWorldLocalResources()
{
    WorldLocalResourcePackageHandle replacement =
        std::make_shared<WorldLocalResourcePackage>();
    WorldLocalResourcePackageHandle retiredPackage =
        std::move(worldLocalResources);
    worldLocalResources = std::move(replacement);

    if (!retireWorldLocalResourcesOnClear ||
        !retiredPackage ||
        retiredPackage->Empty())
    {
        return;
    }

    ResourceRetireQueue& retireQueue = ResourceRetireQueue::GetInstance();
    const uint64_t lastUsedEpoch = retireQueue.GetLastSubmittedEpoch();
    const uint64_t ownerGeneration =
        retiredPackage->ownerGeneration;
    retireQueue.RetireShared(
        "WorldLocalResourcePackage",
        ownerGeneration,
        lastUsedEpoch,
        std::move(retiredPackage));
    retireQueue.CollectCompletedEpoch(retireQueue.GetLastCompletedEpoch());
}

RendererResourceCache::WorldLocalResourceSnapshot RendererResourceCache::CaptureWorldLocalResources() const
{
    return *worldLocalResources;
}

void RendererResourceCache::RestoreWorldLocalResources(WorldLocalResourceSnapshot snapshot)
{
    WorldLocalResourcePackageHandle restoredPackage =
        std::make_shared<WorldLocalResourcePackage>(
            std::move(snapshot));
    ClearWorldLocalResources();
    worldLocalResources = std::move(restoredPackage);
}

void RendererResourceCache::ShutdownSwapchainDependentWorldResources()
{
    for (auto& [objectName, objectResource] :
         worldLocalResources->objectResources)
    {
        if (objectResource)
        {
            objectResource->Shutdown();
        }
    }

    for (auto& [materialKey, materialInstance] :
         worldLocalResources->materialInstances)
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
    WorldLocalResourcePackage& resources =
        GetMutableWorldLocalResources();
    if (!texture)
    {
        resources.worldTextures.erase(bindingName);
        return;
    }

    resources.worldTextures[std::move(bindingName)] =
        std::move(texture);
}

const std::shared_ptr<Texture>* RendererResourceCache::GetGlobalTexture(std::string_view bindingName) const
{
    auto textureIt = globalTextures.find(std::string(bindingName));
    if (textureIt != globalTextures.end())
    {
        return &textureIt->second;
    }

    textureIt =
        worldLocalResources->worldTextures.find(
            std::string(bindingName));
    if (textureIt != worldLocalResources->worldTextures.end())
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
    auto textureIt =
        worldLocalResources->worldTextures.find(
            std::string(bindingName));
    if (textureIt != worldLocalResources->worldTextures.end())
    {
        return &textureIt->second;
    }

    return nullptr;
}


void RendererResourceCache::BindRenderableObject(
    std::string objectKey,
    std::shared_ptr<RenderableObject> object)
{
    WorldLocalResourcePackage& resources =
        GetMutableWorldLocalResources();
    if (!object)
    {
        resources.renderableObjects.erase(objectKey);
        return;
    }

    resources.renderableObjects[std::move(objectKey)] =
        std::move(object);
}

const std::shared_ptr<RenderableObject>* RendererResourceCache::GetRenderableObject(
    std::string_view objectKey) const
{
    auto objectIt =
        worldLocalResources->renderableObjects.find(
            std::string(objectKey));
    if (objectIt != worldLocalResources->renderableObjects.end())
    {
        return &objectIt->second;
    }

    return nullptr;
}

void RendererResourceCache::BindMaterial(std::string materialKey, std::shared_ptr<Material> material)
{
    WorldLocalResourcePackage& resources =
        GetMutableWorldLocalResources();
    if (!material)
    {
        resources.materials.erase(materialKey);
        return;
    }

    resources.materials[std::move(materialKey)] =
        std::move(material);
}

const std::shared_ptr<Material>* RendererResourceCache::GetMaterial(std::string_view materialKey) const
{
    auto materialIt =
        worldLocalResources->materials.find(
            std::string(materialKey));
    if (materialIt != worldLocalResources->materials.end())
    {
        return &materialIt->second;
    }

    return nullptr;
}

void RendererResourceCache::BindMaterialInstance(
    std::string materialInstanceKey,
    std::shared_ptr<MaterialInstance> materialInstance)
{
    WorldLocalResourcePackage& resources =
        GetMutableWorldLocalResources();
    if (!materialInstance)
    {
        resources.materialInstances.erase(
            materialInstanceKey);
        return;
    }

    resources.materialInstances[
        std::move(materialInstanceKey)] =
        std::move(materialInstance);
}

const std::shared_ptr<MaterialInstance>* RendererResourceCache::GetMaterialInstance(
    std::string_view materialInstanceKey) const
{
    auto materialInstanceIt =
        worldLocalResources->materialInstances.find(
            std::string(materialInstanceKey));
    if (materialInstanceIt !=
        worldLocalResources->materialInstances.end())
    {
        return &materialInstanceIt->second;
    }

    return nullptr;
}

void RendererResourceCache::BindObjectResource(
    std::string objectName,
    std::shared_ptr<RendererObjectResourceEntry> objectResource)
{
    WorldLocalResourcePackage& resources =
        GetMutableWorldLocalResources();
    if (!objectResource)
    {
        resources.objectResources.erase(objectName);
        return;
    }

    resources.objectResources[std::move(objectName)] =
        std::move(objectResource);
}

const std::shared_ptr<RendererObjectResourceEntry>* RendererResourceCache::GetObjectResource(
    std::string_view objectName) const
{
    auto objectResourceIt =
        worldLocalResources->objectResources.find(
            std::string(objectName));
    if (objectResourceIt !=
        worldLocalResources->objectResources.end())
    {
        return &objectResourceIt->second;
    }

    return nullptr;
}

void RendererResourceCache::BindTexture(std::string textureKey, std::shared_ptr<Texture> texture)
{
    WorldLocalResourcePackage& resources =
        GetMutableWorldLocalResources();
    if (!texture)
    {
        resources.textures.erase(textureKey);
        return;
    }

    resources.textures[std::move(textureKey)] =
        std::move(texture);
}

const std::shared_ptr<Texture>* RendererResourceCache::GetTexture(std::string_view textureKey) const
{
    auto textureIt =
        worldLocalResources->textures.find(
            std::string(textureKey));
    if (textureIt != worldLocalResources->textures.end())
    {
        return &textureIt->second;
    }

    return nullptr;
}

} // namespace VL
