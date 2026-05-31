#pragma once

#include <memory>
#include <vector>

#include "core/runtimeResult.h"
#include "render/frontend/renderScene.h"

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
    std::weak_ptr<RenderableObject> renderableObject;
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
    std::vector<ResolvedMaterialInstanceGroup> materialInstances;
};

struct ResolvedRenderScene
{
    std::vector<ResolvedMaterialGroup> materialGroups;
};

class ResolvedRenderSceneBuilder
{
public:
    RuntimeResult<ResolvedRenderScene> Build(
        const RenderScene& renderScene,
        const RendererResourceCache& resourceCache) const;
};

} // namespace VL
