#include "render/resource/rendererResourceCache.h"

#include <iostream>
#include <stdexcept>
#include <utility>

#include "materialInstance.h"
#include "renderableObject.h"
#include "render/backend/rendererObjectResourceRegistry.h"
#include "render/resource/renderableObjectKey.h"
#include "render/resource/resourceRetireQueue.h"
#include "world/worldWind.h"

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

RendererResourceCache RendererResourceCache::BeginActiveWorldCandidate(
    uint64_t ownerGeneration) const
{
    RendererResourceCache candidate(
        globalTextures,
        ownerGeneration);
    candidate.worldLocalResources =
        std::make_shared<WorldLocalResourcePackage>(
            *worldLocalResources);
    candidate.worldLocalResources->ownerGeneration = ownerGeneration;
    // Object descriptor layouts depend on the candidate Material. Reusing the
    // active entries would mutate live Vulkan state during prepare.
    candidate.worldLocalResources->objectResources.clear();
    return candidate;
}

RendererResourceCache::ImmutableWorldLocalResourceRefs
RendererResourceCache::CaptureActiveWorldLocalResources() const noexcept
{
    return worldLocalResources;
}

RendererResourceCache RendererResourceCache::BeginWorldExtensionCandidate(uint64_t ownerGeneration) const
{
    RendererResourceCache candidate(globalTextures, ownerGeneration);
    candidate.worldLocalResources = std::make_shared<WorldLocalResourcePackage>(*worldLocalResources);
    candidate.worldLocalResources->ownerGeneration = ownerGeneration;
    return candidate;
}

void RendererResourceCache::TrackDynamicAdditions(ImmutableWorldLocalResourceRefs baseline)
{
    auto& package = GetMutableWorldLocalResources();
    for (const auto& pair : package.renderableObjects)
        if (!baseline->renderableObjects.count(pair.first)) package.dynamicMeshes.insert(pair.first);
    for (const auto& pair : package.materials)
        if (!baseline->materials.count(pair.first)) package.dynamicMaterials.insert(pair.first);
    for (const auto& pair : package.materialInstances)
        if (!baseline->materialInstances.count(pair.first)) package.dynamicInstances.insert(pair.first);
    for (const auto& pair : package.textures)
        if (!baseline->textures.count(pair.first)) package.dynamicTextures.insert(pair.first);
}

namespace
{
template<class Map>
void MergeWorldExtensionMap(Map& active, const Map& candidate, const Map& baseline)
{
    for (const auto& original : baseline)
    {
        const auto current = active.find(original.first);
        if (current == active.end() || current->second != original.second)
            throw std::runtime_error("Blueprint resource changed while preparing extension: " + original.first);
    }
    for (const auto& added : candidate)
        if (!baseline.count(added.first))
        {
            const auto current = active.find(added.first);
            if (current != active.end() && current->second != added.second)
                throw std::runtime_error("Blueprint extension resource key conflict: " + added.first);
            active.emplace(added);
        }
}

template<class Map>
void PruneDynamicMap(Map& resources, std::set<std::string>& dynamicKeys, const std::set<std::string>& used)
{
    for (auto it = dynamicKeys.begin(); it != dynamicKeys.end();)
        if (!used.count(*it))
        {
            // 只给 placement-scoped 资源留退休证据；共享/常驻资源不刷日志。
            if (IsPlacementScopedRenderableKey(*it))
                std::cout << "[Placement] instance renderable released by package: key="
                          << *it << std::endl;
            resources.erase(*it);
            it = dynamicKeys.erase(it);
        }
        else ++it;
}
}

RendererResourceCache RendererResourceCache::RebaseWorldExtensionCandidate(
    RendererResourceCache extension, ImmutableWorldLocalResourceRefs baseline) const
{
    if (worldLocalResources->ownerGeneration != baseline->ownerGeneration)
        throw std::runtime_error("Blueprint extension generation is stale");
    auto rebased = BeginWorldExtensionCandidate(baseline->ownerGeneration);
    auto& output = *rebased.worldLocalResources;
    const auto& candidate = *extension.worldLocalResources;
    MergeWorldExtensionMap(output.worldTextures, candidate.worldTextures, baseline->worldTextures);
    MergeWorldExtensionMap(output.renderableObjects, candidate.renderableObjects, baseline->renderableObjects);
    MergeWorldExtensionMap(output.materials, candidate.materials, baseline->materials);
    MergeWorldExtensionMap(output.materialInstances, candidate.materialInstances, baseline->materialInstances);
    MergeWorldExtensionMap(output.textures, candidate.textures, baseline->textures);
    // 活动 Spawn 可以追加 descriptor；它们来自最新 package，不覆盖、不要求旧 map 指针相等。
    output.dynamicMeshes.insert(candidate.dynamicMeshes.begin(), candidate.dynamicMeshes.end());
    output.dynamicMaterials.insert(candidate.dynamicMaterials.begin(), candidate.dynamicMaterials.end());
    output.dynamicInstances.insert(candidate.dynamicInstances.begin(), candidate.dynamicInstances.end());
    output.dynamicTextures.insert(candidate.dynamicTextures.begin(), candidate.dynamicTextures.end());
    return rebased;
}

