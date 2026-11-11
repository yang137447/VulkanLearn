#pragma once

#include <memory>
#include <vector>

#include "core/runtimeResult.h"
#include "render/frontend/renderScene.h"
#include "render/shadow/materialShadowCasterPolicy.h"

class Material;
class MaterialInstance;
class RenderableObject;

namespace VL
{

class RendererResourceCache;
class RendererObjectResourceEntry;

// CPU-side draw list consumed by the current Vulkan draw path. RenderScene owns
// the stable grouping and handles; this resolved view maps those handles to
// renderer resource cache entries used by backend draw execution.
struct ResolvedDrawPacket
{
    size_t drawPacketIndex = 0;
    // 强引用保证 resolve 完成到录制之间，即使 GT 从 cache 摘除对象，
    // 在途 draw 仍拥有完整的 CPU/GPU 资源生命周期。
    std::shared_ptr<RenderableObject> renderableObject;
    std::shared_ptr<RendererObjectResourceEntry> objectResourceEntry;
};

struct ResolvedMaterialInstanceGroup
{
    std::shared_ptr<MaterialInstance> materialInstance;
    std::vector<ResolvedDrawPacket> draws;
};

struct ResolvedMaterialGroup
{
    std::shared_ptr<Material> material;
    MaterialShadowCasterDecision shadowCaster;
    std::vector<ResolvedMaterialInstanceGroup> materialInstances;
};

struct ResolvedRenderScene
{
    std::vector<ResolvedMaterialGroup> materialGroups;

    bool NeedsObjectResources() const
    {
        for (const auto& group : materialGroups)
            for (const auto& instance : group.materialInstances)
                for (const auto& draw : instance.draws)
                    if (!draw.objectResourceEntry) return true;
        return false;
    }
};

class ResolvedRenderSceneBuilder
{
public:
    RuntimeResult<ResolvedRenderScene> Build(
        const RenderScene& renderScene,
        const RendererResourceCache& resourceCache) const;
};

} // namespace VL
