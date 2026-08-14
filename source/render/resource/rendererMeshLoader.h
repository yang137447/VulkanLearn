#pragma once

#include <optional>
#include <vector>

#include <nlohmann/json.hpp>

#include "mesh/meshAssetTypes.h"

class PipelineFactory;
struct MeshAssetLoadRequest;
struct MeshObjectBuildPlan;

namespace VL
{
class RendererBackendVulkan;
struct RendererResourceLoadContext;

struct RendererMeshLoadResult
{
    std::vector<MeshObjectBuildPlan> objectPlans;
    std::optional<SpeedTreeWindProfile> speedTreeWindProfile;
};

// Creates renderable sections and mesh draw bindings for mesh scene nodes.
// Material creation is delegated to RendererMaterialLoader, while CPU/GPU
// resources are registered in RendererResourceCache.
class RendererMeshLoader
{
public:
    RendererMeshLoader(
        PipelineFactory& pipelineFactory,
        RendererBackendVulkan& rendererBackend,
        RendererResourceLoadContext& loadContext);

    RendererMeshLoadResult LoadMeshObject(
        const nlohmann::basic_json<>& node,
        const MeshAssetLoadRequest& meshLoadRequest) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
    RendererResourceLoadContext& loadContext;
};

} // namespace VL
