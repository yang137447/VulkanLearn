#include "render/frame/renderWorldSnapshot.h"

#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "render/frontend/renderScene.h"
#include "render/resource/rendererResourceCache.h"
#include "render/resource/resourceRetireQueue.h"
#include "renderableObject.h"

namespace VL
{

void PlacementRenderableRuntime::EnsurePlacementRenderables(
    const VL::RenderScene& renderScene, VL::RendererResourceCache& resourceCache)
{
    // World 换代时旧键的帧计数没有意义；整包由 transaction 退休，这里只重置跟踪。
    if (renderScene.worldGeneration != worldGeneration)
    {
        absentFrames.clear();
        objectNames.clear();
        liveCount = 0;
        cachedPlacementCount = 0;
        runtimeCount = 0;
        bindingCache.clear();
        worldGeneration = renderScene.worldGeneration;
    }
    for (const VL::RenderDrawPacket& draw : renderScene.drawPackets)
    {
        if (draw.placementTemplateMeshKey.empty())
            continue;
        if (draw.mesh.key.empty())
            throw std::runtime_error(
                "Placement-scoped draw packet has no instance mesh key: " + draw.debugName);
        const std::shared_ptr<RenderableObject> existing =
            resourceCache.GetRenderableObject(draw.mesh.key);
        if (existing != nullptr)
            continue;
        const std::shared_ptr<RenderableObject> templateObject =
            resourceCache.GetRenderableObject(draw.placementTemplateMeshKey);
        if (templateObject == nullptr)
            throw std::runtime_error(
                "Placement template renderable is missing: template=" +
                draw.placementTemplateMeshKey + " instance=" + draw.mesh.key);
        // 模板未解析绑定时（例如异步预加载的蓝图资产），解析需要来源 draw 的骨骼
        // 资产。按 sceneObjectIdentity 在本帧快照里找来源对象；缺失交给 handler 报错。
        const RenderableObject* sourceObject = nullptr;
        if (!draw.placementBinding.sourceSceneObject.empty())
            for (const VL::RenderDrawPacket& candidate : renderScene.drawPackets)
            {
                if (candidate.sceneObjectIdentity !=
                    draw.placementBinding.sourceSceneObject) continue;
                const std::shared_ptr<RenderableObject> found =
                    resourceCache.GetRenderableObject(candidate.mesh.key);
                if (found != nullptr) sourceObject = found.get();
                break;
            }
        // 克隆本身不感知类型；绑定（毛发骨骼等）由通用 resolver 按 Kind 分派。
        std::shared_ptr<RenderableObject> clone =
            templateObject->CloneForPlacement(draw.debugName,
                draw.placementBinding.tressfx ? &*draw.placementBinding.tressfx : nullptr,
                draw.placementBinding.tressfxGroupConfigs.empty()
                    ? nullptr : &draw.placementBinding.tressfxGroupConfigs);
        std::string bindingError;
        if (!VL::ApplyPlacementBinding(draw.placementBinding, *templateObject,
                sourceObject, *clone, bindingCache, bindingError))
            throw std::runtime_error(bindingError + " instance=" + draw.mesh.key);
        resourceCache.BindRenderableObject(draw.mesh.key, clone);
        absentFrames[draw.mesh.key] = 0;
        objectNames[draw.mesh.key] = draw.debugName;
        std::cout << "[Placement] instance renderable: object=" << draw.debugName
                  << " template=" << draw.placementTemplateMeshKey
                  << " kind=" << static_cast<int>(draw.placementBinding.kind) << std::endl;
    }
}

void PlacementRenderableRuntime::PrunePlacementRenderables(const VL::RenderScene& renderScene)
{
    if (renderScene.worldGeneration != worldGeneration)
    {
        // 换代由 World package 统一退休；旧键的帧计数没有意义。
        absentFrames.clear();
        objectNames.clear();
        liveCount = 0;
        cachedPlacementCount = 0;
        worldGeneration = renderScene.worldGeneration;
        return;
    }
    std::set<std::string> live;
    for (const VL::RenderDrawPacket& draw : renderScene.drawPackets)
        if (!draw.placementTemplateMeshKey.empty())
            live.insert(draw.mesh.key);
    // 只在存活数变化时打一行：spawn 后 1、destroy 后 0，同时作为无泄漏计数。
    if (live.size() != liveCount)
    {
        liveCount = live.size();
        std::cout << "[Placement] live placement renderables: " << liveCount
                  << std::endl;
    }
    // 活动包里实际持有的 placement-scoped renderable 数；包级释放后必须回到 0。
    const size_t cacheCount =
        VL::RendererResourceCache::GetInstance().CountPlacementScopedRenderableObjects();
    if (cacheCount != cachedPlacementCount)
    {
        cachedPlacementCount = cacheCount;
        std::cout << "[Placement] placement-scoped renderables in cache: " << cachedPlacementCount
                  << std::endl;
    }
    std::vector<std::string> retired;
    for (auto it = absentFrames.begin(); it != absentFrames.end();)
    {
        if (live.count(it->first) != 0)
        {
            // 本帧还在快照里：清零缺失计数。不能删除跟踪项，否则对象第一次存活
            // 就会失去跟踪，销毁时永远不会退休（泄漏）。
            it->second = 0;
            ++it;
            continue;
        }
        if (++it->second >= 2)
            retired.push_back(it->first);
        ++it;
    }
    if (!retired.empty())
    {
        const uint64_t lastSubmittedEpoch =
            VL::ResourceRetireQueue::GetInstance().GetLastSubmittedEpoch();
        for (const std::string& key : retired)
        {
            absentFrames.erase(key);
            const auto nameIt = objectNames.find(key);
            const std::string objectName =
                nameIt != objectNames.end() ? nameIt->second : std::string("<unknown>");
            std::shared_ptr<RenderableObject> object =
                VL::RendererResourceCache::GetInstance().TakeRenderableObject(key);
            if (object == nullptr)
            {
                // 蓝图资产释放路径可能已经先摘走了这个 renderable；这里只记录事实，
                // 不能重复退休同一个键。
                std::cout << "[Placement] instance renderable released by package: object="
                          << objectName << " key=" << key << std::endl;
                objectNames.erase(key);
                continue;
            }
            // 上一帧可能还在 GPU 上引用它；按已提交 epoch 交给统一退休队列。
            VL::ResourceRetireQueue::GetInstance().RetireShared(
                key, renderScene.worldGeneration, lastSubmittedEpoch, std::move(object));
            objectNames.erase(key);
            std::cout << "[Placement] instance renderable retired: object=" << objectName
                      << " key=" << key << std::endl;
        }
    }
    // 由 Ensure 创建、尚未退休的运行时放置 renderable 数；destroy 后必须回到 0。
    if (objectNames.size() != runtimeCount)
    {
        runtimeCount = objectNames.size();
        std::cout << "[Placement] runtime placement renderables: "
                  << runtimeCount << std::endl;
    }
}

} // namespace VL


