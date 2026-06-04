#pragma once

#include <vector>

#include <nlohmann/json.hpp>

class PipelineFactory;
struct MeshAssetLoadRequest;
struct MeshObjectBuildPlan;

namespace VL
{
class RendererBackendVulkan;

// Creates renderable sections and mesh draw bindings for mesh scene nodes.
// Material creation is delegated to RendererMaterialLoader, while CPU/GPU
// resources are registered in RendererResourceCache.
class RendererMeshLoader
{
public:
    RendererMeshLoader(PipelineFactory& pipelineFactory, RendererBackendVulkan& rendererBackend);

    std::vector<MeshObjectBuildPlan> LoadMeshObject(
        const nlohmann::basic_json<>& node,
        const MeshAssetLoadRequest& meshLoadRequest) const;

private:
    PipelineFactory& pipelineFactory;
    RendererBackendVulkan& rendererBackend;
};

} // namespace VL