void RendererResourceCache::PruneDynamicResources(const std::set<std::string>& meshes,
    const std::set<std::string>& materials, const std::set<std::string>& instances)
{
    auto& package = GetMutableWorldLocalResources();
    PruneDynamicMap(package.renderableObjects, package.dynamicMeshes, meshes);
    PruneDynamicMap(package.materials, package.dynamicMaterials, materials);
    PruneDynamicMap(package.materialInstances, package.dynamicInstances, instances);
    // 按剩余绑定裁剪，而非等到所有动态 MI 都卸载；常驻资产不能让无关动态贴图长期滞留。
    std::set<const Texture*> usedTextures;
    for (const auto& pair : package.materialInstances)
        if (pair.second)
        {
            const auto snapshot = pair.second->CaptureStateSnapshot();
            for (const auto& binding : snapshot.textures)
                usedTextures.insert(binding.second.texture.get());
        }
    for (const auto& pair : package.worldTextures) usedTextures.insert(pair.second.get());
    for (const auto& pair : globalTextures) usedTextures.insert(pair.second.get());
    for (auto it = package.dynamicTextures.begin(); it != package.dynamicTextures.end();)
    {
        const auto texture = package.textures.find(*it);
        if (texture == package.textures.end() || !usedTextures.count(texture->second.get()))
        {
            package.textures.erase(*it);
            it = package.dynamicTextures.erase(it);
        }
        else ++it;
    }
}

std::optional<RendererResourceCache::WorldLocalMaterialInstanceCapture>
RendererResourceCache::CaptureMaterialInstanceForGeneration(
    uint64_t expectedOwnerGeneration,
    std::string_view normalizedMaterialInstancePath) const
{
    // package 句柄先复制，随后 map 查找和 MI shared_ptr 复制都在同一份
    // World-local 快照上完成；generation 不匹配时不向调用方泄露旧对象。
    const ImmutableWorldLocalResourceRefs package = worldLocalResources;
    if (!package || package->ownerGeneration != expectedOwnerGeneration)
    {
        return std::nullopt;
    }

    const auto materialInstanceIt =
        package->materialInstances.find(std::string(normalizedMaterialInstancePath));
    if (materialInstanceIt == package->materialInstances.end() ||
        !materialInstanceIt->second)
    {
        return std::nullopt;
    }

    WorldLocalMaterialInstanceCapture capture;
    capture.ownerGeneration = package->ownerGeneration;
    capture.materialInstance = materialInstanceIt->second;
    return capture;
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

std::shared_ptr<RenderableObject> RendererResourceCache::TakeRenderableObject(
    std::string_view objectKey)
{
    WorldLocalResourcePackage& resources = GetMutableWorldLocalResources();
    auto objectIt = resources.renderableObjects.find(std::string(objectKey));
    if (objectIt == resources.renderableObjects.end())
        return nullptr;
    std::shared_ptr<RenderableObject> object = std::move(objectIt->second);
    resources.renderableObjects.erase(objectIt);
    resources.dynamicMeshes.erase(std::string(objectKey));
    return object;
}

size_t RendererResourceCache::CountPlacementScopedRenderableObjects() const
{
    if (!worldLocalResources)
        return 0;
    size_t count = 0;
    for (const auto& [key, object] : worldLocalResources->renderableObjects)
        if (object && IsPlacementScopedRenderableKey(key))
            ++count;
    return count;
}

void RendererResourceCache::AdvanceTressfxSimulations(float deltaSeconds,
    const WorldWind& sceneWind)
{
    if (!worldLocalResources)
        return;
    // 场景风作者目标 -> CPU 参考 solver 的向量（厘米/秒）。GT 不拥有渲染线程的
    // SceneWindRuntime，所以这里只取方向与强度，不复制缓动/阵风，也不另造时钟；
    // 强度为 0 时 direction * 0 即零向量，solver 走零风分支。GPU backend 完全不读这里。
    const Eigen::Vector3f sceneWindVelocityCmPerSec =
        sceneWind.direction * sceneWind.strength;
    for (const auto& [key, object] : worldLocalResources->renderableObjects)
    {
        // GPU backend 由 HairSimulationPass 的 compute dispatch 推进，游戏线程不步进。
        if (object && object->IsTressfx() && !object->UsesGpuGuideSimulation())
            object->AdvanceTressfxSimulation(
                deltaSeconds, sceneWindVelocityCmPerSec);
    }
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

void RendererResourceCache::RemoveMaterialInstance(
    std::string_view materialInstanceKey)
{
    GetMutableWorldLocalResources().materialInstances.erase(
        std::string(materialInstanceKey));
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
