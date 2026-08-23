#include "render/resource/rendererResourceCache.h"

#include <stdexcept>
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
        textures.empty() &&
        !subsurfaceResources &&
        !hairResources &&
        !eyeResources;
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
    globalTextures.swap(candidate.globalTextures);
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
        if (globalTextures.find(bindingName) !=
            globalTextures.end())
        {
            throw std::runtime_error(
                "Renderer global texture bindings cannot be removed after creation: " +
                bindingName);
        }
        return;
    }

    const auto existing = globalTextures.find(bindingName);
    if (existing != globalTextures.end())
    {
        if (existing->second != texture)
        {
            throw std::runtime_error(
                "Renderer global texture bindings are immutable after creation: " +
                bindingName);
        }
        return;
    }

    globalTextures.emplace(
        std::move(bindingName),
        std::move(texture));
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

// SSS lookup 只写入当前 candidate package；commit 前 active World 看不到半成品资源。
void RendererResourceCache::BindSubsurfaceResources(
    std::shared_ptr<const SubsurfaceResourceSet> resources)
{
    GetMutableWorldLocalResources().subsurfaceResources =
        std::move(resources);
}

const std::shared_ptr<const SubsurfaceResourceSet>&
RendererResourceCache::GetSubsurfaceResources() const noexcept
{
    return worldLocalResources->subsurfaceResources;
}

// Hair LUT 与 SSS 使用相同的 candidate ownership 边界，避免 World reload 中途暴露半成品。
void RendererResourceCache::BindHairResources(
    std::shared_ptr<const HairResourceSet> resources)
{
    GetMutableWorldLocalResources().hairResources = std::move(resources);
}

const std::shared_ptr<const HairResourceSet>&
RendererResourceCache::GetHairResources() const noexcept
{
    return worldLocalResources->hairResources;
}

// Eye 资源与 profile ID 一起发布，避免材质在 candidate 阶段看到另一代 LUT。
void RendererResourceCache::BindEyeResources(
    std::shared_ptr<const EyeResourceSet> resources)
{
    GetMutableWorldLocalResources().eyeResources = std::move(resources);
}

const std::shared_ptr<const EyeResourceSet>&
RendererResourceCache::GetEyeResources() const noexcept
{
    return worldLocalResources->eyeResources;
}

RendererResourceCache::PreparedEyeResourceReplacement
RendererResourceCache::PrepareEyeResourceReplacement(
    std::shared_ptr<const EyeResourceSet> resources,
    std::shared_ptr<Texture> causticLutTexture) const
{
    if (!resources || !causticLutTexture)
    {
        throw std::runtime_error(
            "Eye resource replacement requires a complete resource set and LUT texture");
    }

    PreparedEyeResourceReplacement replacement;
    replacement.previousPackage = worldLocalResources;
    replacement.replacementPackage =
        std::make_shared<WorldLocalResourcePackage>(*worldLocalResources);
    replacement.replacementPackage->eyeResources = std::move(resources);
    replacement.replacementPackage->worldTextures["eyeCausticLut"] =
        std::move(causticLutTexture);
    return replacement;
}

RendererResourceCache::WorldLocalResourcePackageHandle
RendererResourceCache::CommitPreparedEyeResourceReplacement(
    PreparedEyeResourceReplacement&& replacement) noexcept
{
    if (!replacement.replacementPackage)
    {
        return {};
    }

    WorldLocalResourcePackageHandle retired =
        std::move(worldLocalResources);
    worldLocalResources = std::move(replacement.replacementPackage);
    return retired;
}


} // namespace VL
